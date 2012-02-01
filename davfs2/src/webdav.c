/*  webdav.c: send requests to the WebDAV server.
    Copyright (C) 2006, 2007, 2008, 2009 Werner Baumann

    This file is part of davfs2.

    davfs2 is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    davfs2 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with davfs2; if not, write to the Free Software Foundation,
    Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA. */


#include "config.h"

#include <errno.h>
#include <error.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_ICONV_H
#include <iconv.h>
#endif
#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif
#ifdef HAVE_LIBINTL_H
#include <libintl.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <time.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#include "xalloc.h"
#include "xstrndup.h"
#include "xvasprintf.h"

#include <ne_auth.h>
#include <ne_basic.h>
#include <ne_dates.h>
#include <ne_locks.h>
#include <ne_props.h>
#include <ne_request.h>
#include <ne_session.h>
#include <ne_socket.h>
#include <ne_string.h>
#include <ne_uri.h>
#include <ne_utils.h>
#include <ne_xml.h>

#include "defaults.h"
#include "mount_davfs.h"
#include "webdav.h"

#ifdef ENABLE_NLS
#define _(String) gettext(String)
#else
#define _(String) String
#endif


/* Data Types */
/*============*/

/* Data structures used as userdata in neon callback functions. */

typedef struct {
    const char *path;           /* The *not* url-encoded path. */
    dav_props *results;         /* Start of the linked list of dav_props. */
} propfind_context;

typedef struct {
    int error;                  /* An error occured while reading/writing. */
    const char *file;           /* cache_file to store the data in. */
    int fd;                     /* file descriptor of the open cache file. */
} get_context;

typedef struct {
    int error;
    off64_t total;              /* Total amount of available bytes. */
    off64_t used;               /* Used bytes. */
} quota_context;


/* Private constants */
/*===================*/

/* Properties to be retrieved from the server. This constants
   are used by dav_get_collection(). */
enum {
    ETAG = 0,
    LENGTH,
    CREATION,
    MODIFIED,
    TYPE,
    EXECUTE,
    END
};

static const ne_propname prop_names[] = {
    [ETAG] = {"DAV:", "getetag"},
    [LENGTH] = {"DAV:", "getcontentlength"},
    [CREATION] ={"DAV:", "creationdate"},
    [MODIFIED] = {"DAV:", "getlastmodified"},
    [TYPE] = {"DAV:", "resourcetype"},
    [EXECUTE] = {"http://apache.org/dav/props/", "executable"},
    [END] = {NULL, NULL}
};

static const ne_propname anonymous_prop_names[] = {
    [ETAG] = {NULL, "getetag"},
    [LENGTH] = {NULL, "getcontentlength"},
    [CREATION] ={NULL, "creationdate"},
    [MODIFIED] = {NULL, "getlastmodified"},
    [TYPE] = {NULL, "resourcetype"},
    [EXECUTE] = {NULL, "executable"},
    [END] = {NULL, NULL}
};

/* Properties to be retrieved from the server. This constants
   are used by dav_quota(). */
enum {
    AVAILABLE = 0,
    USED
};

static const ne_propname quota_names[] = {
    [AVAILABLE] = {"DAV:", "quota-available-bytes"},
    [USED] = {"DAV:", "quota-used-bytes"},
    {NULL, NULL}
};

static size_t log_bufsize = 512;

static char *none_match_header = "If-None-Match: *\r\n";


/* Private global variables */
/*==========================*/

/* The neon session.
   Will be set by dav_init_webdav(). */
static ne_session *session;

/* Lock store, lock owner and lock timeout for session.
   Will be set by dav_init_webdav(). */
static ne_lock_store *locks;
static char *owner;
static time_t lock_timeout;

/* Credentials for this session. Will be set by dav_init_webdav(). */
static char *username;
static char *password;
static char *p_username;
static char *p_password;

/* If this is not NULL the server must present exactly this certificate. */
static ne_ssl_certificate *server_cert;

/* Whether to send expect 100-continue header in PUT requests. */
static int use_expect100;

/* Whether to use HEAD for prechecking instead of If-Match and If-None-Match
   on PTU and LOCK. */
static int has_if_match_bug;

/* Some servers sends a weak invalid etag that turns into a valid strong etag
   after one second. With this flag set weak etags are not used at all,
   otherwise the weakness indicator is removed. */
static int drop_weak_etags;

/* Check with HEAD for existence or modification of a file, before locking or
   uploading a new file. */
static int precheck;

/* Ignore the information in the DAV-header because it is wrong. */
static int ignore_dav_header;

/* Will be set to 1 when dav_init_connection() succeeded. */
static int initialized;

/* Whether a terminal is available to communicate with the user.
   Should be reset with set_no_terminal() when forking into daemon mode.
   Needed by  ssl_verify() which may be called at any time. */
static int have_terminal;

#if defined DAV_USE_ICONV && defined HAVE_ICONV_H
/* Handle to convert character encoding from utf-8 to LC_CTYPE.
   If NULL no conversion is done. */
static iconv_t from_utf_8;

/* Handle to convert character encoding from LC_CTYPE to utf-8.
   If NULL no conversion is done. */
static iconv_t to_utf_8;

/* Handle to convert character encing of path names to LC_CTYPE. If NULL
   no conversion is done. */
static iconv_t from_server_enc;

/* Handle to convert from LC_CTYPE to character encoding of path names.
   If NULL no conversion is done. */
static iconv_t to_server_enc;
#endif /* DAV_USE_ICONV && HAVE_ICONV_H */

/* A GNU custom stream, used to redirect neon debug messages to syslog. */
static FILE *log_stream;

/* A user defined header that is added to all requests. */
char *custom_header;

#if NE_VERSION_MINOR > 25
/* Session cookie. */
static char *cookie;
#endif


/* Private function prototypes and inline functions */
/*==================================================*/

#if defined DAV_USE_ICONV && defined HAVE_ICONV_H
static void
convert(char **s, iconv_t conv);
#endif

static int
get_error(int ret, const char *method);

static int
get_ne_error(const char *method);

static struct ne_lock *
lock_by_path(const char *path);

static int
lock_discover(const char *path, time_t *expire);

static void
lock_refresh(struct ne_lock *lock, time_t *expire);

static ssize_t
log_writer(void *cookie, const char *buffer, size_t size);

static char *
normalize_etag(const char *etag);

static void
replace_slashes(char **name);

/* Call-back functions for neon. */

static void
add_header(ne_request *req, void *userdata, ne_buffer *header);

static int
auth(void *userdata, const char *realm, int attempt, char *user, char *pwd);

static int
file_reader(void *userdata, const char *block, size_t length);

#if NE_VERSION_MINOR < 26

static void
lock_result(void *userdata, const struct ne_lock *lock, const char *uri,
            const ne_status *status);

static void
prop_result(void *userdata, const char *href, const ne_prop_result_set *set);

static void
quota_result(void *userdata, const char *href, const ne_prop_result_set *set);

#else /* NE_VERSION_MINOR >= 26 */

static void
lock_result(void *userdata, const struct ne_lock *lock, const ne_uri *uri,
            const ne_status *status);

static void
prop_result(void *userdata, const ne_uri *uri, const ne_prop_result_set *set);

static void
quota_result(void *userdata, const ne_uri *uri, const ne_prop_result_set *set);

#endif /* NE_VERSION_MINOR >= 26 */

static int
quota_reader(void *userdata, const char *block, size_t length);

static int
ssl_verify(void *userdata, int failures, const ne_ssl_certificate *cert);

#if NE_VERSION_MINOR > 25
static int
update_cookie(ne_request *req, void *userdata, const ne_status *status);
#endif /* NE_VERSION_MINOR > 25 */


/* Public functions */
/*==================*/

void
dav_init_webdav(dav_args *args)
{
    if (args->neon_debug & ~NE_DBG_HTTPPLAIN)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Initializing webdav");

#if defined DAV_USE_ICONV && defined HAVE_ICONV_H
    char *lc_charset = nl_langinfo(CODESET);
    if (lc_charset && strcasecmp(lc_charset, "UTF-8") != 0) {
        from_utf_8 = iconv_open(lc_charset, "UTF-8");
        if (from_utf_8 == (iconv_t) -1)
            from_utf_8 = 0;
        to_utf_8 = iconv_open("UTF-8", lc_charset);
        if (to_utf_8 == (iconv_t) -1)
            to_utf_8 = 0;
    }

    if (lc_charset && args->s_charset
            && strcasecmp(args->s_charset, lc_charset) != 0) {
        if (strcasecmp(args->s_charset, "UTF-8") == 0) {
            from_server_enc = from_utf_8;
            to_server_enc = to_utf_8;
        } else {
            from_server_enc = iconv_open(lc_charset, args->s_charset);
            if (from_server_enc == (iconv_t) -1)
                from_server_enc = 0;
            to_server_enc = iconv_open(args->s_charset, lc_charset);
            if (to_server_enc == (iconv_t) -1)
                to_server_enc = 0;
        }
    }
#endif /* DAV_USE_ICONV && HAVE_ICONV_H */

    if (ne_sock_init() != 0)
        error(EXIT_FAILURE, errno, _("socket library initialization failed"));

    if (args->neon_debug & ~NE_DBG_HTTPPLAIN) {
        char *buf = xmalloc(log_bufsize);
        cookie_io_functions_t *log_func = xmalloc(sizeof(cookie_io_functions_t));
        log_func->read = NULL;
        log_func->write = log_writer;
        log_func->seek = NULL;
        log_func->close = NULL;
        log_stream = fopencookie(buf, "w", *log_func);
        if (!log_stream)
            error(EXIT_FAILURE, errno,
                  _("can't open stream to log neon-messages"));
        ne_debug_init(log_stream, args->neon_debug);
    }

    session = ne_session_create(args->scheme, args->host, args->port);

    ne_set_read_timeout(session, args->read_timeout);

#if NE_VERSION_MINOR > 26
    ne_set_connect_timeout(session, args->connect_timeout);
#endif /* NE_VERSION_MINOR > 26 */

    char *useragent = xasprintf("%s/%s", PACKAGE_TARNAME, PACKAGE_VERSION);
    ne_set_useragent(session, useragent);
    free(useragent);

    if (args->username)
        username = xstrdup(args->username);
    if (args->password)
        password = xstrdup(args->password);
#if NE_VERSION_MINOR < 26
    ne_set_server_auth(session, auth, "server");
#else /* NE_VERSION_MINOR >= 26 */
    ne_add_server_auth(session, NE_AUTH_ALL, auth, "server");
#endif /* NE_VERSION_MINOR >= 26 */

    if (args->useproxy && args->p_host) {
        ne_session_proxy(session, args->p_host, args->p_port);
        if (args->p_user)
            p_username = xstrdup(args->p_user);
        if (args->p_passwd)
            p_password = xstrdup(args->p_passwd);
#if NE_VERSION_MINOR < 26
        ne_set_proxy_auth(session, auth, "proxy");
#else /* NE_VERSION_MINOR >= 26 */
        ne_add_proxy_auth(session, NE_AUTH_ALL, auth, "proxy");
#endif /* NE_VERSION_MINOR >= 26 */
    }

    if (strcmp(args->scheme, "https") == 0) {
        if (!ne_has_support(NE_FEATURE_SSL))
            error(EXIT_FAILURE, 0, _("neon library does not support TLS/SSL"));

        ne_ssl_set_verify(session, ssl_verify, NULL);
        if (args->server_cert) {
            server_cert = args->server_cert;
            args->server_cert = NULL;
        } else {
            ne_ssl_trust_default_ca(session);
            if (args->ca_cert)
                ne_ssl_trust_cert(session, args->ca_cert);
        }

        if (args->clicert) {
            uid_t orig = geteuid();
            seteuid(0);
            ne_ssl_client_cert *client_cert
                    = ne_ssl_clicert_read(args->clicert);
            seteuid(orig);
            if (!client_cert)
                error(EXIT_FAILURE, 0, _("can't read client certificate %s"),
                      args->clicert);
            if (client_cert && ne_ssl_clicert_encrypted(client_cert)) {
                char *pw = NULL;
                if (!args->clicert_pw && args->askauth) {
                    printf(_("Please enter the password to decrypt client\n"
                             "certificate %s.\n"), args->clicert);
                    pw = dav_user_input_hidden(_("Password: "));
                } else {
                    pw = xstrdup(args->clicert_pw);
                }
                int ret = 1;
                if (pw) {
                    ret = ne_ssl_clicert_decrypt(client_cert, pw);
                    memset(pw, '\0', strlen(pw));
                    free(pw);
                }
                if (ret)
                    error(EXIT_FAILURE, 0,
                          _("can't decrypt client certificate %s"),
                          args->clicert);
            }
            ne_ssl_set_clicert(session, client_cert);
            ne_ssl_clicert_free(client_cert);
        }
    }

    have_terminal = args->askauth;

    if (args->locks) {
        locks = ne_lockstore_create();
        if (!args->lock_owner) {
            if (!args->username) {
                owner = xstrdup(PACKAGE_STRING);
            } else {
                owner = xstrdup(args->username);
            }
        } else {
            owner = xstrdup(args->lock_owner);
        }
        lock_timeout = args->lock_timeout;
    }

    if (args->header) {
        custom_header = xstrdup(args->header);
        ne_hook_pre_send(session, add_header, custom_header);
    }

#if NE_VERSION_MINOR > 25
    if (args->allow_cookie)
        ne_hook_post_send(session, update_cookie, NULL);
#endif /* NE_VERSION_MINOR > 25 */

    use_expect100 = args->expect100;
    has_if_match_bug = args->if_match_bug;
    drop_weak_etags = args->drop_weak_etags;
    precheck = args->precheck;
    ignore_dav_header = args->ignore_dav_header;
}


int
dav_init_connection(const char *path)
{
    char *spath = ne_path_escape(path);
    ne_server_capabilities caps = {0, 0, 0};
    int ret = ne_options(session, spath, &caps);

    if (!ret) {
        initialized = 1;
        if (!caps.dav_class1 && !ignore_dav_header) {
            if (have_terminal) {
                error(EXIT_FAILURE, 0,
                      _("mounting failed; the server does not support WebDAV"));
            } else {
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
                       _("mounting failed; the server does not support WebDAV"));
                ret = EINVAL;
            }
        }
        if ((caps.dav_class2 || ignore_dav_header) && locks) {
            ne_lockstore_register(locks, session);
        } else if (locks) {
            if (have_terminal)
                error(0, 0, _("warning: the server does not support locks"));
            ne_lockstore_destroy(locks);
            locks = NULL;
        }
    } else {
        ret = get_error(ret, "OPTIONS");
    }

    free(spath);
    return ret;
}


void
dav_close_webdav(void)
{
    if(session && log_stream)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Closing connection to  %s",
               ne_get_server_hostport(session));

    if (locks) {
        struct ne_lock *lock = ne_lockstore_first(locks);
        while (lock) {
            if (session) {
                int ret = ne_unlock(session, lock);
                get_error(ret, "UNLOCK");
            }
            lock = ne_lockstore_next(locks);
        }
    }

    if (session)
        ne_session_destroy(session);
    ne_sock_exit();
}


char *
dav_conv_from_utf_8(const char *s)
{
    char *new = xstrdup(s);
#if defined DAV_USE_ICONV && defined HAVE_ICONV_H
    if (from_utf_8)
        convert(&new, from_utf_8);
#endif
    return new;
}


char *
dav_conv_to_utf_8(const char *s)
{
    char *new = xstrdup(s);
#if defined DAV_USE_ICONV && defined HAVE_ICONV_H
    if (to_utf_8)
        convert(&new, to_utf_8);
#endif
    return new;
}


char *
dav_conv_from_server_enc(const char *s)
{
    char *new = xstrdup(s);
#if defined DAV_USE_ICONV && defined HAVE_ICONV_H
    if (from_server_enc)
        convert(&new, from_server_enc);
#endif
    return new;
}


char *
dav_conv_to_server_enc(const char *s)
{
    char *new = xstrdup(s);
#if defined DAV_USE_ICONV && defined HAVE_ICONV_H
    if (to_server_enc)
        convert(&new, to_server_enc);
#endif
    return new;
}


int
dav_delete(const char *path, time_t *expire)
{
    int ret;
    if (!initialized) {
        ret = dav_init_connection(path);
        if (ret) return ret;
    }

    struct ne_lock *lock = NULL;
    char *spath = ne_path_escape(path);
    ret = ne_delete(session, spath);
    ret = get_error(ret, "DELETE");

    if ((ret == EACCES || ret == ENOENT) && locks) {
        lock_discover(spath, expire);
        lock = lock_by_path(spath);
        if (lock && ret == EACCES) {
            ret = ne_delete(session, spath);
            ret = get_error(ret, "DELETE");
        } else if (lock) {
            ne_unlock(session, lock);
            ret = 0;
        }
    }

    if (!ret && locks) {
        lock = lock_by_path(spath);
        if (lock) {
            ne_lockstore_remove(locks, lock);
            ne_lock_destroy(lock);
        }
    }

    free(spath);
    return ret;
}


int
dav_delete_dir(const char *path)
{
    int ret;
    if (!initialized) {
        ret = dav_init_connection(path);
        if (ret) return ret;
    }

    char *spath = ne_path_escape(path);
    ret = ne_delete(session, spath);
    ret = get_error(ret, "DELETE");

    free(spath);
    return ret;
}


void
dav_delete_props(dav_props *props)
{
    if (props->path)
        free(props->path);
    if (props->name)
        free(props->name);
    if (props->etag) 
        free(props->etag);
    free(props);
}


int
dav_get_collection(const char *path, dav_props **props)
{
    int ret;
    if (!initialized) {
        ret = dav_init_connection(path);
        if (ret) return ret;
    }

    propfind_context ctx;
    ctx.path = path;
    ctx.results = NULL;

    char *spath = ne_path_escape(path);
    ne_propfind_handler *ph = ne_propfind_create(session, spath, NE_DEPTH_ONE);
    ret = ne_propfind_named(ph, prop_names, prop_result, &ctx);
    ret = get_error(ret, "PROPFIND");
    ne_propfind_destroy(ph);
    free(spath);

    if (ret) {
        while(ctx.results) {
            dav_props *tofree = ctx.results;
            ctx.results = ctx.results->next;
            dav_delete_props(tofree);
        }    
    }

    *props = ctx.results;
    return ret;
}


const char *
dav_get_webdav_error()
{
    return ne_get_error(session);
}


int
dav_get_file(const char *path, const char *cache_path, off_t *size,
             char **etag, time_t *mtime, char **mime, int *modified)
{
    int ret;
    if (!initialized) {
        ret = dav_init_connection(path);
        if (ret) return ret;
    }

    get_context ctx;
    ctx.error = 0;
    ctx.file = cache_path;
    ctx.fd = 0;

    char *spath = ne_path_escape(path);
    ne_request *req = ne_request_create(session, "GET", spath);

    if (etag && *etag)
        ne_add_request_header(req, "If-None-Match", *etag);
    char *mod_time = NULL;
    if (mtime && *mtime) {
        mod_time = ne_rfc1123_date(*mtime);
        ne_add_request_header(req, "If-Modified-Since", mod_time);
    }

    ne_add_response_body_reader(req, ne_accept_2xx, file_reader, &ctx);

    ret = ne_request_dispatch(req);
    ret = get_error(ret, "GET");
    if (ctx.error)
        ret = ctx.error;
    const ne_status *status = ne_get_status(req);

    if (!ret && status->code == 200) {
 
        if (modified)
            *modified = 1;

        const char *value = ne_get_response_header(req, "Content-Length");
        if (value)
#if _FILE_OFFSET_BITS == 64
            *size = strtoll(value, NULL, 10);
#else /* _FILE_OFFSET_BITS != 64 */
            *size = strtol(value, NULL, 10);
#endif /* _FILE_OFFSET_BITS != 64 */

        value = ne_get_response_header(req, "Last-Modified");
        if (mtime && value)
            *mtime = ne_httpdate_parse(value);

        if (etag) {
            if (*etag) free(*etag);
            *etag = normalize_etag(ne_get_response_header(req, "ETag"));
        }

        value = ne_get_response_header(req, "Content-Type");
        if (mime && value) {
            if (*mime)
                free(*mime);
            *mime = xstrdup(value);
        }
    }

    ne_request_destroy(req);
    if (ctx.fd > 0)
        close(ctx.fd);
    if (mod_time)
        free(mod_time);
    free(spath);
    return ret;
}


int
dav_head(const char *path, char **etag, time_t *mtime, off_t *length,
         char **mime)
{
    int ret;
    if (!initialized) {
        ret = dav_init_connection(path);
        if (ret) return ret;
    }

    char *spath = ne_path_escape(path);
    ne_request *req = ne_request_create(session, "HEAD", spath);
    ret = ne_request_dispatch(req);
    ret = get_error(ret, "HEAD");

    const char *value = ne_get_response_header(req, "ETag");
    if (!ret && etag && value) {
        if (*etag) free(*etag);
        *etag = normalize_etag(value);
    }

    value = ne_get_response_header(req, "Last-Modified");
    if (!ret && mtime && value) {
        time_t lm = ne_httpdate_parse(value);
        if (lm)
            *mtime = lm;
    }

    value = ne_get_response_header(req, "Content-Length");
    if (!ret && length && value)
        *length = strtol(value, NULL, 10);

    value = ne_get_response_header(req, "Content-Type");
    if (!ret && mime && value) {
        if (*mime)
            free(*mime);
        *mime = xstrdup(value);
    }

    ne_request_destroy(req);
    free(spath);
    return ret;
}


int
dav_lock(const char *path, time_t *expire, int *exists)
{
    int ret;
    if (!initialized) {
        ret = dav_init_connection(path);
        if (ret) return ret;
    }

    if (!locks) {
        *expire = 0;
        return 0;
    }

#if NE_VERSION_MINOR > 25
    if (precheck && has_if_match_bug && !*exists) {
#else /* NE_VERSION_MINOR == 25 */
    if (precheck && !*exists) {
#endif /* NE_VERSION_MINOR == 25 */
        if (dav_head(path, NULL, NULL, NULL, NULL) == 0) {
            return EEXIST;
        }
    }

    char *spath = ne_path_escape(path);

    struct ne_lock *lock = lock_by_path(spath);
    if (lock) {
        free(spath);
        *expire = -1;
        lock_refresh(lock, expire);
        return 0;
    }

    lock = ne_lock_create();
    ne_fill_server_uri(session, &lock->uri);
    lock->uri.path = spath;
    lock->owner = xstrdup(owner);
    lock->timeout = lock_timeout;

#if NE_VERSION_MINOR > 25
    if (!has_if_match_bug && !*exists)
        ne_hook_pre_send(session, add_header, none_match_header);
#endif /* NE_VERSION_MINOR > 25 */
    ret = ne_lock(session, lock);
    ret = get_error(ret, "LOCK");
#if NE_VERSION_MINOR > 25
    if (!has_if_match_bug && !*exists) {
        ne_unhook_pre_send(session, add_header, none_match_header);
        if (ret && strtol(ne_get_error(session), NULL, 10) == 412)
            ret = EEXIST;
    }
#endif /* NE_VERSION_MINOR > 25 */

    if (!ret) {
        ne_lockstore_add(locks, lock);
        if (strtol(ne_get_error(session), NULL, 10) == 201)
            *exists = 1;
        if (lock->timeout <= 0) {
            *expire = LONG_MAX;
        } else {
            *expire = lock->timeout + time(NULL);
        }
    } else {
        if (ret == EACCES && lock_discover(spath, expire) == 0)
            ret = 0;
        ne_lock_destroy(lock);
    }

    return ret;
}


void
dav_lock_refresh(const char *path, time_t *expire)
{
    if (!initialized && dav_init_connection(path) != 0)
        return;

    if (!locks) {
        *expire = 0;
        return;
    }

    char *spath = ne_path_escape(path);
    struct ne_lock *lock = lock_by_path(spath);

    if (!lock) {
        lock_discover(spath, expire);
    } else {
        lock_refresh(lock, expire);
    }

    free(spath);
}


int
dav_make_collection(const char *path)
{
    int ret;
    if (!initialized) {
        ret = dav_init_connection(path);
        if (ret) return ret;
    }

    char *spath = ne_path_escape(path);
    ret = ne_mkcol(session, spath);
    ret = get_error(ret, "MKCOL");

    free(spath);
    return ret;
}


int
dav_move(const char *src, const char *dst)
{
    int ret;
    if (!initialized) {
        ret = dav_init_connection(src);
        if (ret) return ret;
    }

    char *spath = ne_path_escape(src);
    char *dst_path = ne_path_escape(dst);
    ret = ne_move(session, 1, spath, dst_path); 
    ret = get_error(ret, "MOVE");

    if (!ret && locks) {
        struct ne_lock *lock = lock_by_path(spath);
        if (lock) {
            ne_lockstore_remove(locks, lock);
            ne_lock_destroy(lock);
        }
    }

    free(spath);
    free(dst_path);
    return ret;
}


int
dav_put(const char *path, const char *cache_path, int *exists, time_t *expire,
        char **etag, time_t *mtime, char **mime, int execute)
{
    int ret = 0;
    if (!initialized) {
        ret = dav_init_connection(path);
        if (ret) return ret;
    }


    if (precheck && (has_if_match_bug || (*exists && (!etag || !*etag)))
            && (!*exists || (etag && *etag) || (mtime && *mtime))) {
        char *r_etag = NULL;
        time_t r_mtime = 0;
        off_t r_length = 0;
        ret = dav_head(path, &r_etag, &r_mtime, &r_length, NULL);
        if (!ret) {
            if (!*exists && r_length) {
                ret = EEXIST;
            } else if (etag && *etag && r_etag) {
                if (strcmp(*etag, r_etag) != 0)
                    ret = EINVAL;
            } else if (mtime && *mtime && r_mtime) {
                if (*mtime < r_mtime)
                    ret = EINVAL;
            }
        } else if (ret == ENOENT) {
            if (!*exists) {
                ret = 0;
            } else {
                ret = dav_unlock(path, expire);
            }
        }
        if (r_etag)
            free(r_etag);
        if (ret)
            return ret;
    }

    int fd = open(cache_path, O_RDONLY);
    if (fd <= 0)
        return EIO;
    struct stat st;
    if (fstat(fd, &st) != 0)
        return EIO;

    char *spath = ne_path_escape(path);
    ne_request *req = ne_request_create(session, "PUT", spath);

    if (!has_if_match_bug) {
        if (!*exists) {
            ne_add_request_header(req, "If-None-Match", "*");
        } else {
            if (etag && *etag)
                ne_add_request_header(req, "If-Match", *etag);
        }
    }

    if (use_expect100)
#if NE_VERSION_MINOR == 25
        ne_set_request_expect100(req, 1);
#else /* NE_VERSION_MINOR > 25 */
        ne_set_request_flag(req, NE_REQFLAG_EXPECT100, 1);
#endif /* NE_VERSION_MINOR > 25 */

    ne_lock_using_resource(req, spath, 0);
#if _FILE_OFFSET_BITS == 64 && NE_VERSION_MINOR < 27
    ne_set_request_body_fd64(req, fd, 0, st.st_size);
#else /* _FILE_OFFSET_BITS != 64 || NE_VERSION_MINOR >= 27 */
    ne_set_request_body_fd(req, fd, 0, st.st_size);
#endif /* _FILE_OFFSET_BITS != 64 || NE_VERSION_MINOR >= 27 */

    ret = ne_request_dispatch(req);
    ret = get_error(ret, "PUT");

    if (ret == EACCES && lock_discover(spath, expire) == 0) {

        ne_request_destroy(req);
        req = ne_request_create(session, "PUT", spath);

        if (!has_if_match_bug) {
            if (!*exists) {
                ne_add_request_header(req, "If-None-Match", "*");
            } else {
                if (etag && *etag)
                    ne_add_request_header(req, "If-Match", *etag);
            }
        }

        if (use_expect100)
#if NE_VERSION_MINOR == 25
            ne_set_request_expect100(req, 1);
#else /* NE_VERSION_MINOR > 25 */
            ne_set_request_flag(req, NE_REQFLAG_EXPECT100, 1);
#endif /* NE_VERSION_MINOR > 25 */

        ne_lock_using_resource(req, spath, 0);
#if _FILE_OFFSET_BITS == 64  && NE_VERSION_MINOR < 27
        ne_set_request_body_fd64(req, fd, 0, st.st_size);
#else /* _FILE_OFFSET_BITS != 64 || NE_VERSION_MINOR >= 27 */
        ne_set_request_body_fd(req, fd, 0, st.st_size);
#endif /* _FILE_OFFSET_BITS != 64 || NE_VERSION_MINOR >= 27 */

        ret = ne_request_dispatch(req);
        ret = get_error(ret, "PUT");
    }

    int need_head = 0;
    if (!ret) {

        *exists = 1;
        const char *value;

        if (etag) {
            if (*etag) free(*etag);
            *etag = normalize_etag(ne_get_response_header(req, "ETag"));
            if (!*etag)
                need_head = 1;
        }

        if (mtime) {
            value = ne_get_response_header(req, "Last-Modified");
            if (!value)
                value = ne_get_response_header(req, "Date");
            if (value) {
                *mtime = ne_httpdate_parse(value);
            } else {
                need_head = 1;
            }
        }

        if (mime) {
            value = ne_get_response_header(req, "Content-Type");
            if (value) {
                if (*mime)
                    free(*mime);
                *mime = xstrdup(value);
            }
        }
    }

    ne_request_destroy(req);
    free(spath);
    close(fd);

    if (!ret) {
        if (execute == 1)
            dav_set_execute(path, execute);
        if (need_head)
            dav_head(path, etag, mtime, NULL, mime);
    }

    return ret;
}


int
dav_quota(const char *path, off64_t *total, off64_t *used)
{
    int ret;
    if (!initialized) {
        ret = dav_init_connection(path);
        if (ret) return ret;
    }

    static int use_rfc = 1;
    static int use_userinfo = 1;

    if (!use_rfc && !use_userinfo)
        return EIO;

    quota_context ctx;
    ctx.error = 0;
    ctx.total = 0;
    ctx.used = 0;
    ret = EIO;
    char *spath = ne_path_escape(path);

    if (use_rfc) {
        ne_propfind_handler *ph = ne_propfind_create(session, spath,
                                                     NE_DEPTH_ZERO);
        ret = ne_propfind_named(ph, quota_names, quota_result, &ctx);
        ret = get_error(ret, "PROPFIND");
        ne_propfind_destroy(ph);

        if (!ret && ctx.error) {
            ret = EIO;
            if (ctx.error == 2)
                use_rfc = 0;
        }
    }

    if (ret && use_userinfo) {
        ctx.error = 0;
        ne_request *req = ne_request_create(session, "USERINFO", spath);
        ne_add_response_body_reader(req, ne_accept_2xx, quota_reader, &ctx);
        ret = ne_request_dispatch(req);
        ret = get_error(ret, "USERINFO");
        ne_request_destroy(req);

        if (!ret) {
            if (ctx.error)
                ret = EIO;
        } else if (ret != EAGAIN) {
            use_userinfo = 0;
        }
    }

    if (!ret) {
        *total = ctx.total;
        *used = ctx.used;
    }

    free(spath);
    return ret;
}


int
dav_set_execute(const char *path, int set)
{
    int ret;
    if (!initialized) {
        ret = dav_init_connection(path);
        if (ret) return ret;
    }

    ne_proppatch_operation op[2];
    op[0].name = &prop_names[EXECUTE];
    op[0].type = ne_propset;
    if (set) {
        op[0].value = "T";
    } else {
        op[0].value = "F";
    }
    op[1].name = NULL;

    char *spath = ne_path_escape(path);
    ret = ne_proppatch(session, spath, &op[0]);
    ret = get_error(ret, "PROPPATCH");
    free(spath);

    return ret;
}


void
dav_set_no_terminal(void)
{
    have_terminal = 0;
}


int
dav_unlock(const char *path, time_t *expire)
{
    int ret;
    if (!initialized) {
        ret = dav_init_connection(path);
        if (ret) return ret;
    }

    if (!locks) {
        *expire = 0;
        return 0;
    }

    char *spath = ne_path_escape(path);
    struct ne_lock *lock = lock_by_path(spath);
    free(spath);

    if (!lock) {
        *expire = 0;
        return 0;
    }

    ret = ne_unlock(session, lock);
    ret = get_error(ret, "UNLOCK");

    if (!ret || ret == ENOENT || ret == EINVAL) {
        ne_lockstore_remove(locks, lock);
        ne_lock_destroy(lock);
        *expire = 0;
        return 0;
    }

    return ret;
}



/* Private functions */
/*===================*/

#if defined DAV_USE_ICONV && defined HAVE_ICONV_H
static void
convert(char **s, iconv_t conv)
{
    size_t insize = strlen(*s);
    char *in = *s;
    size_t outsize = MB_LEN_MAX * (insize + 1);
    char *buf = xcalloc(outsize, 1);
    char *out = buf;

    iconv(conv, NULL, NULL, &out, &outsize);
    if (iconv(conv, &in, &insize, &out, &outsize) >= 0
            && insize == 0 && outsize >= MB_LEN_MAX) {
        memset(out, 0, MB_LEN_MAX);
        free(*s);
        *s = xstrndup(buf, out - buf + MB_LEN_MAX);
    }

    free(buf);
}
#endif /* DAV_USE_ICONV && HAVE_ICONV_H */


/* Returns a file error code according to ret from the last WebDAV
   method call. If ret has value NE_ERROR the error code from the session is
   fetched and translated.
   ret    : the error code returned from NEON.
   method : name of the WebDAV method, used for debug messages.
   return value : a file error code according to errno.h. */
static int
get_error(int ret, const char *method)
{
    int err;
    switch (ret) {
    case NE_OK:
    case NE_ERROR:
        err = get_ne_error(method);
        break;
    case NE_LOOKUP:
        err = EIO;
        break;
    case NE_AUTH:
        err = EPERM;
        break;
    case NE_PROXYAUTH:
        err = EPERM;
        break;
    case NE_CONNECT:
        err = EAGAIN;
        break;
    case NE_TIMEOUT:
        err = EAGAIN;
        break;
    case NE_FAILED:
        err = EINVAL;
        break;
    case NE_RETRY:
        err = EAGAIN;
        break;
    case NE_REDIRECT:
        err = ENOENT;
        break;
    default:
        err = EIO;
        break;
    }

    return err;
}


/* Get the error from the session and translates it into a file error code.
   method : name of the WebDAV method, used for debug messages.
   return value : a file error code according to errno.h. */
static int
get_ne_error(const char *method)
{
    const char *text = ne_get_error(session);

    char *tail;
    int err = strtol(text, &tail, 10);
    if (tail == text)
        return EIO;

    switch (err) {
        case 200:           /* OK */
        case 201:           /* Created */
        case 202:           /* Accepted */
        case 203:           /* Non-Authoritative Information */
        case 204:           /* No Content */
        case 205:           /* Reset Content */
        case 207:           /* Multi-Status */
        case 304:           /* Not Modified */
            return 0;
        case 401:           /* Unauthorized */
        case 402:           /* Payment Required */
        case 407:           /* Proxy Authentication Required */
            return EPERM;
        case 301:           /* Moved Permanently */
        case 303:           /* See Other */
        case 404:           /* Not Found */
        case 410:           /* Gone */
            return ENOENT;
        case 408:           /* Request Timeout */
        case 504:           /* Gateway Timeout */
            return EAGAIN;
        case 423:           /* Locked */
            return EACCES;
        case 400:           /* Bad Request */
        case 403:           /* Forbidden */
        case 405:           /* Method Not Allowed */
        case 409:           /* Conflict */
        case 411:           /* Length Required */
        case 412:           /* Precondition Failed */
        case 414:           /* Request-URI Too Long */
        case 415:           /* Unsupported Media Type */
        case 424:           /* Failed Dependency */
        case 501:           /* Not Implemented */
            return EINVAL;
        case 413:           /* Request Entity Too Large */
        case 507:           /* Insufficient Storage */
            return ENOSPC;
        case 206:           /* Partial Content */
        case 300:           /* Multiple Choices */
        case 302:           /* Found */
        case 305:           /* Use Proxy */
        case 306:           /* (Unused) */
        case 307:           /* Temporary Redirect */
        case 406:           /* Not Acceptable */
        case 416:           /* Requested Range Not Satisfiable */
        case 417:           /* Expectation Failed */
        case 422:           /* Unprocessable Entity */
        case 500:           /* Internal Server Error */
        case 502:           /* Bad Gateway */
        case 503:           /* Service Unavailable */
        case 505:           /* HTTP Version Not Supported */
            return EIO;
        default:
            return EIO;
    }
}


/* Searches for a lock for resource at path and returns
   this lock if successfull, NULL otherwise. */
static struct ne_lock *
lock_by_path(const char *path)
{
    struct ne_lock *lock = ne_lockstore_first(locks);
    while (lock) {
        if (strcmp(path, lock->uri.path) == 0)
            return lock;
        lock = ne_lockstore_next(locks);
    }

    return NULL;
}


/* Checks if there is already a lock for file path that is owned by owner. If
   successful it stores the lock in the global lock store, refreshes the lock
   and updates expire.
   If no matching lock is found or the session is initialized with the nolocks
   option, it  sets expire to 0 and returns -1.
   If a matching lock is found, but it can not be refreshed, expire is set
   to -1 (= locked, expire time unknown).
   If an error occurs it leaves expire unchanged and returns -1:
   path   : URL-escaped path of the file on the server.
   expire : The time when the lock expires, will be updated.
   return value : 0 if a matching lock is found, -1 otherwise. */
static int
lock_discover(const char *path, time_t *expire)
{
    if (!locks) {
        *expire = 0;
        return -1;
    }

    struct ne_lock *lock = NULL;
    int ret = ne_lock_discover(session, path, lock_result, &lock);
    ret = get_error(ret, "LOCKDISCOVER");

    if (!ret && lock)  {
        *expire = -1;
        lock_refresh(lock, expire);
        return 0;
    } else {
        if (!ret)
            *expire = 0;
        return -1;
    }
}


/* Refreshes lock and updates expire.
   If an error occurs it does nothing.
   lock   : The lock, will be updated on success.
   expire : The time when the lock expires, updated on success. */
static void
lock_refresh(struct ne_lock *lock, time_t *expire)
{
    int ret = ne_lock_refresh(session, lock);
    ret = get_error(ret, "LOCKREFRESH");

    if (!ret) {
        if (lock->timeout <= 0) {
            *expire = LONG_MAX;
        } else {
            *expire = lock->timeout + time(NULL);
        }
    }
}


static ssize_t
log_writer(void *cookie, const char *buffer, size_t size)
{
    if (size <= 0)
        return 0;

    size_t written = 0;
    const char *bpos = buffer;
    while (written < size) {
        size_t n = 2;
        char *cpos = (char *) cookie;
        while (n < log_bufsize && written < size) {
            if (*bpos == '%') {
                *cpos++ = '%';
                n++;
            }
            *cpos++ = *bpos++;
            n++;
            written++;
        }
        *cpos = '\0';
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "%s", (char *) cookie);
    }
    return written;
}


/* Checks etag for weakness indicator and quotation marks.
   The reurn value is either a strong etag with quotation marks or NULL.
   Depending on global variable drop_weak_etags weak etags are either
   dropped or convertet into strong ones. */
static char *
normalize_etag(const char *etag)
{
    if (!etag) return NULL;

    const char * e = etag;
    if (*e == 'W') {
        if (drop_weak_etags) {
            return NULL;
        } else {
            e++;
            if (*e == '/') {
                e++;
            } else {
                return NULL;
            }
        }
    }
    if (!*e) return NULL;

    char *ne = NULL;
    if (*e == '\"') {
        ne = strdup(e);
    } else {
        if (asprintf(&ne, "\"%s\"", e) < 0)
            ne = NULL;;
    }

    return ne;
}


/* Replaces slashes in name by "slash-", "-slash-" or "-slash", depending
   on the position of the slash within name. */
static void
replace_slashes(char **name)
{
    char *slash = strchr(*name, '/');
    while (slash) {
        char *end = *name + strlen(*name) - 1;
        char *nn;
        *slash = '\0';
        if (slash == *name) {
            nn = xasprintf("slash-%s", slash + 1);
        } else if (slash == end) {
            nn = xasprintf("%s-slash", *name);
        } else {
            nn = xasprintf("%s-slash-%s", *name, slash + 1);
        }
        free(*name);
        *name = nn;
        slash = strchr(*name, '/');
    }
}


/* Call-back functions for neon. */

static void
add_header(ne_request *req, void *userdata, ne_buffer *header)
{
    ne_buffer_zappend(header, (char *) userdata);
}


/* Copies credentials from global variables into user and pwd.
   userdata must be a string with value "server" or "proxy", to decide what
   the creditentials are needed for.
   The creditentials are taken form global variables username/password or
   p_username/p_password.
   If attempt > 0, this is logged as an error and the value of attempt is
   returned, so neon will not try again.
   userdata : What the credentials are needed for ("server" or "proxy").
   realm    : Used for error log.
   attempt  : Number of attempts to get credentials. If not 0 an error occured.
   user     : A buffer of size NE_ABUFSIZ to return the username.
   pwd      : A buffer of size NE_ABUFSIZ to return the password.
   return value : value if attempt. neon will not call this function again if
                  it is greater than 0. */
static int
auth(void *userdata, const char *realm, int attempt, char *user, char *pwd)
{
    if (attempt) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), _("authentication failure:"));
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "  %s", realm);
        return attempt;
    }

    if (strcmp((char *) userdata, "server") == 0) {
        if (username)
            strncpy(user, username, NE_ABUFSIZ - 1);
        if (password)
            strncpy(pwd, password, NE_ABUFSIZ - 1);
    } else if (strcmp((char *) userdata, "proxy") == 0) {
        if (p_username)
            strncpy(user, p_username, NE_ABUFSIZ - 1);
        if (p_password)
            strncpy(pwd, p_password, NE_ABUFSIZ - 1);
    }

    return 0;
}


/* Reads HTTP-data from blockand writes them to a local file.
   userdata must be a get_context structure that holds at least the name of
   the local file. If it does not contain a file descriptor, the file is
   opened for writing and the file descriptor is stored in the get_context
   structure. In case of an error a error flag is set. 
   userdata : A get_context structure, containing the name of the local file,
              the file descriptor (if the file is open), and an error flag.
   block    : Buffer containing the data.
   length   : Number of bytes in the buffer.
   return value : 0 on success, EIO otherwise. */
static int
file_reader(void *userdata, const char *block, size_t length)
{
    get_context *ctx = (get_context *) userdata;
    if (!ctx->fd)
        ctx->fd = open(ctx->file, O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
    if (ctx->fd <= 0) {
        ne_set_error(session, _("%i can't open cache file"), 0);
        ctx->error = EIO;
    }

    while (!ctx->error && length > 0) {
        ssize_t ret = write(ctx->fd, block, length);
        if (ret < 0) {
            ctx->error = EIO;
            ne_set_error(session, _("%i error writing to cache file"), 0);
        } else {
            length -= ret;
            block += ret;
        }
    }

    return ctx->error;
}


/* If the owner of this lock is the same as global variable owner, lock is
   stored in the global lock store locks and a pointer to the lock is
   returned in userdata.
   Otherwise it does nothing.
   userdata : *userdata will be set to lock, if lock is ownded ba owner.
   lock     : a lock found by ne_lock_discover() on the server.
   uri      : not used.
   status   : not used. */
 
#if NE_VERSION_MINOR < 26

static void
lock_result(void *userdata, const struct ne_lock *lock, const char *uri,
            const ne_status *status)
{

#else     /* NE_VERSION_MINOR >= 26 */

static void
lock_result(void *userdata, const struct ne_lock *lock, const ne_uri *uri,
            const ne_status *status)
{

#endif    /* NE_VERSION_MINOR >= 26 */

    if (!locks || !owner || !userdata || !lock || !lock->owner)
        return;

    if (strcmp(lock->owner, owner) == 0) {
        struct ne_lock *l = ne_lock_copy(lock);
        ne_lockstore_add(locks, l);
        l->timeout = lock_timeout;
        *((struct ne_lock **) userdata) = l;
    }
}


/* Called by ne_propfind_named(). Evaluates a dav_props structure from
   href/uri and set and stores it in userdata.
   userdata must be a pointer to a propfind_context structure. Its member
   path holds the unescaped path of the collection. Its member results is
   a linked list of the dav_props structures.
   The unescaped version of href/uri->path must must be equal to the path of
   the collection or a descendent of it. It is stored as member path of the
   dav_props structure. It will be normalized (collections have a trailing
   slash, non-collections do not have one).
   The name is derived from path. The name of the
   collection itself will be the empty string. If name contains
   a '/'-character it is replaced by the ugly string "-slash-".
   There must not be two dav_props structure with the same path or the same
   name. in this case one of them is removed from the list, preferable the one
   that is not a directory or that is less specific.
   userdata : A pointer to a propfind_context structure containing the path of
              the collection and the linked list of properties.
if NE_VERSION_MINOR < 26
   href     : Value of the href propertiy returned by the server. It may be
              the complete URL of the collection or the path only.
else
   uri      : ne_uri of the resource as returned from the server.
endif
   set      : Points to the set of properties returned from the server.*/

#if NE_VERSION_MINOR < 26

static void
prop_result(void *userdata, const char *href, const ne_prop_result_set *set)
{
    propfind_context *ctx = (propfind_context *) userdata;
    if (!ctx || !href || !set)
        return;

    ne_uri uri;
    if (ne_uri_parse(href, &uri) != 0 || !uri.path) {
        ne_uri_free(&uri);
        return;
    }

    char *tmp_path = (char *) xmalloc(strlen(uri.path) + 1);
    const char *from = uri.path;

#else /* NE_VERSION_MINOR >= 26 */

static void
prop_result(void *userdata, const ne_uri *uri, const ne_prop_result_set *set)
{
    propfind_context *ctx = (propfind_context *) userdata;
    if (!ctx || !uri || !uri->path || !set)
        return;

    char *tmp_path = (char *) xmalloc(strlen(uri->path) + 1);
    const char *from = uri->path;

#endif /* NE_VERSION_MINOR >= 26 */

    char *to = tmp_path;
    while (*from) {
        while (*from == '/' && *(from + 1) == '/')
            from++;
        *to++ = *from++;
    }
    *to = 0;
    dav_props *result = xcalloc(1, sizeof(dav_props));
    result->path = ne_path_unescape(tmp_path);
    free (tmp_path);

#if NE_VERSION_MINOR < 26
    ne_uri_free(&uri);
#endif


    if (!result->path || strlen(result->path) < 1) {
        dav_delete_props(result);
        return;
    }

    const char *data;

    data = ne_propset_value(set, &prop_names[TYPE]);
    if (!data)
        data = ne_propset_value(set, &anonymous_prop_names[TYPE]);
    if (data && strstr(data, "collection"))
            result->is_dir = 1;

    if (*(result->path + strlen(result->path) - 1) == '/') {
        if (!result->is_dir)
            *(result->path + strlen(result->path) - 1) = '\0';
    } else {
        if (result->is_dir) {
            char *tmp = xasprintf("%s/", result->path);
            free(result->path);
            result->path = tmp;
        }
    }

    if (strstr(result->path, ctx->path) != result->path) {
        dav_delete_props(result);
        return;
    }

    if (strcmp(result->path, ctx->path) == 0) {
        result->name = xstrdup("");
    } else {
        if (strlen(result->path) < (strlen(ctx->path) + result->is_dir + 1)) {
            dav_delete_props(result);
            return;
        }
        result->name = xstrndup(result->path + strlen(ctx->path),
                                  strlen(result->path) - strlen(ctx->path)
                                  - result->is_dir);
        replace_slashes(&result->name);
#if defined DAV_USE_ICONV && defined HAVE_ICONV_H
        if (from_server_enc)
            convert(&result->name, from_server_enc);
#endif
    }

    data = ne_propset_value(set, &prop_names[ETAG]);
    if (!data)
        data = ne_propset_value(set, &anonymous_prop_names[ETAG]);
    result->etag = normalize_etag(data);

    data = ne_propset_value(set, &prop_names[LENGTH]);
    if (!data)
        data = ne_propset_value(set, &anonymous_prop_names[LENGTH]);
    if (data)
#if _FILE_OFFSET_BITS == 64
         result->size = strtoll(data, NULL, 10);
#else /* _FILE_OFFSET_BITS != 64 */
         result->size = strtol(data, NULL, 10);
#endif /* _FILE_OFFSET_BITS != 64 */

    data = ne_propset_value(set, &prop_names[CREATION]);
    if (!data)
        data = ne_propset_value(set, &anonymous_prop_names[CREATION]);
    if (data) {
        result->ctime = ne_iso8601_parse(data);
        if (result->ctime == (time_t) -1)
            result->ctime = ne_httpdate_parse(data);
        if (result->ctime == (time_t) -1)
            result->ctime = 0;
    }

    data = ne_propset_value(set, &prop_names[MODIFIED]);
    if (!data)
        data = ne_propset_value(set, &anonymous_prop_names[MODIFIED]);
    if (data) {
        result->mtime = ne_httpdate_parse(data);
        if (result->mtime == (time_t) -1)
            result->mtime = ne_iso8601_parse(data);
        if (result->mtime == (time_t) -1)
            result->mtime = 0;
    }

    data = ne_propset_value(set, &prop_names[EXECUTE]);
    if (!data)
        data = ne_propset_value(set, &anonymous_prop_names[EXECUTE]);
    if (!data) {
        result->is_exec = -1;
    } else if (*data == 'T') {
        result->is_exec = 1;
    }

    result->next = ctx->results;
    ctx->results = result;
}


static int
quota_reader(void *userdata, const char *block, size_t length)
{
    if (length < 1) return 0;
    quota_context *ctx = (quota_context *) userdata;

    char *quota = strndup(block, length);
    if (!quota) {
        ctx->error = 1;
        return 0;
    }

    char *number = strtok(quota, ",");
    if (number) {
        ctx->total = strtoull(number, NULL, 10);
    } else {
        ctx->error = 1;
        free(quota);
        return 0;
    }

    number = strtok(NULL, ",");
    if (number)
        ctx->used = strtoull(number, NULL, 10);

    free(quota);
    return 0;
}


/* Reads available and used bytes from set and stores them in
   userdata. */
#if NE_VERSION_MINOR < 26

static void
quota_result(void *userdata, const char *href, const ne_prop_result_set *set)
{
    quota_context *ctx = (quota_context *) userdata;
    if (!ctx || !href || !set)
        return;

#else /* NE_VERSION_MINOR >= 26 */

static void
quota_result(void *userdata, const ne_uri *uri, const ne_prop_result_set *set)
{
    quota_context *ctx = (quota_context *) userdata;
    if (!ctx  || !uri || !uri->path || !set)
        return;

#endif /* NE_VERSION_MINOR >= 26 */

    const char *data = ne_propset_value(set, &quota_names[AVAILABLE]);
    if (data) {
        ctx->total = strtoull(data, NULL, 10);
    } else {
        const ne_status *st = ne_propset_status(set, &quota_names[AVAILABLE]);
        if (st && st->klass == 4) {
            ctx->error = 2;
        } else {
            ctx->error = 1;
        }
        return;
    }

    data = ne_propset_value(set, &quota_names[USED]);
    if (data)
        ctx->used = strtoull(data, NULL, 10);

    ctx->total += ctx->used;
}


/* Displays information about cert and asks the user whether to accept
   the certificate or not.
   If no terminal is available (according to global variable Have_terminal)
   it returns an error. Else it displays an error message and certificate
   date and ask whether to accept the certificate. If the user accepts
   it returns 0, otherwise an error.
   In any case the event is logged.
   userdata : not used.
   failures : a constant indicating the kind of error.
   cert     : the server certificate that could not be verified by neon.
   return value : 0 accept the certificate for this session.
                  -1 don't accept the certificate. */
static int
ssl_verify(void *userdata, int failures, const ne_ssl_certificate *cert)
{
    if (server_cert) {
        if (ne_ssl_cert_cmp(cert, server_cert) == 0)
            return 0;
        if (have_terminal)
            error(0, 0, _("the server certificate is not trusted"));
        return -1;
    }

    char *issuer = ne_ssl_readable_dname(ne_ssl_cert_issuer(cert));
    char *subject = ne_ssl_readable_dname(ne_ssl_cert_subject(cert));
    char *digest = xcalloc(1, NE_SSL_DIGESTLEN);
    if (!issuer || !subject || ne_ssl_cert_digest(cert, digest) != 0) {
        if (have_terminal) {
            error(0, 0, _("error processing server certificate"));
        } else {
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
                   _("error processing server certificate"));
        }
        if (issuer) free(issuer);
        if (subject) free(subject);
        if (digest) free(digest);
        return -1;
    }

    int ret = -1;
    if (have_terminal) {
        if (failures & NE_SSL_NOTYETVALID)
            error(0, 0, _("the server certificate is not yet valid"));
        if (failures & NE_SSL_EXPIRED)
            error(0, 0, _("the server certificate has expired"));
        if (failures & NE_SSL_IDMISMATCH)
            error(0, 0, _("the server certificate does not match the server name"));
        if (failures & NE_SSL_UNTRUSTED)
            error(0, 0, _("the server certificate is not trusted"));
        if (failures & ~NE_SSL_FAILMASK)
            error(0, 0, _("unknown certificate error"));
        printf(_("  issuer:      %s"), issuer);
        printf("\n");
        printf(_("  subject:     %s"), subject);
        printf("\n");
        printf(_("  identity:    %s"), ne_ssl_cert_identity(cert));
        printf("\n");
        printf(_("  fingerprint: %s"), digest);
        printf("\n");
            printf(_("You only should accept this certificate, if you can\n"
                     "verify the fingerprint! The server might be faked\n"
                     "or there might be a man-in-the-middle-attack.\n"));
            printf(_("Accept certificate for this session? [y,N] "));
            char *s = NULL;
            size_t n = 0;
            ssize_t len = 0;
            len = getline(&s, &n, stdin);
            if (len < 0)
                abort();
            if (rpmatch(s) > 0)
                ret = 0;
            free(s);
    } 

    if (failures & NE_SSL_NOTYETVALID)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("the server certificate is not yet valid"));
    if (failures & NE_SSL_EXPIRED)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("the server certificate has expired"));
    if (failures & NE_SSL_IDMISMATCH)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("the server certificate does not match the server name"));
    if (failures & NE_SSL_UNTRUSTED)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("the server certificate is not trusted"));
    if (failures & ~NE_SSL_FAILMASK)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("unknown certificate error"));
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), _("  issuer: %s"), issuer);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), _("  subject: %s"), subject);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), _("  identity: %s"),
                       ne_ssl_cert_identity(cert));
    if (!ret) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), _("  accepted by user"));
    }

    if (issuer) free(issuer);
    if (subject) free(subject);
    if (digest) free(digest);
    return ret;
}


#if NE_VERSION_MINOR > 25
static int
update_cookie(ne_request *req, void *userdata, const ne_status *status)
{
    if (status->klass != 2)
        return NE_OK;

    const char *cookie_hdr = ne_get_response_header(req, "Set-Cookie2");
    if (!cookie_hdr) {
        cookie_hdr = ne_get_response_header(req, "Set-Cookie");
    }
    if (!cookie_hdr)
        return NE_OK;

    if (cookie && strstr(cookie_hdr, cookie) == cookie_hdr)
        return NE_OK;

    char *sep = strpbrk(cookie_hdr, "\",; \n\r\0");
    if (!sep)
        return NE_OK;
    if (*sep == '\"')
        sep = strpbrk(sep + 1, "\"");
    if (!sep)
        return NE_OK;

    if (cookie) {
        ne_unhook_pre_send(session, add_header, cookie);
        free(cookie);
        cookie = NULL;
    }

    char *value = xstrndup(cookie_hdr, sep - cookie_hdr + 1);
    cookie = xasprintf("Cookie: $Version=1;%s\r\n", value);
    free(value);

    ne_hook_pre_send(session, add_header, cookie);
    return NE_OK;
}
#endif /* NE_VERSION_MINOR > 25 */
