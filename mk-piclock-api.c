/*
 * mk-piclock-api.c
 *
 * Public libmicrohttpd gateway for mk-piclock.
 * HTTP terminates here. The hardware daemon uses a compact binary IPC protocol.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <signal.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "asset_store.h"
#include "ipc_protocol.h"
#include "util.h"


#define API_NAME "mk-piclock-api"
#define API_VERSION "1.0"
#define PRODUCT_VERSION "1.6.10"
#define DEFAULT_PUBLIC_BIND "0.0.0.0"
#define DEFAULT_PUBLIC_PORT 8080
#define CORE_SOCKET_PATH "/run/mk-piclock/core.sock"
#define WEB_DIR "/opt/mk-piclock/web"
#define API_DOC_DIR "/opt/mk-piclock/api"
#define MAX_REQUEST_BODY (128U * 1024U * 1024U)
#define MAX_STATIC_FILE (64U * 1024U * 1024U)
#define ALLOWED_ORIGIN_MAX 256
#define DISK_RESERVE_BYTES (64ULL * 1024ULL * 1024ULL)
#define DEFAULT_MUSIC_QUOTA_BYTES (1024ULL * 1024ULL * 1024ULL)
#define SOCKET_TIMEOUT_SEC 15
#define MHD_THREAD_POOL_SIZE 2
#define MHD_CONNECTION_LIMIT 12
#define FORM_FIELDS_MAX 64
#define FORM_KEY_MAX 64
#define FORM_VALUE_MAX 512
#define UPLOAD_FILES_MAX 256

static volatile sig_atomic_t g_running = 1;
static char g_allowed_origin[ALLOWED_ORIGIN_MAX];
static uid_t g_expected_core_uid = (uid_t)-1;
static uint64_t g_music_quota_bytes = DEFAULT_MUSIC_QUOTA_BYTES;
static uint64_t g_disk_reserve_bytes = DISK_RESERVE_BYTES;

enum route_id {
    ROUTE_STATUS,
    ROUTE_HEALTH,
    ROUTE_FACES_LIST,
    ROUTE_FONTS_LIST,
    ROUTE_MUSIC_LIST,
    ROUTE_FACE_SOURCE,
    ROUTE_FONT_FILE,
    ROUTE_UPLOAD_FACE,
    ROUTE_UPLOAD_BEDTIME_FACE,
    ROUTE_UPLOAD_MUSIC,
    ROUTE_UPLOAD_FONT,
    ROUTE_DELETE_FACE,
    ROUTE_DELETE_ALL_FACES,
    ROUTE_DELETE_ALL_MUSIC,
    ROUTE_DELETE_FONT,
    ROUTE_DISPLAY_ACTION,
    ROUTE_DISPLAY_FACE,
    ROUTE_DISPLAY_MESSAGE,
    ROUTE_MESSAGE_LIMITS,
    ROUTE_MESSAGE_FIT,
    ROUTE_CONFIG_ALARM,
    ROUTE_CONFIG_AUDIO,
    ROUTE_CONFIG_PERSONALIZATION,
    ROUTE_CONFIG_DISPLAY,
    ROUTE_LOGS,
    ROUTE_LOGS_CLEAR
};

struct api_route {
    const char *method;
    const char *path;
    enum route_id id;
};

static const struct api_route g_routes[] = {
    {"GET",  "/api/v1/status",                         ROUTE_STATUS},
    {"GET",  "/api/v1/health",                         ROUTE_HEALTH},
    {"GET",  "/api/v1/assets/faces",                   ROUTE_FACES_LIST},
    {"GET",  "/api/v1/assets/fonts",                   ROUTE_FONTS_LIST},
    {"GET",  "/api/v1/assets/music",                   ROUTE_MUSIC_LIST},
    {"GET",  "/api/v1/assets/faces/source",            ROUTE_FACE_SOURCE},
    {"GET",  "/api/v1/assets/fonts/file",              ROUTE_FONT_FILE},
    {"POST", "/api/v1/assets/faces/upload",            ROUTE_UPLOAD_FACE},
    {"POST", "/api/v1/assets/faces/bedtime/upload",    ROUTE_UPLOAD_BEDTIME_FACE},
    {"POST", "/api/v1/assets/music/upload",            ROUTE_UPLOAD_MUSIC},
    {"POST", "/api/v1/assets/fonts/upload",            ROUTE_UPLOAD_FONT},
    {"POST", "/api/v1/assets/faces/delete",            ROUTE_DELETE_FACE},
    {"POST", "/api/v1/assets/faces/delete-all",        ROUTE_DELETE_ALL_FACES},
    {"POST", "/api/v1/assets/music/delete-all",        ROUTE_DELETE_ALL_MUSIC},
    {"POST", "/api/v1/assets/fonts/delete",            ROUTE_DELETE_FONT},
    {"POST", "/api/v1/display/action",                  ROUTE_DISPLAY_ACTION},
    {"POST", "/api/v1/display/face",                    ROUTE_DISPLAY_FACE},
    {"POST", "/api/v1/display/message",                 ROUTE_DISPLAY_MESSAGE},
    {"GET",  "/api/v1/display/message/limits",          ROUTE_MESSAGE_LIMITS},
    {"GET",  "/api/v1/display/message/fit",             ROUTE_MESSAGE_FIT},
    {"POST", "/api/v1/config/alarms",                   ROUTE_CONFIG_ALARM},
    {"POST", "/api/v1/config/audio",                    ROUTE_CONFIG_AUDIO},
    {"POST", "/api/v1/config/personalization",          ROUTE_CONFIG_PERSONALIZATION},
    {"POST", "/api/v1/config/display",                  ROUTE_CONFIG_DISPLAY},
    {"GET",  "/api/v1/logs",                            ROUTE_LOGS},
    {"POST", "/api/v1/logs/clear",                      ROUTE_LOGS_CLEAR}
};

struct form_field {
    char key[FORM_KEY_MAX];
    char value[FORM_VALUE_MAX];
    size_t len;
};

struct upload_file {
    char key[FORM_KEY_MAX];
    char filename[MP_ASSET_NAME_MAX];
    char temp_path[512];
    int fd;
    uint64_t next_offset;
    size_t size;
};

struct request_context {
    const struct api_route *route;
    struct MHD_PostProcessor *post;
    struct form_field *fields;
    size_t field_count;
    size_t field_cap;
    struct upload_file *uploads;
    size_t upload_count;
    size_t upload_cap;
    size_t received_bytes;
    int parse_failed;
    int body_too_large;
    int response_queued;
    size_t upload_limit;
};

struct ipc_result {
    unsigned int status;
    unsigned int content_type;
    unsigned char *body;
    size_t body_len;
};

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static int public_port_from_env(void) {
    const char *value = getenv("MK_PICLOCK_API_PORT");
    if (!value || !*value) return DEFAULT_PUBLIC_PORT;
    char *end = NULL;
    errno = 0;
    long port = strtol(value, &end, 10);
    if (errno || end == value || *end || port < 1 || port > 65535) return -1;
    return (int)port;
}

static const char *public_bind_from_env(void) {
    const char *value = getenv("MK_PICLOCK_API_BIND");
    return value && *value ? value : DEFAULT_PUBLIC_BIND;
}

static uint64_t bytes_from_env(const char *name, uint64_t fallback) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno || end == value || *end) return fallback;
    return (uint64_t)parsed;
}

static uid_t resolve_user_uid(const char *environment_name, const char *default_user) {
    const char *value = getenv(environment_name);
    if (value && *value) {
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(value, &end, 10);
        if (!errno && end != value && *end == '\0') return (uid_t)parsed;
        struct passwd *entry = getpwnam(value);
        if (entry) return entry->pw_uid;
    }
    struct passwd *entry = getpwnam(default_user);
    return entry ? entry->pw_uid : (uid_t)-1;
}

static int request_origin_allowed(struct MHD_Connection *connection) {
    const char *origin = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Origin");
    return origin && g_allowed_origin[0] && strcmp(origin, g_allowed_origin) == 0;
}

static size_t route_upload_limit(const struct api_route *route) {
    if (!route) return 0;
    switch (route->id) {
        case ROUTE_UPLOAD_FACE:
        case ROUTE_UPLOAD_BEDTIME_FACE:
            return MP_FACE_UPLOAD_MAX_BYTES;
        case ROUTE_UPLOAD_MUSIC:
            return MP_MUSIC_UPLOAD_MAX_BYTES;
        case ROUTE_UPLOAD_FONT:
            return MP_FONT_UPLOAD_MAX_BYTES;
        default:
            return 0;
    }
}

static const struct api_route *find_route(const char *method, const char *path) {
    for (size_t i = 0; i < sizeof(g_routes) / sizeof(g_routes[0]); i++) {
        if (strcmp(method, g_routes[i].method) == 0 && strcmp(path, g_routes[i].path) == 0)
            return &g_routes[i];
    }
    return NULL;
}

static enum MHD_Result add_header(struct MHD_Response *response, const char *name, const char *value) {
    return MHD_add_response_header(response, name, value);
}

static void add_api_headers(struct MHD_Connection *connection, struct MHD_Response *response) {
    (void)add_header(response, "Cache-Control", "no-store");
    if (request_origin_allowed(connection)) {
        (void)add_header(response, "Access-Control-Allow-Origin", g_allowed_origin);
        (void)add_header(response, "Vary", "Origin");
    }
    (void)add_header(response, "X-MK-PICLOCK-API-Version", API_VERSION);
    (void)add_header(response, "X-Content-Type-Options", "nosniff");
    (void)add_header(response, "Referrer-Policy", "no-referrer");
}

static enum MHD_Result queue_buffer(struct MHD_Connection *connection, unsigned int status,
                                    const char *content_type, void *body, size_t body_len,
                                    enum MHD_ResponseMemoryMode mode, int api_headers) {
    struct MHD_Response *response = MHD_create_response_from_buffer(body_len, body, mode);
    if (!response) {
        if (mode == MHD_RESPMEM_MUST_FREE) free(body);
        return MHD_NO;
    }
    if (content_type) (void)add_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, content_type);
    if (api_headers) add_api_headers(connection, response);
    enum MHD_Result result = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return result;
}

static enum MHD_Result queue_json(struct MHD_Connection *connection, unsigned int status, const char *json) {
    const char *body = json ? json : "{}";
    return queue_buffer(connection, status, "application/json; charset=utf-8",
                        (void *)body, strlen(body), MHD_RESPMEM_MUST_COPY, 1);
}

static enum MHD_Result queue_json_builder(struct MHD_Connection *connection, unsigned int status,
                                          struct mp_buffer *buffer) {
    size_t length = 0;
    char *body = mp_buffer_steal(buffer, &length);
    mp_buffer_free(buffer);
    if (!body) return queue_json(connection, 500, "{\"ok\":false,\"error\":\"JSON response exceeded its limit\"}");
    return queue_buffer(connection, status, "application/json; charset=utf-8",
                        body, length, MHD_RESPMEM_MUST_FREE, 1);
}

static enum MHD_Result queue_options(struct MHD_Connection *connection) {
    if (!request_origin_allowed(connection))
        return queue_json(connection, MHD_HTTP_FORBIDDEN,
                          "{\"ok\":false,\"error\":\"cross-origin access is disabled\"}");
    struct MHD_Response *response = MHD_create_response_from_buffer(0, (void *)"", MHD_RESPMEM_PERSISTENT);
    if (!response) return MHD_NO;
    add_api_headers(connection, response);
    (void)add_header(response, "Access-Control-Allow-Headers", "Content-Type, Accept");
    (void)add_header(response, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    (void)add_header(response, "Access-Control-Max-Age", "600");
    enum MHD_Result result = MHD_queue_response(connection, MHD_HTTP_NO_CONTENT, response);
    MHD_destroy_response(response);
    return result;
}

static const char *content_type_for_path(const char *path) {
    const char *dot = strrchr(path ? path : "", '.');
    if (!dot) return "application/octet-stream";
    if (strcasecmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcasecmp(dot, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcasecmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(dot, ".ttf") == 0) return "font/ttf";
    if (strcasecmp(dot, ".otf") == 0) return "font/otf";
    return "application/octet-stream";
}

static enum MHD_Result queue_file(struct MHD_Connection *connection, const char *path,
                                  const char *content_type, int api_headers,
                                  unsigned int cache_seconds) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return MHD_NO;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > MAX_STATIC_FILE) {
        close(fd);
        return MHD_NO;
    }

    char etag[80];
    snprintf(etag, sizeof(etag), "\"%llx-%llx\"",
             (unsigned long long)st.st_mtime, (unsigned long long)st.st_size);
    const char *client_etag = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "If-None-Match");
    if (client_etag && strcmp(client_etag, etag) == 0) {
        close(fd);
        struct MHD_Response *not_modified = MHD_create_response_from_buffer(0, (void *)"", MHD_RESPMEM_PERSISTENT);
        if (!not_modified) return MHD_NO;
        (void)add_header(not_modified, "ETag", etag);
        if (api_headers) add_api_headers(connection, not_modified);
        else {
            char cache[64];
            snprintf(cache, sizeof(cache), "public, max-age=%u", cache_seconds);
            (void)add_header(not_modified, "Cache-Control", cache);
        }
        enum MHD_Result queued = MHD_queue_response(connection, MHD_HTTP_NOT_MODIFIED, not_modified);
        MHD_destroy_response(not_modified);
        return queued;
    }

    struct MHD_Response *response = MHD_create_response_from_fd64((uint64_t)st.st_size, fd);
    if (!response) {
        close(fd);
        return MHD_NO;
    }
    (void)add_header(response, MHD_HTTP_HEADER_CONTENT_TYPE,
                     content_type ? content_type : content_type_for_path(path));
    (void)add_header(response, "ETag", etag);
    char modified[64];
    struct tm tmv;
    if (gmtime_r(&st.st_mtime, &tmv) &&
        strftime(modified, sizeof(modified), "%a, %d %b %Y %H:%M:%S GMT", &tmv) > 0)
        (void)add_header(response, "Last-Modified", modified);
    if (api_headers) add_api_headers(connection, response);
    else {
        char cache[64];
        snprintf(cache, sizeof(cache), "public, max-age=%u", cache_seconds);
        (void)add_header(response, "Cache-Control", cache);
        (void)add_header(response, "X-Content-Type-Options", "nosniff");
    }
    enum MHD_Result result = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return result;
}

static int path_has_encoded_separator_or_dot(const char *path) {
    for (const char *p = path; p && *p; p++) {
        if (*p != '%' || !p[1] || !p[2]) continue;
        char a = (char)tolower((unsigned char)p[1]);
        char b = (char)tolower((unsigned char)p[2]);
        if ((a == '2' && (b == 'e' || b == 'f')) || (a == '5' && b == 'c')) return 1;
    }
    return 0;
}

static int safe_web_asset_path(const char *path) {
    if (!path || path[0] != '/' || strstr(path, "..") || strchr(path, '\\')) return 0;
    if (path_has_encoded_separator_or_dot(path)) return 0;
    return strncmp(path, "/assets/", 8) == 0 || strncmp(path, "/modules/", 9) == 0;
}

static enum MHD_Result serve_static(struct MHD_Connection *connection, const char *method,
                                    const char *path, int *handled) {
    *handled = 0;
    if (strcmp(method, MHD_HTTP_METHOD_GET) != 0) return MHD_YES;
    char full[2048];
    if (safe_web_asset_path(path)) {
        *handled = 1;
        int n = snprintf(full, sizeof(full), "%s%s", WEB_DIR, path);
        if (n <= 0 || (size_t)n >= sizeof(full))
            return queue_json(connection, MHD_HTTP_URI_TOO_LONG,
                              "{\"ok\":false,\"error\":\"asset path too long\"}");
        enum MHD_Result result = queue_file(connection, full, NULL, 0, 3600);
        return result == MHD_NO ? queue_json(connection, 404, "{\"ok\":false,\"error\":\"asset not found\"}") : result;
    }
    if (strcmp(path, "/api/v1/openapi.json") == 0) {
        *handled = 1;
        snprintf(full, sizeof(full), "%s/openapi-v1.json", API_DOC_DIR);
        enum MHD_Result result = queue_file(connection, full, "application/json; charset=utf-8", 1, 0);
        return result == MHD_NO ? queue_json(connection, 404, "{\"ok\":false,\"error\":\"OpenAPI file not found\"}") : result;
    }
    if (strcmp(path, "/") == 0) {
        *handled = 1;
        snprintf(full, sizeof(full), "%s/index.html", WEB_DIR);
        enum MHD_Result result = queue_file(connection, full, "text/html; charset=utf-8", 0, 0);
        return result == MHD_NO ? queue_json(connection, 404, "{\"ok\":false,\"error\":\"GUI not installed\"}") : result;
    }
    return MHD_YES;
}

static int connect_core(void) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct timeval tv = {.tv_sec = SOCKET_TIMEOUT_SEC, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int socket_buffer = (int)(MP_IPC_MAX_PAYLOAD * 2u);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &socket_buffer, sizeof(socket_buffer));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &socket_buffer, sizeof(socket_buffer));
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    mp_safe_str(addr.sun_path, sizeof(addr.sun_path), CORE_SOCKET_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
#ifdef SO_PEERCRED
    struct ucred credentials;
    socklen_t credentials_len = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_len) != 0 ||
        (g_expected_core_uid != (uid_t)-1 && credentials.uid != g_expected_core_uid && credentials.uid != 0)) {
        close(fd);
        errno = EPERM;
        return -1;
    }
#endif
    return fd;
}

static int ipc_call(uint16_t opcode, const void *payload, size_t payload_len, struct ipc_result *result) {
    memset(result, 0, sizeof(*result));
    if (payload_len > MP_IPC_MAX_PAYLOAD) return -1;
    int fd = connect_core();
    if (fd < 0) return -1;
    struct mp_ipc_request_header request = {
        .magic = MP_IPC_MAGIC,
        .version = MP_IPC_VERSION,
        .opcode = opcode,
        .payload_len = (uint32_t)payload_len
    };
    if (mp_send_packet(fd, &request, sizeof(request), payload, payload_len) != 0) {
        close(fd);
        return -1;
    }
    void *packet = NULL;
    size_t packet_len = 0;
    if (mp_recv_packet_alloc(fd, &packet,
                             sizeof(struct mp_ipc_response_header) + MP_IPC_MAX_PAYLOAD,
                             &packet_len) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    if (packet_len < sizeof(struct mp_ipc_response_header)) {
        free(packet);
        return -1;
    }
    struct mp_ipc_response_header response;
    memcpy(&response, packet, sizeof(response));
    if (response.magic != MP_IPC_MAGIC || response.version != MP_IPC_VERSION ||
        response.body_len > MP_IPC_MAX_PAYLOAD ||
        packet_len != sizeof(response) + response.body_len) {
        free(packet);
        return -1;
    }
    unsigned char *body = NULL;
    if (response.body_len) {
        body = malloc(response.body_len);
        if (!body) {
            free(packet);
            return -1;
        }
        memcpy(body, (unsigned char *)packet + sizeof(response), response.body_len);
    }
    free(packet);
    result->status = response.status;
    result->content_type = response.content_type;
    result->body = body;
    result->body_len = response.body_len;
    return 0;
}

static void ipc_result_free(struct ipc_result *result) {
    if (!result) return;
    free(result->body);
    memset(result, 0, sizeof(*result));
}

static enum MHD_Result queue_ipc_result(struct MHD_Connection *connection, struct ipc_result *result) {
    const char *type = result->content_type == MP_IPC_CONTENT_JSON
        ? "application/json; charset=utf-8"
        : result->content_type == MP_IPC_CONTENT_TEXT ? "text/plain; charset=utf-8" : "application/octet-stream";
    unsigned char *body = result->body;
    size_t len = result->body_len;
    result->body = NULL;
    return queue_buffer(connection, result->status, type,
                        body ? body : (void *)"", len,
                        body ? MHD_RESPMEM_MUST_FREE : MHD_RESPMEM_PERSISTENT, 1);
}

static enum MHD_Result call_core(struct MHD_Connection *connection, uint16_t opcode,
                                 const void *payload, size_t payload_len) {
    struct ipc_result result;
    if (ipc_call(opcode, payload, payload_len, &result) != 0)
        return queue_json(connection, MHD_HTTP_SERVICE_UNAVAILABLE,
                          "{\"ok\":false,\"error\":\"clock core unavailable\"}");
    enum MHD_Result queued = queue_ipc_result(connection, &result);
    ipc_result_free(&result);
    return queued;
}

static int get_asset_state(struct mp_ipc_asset_state *state) {
    struct ipc_result result;
    if (ipc_call(MP_IPC_OP_ASSET_STATE, NULL, 0, &result) != 0) return -1;
    int ok = result.status == 200 && result.content_type == MP_IPC_CONTENT_BINARY &&
             result.body_len == sizeof(*state);
    if (ok) memcpy(state, result.body, sizeof(*state));
    ipc_result_free(&result);
    return ok ? 0 : -1;
}

static int notify_asset(uint8_t kind, uint8_t action, uint32_t count, const char *file) {
    struct mp_ipc_asset_event event;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.action = action;
    event.count = count;
    mp_safe_str(event.file, sizeof(event.file), file);
    struct ipc_result result;
    if (ipc_call(MP_IPC_OP_ASSET_EVENT, &event, sizeof(event), &result) != 0) return -1;
    int ok = result.status >= 200 && result.status < 300;
    ipc_result_free(&result);
    return ok ? 0 : -1;
}

static struct form_field *field_slot(struct request_context *context, const char *key, uint64_t off) {
    for (size_t i = 0; i < context->field_count; i++) {
        if (strcmp(context->fields[i].key, key) == 0) {
            if (off == 0) {
                context->fields[i].len = 0;
                context->fields[i].value[0] = '\0';
            }
            return &context->fields[i];
        }
    }
    if (context->field_count >= FORM_FIELDS_MAX || off != 0) return NULL;
    if (context->field_count == context->field_cap) {
        size_t next = context->field_cap ? context->field_cap * 2 : 8;
        if (next > FORM_FIELDS_MAX) next = FORM_FIELDS_MAX;
        struct form_field *grown = realloc(context->fields, next * sizeof(*grown));
        if (!grown) return NULL;
        context->fields = grown;
        context->field_cap = next;
    }
    struct form_field *field = &context->fields[context->field_count++];
    memset(field, 0, sizeof(*field));
    mp_safe_str(field->key, sizeof(field->key), key);
    return field;
}

static struct upload_file *upload_slot(struct request_context *context, const char *key,
                                       const char *filename, uint64_t off) {
    if (off > 0) {
        for (size_t i = context->upload_count; i > 0; i--) {
            struct upload_file *upload = &context->uploads[i - 1];
            if (strcmp(upload->key, key) == 0 && strcmp(upload->filename, filename) == 0 &&
                upload->next_offset == off) return upload;
        }
        return NULL;
    }
    if (context->upload_count >= UPLOAD_FILES_MAX) return NULL;
    if (context->upload_count == context->upload_cap) {
        size_t next = context->upload_cap ? context->upload_cap * 2 : 4;
        if (next > UPLOAD_FILES_MAX) next = UPLOAD_FILES_MAX;
        struct upload_file *grown = realloc(context->uploads, next * sizeof(*grown));
        if (!grown) return NULL;
        context->uploads = grown;
        context->upload_cap = next;
    }
    struct upload_file *upload = &context->uploads[context->upload_count++];
    memset(upload, 0, sizeof(*upload));
    upload->fd = -1;
    mp_safe_str(upload->key, sizeof(upload->key), key);
    mp_safe_str(upload->filename, sizeof(upload->filename), filename);
    mp_safe_str(upload->temp_path, sizeof(upload->temp_path), "/tmp/mk-piclock-upload-XXXXXX");
    upload->fd = mkstemp(upload->temp_path);
    if (upload->fd < 0) {
        upload->temp_path[0] = '\0';
        return NULL;
    }
    return upload;
}

static int pwrite_full_at(int fd, const void *data, size_t size, uint64_t offset) {
    const unsigned char *p = data;
    size_t left = size;
    while (left > 0) {
        ssize_t n = pwrite(fd, p, left, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += (size_t)n;
        left -= (size_t)n;
        offset += (uint64_t)n;
    }
    return 0;
}

static enum MHD_Result post_iterator(void *cls, enum MHD_ValueKind kind, const char *key,
                                     const char *filename, const char *content_type,
                                     const char *transfer_encoding, const char *data,
                                     uint64_t off, size_t size) {
    (void)kind;
    (void)content_type;
    (void)transfer_encoding;
    struct request_context *context = cls;
    if (!key || context->parse_failed) return MHD_NO;
    if (context->body_too_large) return MHD_YES;
    if (filename && *filename) {
        if (context->upload_limit &&
            (off > context->upload_limit || size > context->upload_limit - (size_t)off)) {
            context->body_too_large = 1;
            return MHD_YES;
        }
        struct upload_file *upload = upload_slot(context, key, filename, off);
        if (!upload || upload->fd < 0 || upload->next_offset != off ||
            (size && pwrite_full_at(upload->fd, data, size, off) != 0)) {
            context->parse_failed = 1;
            return MHD_NO;
        }
        upload->next_offset += size;
        upload->size += size;
        return MHD_YES;
    }
    struct form_field *field = field_slot(context, key, off);
    if (!field || off != field->len || size > sizeof(field->value) - 1 - field->len) {
        context->parse_failed = 1;
        return MHD_NO;
    }
    memcpy(field->value + field->len, data, size);
    field->len += size;
    field->value[field->len] = '\0';
    return MHD_YES;
}

static const char *form_value(const struct request_context *context, const char *key) {
    for (size_t i = 0; i < context->field_count; i++)
        if (strcmp(context->fields[i].key, key) == 0) return context->fields[i].value;
    return NULL;
}

static int parse_int_value(const char *value, int fallback) {
    if (!value || !*value) return fallback;
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno || end == value || *end || parsed < -2147483647L || parsed > 2147483647L) return fallback;
    return (int)parsed;
}

static int form_int(const struct request_context *context, const char *key, int fallback) {
    return parse_int_value(form_value(context, key), fallback);
}

static int parse_time_value(const char *value, int *hour, int *minute) {
    int h = 0, m = 0;
    char extra = '\0';
    if (!value || sscanf(value, "%d:%d%c", &h, &m, &extra) != 2 || h < 0 || h > 23 || m < 0 || m > 59)
        return -1;
    *hour = h;
    *minute = m;
    return 0;
}

static void close_uploads(struct request_context *context) {
    for (size_t i = 0; i < context->upload_count; i++) {
        if (context->uploads[i].fd >= 0) {
            if (fsync(context->uploads[i].fd) != 0) context->parse_failed = 1;
            if (close(context->uploads[i].fd) != 0) context->parse_failed = 1;
            context->uploads[i].fd = -1;
        }
    }
}

static void cleanup_context(struct request_context *context) {
    if (!context) return;
    if (context->post) MHD_destroy_post_processor(context->post);
    close_uploads(context);
    for (size_t i = 0; i < context->upload_count; i++) {
        if (context->uploads[i].temp_path[0]) unlink(context->uploads[i].temp_path);
    }
    free(context->fields);
    free(context->uploads);
    free(context);
}

static const char *query_value(struct MHD_Connection *connection, const char *key) {
    return MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, key);
}

static enum MHD_Result serve_faces_list(struct MHD_Connection *connection) {
    int bedtime = query_value(connection, "kind") && strcmp(query_value(connection, "kind"), "bedtime") == 0;
    int all = parse_int_value(query_value(connection, "all"), 0) != 0;
    int page = parse_int_value(query_value(connection, "page"), 1);
    if (page < 1) page = 1;
    char files[MP_ASSET_LIST_MAX][MP_ASSET_NAME_MAX];
    int count = mp_asset_scan(bedtime ? MP_BEDTIME_FACE_DIR : MP_FACE_DIR,
                              MP_ASSET_SCAN_FACE_RAW, files, MP_ASSET_LIST_MAX);
    int per_page = all ? (count > 0 ? count : 1) : 8;
    int max_page = all ? 1 : (count + per_page - 1) / per_page;
    if (max_page < 1) max_page = 1;
    if (page > max_page) page = max_page;
    int start = all ? 0 : (page - 1) * per_page;
    int end = all ? count : start + per_page;
    if (end > count) end = count;

    struct mp_buffer body;
    if (mp_buffer_init(&body, 4096, MP_IPC_MAX_PAYLOAD) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    mp_buffer_appendf(&body,
        "{\"kind\":\"%s\",\"page\":%d,\"max_page\":%d,\"per_page\":%d,\"count\":%d,\"faces\":[",
        bedtime ? "bedtime" : "normal", page, max_page, per_page, count);
    for (int i = start; i < end && !body.failed; i++) {
        char title[MP_ASSET_NAME_MAX];
        char source_path[768];
        char source_name[MP_ASSET_NAME_MAX];
        mp_asset_face_title(files[i], title, sizeof(title));
        int source_exists = mp_asset_face_source_path(files[i], bedtime, source_path, sizeof(source_path)) == 0 &&
                            access(source_path, R_OK) == 0;
        mp_safe_str(source_name, sizeof(source_name), files[i]);
        char *dot = strrchr(source_name, '.');
        if (dot) mp_safe_str(dot, (size_t)(source_name + sizeof(source_name) - dot), ".png");
        mp_buffer_appendf(&body, "%s{\"id\":%d,\"file\":\"", i == start ? "" : ",", i + 1);
        mp_buffer_append_json_string(&body, files[i]);
        mp_buffer_append(&body, "\",\"title\":\"");
        mp_buffer_append_json_string(&body, title);
        mp_buffer_append(&body, "\",\"source_png\":\"");
        if (source_exists) mp_buffer_append_json_string(&body, source_name);
        mp_buffer_append(&body, "\",\"preview_url\":\"");
        if (source_exists)
            mp_buffer_appendf(&body, "/api/v1/assets/faces/source?kind=%s&file=%s",
                              bedtime ? "bedtime" : "normal", files[i]);
        mp_buffer_appendf(&body, "\",\"source_exists\":%d,\"exists\":1}", source_exists ? 1 : 0);
    }
    mp_buffer_append(&body, "]}");
    return queue_json_builder(connection, 200, &body);
}

static enum MHD_Result serve_fonts_list(struct MHD_Connection *connection) {
    struct mp_ipc_asset_state state;
    if (get_asset_state(&state) != 0)
        return queue_json(connection, 503, "{\"ok\":false,\"error\":\"clock core unavailable\"}");
    char files[MP_ASSET_LIST_MAX][MP_ASSET_NAME_MAX];
    int count = mp_asset_scan(MP_FONT_DIR, MP_ASSET_SCAN_FONT, files, MP_ASSET_LIST_MAX);
    struct mp_buffer body;
    if (mp_buffer_init(&body, 2048, MP_IPC_MAX_PAYLOAD) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    mp_buffer_append(&body, "{\"selected\":\"");
    mp_buffer_append_json_string(&body, state.selected_font);
    mp_buffer_appendf(&body,
        "\",\"builtin\":%d,\"font_size\":%d,\"builtin_fonts\":["
        "{\"id\":0,\"name\":\"Seven Segment\"},{\"id\":1,\"name\":\"Seven Thin\"},"
        "{\"id\":2,\"name\":\"Pixel\"},{\"id\":3,\"name\":\"Pixel Bold\"}],\"uploaded_fonts\":[",
        state.builtin_font, state.font_size);
    for (int i = 0; i < count && !body.failed; i++) {
        mp_buffer_appendf(&body, "%s\"", i ? "," : "");
        mp_buffer_append_json_string(&body, files[i]);
        mp_buffer_append(&body, "\"");
    }
    mp_buffer_append(&body, "]}");
    return queue_json_builder(connection, 200, &body);
}

static enum MHD_Result serve_music_list(struct MHD_Connection *connection) {
    struct mp_ipc_asset_state state;
    if (get_asset_state(&state) != 0)
        return queue_json(connection, 503, "{\"ok\":false,\"error\":\"clock core unavailable\"}");
    char files[MP_ASSET_LIST_MAX][MP_ASSET_NAME_MAX];
    int count = mp_asset_scan(MP_MUSIC_DIR, MP_ASSET_SCAN_MUSIC_MP3, files, MP_ASSET_LIST_MAX);
    struct mp_buffer body;
    if (mp_buffer_init(&body, 4096, MP_IPC_MAX_PAYLOAD) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    mp_buffer_appendf(&body, "{\"global_volume\":%d,\"current\":\"", state.global_volume);
    mp_buffer_append_json_string(&body, state.current_music);
    mp_buffer_append(&body, "\",\"files\":[");
    for (int i = 0; i < count && !body.failed; i++) {
        mp_buffer_appendf(&body, "%s\"", i ? "," : "");
        mp_buffer_append_json_string(&body, files[i]);
        mp_buffer_append(&body, "\"");
    }
    mp_buffer_append(&body, "],\"tracks\":[");
    for (int i = 0; i < count && !body.failed; i++) {
        char path[768];
        int path_len = snprintf(path, sizeof(path), "%s/%.*s", MP_MUSIC_DIR,
                                MP_ASSET_NAME_MAX - 1, files[i]);
        struct mp_id3_metadata metadata;
        memset(&metadata, 0, sizeof(metadata));
        int tagged = path_len >= 0 && (size_t)path_len < sizeof(path) &&
                     mp_read_id3_metadata(path, &metadata) == 0;
        if (!metadata.title[0]) mp_title_from_filename(files[i], metadata.title, sizeof(metadata.title));
        char display[MP_ID3_TEXT_MAX * 2 + 4];
        if (metadata.artist[0])
            snprintf(display, sizeof(display), "%s - %s", metadata.title, metadata.artist);
        else
            mp_safe_str(display, sizeof(display), metadata.title);

        mp_buffer_appendf(&body, "%s{\"file\":\"", i ? "," : "");
        mp_buffer_append_json_string(&body, files[i]);
        mp_buffer_append(&body, "\",\"title\":\"");
        mp_buffer_append_json_string(&body, metadata.title);
        mp_buffer_append(&body, "\",\"artist\":\"");
        mp_buffer_append_json_string(&body, metadata.artist);
        mp_buffer_append(&body, "\",\"display\":\"");
        mp_buffer_append_json_string(&body, display);
        mp_buffer_appendf(&body, "\",\"id3\":%d}", tagged ? 1 : 0);
    }
    mp_buffer_append(&body, "]}");
    return queue_json_builder(connection, 200, &body);
}

static enum MHD_Result serve_face_source(struct MHD_Connection *connection) {
    const char *file = query_value(connection, "file");
    int bedtime = query_value(connection, "kind") && strcmp(query_value(connection, "kind"), "bedtime") == 0;
    char path[768];
    if (!mp_asset_safe_face_filename(file) || mp_asset_face_source_path(file, bedtime, path, sizeof(path)) != 0)
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"invalid face file\"}");
    enum MHD_Result result = queue_file(connection, path, "image/png", 1, 0);
    return result == MHD_NO ? queue_json(connection, 404, "{\"ok\":false,\"error\":\"source image not found\"}") : result;
}

static enum MHD_Result serve_font_file(struct MHD_Connection *connection) {
    const char *file = query_value(connection, "file");
    if (!mp_asset_safe_filename(file) || !mp_asset_has_font_ext(file))
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"invalid font file\"}");
    char path[768];
    snprintf(path, sizeof(path), "%s/%s", MP_FONT_DIR, file);
    enum MHD_Result result = queue_file(connection, path, content_type_for_path(path), 1, 0);
    return result == MHD_NO ? queue_json(connection, 404, "{\"ok\":false,\"error\":\"font not found\"}") : result;
}

static enum MHD_Result notify_or_saved_warning(struct MHD_Connection *connection, uint8_t kind,
                                                uint8_t action, uint32_t count, const char *file,
                                                const char *success_json) {
    if (notify_asset(kind, action, count, file) != 0)
        return queue_json(connection, 503,
            "{\"ok\":false,\"saved\":true,\"error\":\"asset saved but clock core could not reload it\"}");
    return queue_json(connection, 200, success_json);
}

static enum MHD_Result upload_faces(struct MHD_Connection *connection, struct request_context *context, int bedtime) {
    int ok = 0, failed = 0;
    char first[MP_ASSET_NAME_MAX] = "";
    for (size_t i = 0; i < context->upload_count; i++) {
        struct upload_file *upload = &context->uploads[i];
        if (!mp_asset_has_png_ext(upload->filename) || upload->size == 0) {
            failed++;
            continue;
        }
        const char *target_dir = bedtime ? MP_BEDTIME_FACE_DIR : MP_FACE_DIR;
        if (!mp_asset_has_free_space(target_dir, (uint64_t)upload->size + MP_FACE_RAW_BYTES,
                                     g_disk_reserve_bytes)) {
            failed++;
            continue;
        }
        char raw_name[MP_ASSET_NAME_MAX];
        if (mp_asset_save_face_png(upload->temp_path, upload->filename, bedtime, raw_name, sizeof(raw_name)) != 0) {
            failed++;
            continue;
        }
        if (!first[0]) mp_safe_str(first, sizeof(first), raw_name);
        ok++;
    }
    if (ok == 0)
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"no valid PNG face files were uploaded\"}");
    char json[256];
    snprintf(json, sizeof(json), "{\"ok\":true,\"uploaded\":%d,\"skipped\":%d}", ok, failed);
    return notify_or_saved_warning(connection, bedtime ? MP_IPC_ASSET_BEDTIME_FACE : MP_IPC_ASSET_FACE,
                                   MP_IPC_ASSET_UPLOADED, (uint32_t)ok, first, json);
}

static enum MHD_Result upload_music(struct MHD_Connection *connection, struct request_context *context) {
    if (mp_asset_ensure_dir(MP_MUSIC_DIR) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"music directory unavailable\"}");
    int ok = 0, failed = 0;
    char first[MP_ASSET_NAME_MAX] = "";
    for (size_t i = 0; i < context->upload_count; i++) {
        struct upload_file *upload = &context->uploads[i];
        char name[MP_ASSET_NAME_MAX];
        mp_asset_sanitize_filename(upload->filename, name, sizeof(name), "alarm.mp3");
        if (!mp_asset_safe_filename(name) || !mp_asset_has_mp3_ext(name) || upload->size == 0 ||
            upload->size > MP_MUSIC_UPLOAD_MAX_BYTES || mp_asset_validate_mp3(upload->temp_path) != 0) {
            failed++;
            continue;
        }
        char target[768];
        snprintf(target, sizeof(target), "%s/%s", MP_MUSIC_DIR, name);
        uint64_t current_music_bytes = mp_asset_directory_bytes(MP_MUSIC_DIR, MP_ASSET_SCAN_MUSIC_MP3);
        uint64_t replaced_bytes = 0;
        struct stat existing;
        if (stat(target, &existing) == 0 && S_ISREG(existing.st_mode) && existing.st_size > 0)
            replaced_bytes = (uint64_t)existing.st_size;
        uint64_t retained_bytes = current_music_bytes >= replaced_bytes
            ? current_music_bytes - replaced_bytes : 0;
        if ((uint64_t)upload->size > g_music_quota_bytes ||
            retained_bytes > g_music_quota_bytes - (uint64_t)upload->size ||
            !mp_asset_has_free_space(MP_MUSIC_DIR, upload->size, g_disk_reserve_bytes)) {
            failed++;
            continue;
        }
        if (mp_asset_move_file(upload->temp_path, target) != 0) {
            failed++;
            continue;
        }
        upload->temp_path[0] = '\0';
        if (!first[0]) mp_safe_str(first, sizeof(first), name);
        ok++;
    }
    if (ok == 0)
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"no valid MP3 files were uploaded\"}");
    char json[256];
    snprintf(json, sizeof(json), "{\"ok\":true,\"uploaded\":%d,\"skipped\":%d}", ok, failed);
    return notify_or_saved_warning(connection, MP_IPC_ASSET_MUSIC, MP_IPC_ASSET_UPLOADED,
                                   (uint32_t)ok, first, json);
}

static enum MHD_Result upload_font(struct MHD_Connection *connection, struct request_context *context) {
    if (mp_asset_ensure_dir(MP_FONT_DIR) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"font directory unavailable\"}");
    for (size_t i = 0; i < context->upload_count; i++) {
        struct upload_file *upload = &context->uploads[i];
        char name[MP_ASSET_NAME_MAX];
        mp_asset_sanitize_filename(upload->filename, name, sizeof(name), "uploaded_font.ttf");
        if (!mp_asset_safe_filename(name) || !mp_asset_has_font_ext(name) || upload->size == 0 ||
            upload->size > MP_FONT_UPLOAD_MAX_BYTES) continue;
        if (!mp_asset_has_free_space(MP_FONT_DIR, upload->size, g_disk_reserve_bytes))
            return queue_json(connection, 507, "{\"ok\":false,\"error\":\"insufficient storage\"}");
        if (mp_asset_validate_font(upload->temp_path) != 0)
            return queue_json(connection, 400, "{\"ok\":false,\"error\":\"uploaded file is not a readable font\"}");
        char target[768];
        snprintf(target, sizeof(target), "%s/%s", MP_FONT_DIR, name);
        if (mp_asset_move_file(upload->temp_path, target) != 0)
            return queue_json(connection, 500, "{\"ok\":false,\"error\":\"could not save font\"}");
        upload->temp_path[0] = '\0';
        return notify_or_saved_warning(connection, MP_IPC_ASSET_FONT, MP_IPC_ASSET_UPLOADED,
                                       1, name, "{\"ok\":true,\"uploaded\":1}");
    }
    return queue_json(connection, 400, "{\"ok\":false,\"error\":\"no valid font was uploaded\"}");
}

static enum MHD_Result delete_face(struct MHD_Connection *connection, const struct request_context *context) {
    const char *file = form_value(context, "file");
    int bedtime = form_value(context, "kind") && strcmp(form_value(context, "kind"), "bedtime") == 0;
    if (!mp_asset_safe_face_filename(file))
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"invalid face filename\"}");
    const char *dir = bedtime ? MP_BEDTIME_FACE_DIR : MP_FACE_DIR;
    (void)mp_asset_delete_file(dir, file);
    char source[768];
    if (mp_asset_face_source_path(file, bedtime, source, sizeof(source)) == 0) (void)unlink(source);
    if (notify_asset(bedtime ? MP_IPC_ASSET_BEDTIME_FACE : MP_IPC_ASSET_FACE,
                     MP_IPC_ASSET_DELETED, 1, file) != 0)
        return queue_json(connection, 503, "{\"ok\":false,\"deleted\":true,\"error\":\"clock core unavailable\"}");
    return queue_json(connection, 200, "{\"ok\":true,\"deleted\":true}");
}

static enum MHD_Result delete_all_faces(struct MHD_Connection *connection) {
    int normal = mp_asset_delete_faces(0);
    int bedtime = mp_asset_delete_faces(1);
    if (notify_asset(MP_IPC_ASSET_FACE, MP_IPC_ASSET_DELETED_ALL,
                     (uint32_t)(normal + bedtime), NULL) != 0)
        return queue_json(connection, 503, "{\"ok\":false,\"deleted\":true,\"error\":\"clock core unavailable\"}");
    char json[192];
    snprintf(json, sizeof(json), "{\"ok\":true,\"deleted_normal\":%d,\"deleted_bedtime\":%d}", normal, bedtime);
    return queue_json(connection, 200, json);
}

static enum MHD_Result delete_all_music(struct MHD_Connection *connection) {
    int deleted = mp_asset_delete_music();
    if (notify_asset(MP_IPC_ASSET_MUSIC, MP_IPC_ASSET_DELETED_ALL, (uint32_t)deleted, NULL) != 0)
        return queue_json(connection, 503, "{\"ok\":false,\"deleted\":true,\"error\":\"clock core unavailable\"}");
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":true,\"deleted_music\":%d}", deleted);
    return queue_json(connection, 200, json);
}

static enum MHD_Result delete_font(struct MHD_Connection *connection, const struct request_context *context) {
    const char *file = form_value(context, "font");
    if (!mp_asset_safe_filename(file) || !mp_asset_has_font_ext(file))
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"invalid font filename\"}");
    (void)mp_asset_delete_file(MP_FONT_DIR, file);
    if (notify_asset(MP_IPC_ASSET_FONT, MP_IPC_ASSET_DELETED, 1, file) != 0)
        return queue_json(connection, 503, "{\"ok\":false,\"deleted\":true,\"error\":\"clock core unavailable\"}");
    return queue_json(connection, 200, "{\"ok\":true,\"deleted\":true}");
}

static enum MHD_Result dispatch_route(struct MHD_Connection *connection,
                                      struct request_context *context) {
    switch (context->route->id) {
        case ROUTE_STATUS:
        case ROUTE_HEALTH:
            return call_core(connection, MP_IPC_OP_STATUS, NULL, 0);
        case ROUTE_FACES_LIST:
            return serve_faces_list(connection);
        case ROUTE_FONTS_LIST:
            return serve_fonts_list(connection);
        case ROUTE_MUSIC_LIST:
            return serve_music_list(connection);
        case ROUTE_FACE_SOURCE:
            return serve_face_source(connection);
        case ROUTE_FONT_FILE:
            return serve_font_file(connection);
        case ROUTE_UPLOAD_FACE:
            return upload_faces(connection, context, 0);
        case ROUTE_UPLOAD_BEDTIME_FACE:
            return upload_faces(connection, context, 1);
        case ROUTE_UPLOAD_MUSIC:
            return upload_music(connection, context);
        case ROUTE_UPLOAD_FONT:
            return upload_font(connection, context);
        case ROUTE_DELETE_FACE:
            return delete_face(connection, context);
        case ROUTE_DELETE_ALL_FACES:
            return delete_all_faces(connection);
        case ROUTE_DELETE_ALL_MUSIC:
            return delete_all_music(connection);
        case ROUTE_DELETE_FONT:
            return delete_font(connection, context);
        case ROUTE_DISPLAY_ACTION: {
            struct mp_ipc_display_action request;
            memset(&request, 0, sizeof(request));
            const char *action = form_value(context, "do");
            if (action && strcmp(action, "clock") == 0) request.action = MP_IPC_ACTION_CLOCK;
            else if (action && strcmp(action, "clear") == 0) request.action = MP_IPC_ACTION_CLEAR;
            else if (action && strcmp(action, "stop") == 0) request.action = MP_IPC_ACTION_STOP_AUDIO;
            else if (action && strcmp(action, "play-music") == 0) request.action = MP_IPC_ACTION_PLAY_MUSIC;
            else return queue_json(connection, 400, "{\"ok\":false,\"error\":\"unknown display action\"}");
            mp_safe_str(request.file, sizeof(request.file), form_value(context, "file"));
            return call_core(connection, MP_IPC_OP_DISPLAY_ACTION, &request, sizeof(request));
        }
        case ROUTE_DISPLAY_FACE: {
            struct mp_ipc_display_face request;
            memset(&request, 0, sizeof(request));
            request.id = form_int(context, "id", 1);
            mp_safe_str(request.file, sizeof(request.file), form_value(context, "file"));
            return call_core(connection, MP_IPC_OP_DISPLAY_FACE, &request, sizeof(request));
        }
        case ROUTE_DISPLAY_MESSAGE: {
            struct mp_ipc_display_message request;
            memset(&request, 0, sizeof(request));
            request.face_id = form_int(context, "face_id", 1);
            const char *face = form_value(context, "face_file");
            if (!face) face = form_value(context, "file");
            const char *text = form_value(context, "message_text");
            if (!text) text = form_value(context, "message");
            int delay_seconds = form_int(context, "delay_seconds", 0);
            if (delay_seconds != 0 && delay_seconds != 10 && delay_seconds != 30 && delay_seconds != 60)
                return queue_json(connection, 400,
                    "{\"ok\":false,\"error\":\"delay_seconds must be 0, 10, 30, or 60\"}");
            request.delay_seconds = (uint16_t)delay_seconds;
            mp_safe_str(request.face_file, sizeof(request.face_file), face);
            mp_safe_str(request.text, sizeof(request.text), text);
            return call_core(connection, MP_IPC_OP_DISPLAY_MESSAGE, &request, sizeof(request));
        }
        case ROUTE_MESSAGE_LIMITS:
            return call_core(connection, MP_IPC_OP_MESSAGE_LIMITS, NULL, 0);
        case ROUTE_MESSAGE_FIT: {
            struct mp_ipc_message_fit request;
            memset(&request, 0, sizeof(request));
            mp_safe_str(request.text, sizeof(request.text), query_value(connection, "text"));
            return call_core(connection, MP_IPC_OP_MESSAGE_FIT, &request, sizeof(request));
        }
        case ROUTE_CONFIG_ALARM: {
            struct mp_ipc_alarm_config request;
            memset(&request, 0, sizeof(request));
            request.id = (uint8_t)form_int(context, "id", 1);
            request.enabled = (uint8_t)(form_int(context, "enabled", 0) != 0);
            int hour = 7, minute = 0;
            (void)parse_time_value(form_value(context, "time"), &hour, &minute);
            request.hour = (uint8_t)hour;
            request.minute = (uint8_t)minute;
            for (int day = 0; day < 7; day++) {
                char key[16];
                snprintf(key, sizeof(key), "day%d", day);
                if (form_value(context, key)) request.weekdays |= (uint8_t)(1u << day);
            }
            request.start_volume = (uint8_t)form_int(context, "start_volume", 20);
            request.end_volume = (uint8_t)form_int(context, "end_volume", 80);
            mp_safe_str(request.music_file, sizeof(request.music_file), form_value(context, "music_file"));
            return call_core(connection, MP_IPC_OP_CONFIG_ALARM, &request, sizeof(request));
        }
        case ROUTE_CONFIG_AUDIO: {
            struct mp_ipc_audio_config request;
            memset(&request, 0, sizeof(request));
            if (form_value(context, "global_volume") != NULL) {
                request.present_mask |= MP_IPC_AUDIO_GLOBAL_VOLUME;
                request.global_volume = (uint8_t)form_int(context, "global_volume", 80);
            }
            if (form_value(context, "show_song_metadata") != NULL) {
                request.present_mask |= MP_IPC_AUDIO_SHOW_METADATA;
                request.show_song_metadata = (uint8_t)(form_int(context, "show_song_metadata", 1) != 0);
            }
            if (request.present_mask == 0)
                return queue_json(connection, 400, "{\"ok\":false,\"error\":\"no audio settings supplied\"}");
            return call_core(connection, MP_IPC_OP_CONFIG_AUDIO, &request, sizeof(request));
        }
        case ROUTE_CONFIG_PERSONALIZATION: {
            struct mp_ipc_personalization_config request;
            memset(&request, 0, sizeof(request));
            mp_safe_str(request.clock_name, sizeof(request.clock_name), form_value(context, "clock_name"));
            return call_core(connection, MP_IPC_OP_CONFIG_PERSONALIZATION, &request, sizeof(request));
        }
        case ROUTE_CONFIG_DISPLAY: {
            struct mp_ipc_display_config request;
            memset(&request, 0, sizeof(request));
            if (form_value(context, "oled_font")) {
                request.present_mask |= MP_IPC_DISPLAY_FONT;
                request.oled_font = (uint8_t)form_int(context, "oled_font", 0);
            }
            if (form_value(context, "oled_font_size")) {
                request.present_mask |= MP_IPC_DISPLAY_FONT_SIZE;
                request.oled_font_size = (uint8_t)form_int(context, "oled_font_size", 48);
            }
            if (form_value(context, "oled_font_file") != NULL) {
                request.present_mask |= MP_IPC_DISPLAY_FONT_FILE;
                mp_safe_str(request.oled_font_file, sizeof(request.oled_font_file), form_value(context, "oled_font_file"));
            }
            if (form_value(context, "bedtime_enabled")) {
                request.present_mask |= MP_IPC_DISPLAY_BEDTIME_ENABLED;
                request.bedtime_enabled = (uint8_t)(form_int(context, "bedtime_enabled", 0) != 0);
            }
            if (form_value(context, "bedtime_dim_percent")) {
                request.present_mask |= MP_IPC_DISPLAY_BEDTIME_DIM;
                request.bedtime_dim_percent = (uint8_t)form_int(context, "bedtime_dim_percent", 35);
            }
            if (form_value(context, "clock_24h_mode")) {
                request.present_mask |= MP_IPC_DISPLAY_CLOCK_MODE;
                request.clock_24h_mode = (uint8_t)(form_int(context, "clock_24h_mode", 0) != 0);
            }
            int hour, minute;
            if (parse_time_value(form_value(context, "bedtime_start"), &hour, &minute) == 0) {
                request.present_mask |= MP_IPC_DISPLAY_BEDTIME_START;
                request.bedtime_start_hour = (uint8_t)hour;
                request.bedtime_start_minute = (uint8_t)minute;
            }
            if (parse_time_value(form_value(context, "bedtime_end"), &hour, &minute) == 0) {
                request.present_mask |= MP_IPC_DISPLAY_BEDTIME_END;
                request.bedtime_end_hour = (uint8_t)hour;
                request.bedtime_end_minute = (uint8_t)minute;
            }
            return call_core(connection, MP_IPC_OP_CONFIG_DISPLAY, &request, sizeof(request));
        }
        case ROUTE_LOGS:
            return call_core(connection, MP_IPC_OP_LOGS_GET, NULL, 0);
        case ROUTE_LOGS_CLEAR:
            return call_core(connection, MP_IPC_OP_LOGS_CLEAR, NULL, 0);
    }
    return queue_json(connection, 404, "{\"ok\":false,\"error\":\"route not found\"}");
}

static enum MHD_Result serve_local_api(struct MHD_Connection *connection, const char *method,
                                       const char *path, int *handled) {
    *handled = 0;
    if (strcmp(method, MHD_HTTP_METHOD_GET) != 0) return MHD_YES;
    if (strcmp(path, "/api/v1") == 0 || strcmp(path, "/api/v1/") == 0) {
        *handled = 1;
        char discovery[512];
        snprintf(discovery, sizeof(discovery),
            "{\"name\":\"mk-piclock API\",\"api_version\":\"1.0\"," 
            "\"product_version\":\"%s\",\"http_engine\":\"libmicrohttpd\"," 
            "\"core_protocol\":\"binary-ipc-v%u\"," 
            "\"status\":\"/api/v1/status\",\"capabilities\":\"/api/v1/capabilities\"," 
            "\"openapi\":\"/api/v1/openapi.json\"}",
            PRODUCT_VERSION, (unsigned int)MP_IPC_VERSION);
        return queue_json(connection, 200, discovery);
    }
    if (strcmp(path, "/api/v1/capabilities") == 0) {
        *handled = 1;
        return queue_json(connection, 200,
            "{\"ok\":true,\"api_version\":\"1.0\",\"capabilities\":["
            "\"status.read\",\"display.control\",\"display.message\",\"display.message.delay\","
            "\"alarm.configure\",\"audio.configure\",\"audio.metadata\",\"touch.input\",\"assets.read\","
            "\"assets.upload\",\"assets.delete\",\"logs.read\"]}");
    }
    return MHD_YES;
}

static enum MHD_Result request_handler(void *cls, struct MHD_Connection *connection,
                                       const char *url, const char *method, const char *version,
                                       const char *upload_data, size_t *upload_data_size,
                                       void **con_cls) {
    (void)cls;
    (void)version;
    struct request_context *context = *con_cls;
    if (!context) {
        context = calloc(1, sizeof(*context));
        if (!context) return MHD_NO;
        context->route = find_route(method, url);
        context->upload_limit = route_upload_limit(context->route);
        if (context->route && strcmp(method, MHD_HTTP_METHOD_POST) == 0) {
            const char *content_type = MHD_lookup_connection_value(
                connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_TYPE);
            int form_body = content_type &&
                (strncasecmp(content_type, "multipart/form-data", 19) == 0 ||
                 strncasecmp(content_type, "application/x-www-form-urlencoded", 33) == 0);
            if (form_body) {
                context->post = MHD_create_post_processor(connection, 32768, post_iterator, context);
                if (!context->post) context->parse_failed = 1;
            }
        }
        *con_cls = context;
        return MHD_YES;
    }
    if (context->response_queued) return MHD_YES;
    if (*upload_data_size > 0) {
        if (*upload_data_size > MAX_REQUEST_BODY - context->received_bytes) {
            context->body_too_large = 1;
        } else {
            context->received_bytes += *upload_data_size;
            if (!context->parse_failed && context->post &&
                MHD_post_process(context->post, upload_data, *upload_data_size) == MHD_NO)
                context->parse_failed = 1;
        }
        *upload_data_size = 0;
        return MHD_YES;
    }

    context->response_queued = 1;
    if (context->post) {
        MHD_destroy_post_processor(context->post);
        context->post = NULL;
    }
    close_uploads(context);

    if (context->body_too_large)
        return queue_json(connection, MHD_HTTP_CONTENT_TOO_LARGE,
                          "{\"ok\":false,\"error\":\"request or file exceeds its configured limit\"}");
    if (context->parse_failed)
        return queue_json(connection, MHD_HTTP_BAD_REQUEST,
                          "{\"ok\":false,\"error\":\"request body could not be parsed\"}");
    if (strcmp(method, MHD_HTTP_METHOD_OPTIONS) == 0) return queue_options(connection);

    int handled = 0;
    enum MHD_Result result = serve_local_api(connection, method, url, &handled);
    if (handled) return result;
    if (context->route) return dispatch_route(connection, context);
    result = serve_static(connection, method, url, &handled);
    if (handled) return result;
    return queue_json(connection, 404, "{\"ok\":false,\"error\":\"route not found\"}");
}

static void request_completed(void *cls, struct MHD_Connection *connection, void **con_cls,
                              enum MHD_RequestTerminationCode toe) {
    (void)cls;
    (void)connection;
    (void)toe;
    cleanup_context(*con_cls);
    *con_cls = NULL;
}

int main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    const char *allowed_origin = getenv("MK_PICLOCK_ALLOWED_ORIGIN");
    if (allowed_origin && *allowed_origin) mp_safe_str(g_allowed_origin, sizeof(g_allowed_origin), allowed_origin);
    g_expected_core_uid = resolve_user_uid("MK_PICLOCK_CORE_USER", "mk-piclock-core");
    g_music_quota_bytes = bytes_from_env("MK_PICLOCK_MUSIC_QUOTA_BYTES", DEFAULT_MUSIC_QUOTA_BYTES);
    g_disk_reserve_bytes = bytes_from_env("MK_PICLOCK_DISK_RESERVE_BYTES", DISK_RESERVE_BYTES);

    (void)mp_asset_ensure_dir(MP_FACE_DIR);
    (void)mp_asset_ensure_dir(MP_BEDTIME_FACE_DIR);
    (void)mp_asset_ensure_dir(MP_MUSIC_DIR);
    (void)mp_asset_ensure_dir(MP_FONT_DIR);

    int public_port = public_port_from_env();
    if (public_port < 0) {
        fprintf(stderr, "Invalid MK_PICLOCK_API_PORT\n");
        return 1;
    }
    const char *public_bind = public_bind_from_env();
    struct sockaddr_in bind_address;
    memset(&bind_address, 0, sizeof(bind_address));
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons((uint16_t)public_port);
    if (inet_pton(AF_INET, public_bind, &bind_address.sin_addr) != 1) {
        fprintf(stderr, "Invalid MK_PICLOCK_API_BIND IPv4 address: %s\n", public_bind);
        return 1;
    }

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
        (uint16_t)public_port,
        NULL, NULL,
        request_handler, NULL,
        MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&bind_address,
        MHD_OPTION_THREAD_POOL_SIZE, (unsigned int)MHD_THREAD_POOL_SIZE,
        MHD_OPTION_CONNECTION_LIMIT, (unsigned int)MHD_CONNECTION_LIMIT,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)SOCKET_TIMEOUT_SEC,
        MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
        MHD_OPTION_END);
    if (!daemon) {
        fprintf(stderr, "%s: failed to start libmicrohttpd on %s:%d\n", API_NAME, public_bind, public_port);
        return 1;
    }
    fprintf(stderr,
        "%s %s listening on %s:%d; HTTP=libmicrohttpd %s; core=binary-ipc-v%u; threads=%d\n",
        API_NAME, API_VERSION, public_bind, public_port, MHD_get_version(),
        (unsigned int)MP_IPC_VERSION, MHD_THREAD_POOL_SIZE);
    while (g_running) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 250000000L};
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR && g_running) {}
    }
    MHD_stop_daemon(daemon);
    return 0;
}
