/*
 * mk-piclock-api.c
 *
 * Public libmicrohttpd gateway for mk-piclock.
 * HTTP terminates here. The hardware daemon uses a compact binary IPC protocol.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <linux/wireless.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <signal.h>
#include <pwd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/timex.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "asset_store.h"
#include "ipc_protocol.h"
#include "music_jobs.h"
#include "util.h"


#define API_NAME "mk-piclock-api"
#define API_VERSION "1.25"
#define PRODUCT_VERSION "1.8.0"
#define BUILD_TIMESTAMP __DATE__ " " __TIME__
#define DEFAULT_PUBLIC_BIND "0.0.0.0"
#define DEFAULT_PUBLIC_PORT 8080
#define CORE_SOCKET_PATH "/run/mk-piclock/core.sock"
#define WEB_DIR "/opt/mk-piclock/web"
#define API_DOC_DIR "/opt/mk-piclock/api"
#define WEB_PASSWORD_FILE "/opt/mk-piclock/config/web-password.txt"
#define WEB_PASSWORD_TEMP "/opt/mk-piclock/config/.web-password.tmp"
#define WEB_PASSWORD_MAX 64
#define AUTH_COOKIE_NAME "mkpiclock_auth"
#define MAX_REQUEST_BODY (512U * 1024U * 1024U)
#define MAX_STATIC_FILE (64U * 1024U * 1024U)
#define MAX_LIBRARY_DOWNLOAD (512ULL * 1024ULL * 1024ULL)
#define MAX_BACKUP_BYTES (512ULL * 1024ULL * 1024ULL)
#define BACKUP_ENTRY_MAX 2048
#define MAX_API_JSON_RESPONSE (1024U * 1024U)
#define ALLOWED_ORIGIN_MAX 256
#define DISK_RESERVE_BYTES (64ULL * 1024ULL * 1024ULL)
#define DEFAULT_MUSIC_QUOTA_BYTES (1024ULL * 1024ULL * 1024ULL)
#define DEFAULT_STORY_QUOTA_BYTES (2ULL * 1024ULL * 1024ULL * 1024ULL)
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
static uint64_t g_story_quota_bytes = DEFAULT_STORY_QUOTA_BYTES;
static uint64_t g_disk_reserve_bytes = DISK_RESERVE_BYTES;
static pthread_mutex_t g_maintenance_lock = PTHREAD_MUTEX_INITIALIZER;


enum route_id {
    ROUTE_AUTH_STATUS,
    ROUTE_AUTH_LOGIN,
    ROUTE_AUTH_PASSWORD,
    ROUTE_STATUS,
    ROUTE_HEALTH,
    ROUTE_IMAGES_LIST,
    ROUTE_BEDTIME_IMAGES_LIST,
    ROUTE_IMAGES_DOWNLOAD,
    ROUTE_BEDTIME_IMAGES_DOWNLOAD,
    ROUTE_FONTS_LIST,
    ROUTE_MUSIC_LIST,
    ROUTE_STORIES_LIST,
    ROUTE_MUSIC_JOBS,
    ROUTE_CLEAR_MUSIC_QUEUE,
    ROUTE_IMAGE_SOURCE,
    ROUTE_BEDTIME_IMAGE_SOURCE,
    ROUTE_FONT_FILE,
    ROUTE_UPLOAD_IMAGE,
    ROUTE_UPLOAD_BEDTIME_IMAGE,
    ROUTE_UPLOAD_MUSIC,
    ROUTE_UPLOAD_STORY,
    ROUTE_UPLOAD_FONT,
    ROUTE_DELETE_IMAGE,
    ROUTE_DELETE_BEDTIME_IMAGE,
    ROUTE_DELETE_ALL_IMAGES,
    ROUTE_DELETE_ALL_BEDTIME_IMAGES,
    ROUTE_DELETE_MUSIC,
    ROUTE_DELETE_ALL_MUSIC,
    ROUTE_DELETE_STORY,
    ROUTE_DELETE_ALL_STORIES,
    ROUTE_DELETE_FONT,
    ROUTE_DISPLAY_ACTION,
    ROUTE_DISPLAY_PREVIEW,
    ROUTE_BRIGHTNESS_PREVIEW,
    ROUTE_DISPLAY_MESSAGE,
    ROUTE_MESSAGE_PREVIEW,
    ROUTE_CONFIG_ALARM,
    ROUTE_CONFIG_AUDIO,
    ROUTE_CONFIG_PERSONALIZATION,
    ROUTE_CONFIG_DISPLAY,
    ROUTE_CONFIG_LED,
    ROUTE_CONFIG_LED_GLOBAL,
    ROUTE_LED_PREVIEW,
    ROUTE_LOGS,
    ROUTE_LOGS_CLEAR,
    ROUTE_DIAGNOSTICS,
    ROUTE_DIAGNOSTIC_REPORT,
    ROUTE_BACKUP_DOWNLOAD,
    ROUTE_BACKUP_RESTORE,
    ROUTE_FACTORY_RESET
};

struct api_route {
    const char *method;
    const char *path;
    enum route_id id;
};

static const struct api_route g_routes[] = {
    {"GET",  "/api/v1/auth/status",                    ROUTE_AUTH_STATUS},
    {"POST", "/api/v1/auth/login",                     ROUTE_AUTH_LOGIN},
    {"POST", "/api/v1/auth/password",                  ROUTE_AUTH_PASSWORD},
    {"GET",  "/api/v1/status",                         ROUTE_STATUS},
    {"GET",  "/api/v1/health",                         ROUTE_HEALTH},
    {"GET",  "/api/v1/assets/images",                   ROUTE_IMAGES_LIST},
    {"GET",  "/api/v1/assets/bedtime-images",          ROUTE_BEDTIME_IMAGES_LIST},
    {"GET",  "/api/v1/assets/images/download",          ROUTE_IMAGES_DOWNLOAD},
    {"GET",  "/api/v1/assets/bedtime-images/download", ROUTE_BEDTIME_IMAGES_DOWNLOAD},
    {"GET",  "/api/v1/assets/fonts",                    ROUTE_FONTS_LIST},
    {"GET",  "/api/v1/assets/music",                    ROUTE_MUSIC_LIST},
    {"GET",  "/api/v1/assets/stories",                  ROUTE_STORIES_LIST},
    {"GET",  "/api/v1/assets/music/jobs",               ROUTE_MUSIC_JOBS},
    {"POST", "/api/v1/assets/music/jobs/clear",         ROUTE_CLEAR_MUSIC_QUEUE},
    {"GET",  "/api/v1/assets/images/source",            ROUTE_IMAGE_SOURCE},
    {"GET",  "/api/v1/assets/bedtime-images/source",   ROUTE_BEDTIME_IMAGE_SOURCE},
    {"GET",  "/api/v1/assets/fonts/file",               ROUTE_FONT_FILE},
    {"POST", "/api/v1/assets/images/upload",            ROUTE_UPLOAD_IMAGE},
    {"POST", "/api/v1/assets/bedtime-images/upload",   ROUTE_UPLOAD_BEDTIME_IMAGE},
    {"POST", "/api/v1/assets/music/upload",             ROUTE_UPLOAD_MUSIC},
    {"POST", "/api/v1/assets/stories/upload",           ROUTE_UPLOAD_STORY},
    {"POST", "/api/v1/assets/fonts/upload",             ROUTE_UPLOAD_FONT},
    {"POST", "/api/v1/assets/images/delete",            ROUTE_DELETE_IMAGE},
    {"POST", "/api/v1/assets/bedtime-images/delete",   ROUTE_DELETE_BEDTIME_IMAGE},
    {"POST", "/api/v1/assets/images/delete-all",        ROUTE_DELETE_ALL_IMAGES},
    {"POST", "/api/v1/assets/bedtime-images/delete-all", ROUTE_DELETE_ALL_BEDTIME_IMAGES},
    {"POST", "/api/v1/assets/music/delete",            ROUTE_DELETE_MUSIC},
    {"POST", "/api/v1/assets/music/delete-all",        ROUTE_DELETE_ALL_MUSIC},
    {"POST", "/api/v1/assets/stories/delete",          ROUTE_DELETE_STORY},
    {"POST", "/api/v1/assets/stories/delete-all",      ROUTE_DELETE_ALL_STORIES},
    {"POST", "/api/v1/assets/fonts/delete",            ROUTE_DELETE_FONT},
    {"POST", "/api/v1/display/action",                  ROUTE_DISPLAY_ACTION},
    {"GET",  "/api/v1/display/preview",                 ROUTE_DISPLAY_PREVIEW},
    {"POST", "/api/v1/display/brightness-preview",      ROUTE_BRIGHTNESS_PREVIEW},
    {"POST", "/api/v1/display/message",                 ROUTE_DISPLAY_MESSAGE},
    {"POST", "/api/v1/display/message/preview",         ROUTE_MESSAGE_PREVIEW},
    {"POST", "/api/v1/config/alarms",                   ROUTE_CONFIG_ALARM},
    {"POST", "/api/v1/config/audio",                    ROUTE_CONFIG_AUDIO},
    {"POST", "/api/v1/config/personalization",          ROUTE_CONFIG_PERSONALIZATION},
    {"POST", "/api/v1/config/display",                  ROUTE_CONFIG_DISPLAY},
    {"POST", "/api/v1/config/led",                      ROUTE_CONFIG_LED},
    {"POST", "/api/v1/config/led-global",               ROUTE_CONFIG_LED_GLOBAL},
    {"POST", "/api/v1/led/preview",                    ROUTE_LED_PREVIEW},
    {"GET",  "/api/v1/logs",                            ROUTE_LOGS},
    {"POST", "/api/v1/logs/clear",                      ROUTE_LOGS_CLEAR},
    {"GET",  "/api/v1/diagnostics",                     ROUTE_DIAGNOSTICS},
    {"GET",  "/api/v1/diagnostics/report",              ROUTE_DIAGNOSTIC_REPORT},
    {"GET",  "/api/v1/backup/download",                 ROUTE_BACKUP_DOWNLOAD},
    {"POST", "/api/v1/backup/restore",                  ROUTE_BACKUP_RESTORE},
    {"POST", "/api/v1/factory-reset",                   ROUTE_FACTORY_RESET}
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

static int ipc_call(uint16_t opcode, const void *payload, size_t payload_len,
                    struct ipc_result *result);
static void ipc_result_free(struct ipc_result *result);
static int notify_asset(uint8_t kind, uint8_t action, uint32_t count, const char *file);
static int json_integer_value(const char *json, const char *key, int fallback);
static const char *form_value(const struct request_context *context, const char *key);
static int core_alarm_active(void);

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
        case ROUTE_UPLOAD_IMAGE:
        case ROUTE_UPLOAD_BEDTIME_IMAGE:
            return MP_IMAGE_UPLOAD_MAX_BYTES;
        case ROUTE_UPLOAD_MUSIC:
            return MP_MUSIC_UPLOAD_MAX_BYTES;
        case ROUTE_UPLOAD_STORY:
            return MP_STORY_UPLOAD_MAX_BYTES;
        case ROUTE_UPLOAD_FONT:
            return MP_FONT_UPLOAD_MAX_BYTES;
        case ROUTE_BACKUP_RESTORE:
            return (size_t)MAX_BACKUP_BYTES;
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

static enum MHD_Result queue_json_with_cookie(struct MHD_Connection *connection,
                                              unsigned int status, const char *json,
                                              const char *cookie) {
    const char *body = json ? json : "{}";
    struct MHD_Response *response = MHD_create_response_from_buffer(
        strlen(body), (void *)body, MHD_RESPMEM_MUST_COPY);
    if (!response) return MHD_NO;
    (void)add_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json; charset=utf-8");
    if (cookie && *cookie) (void)add_header(response, "Set-Cookie", cookie);
    add_api_headers(connection, response);
    enum MHD_Result result = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return result;
}

static int read_web_password(char *out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    int fd = open(WEB_PASSWORD_FILE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 1 ||
        st.st_size > WEB_PASSWORD_MAX + 2) {
        close(fd);
        return 0;
    }
    ssize_t received = read(fd, out, out_len - 1);
    close(fd);
    if (received <= 0) {
        out[0] = '\0';
        return 0;
    }
    out[received] = '\0';
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] != '\0';
}

static int valid_web_password(const char *password) {
    if (!password) return 0;
    size_t length = strlen(password);
    if (length > WEB_PASSWORD_MAX) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)password[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static int password_equal(const char *left, const char *right) {
    size_t left_len = left ? strlen(left) : 0;
    size_t right_len = right ? strlen(right) : 0;
    size_t length = left_len > right_len ? left_len : right_len;
    unsigned int difference = (unsigned int)(left_len ^ right_len);
    for (size_t i = 0; i < length; i++) {
        unsigned char a = i < left_len ? (unsigned char)left[i] : 0;
        unsigned char b = i < right_len ? (unsigned char)right[i] : 0;
        difference |= (unsigned int)(a ^ b);
    }
    return difference == 0;
}

static void password_hex_encode(const char *password, char *out, size_t out_len) {
    static const char digits[] = "0123456789abcdef";
    if (!out || out_len == 0) return;
    out[0] = '\0';
    size_t length = password ? strlen(password) : 0;
    if (length * 2 + 1 > out_len) return;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)password[i];
        out[i * 2] = digits[c >> 4];
        out[i * 2 + 1] = digits[c & 0x0f];
    }
    out[length * 2] = '\0';
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int password_hex_decode(const char *encoded, size_t encoded_len,
                               char *out, size_t out_len) {
    if (!encoded || !out || encoded_len == 0 || (encoded_len & 1u) != 0 ||
        encoded_len / 2 + 1 > out_len) return -1;
    for (size_t i = 0; i < encoded_len; i += 2) {
        int high = hex_digit_value(encoded[i]);
        int low = hex_digit_value(encoded[i + 1]);
        if (high < 0 || low < 0) return -1;
        out[i / 2] = (char)((high << 4) | low);
    }
    out[encoded_len / 2] = '\0';
    return 0;
}

static int auth_cookie_matches(struct MHD_Connection *connection, const char *expected) {
    const char *cookies = MHD_lookup_connection_value(
        connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_COOKIE);
    if (!cookies || !expected || !*expected) return 0;
    const size_t name_len = strlen(AUTH_COOKIE_NAME);
    const char *cursor = cookies;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == ';') cursor++;
        const char *segment_end = strchr(cursor, ';');
        if (!segment_end) segment_end = cursor + strlen(cursor);
        const char *equals = memchr(cursor, '=', (size_t)(segment_end - cursor));
        if (equals && (size_t)(equals - cursor) == name_len &&
            strncmp(cursor, AUTH_COOKIE_NAME, name_len) == 0) {
            const char *value = equals + 1;
            size_t value_len = (size_t)(segment_end - value);
            while (value_len > 0 && value[value_len - 1] == ' ') value_len--;
            char decoded[WEB_PASSWORD_MAX + 1];
            if (password_hex_decode(value, value_len, decoded, sizeof(decoded)) == 0)
                return password_equal(decoded, expected);
            return 0;
        }
        cursor = *segment_end ? segment_end + 1 : segment_end;
    }
    return 0;
}

static void build_auth_cookie(const char *password, char *out, size_t out_len) {
    char encoded[WEB_PASSWORD_MAX * 2 + 1];
    password_hex_encode(password, encoded, sizeof(encoded));
    snprintf(out, out_len, AUTH_COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict", encoded);
}

static const char *clear_auth_cookie(void) {
    return AUTH_COOKIE_NAME "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict";
}

static int write_web_password(const char *password) {
    if (!password || !*password) {
        if (unlink(WEB_PASSWORD_FILE) != 0 && errno != ENOENT) return -1;
        (void)unlink(WEB_PASSWORD_TEMP);
        return 0;
    }
    int fd = open(WEB_PASSWORD_TEMP,
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                  0640);
    if (fd < 0) return -1;
    size_t length = strlen(password);
    int ok = mp_write_full(fd, password, length) == 0 &&
             mp_write_full(fd, "\n", 1) == 0 && fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(WEB_PASSWORD_TEMP, WEB_PASSWORD_FILE) != 0) {
        (void)unlink(WEB_PASSWORD_TEMP);
        return -1;
    }
    return 0;
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
    if (strcasecmp(dot, ".webmanifest") == 0) return "application/manifest+json; charset=utf-8";
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
    if (cache_seconds > 0 && client_etag && strcmp(client_etag, etag) == 0) {
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
        if (cache_seconds == 0) {
            (void)add_header(response, "Cache-Control", "no-store");
        } else {
            char cache[64];
            snprintf(cache, sizeof(cache), "public, max-age=%u", cache_seconds);
            (void)add_header(response, "Cache-Control", cache);
        }
        (void)add_header(response, "X-Content-Type-Options", "nosniff");
    }
    enum MHD_Result result = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return result;
}


struct zip_entry {
    char name[512];
    uint32_t crc32;
    uint32_t size;
    uint32_t local_offset;
    uint16_t dos_time;
    uint16_t dos_date;
};

static int write_u16(FILE *file, uint16_t value) {
    unsigned char bytes[2] = {(unsigned char)(value & 0xffu), (unsigned char)((value >> 8) & 0xffu)};
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes) ? 0 : -1;
}

static int write_u32(FILE *file, uint32_t value) {
    unsigned char bytes[4] = {
        (unsigned char)(value & 0xffu),
        (unsigned char)((value >> 8) & 0xffu),
        (unsigned char)((value >> 16) & 0xffu),
        (unsigned char)((value >> 24) & 0xffu)
    };
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes) ? 0 : -1;
}

static uint32_t g_crc32_table[256];
static pthread_once_t g_crc32_once = PTHREAD_ONCE_INIT;

static void crc32_init_table(void) {
    for (uint32_t value = 0; value < 256; value++) {
        uint32_t crc = value;
        for (unsigned int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
        g_crc32_table[value] = crc;
    }
}

static uint32_t crc32_update(uint32_t crc, const unsigned char *data, size_t length) {
    if (!data && length != 0) return crc;
    (void)pthread_once(&g_crc32_once, crc32_init_table);
    crc = ~crc;
    for (size_t i = 0; i < length; i++)
        crc = g_crc32_table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    return ~crc;
}

static void zip_dos_datetime(time_t value, uint16_t *dos_time, uint16_t *dos_date) {
    struct tm tmv;
    if (!gmtime_r(&value, &tmv)) memset(&tmv, 0, sizeof(tmv));
    int year = tmv.tm_year + 1900;
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;
    int month = tmv.tm_mon + 1;
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    int day = tmv.tm_mday;
    if (day < 1) day = 1;
    if (day > 31) day = 31;
    *dos_time = (uint16_t)(((tmv.tm_hour & 31) << 11) | ((tmv.tm_min & 63) << 5) | ((tmv.tm_sec / 2) & 31));
    *dos_date = (uint16_t)(((year - 1980) << 9) | ((month & 15) << 5) | (day & 31));
}

static int copy_file_to_zip(FILE *zip, const char *path, struct zip_entry *entry) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > UINT32_MAX) {
        close(fd);
        return -1;
    }
    entry->size = (uint32_t)st.st_size;
    zip_dos_datetime(st.st_mtime, &entry->dos_time, &entry->dos_date);

    unsigned char buffer[32768];
    uint32_t crc = 0;
    ssize_t got;
    while ((got = read(fd, buffer, sizeof(buffer))) > 0)
        crc = crc32_update(crc, buffer, (size_t)got);
    if (got < 0 || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    entry->crc32 = crc;

    off_t offset = ftello(zip);
    size_t name_len = strlen(entry->name);
    if (offset < 0 || (uint64_t)offset > UINT32_MAX || name_len > UINT16_MAX) {
        close(fd);
        return -1;
    }
    entry->local_offset = (uint32_t)offset;
    if (write_u32(zip, 0x04034b50u) != 0 || write_u16(zip, 20) != 0 ||
        write_u16(zip, 0) != 0 || write_u16(zip, 0) != 0 ||
        write_u16(zip, entry->dos_time) != 0 || write_u16(zip, entry->dos_date) != 0 ||
        write_u32(zip, entry->crc32) != 0 || write_u32(zip, entry->size) != 0 ||
        write_u32(zip, entry->size) != 0 || write_u16(zip, (uint16_t)name_len) != 0 ||
        write_u16(zip, 0) != 0 || fwrite(entry->name, 1, name_len, zip) != name_len) {
        close(fd);
        return -1;
    }
    while ((got = read(fd, buffer, sizeof(buffer))) > 0) {
        if (fwrite(buffer, 1, (size_t)got, zip) != (size_t)got) {
            close(fd);
            return -1;
        }
    }
    close(fd);
    return got < 0 ? -1 : 0;
}

static int finish_zip(FILE *zip, struct zip_entry *entries, int count);

static int build_image_zip(int bedtime, const char *output_path) {
    char raw_files[MP_ASSET_LIST_MAX][MP_ASSET_NAME_MAX];
    int raw_count = mp_asset_scan(bedtime ? MP_BEDTIME_IMAGE_DIR : MP_IMAGE_DIR,
                                  MP_ASSET_SCAN_IMAGE_RAW, raw_files, MP_ASSET_LIST_MAX);
    struct zip_entry entries[MP_ASSET_LIST_MAX];
    memset(entries, 0, sizeof(entries));
    int count = 0;
    uint64_t total_source_bytes = 0;

    FILE *zip = fopen(output_path, "wb");
    if (!zip) return -1;
    for (int i = 0; i < raw_count; i++) {
        char source_path[768];
        if (mp_asset_image_source_path(raw_files[i], bedtime, source_path, sizeof(source_path)) != 0)
            continue;
        struct stat st;
        if (stat(source_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) continue;
        total_source_bytes += (uint64_t)st.st_size;
        if (total_source_bytes > MAX_LIBRARY_DOWNLOAD || count >= MP_ASSET_LIST_MAX) {
            fclose(zip);
            unlink(output_path);
            errno = EFBIG;
            return -1;
        }
        mp_safe_str(entries[count].name, sizeof(entries[count].name), raw_files[i]);
        char *dot = strrchr(entries[count].name, '.');
        if (!dot) continue;
        mp_safe_str(dot, (size_t)(entries[count].name + sizeof(entries[count].name) - dot), ".png");
        if (copy_file_to_zip(zip, source_path, &entries[count]) != 0) {
            fclose(zip);
            unlink(output_path);
            return -1;
        }
        count++;
    }
    if (count == 0) {
        fclose(zip);
        unlink(output_path);
        errno = ENOENT;
        return -1;
    }

    int failed = finish_zip(zip, entries, count) != 0;
    if (fclose(zip) != 0) failed = 1;
    if (failed) {
        unlink(output_path);
        return -1;
    }
    return 0;
}

static enum MHD_Result queue_download_file(struct MHD_Connection *connection, const char *path,
                                           const char *download_name) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return MHD_NO;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > MAX_LIBRARY_DOWNLOAD) {
        close(fd);
        return MHD_NO;
    }
    struct MHD_Response *response = MHD_create_response_from_fd64((uint64_t)st.st_size, fd);
    if (!response) {
        close(fd);
        return MHD_NO;
    }
    (void)add_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "application/zip");
    char disposition[256];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", download_name);
    (void)add_header(response, "Content-Disposition", disposition);
    add_api_headers(connection, response);
    enum MHD_Result result = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return result;
}

static enum MHD_Result download_images(struct MHD_Connection *connection, int bedtime) {
    char path[] = "/tmp/mk-piclock-images-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return queue_json(connection, 500, "{\"ok\":false,\"error\":\"download could not be prepared\"}");
    close(fd);
    if (build_image_zip(bedtime, path) != 0) {
        int error = errno;
        unlink(path);
        if (error == ENOENT)
            return queue_json(connection, 404, "{\"ok\":false,\"error\":\"no source PNG images are available\"}");
        if (error == EFBIG)
            return queue_json(connection, 413, "{\"ok\":false,\"error\":\"image library is too large to download at once\"}");
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"image ZIP could not be created\"}");
    }
    enum MHD_Result result = queue_download_file(connection, path,
        bedtime ? "mk-piclock-bedtime-images.zip" : "mk-piclock-day-images.zip");
    unlink(path);
    return result == MHD_NO
        ? queue_json(connection, 500, "{\"ok\":false,\"error\":\"image ZIP could not be sent\"}")
        : result;
}


static int copy_memory_to_zip(FILE *zip, const void *data, size_t size, const char *name,
                              struct zip_entry *entry) {
    if (!zip || !data || !name || !entry || size > UINT32_MAX || strlen(name) > UINT16_MAX) return -1;
    memset(entry, 0, sizeof(*entry));
    mp_safe_str(entry->name, sizeof(entry->name), name);
    entry->size = (uint32_t)size;
    entry->crc32 = crc32_update(0, data, size);
    zip_dos_datetime(time(NULL), &entry->dos_time, &entry->dos_date);
    off_t offset = ftello(zip);
    if (offset < 0 || (uint64_t)offset > UINT32_MAX) return -1;
    entry->local_offset = (uint32_t)offset;
    size_t name_len = strlen(entry->name);
    if (write_u32(zip, 0x04034b50u) != 0 || write_u16(zip, 20) != 0 ||
        write_u16(zip, 0) != 0 || write_u16(zip, 0) != 0 ||
        write_u16(zip, entry->dos_time) != 0 || write_u16(zip, entry->dos_date) != 0 ||
        write_u32(zip, entry->crc32) != 0 || write_u32(zip, entry->size) != 0 ||
        write_u32(zip, entry->size) != 0 || write_u16(zip, (uint16_t)name_len) != 0 ||
        write_u16(zip, 0) != 0 || fwrite(entry->name, 1, name_len, zip) != name_len ||
        fwrite(data, 1, size, zip) != size) return -1;
    return 0;
}

static int finish_zip(FILE *zip, struct zip_entry *entries, int count) {
    if (!zip || !entries || count < 0 || count > UINT16_MAX) return -1;
    off_t central_offset_value = ftello(zip);
    if (central_offset_value < 0 || (uint64_t)central_offset_value > UINT32_MAX) return -1;
    uint32_t central_offset = (uint32_t)central_offset_value;
    for (int i = 0; i < count; i++) {
        size_t name_len = strlen(entries[i].name);
        if (write_u32(zip, 0x02014b50u) != 0 || write_u16(zip, 20) != 0 ||
            write_u16(zip, 20) != 0 || write_u16(zip, 0) != 0 || write_u16(zip, 0) != 0 ||
            write_u16(zip, entries[i].dos_time) != 0 || write_u16(zip, entries[i].dos_date) != 0 ||
            write_u32(zip, entries[i].crc32) != 0 || write_u32(zip, entries[i].size) != 0 ||
            write_u32(zip, entries[i].size) != 0 || write_u16(zip, (uint16_t)name_len) != 0 ||
            write_u16(zip, 0) != 0 || write_u16(zip, 0) != 0 || write_u16(zip, 0) != 0 ||
            write_u16(zip, 0) != 0 || write_u32(zip, 0100644u << 16) != 0 ||
            write_u32(zip, entries[i].local_offset) != 0 ||
            fwrite(entries[i].name, 1, name_len, zip) != name_len) return -1;
    }
    off_t end_value = ftello(zip);
    if (end_value < 0 || (uint64_t)end_value > UINT32_MAX) return -1;
    uint32_t central_size = (uint32_t)end_value - central_offset;
    if (write_u32(zip, 0x06054b50u) != 0 || write_u16(zip, 0) != 0 ||
        write_u16(zip, 0) != 0 || write_u16(zip, (uint16_t)count) != 0 ||
        write_u16(zip, (uint16_t)count) != 0 || write_u32(zip, central_size) != 0 ||
        write_u32(zip, central_offset) != 0 || write_u16(zip, 0) != 0 ||
        fflush(zip) != 0 || fsync(fileno(zip)) != 0) return -1;
    return 0;
}

static int backup_file_allowed(const char *prefix, const char *name) {
    if (!mp_asset_safe_filename(name) || name[0] == '.') return 0;
    if (strcmp(prefix, "assets/images") == 0 || strcmp(prefix, "assets/bedtime-images") == 0)
        return mp_asset_has_png_ext(name) || mp_asset_has_raw_ext(name);
    if (strcmp(prefix, "assets/fonts") == 0) return mp_asset_has_font_ext(name);
    return 0;
}

static int add_directory_to_backup(FILE *zip, struct zip_entry *entries, int *count,
                                   const char *directory, const char *prefix,
                                   uint64_t *total_bytes) {
    DIR *dir = opendir(directory);
    if (!dir) return errno == ENOENT ? 0 : -1;
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (!backup_file_allowed(prefix, item->d_name)) continue;
        if (*count >= BACKUP_ENTRY_MAX) {
            closedir(dir);
            errno = EFBIG;
            return -1;
        }
        char path[PATH_MAX];
        char archive_name[512];
        if (snprintf(path, sizeof(path), "%s/%s", directory, item->d_name) >= (int)sizeof(path) ||
            snprintf(archive_name, sizeof(archive_name), "%s/%s", prefix, item->d_name) >= (int)sizeof(archive_name))
            continue;
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) continue;
        if ((uint64_t)st.st_size > MAX_BACKUP_BYTES - *total_bytes) {
            closedir(dir);
            errno = EFBIG;
            return -1;
        }
        *total_bytes += (uint64_t)st.st_size;
        memset(&entries[*count], 0, sizeof(entries[*count]));
        mp_safe_str(entries[*count].name, sizeof(entries[*count].name), archive_name);
        if (copy_file_to_zip(zip, path, &entries[*count]) != 0) {
            closedir(dir);
            return -1;
        }
        (*count)++;
    }
    closedir(dir);
    return 0;
}

static int build_full_backup(const char *output_path) {
    struct ipc_result config;
    if (ipc_call(MP_IPC_OP_CONFIG_EXPORT, NULL, 0, &config) != 0 || config.status != 200 ||
        config.content_type != MP_IPC_CONTENT_TEXT || config.body_len == 0 ||
        config.body_len > MP_IPC_CONFIG_MAX_BYTES) {
        ipc_result_free(&config);
        return -1;
    }
    struct zip_entry *entries = calloc(BACKUP_ENTRY_MAX, sizeof(*entries));
    if (!entries) {
        ipc_result_free(&config);
        return -1;
    }
    FILE *zip = fopen(output_path, "wb");
    if (!zip) {
        free(entries);
        ipc_result_free(&config);
        return -1;
    }
    int count = 0;
    uint64_t total_bytes = config.body_len;
    char metadata[512];
    time_t now = time(NULL);
    int metadata_len = snprintf(metadata, sizeof(metadata),
        "{\n  \"product\": \"mk-piclock\",\n  \"version\": \"%s\",\n  \"api_version\": \"%s\",\n  \"created_at\": %lld,\n  \"format\": 1\n}\n",
        PRODUCT_VERSION, API_VERSION, (long long)now);
    int failed = metadata_len <= 0 ||
        copy_memory_to_zip(zip, metadata, (size_t)metadata_len,
                           "backup.json", &entries[count++]) != 0 ||
        copy_memory_to_zip(zip, config.body, config.body_len,
                           "config/clock.conf", &entries[count++]) != 0 ||
        add_directory_to_backup(zip, entries, &count, MP_IMAGE_DIR, "assets/images", &total_bytes) != 0 ||
        add_directory_to_backup(zip, entries, &count, MP_BEDTIME_IMAGE_DIR, "assets/bedtime-images", &total_bytes) != 0 ||
        add_directory_to_backup(zip, entries, &count, MP_FONT_DIR, "assets/fonts", &total_bytes) != 0 ||
        finish_zip(zip, entries, count) != 0;
    if (fclose(zip) != 0) failed = 1;
    if (failed) {
        unlink(output_path);
        free(entries);
        ipc_result_free(&config);
        return -1;
    }
    free(entries);
    ipc_result_free(&config);
    return 0;
}

static enum MHD_Result download_backup_locked(struct MHD_Connection *connection) {
    char path[] = "/tmp/mk-piclock-backup-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return queue_json(connection, 500, "{\"ok\":false,\"error\":\"backup could not be prepared\"}");
    close(fd);
    if (build_full_backup(path) != 0) {
        unlink(path);
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"backup could not be created\"}");
    }
    enum MHD_Result result = queue_download_file(connection, path, "mk-piclock-backup.zip");
    unlink(path);
    return result == MHD_NO
        ? queue_json(connection, 500, "{\"ok\":false,\"error\":\"backup could not be sent\"}") : result;
}

static enum MHD_Result download_backup(struct MHD_Connection *connection) {
    if (pthread_mutex_trylock(&g_maintenance_lock) != 0)
        return queue_json(connection, 409,
            "{\"ok\":false,\"error\":\"another backup or maintenance task is running\"}");
    enum MHD_Result result = download_backup_locked(connection);
    pthread_mutex_unlock(&g_maintenance_lock);
    return result;
}

static uint16_t read_le16(const unsigned char *value) {
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static uint32_t read_le32(const unsigned char *value) {
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static int restore_archive_path(const char *name, const char *stage_root,
                                char *output, size_t output_len) {
    const char *relative = NULL;
    if (strcmp(name, "config/clock.conf") == 0) relative = name;
    else if (strncmp(name, "assets/images/", 14) == 0 &&
             backup_file_allowed("assets/images", name + 14)) relative = name;
    else if (strncmp(name, "assets/bedtime-images/", 23) == 0 &&
             backup_file_allowed("assets/bedtime-images", name + 23)) relative = name;
    else if (strncmp(name, "assets/fonts/", 13) == 0 &&
             backup_file_allowed("assets/fonts", name + 13)) relative = name;
    else if (strcmp(name, "backup.json") == 0) return 1;
    else return -1;
    if (strstr(relative, "..") || strchr(relative, '\\')) return -1;
    return snprintf(output, output_len, "%s/%s", stage_root, relative) < (int)output_len ? 0 : -1;
}

static int prepare_restore_stage(const char *stage_root) {
    static const char *const paths[] = {
        "config",
        "assets",
        "assets/images",
        "assets/bedtime-images",
        "assets/fonts"
    };
    char path[PATH_MAX];
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int written = snprintf(path, sizeof(path), "%s/%s", stage_root, paths[i]);
        if (written <= 0 || (size_t)written >= sizeof(path) || mp_asset_ensure_dir(path) != 0)
            return -1;
    }
    return 0;
}

static int extract_backup_archive(const char *archive_path, const char *stage_root,
                                  int *file_count) {
    if (prepare_restore_stage(stage_root) != 0) return -1;
    FILE *archive = fopen(archive_path, "rb");
    if (!archive) return -1;
    int count = 0;
    int entry_count = 0;
    int saw_metadata = 0;
    int saw_config = 0;
    uint64_t total = 0;
    for (;;) {
        unsigned char signature_bytes[4];
        size_t got = fread(signature_bytes, 1, sizeof(signature_bytes), archive);
        if (got == 0 && feof(archive)) break;
        if (got != sizeof(signature_bytes)) { fclose(archive); return -1; }
        uint32_t signature = read_le32(signature_bytes);
        if (signature == 0x02014b50u || signature == 0x06054b50u) break;
        if (signature != 0x04034b50u || entry_count >= BACKUP_ENTRY_MAX) {
            fclose(archive);
            return -1;
        }
        entry_count++;
        unsigned char header[26];
        if (fread(header, 1, sizeof(header), archive) != sizeof(header)) { fclose(archive); return -1; }
        uint16_t flags = read_le16(header + 2);
        uint16_t method = read_le16(header + 4);
        uint32_t expected_crc = read_le32(header + 10);
        uint32_t compressed_size = read_le32(header + 14);
        uint32_t uncompressed_size = read_le32(header + 18);
        uint16_t name_length = read_le16(header + 22);
        uint16_t extra_length = read_le16(header + 24);
        if (flags != 0 || method != 0 || compressed_size != uncompressed_size ||
            name_length == 0 || name_length >= 512 ||
            total + uncompressed_size > MAX_BACKUP_BYTES) { fclose(archive); return -1; }
        char name[512];
        if (fread(name, 1, name_length, archive) != name_length) { fclose(archive); return -1; }
        name[name_length] = '\0';
        if (extra_length && fseeko(archive, extra_length, SEEK_CUR) != 0) { fclose(archive); return -1; }
        char output[PATH_MAX];
        int path_result = restore_archive_path(name, stage_root, output, sizeof(output));
        if (path_result == 1) {
            if (saw_metadata || uncompressed_size == 0 || uncompressed_size >= 4096) {
                fclose(archive);
                return -1;
            }
            char metadata[4096];
            if (fread(metadata, 1, uncompressed_size, archive) != uncompressed_size) {
                fclose(archive);
                return -1;
            }
            metadata[uncompressed_size] = '\0';
            if (crc32_update(0, (const unsigned char *)metadata, uncompressed_size) != expected_crc ||
                !strstr(metadata, "\"product\": \"mk-piclock\"") ||
                !strstr(metadata, "\"format\": 1")) {
                fclose(archive);
                return -1;
            }
            saw_metadata = 1;
            total += uncompressed_size;
            continue;
        }
        if (path_result != 0) { fclose(archive); return -1; }
        if (strcmp(name, "config/clock.conf") == 0) {
            if (saw_config) { fclose(archive); return -1; }
            saw_config = 1;
        }
        int fd = open(output, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0640);
        if (fd < 0) { fclose(archive); return -1; }
        unsigned char buffer[32768];
        uint32_t remaining = uncompressed_size;
        uint32_t crc = 0;
        int failed = 0;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            if (fread(buffer, 1, chunk, archive) != chunk || mp_write_full(fd, buffer, chunk) != 0) {
                failed = 1;
                break;
            }
            crc = crc32_update(crc, buffer, chunk);
            remaining -= (uint32_t)chunk;
        }
        if (fsync(fd) != 0 || close(fd) != 0) failed = 1;
        if (failed || crc != expected_crc) { fclose(archive); return -1; }
        total += uncompressed_size;
        count++;
    }
    fclose(archive);
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/config/clock.conf", stage_root);
    struct stat config_stat;
    if (!saw_metadata || !saw_config || stat(config_path, &config_stat) != 0 || !S_ISREG(config_stat.st_mode) ||
        config_stat.st_size <= 0 || config_stat.st_size > MP_IPC_CONFIG_MAX_BYTES) return -1;
    if (file_count) *file_count = count;
    return 0;
}

static int remove_tree_contents_fd(int directory_fd, unsigned int depth) {
    if (directory_fd < 0 || depth > 64) {
        if (directory_fd >= 0) close(directory_fd);
        return -1;
    }
    DIR *directory = fdopendir(directory_fd);
    if (!directory) {
        close(directory_fd);
        return -1;
    }
    int failed = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        struct stat state;
        if (fstatat(dirfd(directory), entry->d_name, &state, AT_SYMLINK_NOFOLLOW) != 0) {
            failed = 1;
            continue;
        }
        if (S_ISDIR(state.st_mode)) {
            int child_fd = openat(dirfd(directory), entry->d_name,
                                  O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
            if (child_fd < 0 || remove_tree_contents_fd(child_fd, depth + 1) != 0 ||
                unlinkat(dirfd(directory), entry->d_name, AT_REMOVEDIR) != 0)
                failed = 1;
        } else if (unlinkat(dirfd(directory), entry->d_name, 0) != 0) {
            failed = 1;
        }
    }
    if (closedir(directory) != 0) failed = 1;
    return failed ? -1 : 0;
}

static void remove_tree(const char *path) {
    if (!path || !*path) return;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        (void)unlink(path);
        return;
    }
    (void)remove_tree_contents_fd(fd, 0);
    (void)rmdir(path);
}

static int clear_flat_asset_directory(const char *directory) {
    int fd = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return -1;
    }
    int failed = 0;
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (item->d_name[0] == '.') continue;
        struct stat state;
        if (fstatat(dirfd(dir), item->d_name, &state, AT_SYMLINK_NOFOLLOW) != 0) {
            failed = 1;
            continue;
        }
        if (S_ISREG(state.st_mode) && unlinkat(dirfd(dir), item->d_name, 0) != 0)
            failed = 1;
    }
    if (closedir(dir) != 0) failed = 1;
    return failed ? -1 : 0;
}

static int copy_file(const char *source, const char *target) {
    int in = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0) return -1;
    int out = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0640);
    if (out < 0) { close(in); return -1; }
    unsigned char buffer[32768];
    ssize_t got;
    int failed = 0;
    while ((got = read(in, buffer, sizeof(buffer))) > 0)
        if (mp_write_full(out, buffer, (size_t)got) != 0) { failed = 1; break; }
    if (got < 0 || fsync(out) != 0) failed = 1;
    close(in);
    if (close(out) != 0) failed = 1;
    if (failed) unlink(target);
    return failed ? -1 : 0;
}

static int copy_staged_directory(const char *stage_root, const char *relative, const char *target_dir) {
    char source_dir[PATH_MAX];
    int written = snprintf(source_dir, sizeof(source_dir), "%s/%s", stage_root, relative);
    if (written <= 0 || (size_t)written >= sizeof(source_dir) || mp_asset_ensure_dir(target_dir) != 0)
        return -1;
    DIR *dir = opendir(source_dir);
    if (!dir) return errno == ENOENT ? 0 : -1;
    struct dirent *item;
    int failed = 0;
    while ((item = readdir(dir)) != NULL) {
        if (item->d_name[0] == '.') continue;
        char source[PATH_MAX], target[PATH_MAX];
        if (snprintf(source, sizeof(source), "%s/%s", source_dir, item->d_name) >= (int)sizeof(source) ||
            snprintf(target, sizeof(target), "%s/%s", target_dir, item->d_name) >= (int)sizeof(target)) {
            failed = 1;
            break;
        }
        if (copy_file(source, target) != 0) { failed = 1; break; }
    }
    closedir(dir);
    return failed ? -1 : 0;
}

static int move_flat_asset_files(const char *source_dir, const char *target_dir) {
    if (mp_asset_ensure_dir(target_dir) != 0) return -1;
    DIR *dir = opendir(source_dir);
    if (!dir) return errno == ENOENT ? 0 : -1;
    struct dirent *item;
    int failed = 0;
    while ((item = readdir(dir)) != NULL) {
        if (item->d_name[0] == '.') continue;
        char source[PATH_MAX], target[PATH_MAX];
        if (snprintf(source, sizeof(source), "%s/%s", source_dir, item->d_name) >= (int)sizeof(source) ||
            snprintf(target, sizeof(target), "%s/%s", target_dir, item->d_name) >= (int)sizeof(target)) {
            failed = 1;
            break;
        }
        struct stat st;
        if (lstat(source, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (rename(source, target) != 0) {
            failed = 1;
            break;
        }
    }
    closedir(dir);
    return failed ? -1 : 0;
}

struct asset_transaction_entry {
    const char *directory;
    const char *rollback_name;
};

static const struct asset_transaction_entry RESTORE_ASSETS[] = {
    {MP_IMAGE_DIR, "images"},
    {MP_BEDTIME_IMAGE_DIR, "bedtime-images"},
    {MP_FONT_DIR, "fonts"}
};

static const struct asset_transaction_entry RESET_ASSETS[] = {
    {MP_IMAGE_DIR, "images"},
    {MP_BEDTIME_IMAGE_DIR, "bedtime-images"},
    {MP_MUSIC_DIR, "music"},
    {MP_STORY_DIR, "stories"},
    {MP_FONT_DIR, "fonts"}
};

static int rollback_path(char *out, size_t out_len, const char *root, const char *name) {
    int written = snprintf(out, out_len, "%s/%s", root, name);
    return written > 0 && (size_t)written < out_len ? 0 : -1;
}

static void restore_asset_set(const char *rollback_root,
                              const struct asset_transaction_entry *assets,
                              size_t asset_count) {
    if (!rollback_root || !*rollback_root || !assets) return;
    for (size_t i = 0; i < asset_count; i++) {
        char source[PATH_MAX];
        if (rollback_path(source, sizeof(source), rollback_root, assets[i].rollback_name) != 0)
            continue;
        (void)clear_flat_asset_directory(assets[i].directory);
        (void)move_flat_asset_files(source, assets[i].directory);
    }
    remove_tree(rollback_root);
}

static int stage_asset_set(char *rollback_root, size_t rollback_root_len,
                           const struct asset_transaction_entry *assets,
                           size_t asset_count) {
    if (!rollback_root || rollback_root_len < 32 || !assets || asset_count == 0) return -1;
    int written = snprintf(rollback_root, rollback_root_len,
                           "%s/.restore-old-XXXXXX", MP_APP_ROOT "/assets");
    if (written <= 0 || (size_t)written >= rollback_root_len || !mkdtemp(rollback_root)) return -1;

    for (size_t i = 0; i < asset_count; i++) {
        char target[PATH_MAX];
        if (rollback_path(target, sizeof(target), rollback_root, assets[i].rollback_name) != 0 ||
            move_flat_asset_files(assets[i].directory, target) != 0) {
            restore_asset_set(rollback_root, assets, i + 1);
            rollback_root[0] = '\0';
            return -1;
        }
    }
    return 0;
}

static int load_config_blob(const char *path, struct mp_ipc_config_blob *blob) {
    memset(blob, 0, sizeof(*blob));
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > MP_IPC_CONFIG_MAX_BYTES) { close(fd); return -1; }
    int read_ok = mp_read_full(fd, blob->data, (size_t)st.st_size) == 0;
    close(fd);
    if (!read_ok) return -1;
    blob->length = (uint32_t)st.st_size;
    return 0;
}

static enum MHD_Result restore_backup_locked(struct MHD_Connection *connection,
                                             struct request_context *context) {
    if (core_alarm_active())
        return queue_json(connection, 409,
            "{\"ok\":false,\"error\":\"Press the touch sensor to stop the alarm before restoring a backup\"}");
    if (context->upload_count != 1 || !context->uploads[0].temp_path[0])
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"choose one mk-piclock backup ZIP\"}");
    char stage_template[] = "/tmp/mk-piclock-restore-XXXXXX";
    char *stage = mkdtemp(stage_template);
    if (!stage) return queue_json(connection, 500, "{\"ok\":false,\"error\":\"restore staging could not be created\"}");
    int extracted = 0;
    if (extract_backup_archive(context->uploads[0].temp_path, stage, &extracted) != 0) {
        remove_tree(stage);
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"backup ZIP is invalid or unsupported\"}");
    }
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/config/clock.conf", stage);
    struct mp_ipc_config_blob *blob = calloc(1, sizeof(*blob));
    if (!blob || load_config_blob(config_path, blob) != 0) {
        free(blob);
        remove_tree(stage);
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"backup configuration is missing\"}");
    }
    struct ipc_result ping;
    if (ipc_call(MP_IPC_OP_PING, NULL, 0, &ping) != 0 || ping.status != 200) {
        ipc_result_free(&ping);
        free(blob);
        remove_tree(stage);
        return queue_json(connection, 503, "{\"ok\":false,\"error\":\"clock core is unavailable\"}");
    }
    ipc_result_free(&ping);

    char rollback_root[PATH_MAX] = "";
    if (stage_asset_set(rollback_root, sizeof(rollback_root), RESTORE_ASSETS,
                        sizeof(RESTORE_ASSETS) / sizeof(RESTORE_ASSETS[0])) != 0) {
        free(blob);
        remove_tree(stage);
        return queue_json(connection, 500,
            "{\"ok\":false,\"error\":\"current files could not be protected before restore\"}");
    }
    int copy_ok = copy_staged_directory(stage, "assets/images", MP_IMAGE_DIR) == 0 &&
                  copy_staged_directory(stage, "assets/bedtime-images", MP_BEDTIME_IMAGE_DIR) == 0 &&
                  copy_staged_directory(stage, "assets/fonts", MP_FONT_DIR) == 0;
    struct ipc_result imported;
    memset(&imported, 0, sizeof(imported));
    int import_ok = copy_ok && ipc_call(MP_IPC_OP_CONFIG_IMPORT, blob, sizeof(*blob), &imported) == 0 &&
                    imported.status >= 200 && imported.status < 300;
    ipc_result_free(&imported);
    free(blob);
    remove_tree(stage);
    if (!import_ok) {
        restore_asset_set(rollback_root, RESTORE_ASSETS,
                          sizeof(RESTORE_ASSETS) / sizeof(RESTORE_ASSETS[0]));
        return queue_json(connection, 500,
            "{\"ok\":false,\"error\":\"restore failed; the previous user files were restored\"}");
    }
    remove_tree(rollback_root);
    char response[160];
    snprintf(response, sizeof(response), "{\"ok\":true,\"restored_files\":%d}", extracted);
    return queue_json(connection, 200, response);
}

static enum MHD_Result restore_backup(struct MHD_Connection *connection,
                                      struct request_context *context) {
    if (pthread_mutex_trylock(&g_maintenance_lock) != 0)
        return queue_json(connection, 409,
            "{\"ok\":false,\"error\":\"another backup or maintenance task is running\"}");
    enum MHD_Result result = restore_backup_locked(connection, context);
    pthread_mutex_unlock(&g_maintenance_lock);
    return result;
}

static int core_alarm_active(void) {
    struct ipc_result status;
    if (ipc_call(MP_IPC_OP_STATUS, NULL, 0, &status) != 0) return 0;
    int active = 0;
    if (status.body && status.body_len < MP_IPC_MAX_PAYLOAD) {
        char *json = malloc(status.body_len + 1);
        if (json) {
            memcpy(json, status.body, status.body_len);
            json[status.body_len] = '\0';
            active = json_integer_value(json, "alarm_active", 0);
            free(json);
        }
    }
    ipc_result_free(&status);
    return active;
}

static enum MHD_Result factory_reset_locked(struct MHD_Connection *connection,
                                            const struct request_context *context) {
    if (!form_value(context, "confirm") || strcmp(form_value(context, "confirm"), "RESET") != 0)
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"type RESET to confirm\"}");
    if (core_alarm_active())
        return queue_json(connection, 409,
            "{\"ok\":false,\"error\":\"Press the touch sensor to stop the alarm before resetting\"}");
    (void)mp_music_jobs_clear_queued();
    if (mp_music_jobs_active() > 0)
        return queue_json(connection, 409,
            "{\"ok\":false,\"error\":\"Wait for music processing to finish before resetting\"}");
    char rollback_root[PATH_MAX] = "";
    if (stage_asset_set(rollback_root, sizeof(rollback_root), RESET_ASSETS,
                        sizeof(RESET_ASSETS) / sizeof(RESET_ASSETS[0])) != 0)
        return queue_json(connection, 500,
            "{\"ok\":false,\"error\":\"current files could not be protected before reset\"}");
    struct ipc_result result;
    if (ipc_call(MP_IPC_OP_FACTORY_RESET, NULL, 0, &result) != 0 ||
        result.status < 200 || result.status >= 300) {
        ipc_result_free(&result);
        restore_asset_set(rollback_root, RESET_ASSETS,
                          sizeof(RESET_ASSETS) / sizeof(RESET_ASSETS[0]));
        return queue_json(connection, 503, "{\"ok\":false,\"error\":\"clock core could not reset\"}");
    }
    ipc_result_free(&result);
    remove_tree(rollback_root);
    (void)notify_asset(MP_IPC_ASSET_IMAGE, MP_IPC_ASSET_DELETED_ALL, 0, "");
    (void)notify_asset(MP_IPC_ASSET_BEDTIME_IMAGE, MP_IPC_ASSET_DELETED_ALL, 0, "");
    (void)notify_asset(MP_IPC_ASSET_MUSIC, MP_IPC_ASSET_DELETED_ALL, 0, "");
    (void)notify_asset(MP_IPC_ASSET_STORY, MP_IPC_ASSET_DELETED_ALL, 0, "");
    (void)notify_asset(MP_IPC_ASSET_FONT, MP_IPC_ASSET_DELETED_ALL, 0, "");
    (void)write_web_password("");
    return queue_json_with_cookie(connection, 200, "{\"ok\":true}", clear_auth_cookie());
}

static enum MHD_Result factory_reset(struct MHD_Connection *connection,
                                     const struct request_context *context) {
    if (pthread_mutex_trylock(&g_maintenance_lock) != 0)
        return queue_json(connection, 409,
            "{\"ok\":false,\"error\":\"another backup or maintenance task is running\"}");
    enum MHD_Result result = factory_reset_locked(connection, context);
    pthread_mutex_unlock(&g_maintenance_lock);
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
        enum MHD_Result result = queue_file(connection, full, NULL, 0,
                                            0);
        return result == MHD_NO ? queue_json(connection, 404, "{\"ok\":false,\"error\":\"asset not found\"}") : result;
    }
    if (strcmp(path, "/favicon.ico") == 0) {
        *handled = 1;
        snprintf(full, sizeof(full), "%s/favicon.ico", WEB_DIR);
        enum MHD_Result result = queue_file(connection, full, "image/x-icon", 0, 0);
        return result == MHD_NO ? queue_json(connection, 404, "{\"ok\":false,\"error\":\"favicon not found\"}") : result;
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

static int notify_processed_music(const char *file, void *userdata) {
    (void)userdata;
    return notify_asset(MP_IPC_ASSET_MUSIC, MP_IPC_ASSET_UPLOADED, 1, file);
}


struct diagnostic_info {
    char hostname[256];
    char interface_name[IFNAMSIZ];
    char ip_address[INET_ADDRSTRLEN];
    char ssid[IW_ESSID_MAX_SIZE + 1];
    int wifi_signal_percent;
    int wifi_signal_dbm;
    int wifi_signal_available;
    int ntp_synchronized;
    int system_time_valid;
    double cpu_temperature_c;
    uint64_t storage_free_bytes;
    uint64_t storage_used_bytes;
    uint64_t storage_total_bytes;
    uint64_t day_images_bytes;
    uint64_t bedtime_images_bytes;
    uint64_t music_bytes;
    uint64_t stories_bytes;
    uint64_t fonts_bytes;
    uint64_t day_images_files;
    uint64_t bedtime_images_files;
    uint64_t music_files;
    uint64_t stories_files;
    uint64_t fonts_files;
    int api_healthy;
    int core_healthy;
    int oled_ok;
    int touch_ok;
    int led_ok;
    int led_enabled;
    int led_max_brightness;
    int led_red_gain;
    int led_green_gain;
    int led_blue_gain;
    int led_idle_off;
    int led_bedtime_fade_minutes;
    int led_touch_blink_enabled;
    int led_touch_blink_brightness;
    int led_pwm_hz;
    int led_pwm_levels;
    int led_write_errors;
    int led_common_cathode;
    int led_gpio_red;
    int led_gpio_green;
    int led_gpio_blue;
    char led_scene[32];
    char led_colour[16];
    char led_output[16];
    char led_touch_blink_color[16];
    long long last_successful_alarm;
    char next_alarm_text[128];

    char os_pretty_name[256];
    char os_version_id[64];
    char os_codename[64];
    char kernel_release[128];
    char architecture[64];
    char hardware_model[256];
    char pi_serial[64];
    char board_revision[64];
    char machine_id[64];
    char inventory_id[32];
    char cpu_signature[192];
    uint64_t uptime_seconds;

    char root_device[128];
    char root_disk[128];
    char root_filesystem[64];
    char root_mount_options[256];
    int root_read_only;
    char boot_device[128];
    char boot_filesystem[64];
    char boot_mount_point[64];

    int sd_present;
    char sd_device[128];
    char sd_type[32];
    char sd_name[128];
    char sd_manufacturer_id[64];
    char sd_oem_id[64];
    char sd_serial[64];
    char sd_manufacture_date[64];
    char sd_cid[128];
    uint64_t sd_capacity_bytes;
};

static int json_integer_value(const char *json, const char *key, int fallback) {
    if (!json || !key) return fallback;
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *found = strstr(json, pattern);
    if (!found) return fallback;
    found += strlen(pattern);
    char *end = NULL;
    errno = 0;
    long long value = strtoll(found, &end, 10);
    if (errno || end == found || value < INT_MIN || value > INT_MAX) return fallback;
    return (int)value;
}

static long long json_long_long_value(const char *json, const char *key, long long fallback) {
    if (!json || !key) return fallback;
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *found = strstr(json, pattern);
    if (!found) return fallback;
    found += strlen(pattern);
    char *end = NULL;
    errno = 0;
    long long value = strtoll(found, &end, 10);
    return errno || end == found ? fallback : value;
}

static void json_string_value(const char *json, const char *key, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!json || !key) return;
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *found = strstr(json, pattern);
    if (!found) return;
    found += strlen(pattern);
    size_t used = 0;
    while (*found && *found != '"' && used + 1 < out_len) {
        if (*found == '\\' && found[1]) {
            found++;
            if (*found == 'n') out[used++] = '\n';
            else if (*found == 'r') out[used++] = '\r';
            else if (*found == 't') out[used++] = '\t';
            else out[used++] = *found;
            found++;
            continue;
        }
        out[used++] = *found++;
    }
    out[used] = '\0';
}

static void trim_diagnostic_text(char *text) {
    if (!text) return;
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r' ||
                          text[length - 1] == ' ' || text[length - 1] == '\t' ||
                          text[length - 1] == '\0')) {
        text[--length] = '\0';
    }
    size_t start = 0;
    while (text[start] == ' ' || text[start] == '\t') start++;
    if (start) memmove(text, text + start, strlen(text + start) + 1);
}

static int read_diagnostic_file(const char *path, char *out, size_t out_len) {
    if (!path || !out || out_len < 2) return -1;
    out[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t got = read(fd, out, out_len - 1);
    int saved = errno;
    close(fd);
    if (got < 0) {
        errno = saved;
        return -1;
    }
    out[got] = '\0';
    for (ssize_t i = 0; i < got; i++) {
        if (out[i] == '\0') out[i] = ' ';
    }
    trim_diagnostic_text(out);
    return out[0] ? 0 : -1;
}

static int read_os_release_value(const char *key, char *out, size_t out_len) {
    if (!key || !out || out_len == 0) return -1;
    out[0] = '\0';
    FILE *file = fopen("/etc/os-release", "r");
    if (!file) return -1;
    char line[512];
    int found = 0;
    size_t key_len = strlen(key);
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') continue;
        char *value = line + key_len + 1;
        trim_diagnostic_text(value);
        size_t length = strlen(value);
        if (length >= 2 && ((value[0] == '"' && value[length - 1] == '"') ||
                            (value[0] == '\'' && value[length - 1] == '\''))) {
            value[length - 1] = '\0';
            value++;
        }
        mp_safe_str(out, out_len, value);
        found = out[0] != '\0';
        break;
    }
    fclose(file);
    return found ? 0 : -1;
}


static int read_cpuinfo_value(const char *key, char *out, size_t out_len) {
    if (!key || !out || out_len == 0) return -1;
    out[0] = '\0';
    FILE *file = fopen("/proc/cpuinfo", "r");
    if (!file) return -1;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        trim_diagnostic_text(line);
        if (strcmp(line, key) != 0) continue;
        char *value = colon + 1;
        trim_diagnostic_text(value);
        mp_safe_str(out, out_len, value);
        found = out[0] != '\0';
        break;
    }
    fclose(file);
    return found ? 0 : -1;
}

static void normalize_pi_serial(char *serial) {
    if (!serial) return;
    trim_diagnostic_text(serial);
    if (serial[0] == '0' && (serial[1] == 'x' || serial[1] == 'X'))
        memmove(serial, serial + 2, strlen(serial + 2) + 1);
}

static void build_inventory_id(const char *serial, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!serial || !serial[0]) return;
    char compact[64];
    size_t used = 0;
    for (const unsigned char *cursor = (const unsigned char *)serial;
         *cursor && used + 1 < sizeof(compact); cursor++) {
        if (isalnum(*cursor)) compact[used++] = (char)toupper(*cursor);
    }
    compact[used] = '\0';
    if (used >= 8) {
        const char *tail = compact + used - 8;
        snprintf(out, out_len, "MK-%.4s-%.4s", tail, tail + 4);
    } else if (used > 0) {
        static const char prefix[] = "MK-";
        const size_t prefix_len = sizeof(prefix) - 1;
        if (out_len <= prefix_len) {
            memcpy(out, prefix, out_len - 1);
            out[out_len - 1] = '\0';
            return;
        }
        size_t copy_len = used;
        const size_t available = out_len - prefix_len - 1;
        if (copy_len > available) copy_len = available;
        memcpy(out, prefix, prefix_len);
        memcpy(out + prefix_len, compact, copy_len);
        out[prefix_len + copy_len] = '\0';
    }
}

static void read_pi_identity(struct diagnostic_info *info) {
    if (read_diagnostic_file("/sys/firmware/devicetree/base/serial-number",
                             info->pi_serial, sizeof(info->pi_serial)) != 0 &&
        read_diagnostic_file("/proc/device-tree/serial-number",
                             info->pi_serial, sizeof(info->pi_serial)) != 0) {
        (void)read_cpuinfo_value("Serial", info->pi_serial, sizeof(info->pi_serial));
    }
    normalize_pi_serial(info->pi_serial);
    (void)read_cpuinfo_value("Revision", info->board_revision, sizeof(info->board_revision));
    (void)read_diagnostic_file("/etc/machine-id", info->machine_id, sizeof(info->machine_id));
    build_inventory_id(info->pi_serial, info->inventory_id, sizeof(info->inventory_id));

    char implementer[32] = "";
    char cpu_architecture[32] = "";
    char variant[32] = "";
    char part[32] = "";
    char revision[32] = "";
    (void)read_cpuinfo_value("CPU implementer", implementer, sizeof(implementer));
    (void)read_cpuinfo_value("CPU architecture", cpu_architecture, sizeof(cpu_architecture));
    (void)read_cpuinfo_value("CPU variant", variant, sizeof(variant));
    (void)read_cpuinfo_value("CPU part", part, sizeof(part));
    (void)read_cpuinfo_value("CPU revision", revision, sizeof(revision));
    if (implementer[0] || cpu_architecture[0] || variant[0] || part[0] || revision[0]) {
        snprintf(info->cpu_signature, sizeof(info->cpu_signature),
                 "implementer %s / architecture %s / variant %s / part %s / revision %s",
                 implementer[0] ? implementer : "unknown",
                 cpu_architecture[0] ? cpu_architecture : "unknown",
                 variant[0] ? variant : "unknown",
                 part[0] ? part : "unknown",
                 revision[0] ? revision : "unknown");
    }
}

static int option_list_contains(const char *options, const char *wanted) {
    if (!options || !wanted) return 0;
    size_t wanted_len = strlen(wanted);
    const char *cursor = options;
    while (*cursor) {
        while (*cursor == ',') cursor++;
        const char *end = strchr(cursor, ',');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length == wanted_len && strncmp(cursor, wanted, length) == 0) return 1;
        if (!end) break;
        cursor = end + 1;
    }
    return 0;
}

static int read_mount_details(const char *mount_point, char *device, size_t device_len,
                              char *filesystem, size_t filesystem_len,
                              char *options, size_t options_len) {
    FILE *mounts = setmntent("/proc/self/mounts", "r");
    if (!mounts) return -1;
    struct mntent entry;
    char buffer[4096];
    int found = 0;
    while (getmntent_r(mounts, &entry, buffer, sizeof(buffer))) {
        if (strcmp(entry.mnt_dir, mount_point) != 0) continue;
        mp_safe_str(device, device_len, entry.mnt_fsname);
        mp_safe_str(filesystem, filesystem_len, entry.mnt_type);
        mp_safe_str(options, options_len, entry.mnt_opts);
        found = 1;
        break;
    }
    endmntent(mounts);
    return found ? 0 : -1;
}

static int resolve_mount_block_device(const char *mount_point, char *out, size_t out_len) {
    if (!mount_point || !out || out_len == 0) return -1;
    out[0] = '\0';
    struct stat state;
    if (stat(mount_point, &state) != 0) return -1;
    char sys_path[128];
    snprintf(sys_path, sizeof(sys_path), "/sys/dev/block/%u:%u",
             major(state.st_dev), minor(state.st_dev));
    char target[PATH_MAX];
    ssize_t length = readlink(sys_path, target, sizeof(target) - 1);
    if (length <= 0) return -1;
    target[length] = '\0';
    const char *name = strrchr(target, '/');
    name = name ? name + 1 : target;
    if (!name[0]) return -1;
    size_t name_len = strlen(name);
    if (out_len < 6 || name_len > out_len - 6) return -1;
    memcpy(out, "/dev/", 5);
    memcpy(out + 5, name, name_len + 1);
    return 0;
}

static void parent_block_device(const char *device, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!device || strncmp(device, "/dev/", 5) != 0) return;
    char name[128];
    mp_safe_str(name, sizeof(name), device + 5);
    size_t length = strlen(name);
    if (strncmp(name, "mmcblk", 6) == 0 || strncmp(name, "nvme", 4) == 0) {
        char *partition = strrchr(name, 'p');
        if (partition && partition[1] && strspn(partition + 1, "0123456789") == strlen(partition + 1))
            *partition = '\0';
    } else {
        while (length > 0 && isdigit((unsigned char)name[length - 1])) name[--length] = '\0';
    }
    if (!name[0]) return;
    snprintf(out, out_len, "/dev/%s", name);
}

static int read_default_interface(char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    FILE *routes = fopen("/proc/net/route", "r");
    if (!routes) return -1;
    char line[512];
    (void)fgets(line, sizeof(line), routes);
    while (fgets(line, sizeof(line), routes)) {
        char name[IFNAMSIZ] = "";
        unsigned long destination = 1;
        unsigned long gateway = 0;
        unsigned int flags = 0;
        if (sscanf(line, "%15s %lx %lx %X", name, &destination, &gateway, &flags) >= 4 &&
            destination == 0 && (flags & 0x1U)) {
            mp_safe_str(out, out_len, name);
            break;
        }
    }
    fclose(routes);
    return out[0] ? 0 : -1;
}

static void find_primary_ipv4(struct diagnostic_info *info) {
    char preferred[IFNAMSIZ] = "";
    (void)read_default_interface(preferred, sizeof(preferred));

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct ifreq interfaces[32];
    struct ifconf list;
    memset(&interfaces, 0, sizeof(interfaces));
    memset(&list, 0, sizeof(list));
    list.ifc_len = sizeof(interfaces);
    list.ifc_req = interfaces;
    if (ioctl(fd, SIOCGIFCONF, &list) != 0) {
        close(fd);
        return;
    }

    int best_score = -1;
    size_t count = (size_t)list.ifc_len / sizeof(struct ifreq);
    for (size_t i = 0; i < count; i++) {
        struct ifreq *candidate = &interfaces[i];
        if (candidate->ifr_addr.sa_family != AF_INET) continue;
        struct ifreq flags_request;
        memset(&flags_request, 0, sizeof(flags_request));
        mp_safe_str(flags_request.ifr_name, sizeof(flags_request.ifr_name), candidate->ifr_name);
        if (ioctl(fd, SIOCGIFFLAGS, &flags_request) != 0) continue;
        if (!(flags_request.ifr_flags & IFF_UP) || (flags_request.ifr_flags & IFF_LOOPBACK)) continue;

        struct sockaddr_in *address = (struct sockaddr_in *)&candidate->ifr_addr;
        char text[INET_ADDRSTRLEN] = "";
        if (!inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) continue;
        int score = 10;
        if (preferred[0] && strcmp(candidate->ifr_name, preferred) == 0) score = 100;
        else if (strncmp(candidate->ifr_name, "wl", 2) == 0) score = 70;
        else if (strncmp(candidate->ifr_name, "en", 2) == 0 || strncmp(candidate->ifr_name, "eth", 3) == 0) score = 50;
        if (score <= best_score) continue;
        best_score = score;
        mp_safe_str(info->interface_name, sizeof(info->interface_name), candidate->ifr_name);
        mp_safe_str(info->ip_address, sizeof(info->ip_address), text);
    }
    close(fd);
}

static void find_wireless_interface(struct diagnostic_info *info) {
    FILE *wireless = fopen("/proc/net/wireless", "r");
    if (!wireless) return;
    char line[512];
    while (fgets(line, sizeof(line), wireless)) {
        char name[IFNAMSIZ] = "";
        if (sscanf(line, " %15[^:]:", name) == 1) {
            if (!info->interface_name[0])
                mp_safe_str(info->interface_name, sizeof(info->interface_name), name);
            break;
        }
    }
    fclose(wireless);
}

static void read_wifi_details(struct diagnostic_info *info) {
    if (!info->interface_name[0]) return;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        struct iwreq request;
        char essid[IW_ESSID_MAX_SIZE + 1];
        memset(&request, 0, sizeof(request));
        memset(essid, 0, sizeof(essid));
        mp_safe_str(request.ifr_name, sizeof(request.ifr_name), info->interface_name);
        request.u.essid.pointer = essid;
        request.u.essid.length = IW_ESSID_MAX_SIZE;
        request.u.essid.flags = 0;
        if (ioctl(fd, SIOCGIWESSID, &request) == 0) {
            size_t length = request.u.essid.length;
            if (length > IW_ESSID_MAX_SIZE) length = IW_ESSID_MAX_SIZE;
            essid[length] = '\0';
            mp_safe_str(info->ssid, sizeof(info->ssid), essid);
        }
        close(fd);
    }

    FILE *wireless = fopen("/proc/net/wireless", "r");
    if (!wireless) return;
    char line[512];
    while (fgets(line, sizeof(line), wireless)) {
        char name[IFNAMSIZ] = "";
        double quality = 0.0;
        double level = 0.0;
        if (sscanf(line, " %15[^:]: %*d %lf %lf", name, &quality, &level) == 3 &&
            strcmp(name, info->interface_name) == 0) {
            int percent = (int)((quality / 70.0) * 100.0 + 0.5);
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;
            int dbm = (int)level;
            if (dbm > 100) dbm -= 256;
            info->wifi_signal_percent = percent;
            info->wifi_signal_dbm = dbm;
            info->wifi_signal_available = 1;
            break;
        }
    }
    fclose(wireless);
}

static int is_primary_mmc_name(const char *name) {
    if (!name || strncmp(name, "mmcblk", 6) != 0) return 0;
    const char *cursor = name + 6;
    if (!isdigit((unsigned char)*cursor)) return 0;
    while (isdigit((unsigned char)*cursor)) cursor++;
    return *cursor == '\0';
}

static int select_sd_block_device(const struct diagnostic_info *info, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    if (strncmp(info->root_disk, "/dev/mmcblk", 11) == 0) {
        mp_safe_str(out, out_len, info->root_disk + 5);
        return 0;
    }
    DIR *directory = opendir("/sys/class/block");
    if (!directory) return -1;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!is_primary_mmc_name(entry->d_name)) continue;
        mp_safe_str(out, out_len, entry->d_name);
        break;
    }
    closedir(directory);
    return out[0] ? 0 : -1;
}

static uint64_t read_uint64_file(const char *path) {
    char text[64];
    if (read_diagnostic_file(path, text, sizeof(text)) != 0) return 0;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    return errno || end == text ? 0 : (uint64_t)value;
}

static void directory_usage_fd(int fd, unsigned int depth,
                               uint64_t *bytes, uint64_t *files) {
    if (fd < 0 || !bytes || !files || depth > 32) {
        if (fd >= 0) close(fd);
        return;
    }

    DIR *directory = fdopendir(fd);
    if (!directory) {
        close(fd);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        struct stat state;
        if (fstatat(dirfd(directory), entry->d_name, &state, AT_SYMLINK_NOFOLLOW) != 0) continue;

        if (S_ISREG(state.st_mode)) {
            if (state.st_size > 0 && (uint64_t)state.st_size <= UINT64_MAX - *bytes)
                *bytes += (uint64_t)state.st_size;
            if (*files < UINT64_MAX) (*files)++;
        } else if (S_ISDIR(state.st_mode) && depth < 32) {
            int child_fd = openat(dirfd(directory), entry->d_name,
                                  O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
            if (child_fd >= 0) directory_usage_fd(child_fd, depth + 1, bytes, files);
        }
    }

    closedir(directory);
}

static void directory_usage_recursive(const char *path, unsigned int depth,
                                      uint64_t *bytes, uint64_t *files) {
    if (!path || !bytes || !files || depth > 32) return;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd >= 0) directory_usage_fd(fd, depth, bytes, files);
}

static void read_asset_usage(struct diagnostic_info *info) {
    directory_usage_recursive(MP_IMAGE_DIR, 0, &info->day_images_bytes, &info->day_images_files);
    directory_usage_recursive(MP_BEDTIME_IMAGE_DIR, 0,
                              &info->bedtime_images_bytes, &info->bedtime_images_files);
    directory_usage_recursive(MP_MUSIC_DIR, 0, &info->music_bytes, &info->music_files);
    directory_usage_recursive(MP_STORY_DIR, 0, &info->stories_bytes, &info->stories_files);
    directory_usage_recursive(MP_FONT_DIR, 0, &info->fonts_bytes, &info->fonts_files);
}

static void read_platform_details(struct diagnostic_info *info) {
    (void)read_os_release_value("PRETTY_NAME", info->os_pretty_name, sizeof(info->os_pretty_name));
    (void)read_os_release_value("VERSION_ID", info->os_version_id, sizeof(info->os_version_id));
    (void)read_os_release_value("VERSION_CODENAME", info->os_codename, sizeof(info->os_codename));

    struct utsname identity;
    if (uname(&identity) == 0) {
        mp_safe_str(info->kernel_release, sizeof(info->kernel_release), identity.release);
        mp_safe_str(info->architecture, sizeof(info->architecture), identity.machine);
    }
    (void)read_diagnostic_file("/proc/device-tree/model", info->hardware_model, sizeof(info->hardware_model));
    if (!info->hardware_model[0])
        (void)read_diagnostic_file("/sys/firmware/devicetree/base/model", info->hardware_model, sizeof(info->hardware_model));
    read_pi_identity(info);

    char uptime[128];
    if (read_diagnostic_file("/proc/uptime", uptime, sizeof(uptime)) == 0) {
        char *end = NULL;
        double seconds = strtod(uptime, &end);
        if (end != uptime && seconds > 0.0) info->uptime_seconds = (uint64_t)seconds;
    }
}

static void read_storage_details(struct diagnostic_info *info) {
    char options[256] = "";
    if (read_mount_details("/", info->root_device, sizeof(info->root_device),
                           info->root_filesystem, sizeof(info->root_filesystem),
                           options, sizeof(options)) == 0) {
        mp_safe_str(info->root_mount_options, sizeof(info->root_mount_options), options);
        info->root_read_only = option_list_contains(options, "ro");
        char resolved_device[128] = "";
        if ((strncmp(info->root_device, "/dev/", 5) != 0 || strcmp(info->root_device, "/dev/root") == 0) &&
            resolve_mount_block_device("/", resolved_device, sizeof(resolved_device)) == 0)
            mp_safe_str(info->root_device, sizeof(info->root_device), resolved_device);
        parent_block_device(info->root_device, info->root_disk, sizeof(info->root_disk));
    }

    char boot_options[256] = "";
    if (read_mount_details("/boot/firmware", info->boot_device, sizeof(info->boot_device),
                           info->boot_filesystem, sizeof(info->boot_filesystem),
                           boot_options, sizeof(boot_options)) == 0) {
        mp_safe_str(info->boot_mount_point, sizeof(info->boot_mount_point), "/boot/firmware");
    } else if (read_mount_details("/boot", info->boot_device, sizeof(info->boot_device),
                                  info->boot_filesystem, sizeof(info->boot_filesystem),
                                  boot_options, sizeof(boot_options)) == 0) {
        mp_safe_str(info->boot_mount_point, sizeof(info->boot_mount_point), "/boot");
    }

    char block_name[64] = "";
    if (select_sd_block_device(info, block_name, sizeof(block_name)) != 0) return;
    info->sd_present = 1;
    snprintf(info->sd_device, sizeof(info->sd_device), "/dev/%s", block_name);

    char path[PATH_MAX];
#define READ_SD_FIELD(field, target) do { \
    snprintf(path, sizeof(path), "/sys/class/block/%s/device/%s", block_name, field); \
    (void)read_diagnostic_file(path, target, sizeof(target)); \
} while (0)
    READ_SD_FIELD("type", info->sd_type);
    READ_SD_FIELD("name", info->sd_name);
    READ_SD_FIELD("manfid", info->sd_manufacturer_id);
    READ_SD_FIELD("oemid", info->sd_oem_id);
    READ_SD_FIELD("serial", info->sd_serial);
    READ_SD_FIELD("date", info->sd_manufacture_date);
    READ_SD_FIELD("cid", info->sd_cid);
#undef READ_SD_FIELD
    snprintf(path, sizeof(path), "/sys/class/block/%s/size", block_name);
    uint64_t sectors = read_uint64_file(path);
    if (sectors <= UINT64_MAX / 512ULL) info->sd_capacity_bytes = sectors * 512ULL;
}

static void read_core_diagnostics(struct diagnostic_info *info) {
    struct ipc_result ping;
    if (ipc_call(MP_IPC_OP_PING, NULL, 0, &ping) == 0) {
        info->core_healthy = ping.status >= 200 && ping.status < 300;
        ipc_result_free(&ping);
    }
    struct ipc_result status;
    if (ipc_call(MP_IPC_OP_STATUS, NULL, 0, &status) != 0) return;
    if (status.status == 200 && status.body && status.body_len < MP_IPC_MAX_PAYLOAD) {
        char *json = malloc(status.body_len + 1);
        if (json) {
            memcpy(json, status.body, status.body_len);
            json[status.body_len] = '\0';
            info->oled_ok = json_integer_value(json, "oled_ok", 0);
            info->touch_ok = json_integer_value(json, "touch_ok", 0);
            info->led_ok = json_integer_value(json, "led_ok", 0);
            info->led_enabled = json_integer_value(json, "led_enabled", 0);
            info->led_max_brightness = json_integer_value(json, "led_max_brightness", 0);
            info->led_red_gain = json_integer_value(json, "led_red_gain", 0);
            info->led_green_gain = json_integer_value(json, "led_green_gain", 0);
            info->led_blue_gain = json_integer_value(json, "led_blue_gain", 0);
            info->led_idle_off = json_integer_value(json, "led_idle_off", 0);
            info->led_bedtime_fade_minutes = json_integer_value(json, "led_bedtime_fade_minutes", 0);
            info->led_touch_blink_enabled = json_integer_value(json, "led_touch_blink_enabled", 0);
            info->led_touch_blink_brightness = json_integer_value(json, "led_touch_blink_brightness", 0);
            info->led_pwm_hz = json_integer_value(json, "led_pwm_hz", 0);
            info->led_pwm_levels = json_integer_value(json, "led_pwm_levels", 0);
            info->led_write_errors = json_integer_value(json, "led_write_errors", 0);
            info->led_common_cathode = json_integer_value(json, "led_common_cathode", 0);
            info->led_gpio_red = json_integer_value(json, "led_gpio_red", 0);
            info->led_gpio_green = json_integer_value(json, "led_gpio_green", 0);
            info->led_gpio_blue = json_integer_value(json, "led_gpio_blue", 0);
            json_string_value(json, "led_scene", info->led_scene, sizeof(info->led_scene));
            json_string_value(json, "led_colour", info->led_colour, sizeof(info->led_colour));
            json_string_value(json, "led_output", info->led_output, sizeof(info->led_output));
            json_string_value(json, "led_touch_blink_color", info->led_touch_blink_color,
                              sizeof(info->led_touch_blink_color));
            info->last_successful_alarm = json_long_long_value(json, "last_successful_alarm", 0);
            json_string_value(json, "next_alarm_text", info->next_alarm_text, sizeof(info->next_alarm_text));
            free(json);
        }
    }
    ipc_result_free(&status);
}

static void collect_diagnostics(struct diagnostic_info *info) {
    memset(info, 0, sizeof(*info));
    info->api_healthy = 1;
    if (gethostname(info->hostname, sizeof(info->hostname) - 1) != 0)
        mp_safe_str(info->hostname, sizeof(info->hostname), "unknown");
    find_primary_ipv4(info);
    find_wireless_interface(info);
    read_wifi_details(info);

    struct timex clock_state;
    memset(&clock_state, 0, sizeof(clock_state));
    int time_state = adjtimex(&clock_state);
    info->ntp_synchronized = time_state != TIME_ERROR && !(clock_state.status & STA_UNSYNC);
    time_t now = time(NULL);
    struct tm tmv;
    if (localtime_r(&now, &tmv)) info->system_time_valid = tmv.tm_year + 1900 >= 2024;

    FILE *temperature = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    long millidegrees = 0;
    if (temperature) {
        if (fscanf(temperature, "%ld", &millidegrees) == 1)
            info->cpu_temperature_c = (double)millidegrees / 1000.0;
        fclose(temperature);
    }
    struct statvfs storage;
    if (statvfs("/", &storage) == 0) {
        uint64_t block_size = (uint64_t)storage.f_frsize;
        uint64_t total_blocks = (uint64_t)storage.f_blocks;
        uint64_t free_blocks = (uint64_t)storage.f_bfree;
        info->storage_free_bytes = (uint64_t)storage.f_bavail * block_size;
        info->storage_used_bytes = total_blocks >= free_blocks ? (total_blocks - free_blocks) * block_size : 0;
        info->storage_total_bytes = total_blocks * block_size;
    }
    read_asset_usage(info);
    read_platform_details(info);
    read_storage_details(info);
    read_core_diagnostics(info);
}

static void append_json_string_field(struct mp_buffer *body, const char *name, const char *value) {
    mp_buffer_appendf(body, ",\"%s\":\"", name);
    mp_buffer_append_json_string(body, value ? value : "");
    mp_buffer_append(body, "\"");
}

static enum MHD_Result serve_diagnostics(struct MHD_Connection *connection) {
    struct diagnostic_info info;
    collect_diagnostics(&info);
    struct mp_buffer body;
    if (mp_buffer_init(&body, 4096, 32768) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    mp_buffer_append(&body, "{\"ok\":true");
    append_json_string_field(&body, "product_version", PRODUCT_VERSION);
    append_json_string_field(&body, "api_version", API_VERSION);
    append_json_string_field(&body, "compiled_at", BUILD_TIMESTAMP);
    append_json_string_field(&body, "hostname", info.hostname);
    append_json_string_field(&body, "interface", info.interface_name);
    append_json_string_field(&body, "ip_address", info.ip_address);
    append_json_string_field(&body, "ssid", info.ssid);
    mp_buffer_appendf(&body,
        ",\"wifi_signal_percent\":%d,\"wifi_signal_dbm\":%d,\"wifi_signal_available\":%d"
        ",\"ntp_synchronized\":%d,\"system_time_valid\":%d"
        ",\"cpu_temperature_c\":%.1f,\"storage_free_bytes\":%llu,\"storage_used_bytes\":%llu"
        ",\"storage_total_bytes\":%llu,\"day_images_bytes\":%llu,\"bedtime_images_bytes\":%llu"
        ",\"music_bytes\":%llu,\"stories_bytes\":%llu,\"fonts_bytes\":%llu,\"day_images_files\":%llu"
        ",\"bedtime_images_files\":%llu,\"music_files\":%llu,\"stories_files\":%llu,\"fonts_files\":%llu"
        ",\"api_healthy\":%d,\"core_healthy\":%d,\"oled_ok\":%d,\"touch_ok\":%d"
        ",\"led_ok\":%d,\"led_enabled\":%d,\"led_max_brightness\":%d"
        ",\"led_red_gain\":%d,\"led_green_gain\":%d,\"led_blue_gain\":%d"
        ",\"led_idle_off\":%d,\"led_bedtime_fade_minutes\":%d"
        ",\"led_touch_blink_enabled\":%d,\"led_touch_blink_brightness\":%d"
        ",\"led_pwm_hz\":%d,\"led_pwm_levels\":%d,\"led_write_errors\":%d"
        ",\"led_common_cathode\":%d,\"led_gpio_red\":%d,\"led_gpio_green\":%d,\"led_gpio_blue\":%d"
        ",\"last_successful_alarm\":%lld,\"uptime_seconds\":%llu",
        info.wifi_signal_percent, info.wifi_signal_dbm, info.wifi_signal_available,
        info.ntp_synchronized, info.system_time_valid, info.cpu_temperature_c,
        (unsigned long long)info.storage_free_bytes, (unsigned long long)info.storage_used_bytes,
        (unsigned long long)info.storage_total_bytes,
        (unsigned long long)info.day_images_bytes, (unsigned long long)info.bedtime_images_bytes,
        (unsigned long long)info.music_bytes, (unsigned long long)info.stories_bytes,
        (unsigned long long)info.fonts_bytes,
        (unsigned long long)info.day_images_files, (unsigned long long)info.bedtime_images_files,
        (unsigned long long)info.music_files, (unsigned long long)info.stories_files,
        (unsigned long long)info.fonts_files,
        info.api_healthy, info.core_healthy, info.oled_ok, info.touch_ok,
        info.led_ok, info.led_enabled, info.led_max_brightness,
        info.led_red_gain, info.led_green_gain, info.led_blue_gain,
        info.led_idle_off, info.led_bedtime_fade_minutes,
        info.led_touch_blink_enabled, info.led_touch_blink_brightness,
        info.led_pwm_hz, info.led_pwm_levels, info.led_write_errors,
        info.led_common_cathode, info.led_gpio_red, info.led_gpio_green, info.led_gpio_blue,
        info.last_successful_alarm, (unsigned long long)info.uptime_seconds);
    append_json_string_field(&body, "led_scene", info.led_scene);
    append_json_string_field(&body, "led_colour", info.led_colour);
    append_json_string_field(&body, "led_output", info.led_output);
    append_json_string_field(&body, "led_touch_blink_color", info.led_touch_blink_color);
    append_json_string_field(&body, "next_alarm_text", info.next_alarm_text);
    append_json_string_field(&body, "os_pretty_name", info.os_pretty_name);
    append_json_string_field(&body, "os_version_id", info.os_version_id);
    append_json_string_field(&body, "os_codename", info.os_codename);
    append_json_string_field(&body, "kernel_release", info.kernel_release);
    append_json_string_field(&body, "architecture", info.architecture);
    append_json_string_field(&body, "hardware_model", info.hardware_model);
    append_json_string_field(&body, "pi_serial", info.pi_serial);
    append_json_string_field(&body, "board_revision", info.board_revision);
    append_json_string_field(&body, "machine_id", info.machine_id);
    append_json_string_field(&body, "inventory_id", info.inventory_id);
    append_json_string_field(&body, "cpu_signature", info.cpu_signature);
    append_json_string_field(&body, "root_device", info.root_device);
    append_json_string_field(&body, "root_disk", info.root_disk);
    append_json_string_field(&body, "root_filesystem", info.root_filesystem);
    mp_buffer_appendf(&body, ",\"root_read_only\":%d", info.root_read_only);
    append_json_string_field(&body, "boot_device", info.boot_device);
    append_json_string_field(&body, "boot_filesystem", info.boot_filesystem);
    append_json_string_field(&body, "boot_mount_point", info.boot_mount_point);
    mp_buffer_appendf(&body, ",\"sd_present\":%d,\"sd_capacity_bytes\":%llu",
                      info.sd_present, (unsigned long long)info.sd_capacity_bytes);
    append_json_string_field(&body, "sd_device", info.sd_device);
    append_json_string_field(&body, "sd_type", info.sd_type);
    append_json_string_field(&body, "sd_name", info.sd_name);
    append_json_string_field(&body, "sd_manufacturer_id", info.sd_manufacturer_id);
    append_json_string_field(&body, "sd_oem_id", info.sd_oem_id);
    append_json_string_field(&body, "sd_serial", info.sd_serial);
    append_json_string_field(&body, "sd_manufacture_date", info.sd_manufacture_date);
    append_json_string_field(&body, "sd_cid", info.sd_cid);
    mp_buffer_append(&body, "}");
    return queue_json_builder(connection, 200, &body);
}


static enum MHD_Result queue_text_download(struct MHD_Connection *connection, char *body,
                                           size_t body_len, const char *filename) {
    struct MHD_Response *response = MHD_create_response_from_buffer(body_len, body, MHD_RESPMEM_MUST_FREE);
    if (!response) {
        free(body);
        return MHD_NO;
    }
    (void)add_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain; charset=utf-8");
    char disposition[256];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);
    (void)add_header(response, "Content-Disposition", disposition);
    add_api_headers(connection, response);
    enum MHD_Result result = MHD_queue_response(connection, 200, response);
    MHD_destroy_response(response);
    return result;
}

static enum MHD_Result serve_diagnostic_report(struct MHD_Connection *connection) {
    struct diagnostic_info info;
    collect_diagnostics(&info);
    struct mp_buffer report;
    if (mp_buffer_init(&report, 4096, 65536) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    time_t now = time(NULL);
    char generated[64];
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(generated, sizeof(generated), "%Y-%m-%d %H:%M:%S %Z", &tmv);
    char last_alarm[64] = "Never";
    if (info.last_successful_alarm > 0) {
        time_t alarm_time = (time_t)info.last_successful_alarm;
        struct tm alarm_tm;
        localtime_r(&alarm_time, &alarm_tm);
        strftime(last_alarm, sizeof(last_alarm), "%Y-%m-%d %H:%M:%S %Z", &alarm_tm);
    }
    mp_buffer_appendf(&report,
        "mk-piclock Diagnostic Report\nGenerated: %s\nSoftware: %s\nAPI: %s\nCompiled: %s\n\n"
        "Platform\nHardware: %s\nOperating system: %s\nOS version: %s\nOS codename: %s\n"
        "Kernel: %s\nArchitecture: %s\nUptime: %llu seconds\n\n"
        "Device identity\nInventory ID: %s\nRaspberry Pi serial: %s\nBoard revision: %s\n"
        "OS machine ID: %s\nCPU signature: %s\n\n"
        "Network\nHostname: %s\nInterface: %s\nIP address: %s\nSSID: %s\n",
        generated, PRODUCT_VERSION, API_VERSION, BUILD_TIMESTAMP,
        info.hardware_model[0] ? info.hardware_model : "Unavailable",
        info.os_pretty_name[0] ? info.os_pretty_name : "Unavailable",
        info.os_version_id[0] ? info.os_version_id : "Unavailable",
        info.os_codename[0] ? info.os_codename : "Unavailable",
        info.kernel_release[0] ? info.kernel_release : "Unavailable",
        info.architecture[0] ? info.architecture : "Unavailable",
        (unsigned long long)info.uptime_seconds,
        info.inventory_id[0] ? info.inventory_id : "Unavailable",
        info.pi_serial[0] ? info.pi_serial : "Unavailable",
        info.board_revision[0] ? info.board_revision : "Unavailable",
        info.machine_id[0] ? info.machine_id : "Unavailable",
        info.cpu_signature[0] ? info.cpu_signature : "Unavailable",
        info.hostname, info.interface_name[0] ? info.interface_name : "Unavailable",
        info.ip_address[0] ? info.ip_address : "Unavailable",
        info.ssid[0] ? info.ssid : "Unavailable");
    if (info.wifi_signal_available)
        mp_buffer_appendf(&report, "Wi-Fi signal: %d%% (%d dBm)\n",
                          info.wifi_signal_percent, info.wifi_signal_dbm);
    else
        mp_buffer_append(&report, "Wi-Fi signal: Unavailable\n");
    mp_buffer_appendf(&report,
        "NTP synchronized: %s\nSystem time valid: %s\n\n"
        "System storage\nRoot partition: %s\nSystem drive: %s\nRoot filesystem: %s\nRoot state: %s\n"
        "Used storage: %llu bytes\nAvailable storage: %llu bytes\nTotal storage: %llu bytes\n"
        "Asset usage\nDay images: %llu bytes in %llu files\nBedtime images: %llu bytes in %llu files\n"
        "Music: %llu bytes in %llu files\nStories: %llu bytes in %llu files\nFonts: %llu bytes in %llu files\n"
        "Boot partition: %s\nBoot filesystem: %s\nBoot mount point: %s\n\n"
        "SD card\nPresent: %s\nDevice: %s\nType: %s\nProduct name: %s\n"
        "Manufacturer ID: %s\nOEM ID: %s\nSerial: %s\nManufactured: %s\nCapacity: %llu bytes\nCID: %s\n\n"
        "Hardware health\nCPU temperature: %.1f C\nOLED: %s\nTouch sensor: %s\n"
        "RGB GPIO: %s\nRGB scene: %s\nRGB colour: %s\nRGB output: %s\n"
        "RGB GPIO pins: red 5, green 6, blue 13\nRGB wiring: common cathode\n"
        "RGB PWM: %d Hz, %d levels\nRGB GPIO write errors: %d\n"
        "RGB master: %s\nRGB maximum brightness: %d%%\nRGB channel gains: %d%% / %d%% / %d%%\n"
        "RGB idle off: %s\nRGB bedtime fade: %d minutes\n\n"
        "Services\nAPI service: %s\nCore service: %s\n\n"
        "Alarms\nNext alarm: %s\nLast successful alarm: %s\n",
        info.ntp_synchronized ? "Yes" : "No", info.system_time_valid ? "Yes" : "No",
        info.root_device[0] ? info.root_device : "Unavailable",
        info.root_disk[0] ? info.root_disk : "Unavailable",
        info.root_filesystem[0] ? info.root_filesystem : "Unavailable",
        info.root_device[0] ? (info.root_read_only ? "Read-only" : "Read/write") : "Unavailable",
        (unsigned long long)info.storage_used_bytes,
        (unsigned long long)info.storage_free_bytes, (unsigned long long)info.storage_total_bytes,
        (unsigned long long)info.day_images_bytes, (unsigned long long)info.day_images_files,
        (unsigned long long)info.bedtime_images_bytes, (unsigned long long)info.bedtime_images_files,
        (unsigned long long)info.music_bytes, (unsigned long long)info.music_files,
        (unsigned long long)info.stories_bytes, (unsigned long long)info.stories_files,
        (unsigned long long)info.fonts_bytes, (unsigned long long)info.fonts_files,
        info.boot_device[0] ? info.boot_device : "Unavailable",
        info.boot_filesystem[0] ? info.boot_filesystem : "Unavailable",
        info.boot_mount_point[0] ? info.boot_mount_point : "Unavailable",
        info.sd_present ? "Yes" : "No",
        info.sd_device[0] ? info.sd_device : "Unavailable",
        info.sd_type[0] ? info.sd_type : "Unavailable",
        info.sd_name[0] ? info.sd_name : "Unavailable",
        info.sd_manufacturer_id[0] ? info.sd_manufacturer_id : "Unavailable",
        info.sd_oem_id[0] ? info.sd_oem_id : "Unavailable",
        info.sd_serial[0] ? info.sd_serial : "Unavailable",
        info.sd_manufacture_date[0] ? info.sd_manufacture_date : "Unavailable",
        (unsigned long long)info.sd_capacity_bytes,
        info.sd_cid[0] ? info.sd_cid : "Unavailable",
        info.cpu_temperature_c,
        info.oled_ok ? "Working" : "Unavailable", info.touch_ok ? "Working" : "Unavailable",
        info.led_ok ? "Ready" : "Unavailable",
        info.led_scene[0] ? info.led_scene : "Unavailable",
        info.led_colour[0] ? info.led_colour : "Unavailable",
        info.led_output[0] ? info.led_output : "Unavailable",
        info.led_pwm_hz, info.led_pwm_levels, info.led_write_errors,
        info.led_enabled ? "Enabled" : "Disabled", info.led_max_brightness,
        info.led_red_gain, info.led_green_gain, info.led_blue_gain,
        info.led_idle_off ? "Yes" : "No", info.led_bedtime_fade_minutes,
        info.api_healthy ? "Working" : "Unavailable", info.core_healthy ? "Working" : "Unavailable",
        info.next_alarm_text[0] ? info.next_alarm_text : "No alarm scheduled", last_alarm);
    size_t length = 0;
    char *body = mp_buffer_steal(&report, &length);
    mp_buffer_free(&report);
    if (!body) return queue_json(connection, 500, "{\"ok\":false,\"error\":\"report could not be created\"}");
    return queue_text_download(connection, body, length, "mk-piclock-diagnostic-report.txt");
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

static uint64_t form_u64(const struct request_context *context, const char *key, uint64_t fallback) {
    const char *value = form_value(context, key);
    if (!value || !*value) return fallback;
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno || end == value || *end) return fallback;
    return (uint64_t)parsed;
}

static enum MHD_Result serve_auth_status(struct MHD_Connection *connection) {
    char password[WEB_PASSWORD_MAX + 1];
    int required = read_web_password(password, sizeof(password));
    int authenticated = !required || auth_cookie_matches(connection, password);
    char body[192];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"password_required\":%s,\"authenticated\":%s,"
             "\"api_version\":\"%s\",\"product_version\":\"%s\"}",
             required ? "true" : "false", authenticated ? "true" : "false",
             API_VERSION, PRODUCT_VERSION);
    return queue_json(connection, 200, body);
}

static enum MHD_Result serve_auth_login(struct MHD_Connection *connection,
                                        const struct request_context *context) {
    char expected[WEB_PASSWORD_MAX + 1];
    if (!read_web_password(expected, sizeof(expected)))
        return queue_json_with_cookie(connection, 200,
            "{\"ok\":true,\"password_required\":false}", clear_auth_cookie());

    const char *password = form_value(context, "password");
    if (!password || !password_equal(password, expected))
        return queue_json(connection, MHD_HTTP_UNAUTHORIZED,
            "{\"ok\":false,\"error\":\"Password is incorrect\"}");

    char cookie[WEB_PASSWORD_MAX * 2 + 96];
    build_auth_cookie(expected, cookie, sizeof(cookie));
    return queue_json_with_cookie(connection, 200,
        "{\"ok\":true,\"authenticated\":true}", cookie);
}

static enum MHD_Result configure_web_password(struct MHD_Connection *connection,
                                              const struct request_context *context) {
    const char *password = form_value(context, "password");
    if (!password) password = "";
    if (!valid_web_password(password))
        return queue_json(connection, 400,
            "{\"ok\":false,\"error\":\"Password must be 64 characters or fewer and cannot contain line breaks\"}");
    if (write_web_password(password) != 0)
        return queue_json(connection, 500,
            "{\"ok\":false,\"error\":\"Password could not be saved\"}");

    if (!password[0])
        return queue_json_with_cookie(connection, 200,
            "{\"ok\":true,\"password_required\":false}", clear_auth_cookie());

    char cookie[WEB_PASSWORD_MAX * 2 + 96];
    build_auth_cookie(password, cookie, sizeof(cookie));
    return queue_json_with_cookie(connection, 200,
        "{\"ok\":true,\"password_required\":true}", cookie);
}

static int parse_led_scene(const char *value) {
    if (!value) return -1;
    if (strcmp(value, "alarm") == 0) return MP_LED_SCENE_ALARM;
    if (strcmp(value, "bedtime") == 0) return MP_LED_SCENE_BEDTIME;
    if (strcmp(value, "message") == 0) return MP_LED_SCENE_MESSAGE;
    if (strcmp(value, "music") == 0) return MP_LED_SCENE_MUSIC;
    if (strcmp(value, "daytime") == 0) return MP_LED_SCENE_DAYTIME;
    if (strcmp(value, "stories") == 0) return MP_LED_SCENE_STORIES;
    return -1;
}

static int parse_led_effect(const char *value) {
    if (!value || strcmp(value, "solid") == 0) return MP_LED_EFFECT_SOLID;
    if (strcmp(value, "fade") == 0) return MP_LED_EFFECT_FADE;
    if (strcmp(value, "rainbow") == 0) return MP_LED_EFFECT_RAINBOW;
    return -1;
}

static int parse_hex_colour(const char *value, uint8_t *red, uint8_t *green, uint8_t *blue) {
    if (!value || !red || !green || !blue) return -1;
    if (*value == '#') value++;
    if (strlen(value) != 6) return -1;
    unsigned int rgb = 0;
    for (int i = 0; i < 6; i++) {
        unsigned int digit;
        char ch = value[i];
        if (ch >= '0' && ch <= '9') digit = (unsigned int)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') digit = (unsigned int)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') digit = (unsigned int)(ch - 'A' + 10);
        else return -1;
        rgb = (rgb << 4) | digit;
    }
    *red = (uint8_t)((rgb >> 16) & 0xFFu);
    *green = (uint8_t)((rgb >> 8) & 0xFFu);
    *blue = (uint8_t)(rgb & 0xFFu);
    return 0;
}

static int build_led_profile(const struct request_context *context,
                             struct mp_led_profile *profile, int *scene) {
    if (!context || !profile || !scene) return -1;
    *scene = parse_led_scene(form_value(context, "scene"));
    int effect = parse_led_effect(form_value(context, "effect"));
    int brightness = form_int(context, "brightness", 50);
    int cycle_seconds = form_int(context, "cycle_seconds", effect == MP_LED_EFFECT_RAINBOW ? 12 : 8);
    if (*scene < 0 || effect < 0 || brightness < 0 || brightness > 100 ||
        cycle_seconds < 2 || cycle_seconds > 60) return -1;
    memset(profile, 0, sizeof(*profile));
    profile->effect = (uint8_t)effect;
    profile->brightness = (uint8_t)brightness;
    profile->cycle_seconds = (uint8_t)cycle_seconds;
    if (parse_hex_colour(form_value(context, "color1"),
                         &profile->red1, &profile->green1, &profile->blue1) != 0 ||
        parse_hex_colour(form_value(context, "color2"),
                         &profile->red2, &profile->green2, &profile->blue2) != 0) return -1;
    return 0;
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

static enum MHD_Result serve_images_list(struct MHD_Connection *connection, int bedtime) {
    int all = parse_int_value(query_value(connection, "all"), 0) != 0;
    int page = parse_int_value(query_value(connection, "page"), 1);
    if (page < 1) page = 1;
    char files[MP_ASSET_LIST_MAX][MP_ASSET_NAME_MAX];
    int count = mp_asset_scan(bedtime ? MP_BEDTIME_IMAGE_DIR : MP_IMAGE_DIR,
                              MP_ASSET_SCAN_IMAGE_RAW, files, MP_ASSET_LIST_MAX);
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
        "{\"kind\":\"%s\",\"page\":%d,\"max_page\":%d,\"per_page\":%d,\"count\":%d,\"images\":[",
        bedtime ? "bedtime" : "normal", page, max_page, per_page, count);
    for (int i = start; i < end && !body.failed; i++) {
        char title[MP_ASSET_NAME_MAX];
        char source_path[768];
        char source_name[MP_ASSET_NAME_MAX];
        mp_asset_image_title(files[i], title, sizeof(title));
        int source_exists = mp_asset_image_source_path(files[i], bedtime, source_path, sizeof(source_path)) == 0 &&
                            access(source_path, R_OK) == 0;
        mp_safe_str(source_name, sizeof(source_name), files[i]);
        char *dot = strrchr(source_name, '.');
        if (dot) mp_safe_str(dot, (size_t)(source_name + sizeof(source_name) - dot), ".png");
        mp_buffer_appendf(&body, "%s{\"file\":\"", i == start ? "" : ",");
        mp_buffer_append_json_string(&body, files[i]);
        mp_buffer_append(&body, "\",\"title\":\"");
        mp_buffer_append_json_string(&body, title);
        mp_buffer_append(&body, "\",\"source_png\":\"");
        if (source_exists) mp_buffer_append_json_string(&body, source_name);
        mp_buffer_append(&body, "\",\"preview_url\":\"");
        if (source_exists)
            mp_buffer_appendf(&body, "%s?file=%s",
                              bedtime ? "/api/v1/assets/bedtime-images/source" : "/api/v1/assets/images/source",
                              files[i]);
        mp_buffer_appendf(&body, "\",\"source_exists\":%d}", source_exists ? 1 : 0);
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
    if (mp_buffer_init(&body, 4096, MAX_API_JSON_RESPONSE) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    mp_buffer_appendf(&body, "{\"global_volume\":%d,\"current\":\"", state.global_volume);
    mp_buffer_append_json_string(&body, state.current_music);
    mp_buffer_append(&body, "\",\"tracks\":[");
    for (int i = 0; i < count && !body.failed; i++) {
        char path[768];
        int path_len = snprintf(path, sizeof(path), "%s/%.*s", MP_MUSIC_DIR,
                                MP_ASSET_NAME_MAX - 1, files[i]);
        struct mp_id3_metadata metadata;
        memset(&metadata, 0, sizeof(metadata));
        int tagged = path_len >= 0 && (size_t)path_len < sizeof(path) &&
                     mp_read_id3_metadata(path, &metadata) == 0;
        struct mp_audio_info audio_info;
        memset(&audio_info, 0, sizeof(audio_info));
        if (path_len >= 0 && (size_t)path_len < sizeof(path))
            (void)mp_asset_read_mp3_info(path, &audio_info);
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
        mp_buffer_append(&body, "\",\"album\":\"");
        mp_buffer_append_json_string(&body, metadata.album);
        mp_buffer_append(&body, "\",\"year\":\"");
        mp_buffer_append_json_string(&body, metadata.year);
        mp_buffer_append(&body, "\",\"track\":\"");
        mp_buffer_append_json_string(&body, metadata.track);
        mp_buffer_append(&body, "\",\"genre\":\"");
        mp_buffer_append_json_string(&body, metadata.genre);
        mp_buffer_append(&body, "\",\"display\":\"");
        mp_buffer_append_json_string(&body, display);
        mp_buffer_appendf(&body,
            "\",\"id3\":%d,\"duration_seconds\":%.3f,\"duration_estimated\":%d,"
            "\"bitrate_kbps\":%d,\"bitrate_mode\":\"%s\",\"sample_rate_hz\":%ld,"
            "\"channels\":%d,\"mpeg_layer\":%d,\"file_size_bytes\":%llu}",
            tagged ? 1 : 0, audio_info.duration_seconds, audio_info.duration_estimated,
            audio_info.bitrate_kbps, audio_info.vbr_mode == 1 ? "VBR" : audio_info.vbr_mode == 2 ? "ABR" : "CBR",
            audio_info.sample_rate_hz, audio_info.channels, audio_info.layer,
            (unsigned long long)audio_info.file_size_bytes);
    }
    mp_buffer_append(&body, "]}");
    return queue_json_builder(connection, 200, &body);
}

static enum MHD_Result serve_stories_list(struct MHD_Connection *connection) {
    struct mp_ipc_asset_state state;
    if (get_asset_state(&state) != 0)
        return queue_json(connection, 503, "{\"ok\":false,\"error\":\"clock core unavailable\"}");
    char files[MP_ASSET_LIST_MAX][MP_ASSET_NAME_MAX];
    int count = mp_asset_scan(MP_STORY_DIR, MP_ASSET_SCAN_MUSIC_MP3, files, MP_ASSET_LIST_MAX);
    struct mp_buffer body;
    if (mp_buffer_init(&body, 4096, MAX_API_JSON_RESPONSE) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    mp_buffer_appendf(&body,
        "{\"story_enabled\":%d,\"story_volume\":%d,\"story_message\":\"",
        state.story_enabled, state.story_volume);
    mp_buffer_append_json_string(&body, state.story_message);
    mp_buffer_append(&body, "\",\"current\":\"");
    mp_buffer_append_json_string(&body, state.current_music);
    mp_buffer_append(&body, "\",\"tracks\":[");
    for (int i = 0; i < count && !body.failed; i++) {
        char path[768];
        int path_len = snprintf(path, sizeof(path), "%s/%.*s", MP_STORY_DIR,
                                MP_ASSET_NAME_MAX - 1, files[i]);
        struct mp_id3_metadata metadata;
        memset(&metadata, 0, sizeof(metadata));
        int tagged = path_len >= 0 && (size_t)path_len < sizeof(path) &&
                     mp_read_id3_metadata(path, &metadata) == 0;
        struct mp_audio_info audio_info;
        memset(&audio_info, 0, sizeof(audio_info));
        if (path_len >= 0 && (size_t)path_len < sizeof(path))
            (void)mp_asset_read_mp3_info(path, &audio_info);
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
        mp_buffer_appendf(&body,
            "\",\"id3\":%d,\"duration_seconds\":%.3f,\"duration_estimated\":%d,"
            "\"bitrate_kbps\":%d,\"bitrate_mode\":\"%s\",\"sample_rate_hz\":%ld,"
            "\"channels\":%d,\"mpeg_layer\":%d,\"file_size_bytes\":%llu}",
            tagged ? 1 : 0, audio_info.duration_seconds, audio_info.duration_estimated,
            audio_info.bitrate_kbps, audio_info.vbr_mode == 1 ? "VBR" : audio_info.vbr_mode == 2 ? "ABR" : "CBR",
            audio_info.sample_rate_hz, audio_info.channels, audio_info.layer,
            (unsigned long long)audio_info.file_size_bytes);
    }
    mp_buffer_append(&body, "]}");
    return queue_json_builder(connection, 200, &body);
}

static enum MHD_Result serve_music_jobs(struct MHD_Connection *connection) {
    struct mp_music_job_snapshot jobs[MP_MUSIC_JOB_MAX];
    int count = mp_music_jobs_snapshot(jobs, MP_MUSIC_JOB_MAX);
    struct mp_buffer body;
    if (mp_buffer_init(&body, 2048, MAX_API_JSON_RESPONSE) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    mp_buffer_append(&body, "{\"ok\":true,\"jobs\":[");
    for (int i = 0; i < count && !body.failed; i++) {
        mp_buffer_appendf(&body, "%s{\"id\":%llu,\"state\":\"%s\",\"progress\":%u,\"file\":\"",
                          i ? "," : "", (unsigned long long)jobs[i].id,
                          mp_music_job_state_name(jobs[i].state), jobs[i].progress);
        mp_buffer_append_json_string(&body, jobs[i].file);
        mp_buffer_append(&body, "\",\"error\":\"");
        mp_buffer_append_json_string(&body, jobs[i].error);
        mp_buffer_appendf(&body,
            "\",\"created_at\":%lld,\"completed_at\":%lld,\"bitrate_kbps\":%d,"
            "\"sample_rate_hz\":%ld,\"lowpass_hz\":%d}",
            (long long)jobs[i].created_at, (long long)jobs[i].completed_at,
            jobs[i].settings.bitrate_kbps, jobs[i].settings.sample_rate_hz,
            jobs[i].settings.lowpass_hz);
    }
    mp_buffer_append(&body, "]}");
    return queue_json_builder(connection, 200, &body);
}

static enum MHD_Result serve_image_source(struct MHD_Connection *connection, int bedtime) {
    const char *file = query_value(connection, "file");
    char path[768];
    if (!mp_asset_safe_image_filename(file) || mp_asset_image_source_path(file, bedtime, path, sizeof(path)) != 0)
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"invalid image file\"}");
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

static enum MHD_Result upload_images(struct MHD_Connection *connection, struct request_context *context, int bedtime) {
    int ok = 0, failed = 0;
    char first[MP_ASSET_NAME_MAX] = "";
    for (size_t i = 0; i < context->upload_count; i++) {
        struct upload_file *upload = &context->uploads[i];
        if (!mp_asset_has_png_ext(upload->filename) || upload->size == 0) {
            failed++;
            continue;
        }
        const char *target_dir = bedtime ? MP_BEDTIME_IMAGE_DIR : MP_IMAGE_DIR;
        if (!mp_asset_has_free_space(target_dir, (uint64_t)upload->size + MP_IMAGE_RAW_BYTES,
                                     g_disk_reserve_bytes)) {
            failed++;
            continue;
        }
        char raw_name[MP_ASSET_NAME_MAX];
        if (mp_asset_save_image_png(upload->temp_path, upload->filename, bedtime, raw_name, sizeof(raw_name)) != 0) {
            failed++;
            continue;
        }
        if (!first[0]) mp_safe_str(first, sizeof(first), raw_name);
        ok++;
    }
    if (ok == 0)
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"no valid PNG image files were uploaded\"}");
    char json[256];
    snprintf(json, sizeof(json), "{\"ok\":true,\"uploaded\":%d,\"skipped\":%d}", ok, failed);
    return notify_or_saved_warning(connection, bedtime ? MP_IPC_ASSET_BEDTIME_IMAGE : MP_IPC_ASSET_IMAGE,
                                   MP_IPC_ASSET_UPLOADED, (uint32_t)ok, first, json);
}

static int valid_music_settings(const struct mp_audio_optimize_settings *settings) {
    if (!settings) return 0;
    int bitrate_ok = settings->bitrate_kbps == 64 || settings->bitrate_kbps == 96 ||
                     settings->bitrate_kbps == 128 || settings->bitrate_kbps == 160;
    int rate_ok = settings->sample_rate_hz == 32000 || settings->sample_rate_hz == 44100;
    int lowpass_ok = settings->lowpass_hz == 12000 || settings->lowpass_hz == 16000 ||
                     settings->lowpass_hz == 18000;
    return bitrate_ok && rate_ok && lowpass_ok;
}

static enum MHD_Result upload_music(struct MHD_Connection *connection, struct request_context *context) {
    if (mp_asset_ensure_dir(MP_MUSIC_DIR) != 0 || mp_asset_ensure_dir(MP_MUSIC_PROCESS_DIR) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"music processing directory unavailable\"}");
    if (mp_music_jobs_active() > 0)
        return queue_json(connection, 409,
                          "{\"ok\":false,\"error\":\"wait for all selected songs to finish before uploading more\"}");
    if (context->upload_count == 0 || context->upload_count > MP_MUSIC_JOB_MAX)
        return queue_json(connection, 400,
                          "{\"ok\":false,\"error\":\"upload between 1 and 32 MP3 files at a time\"}");

    struct mp_audio_optimize_settings settings = {
        .bitrate_kbps = form_int(context, "bitrate_kbps", 96),
        .sample_rate_hz = form_int(context, "sample_rate_hz", 44100),
        .lowpass_hz = form_int(context, "lowpass_hz", 16000),
        .quality = 2
    };
    if (!valid_music_settings(&settings))
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"unsupported MP3 processing settings\"}");

    char names[MP_MUSIC_JOB_MAX][MP_ASSET_NAME_MAX];
    char staged[MP_MUSIC_JOB_MAX][768];
    struct mp_music_job_request requests[MP_MUSIC_JOB_MAX];
    uint64_t job_ids[MP_MUSIC_JOB_MAX];
    uint64_t required = 0;
    memset(staged, 0, sizeof(staged));
    memset(requests, 0, sizeof(requests));
    memset(job_ids, 0, sizeof(job_ids));

    for (size_t i = 0; i < context->upload_count; i++) {
        struct upload_file *upload = &context->uploads[i];
        mp_asset_sanitize_filename(upload->filename, names[i], sizeof(names[i]), "alarm.mp3");
        if (!mp_asset_safe_filename(names[i]) || !mp_asset_has_mp3_ext(names[i]) ||
            upload->size == 0 || upload->size > MP_MUSIC_UPLOAD_MAX_BYTES ||
            mp_asset_validate_mp3(upload->temp_path) != 0)
            return queue_json(connection, 400,
                              "{\"ok\":false,\"error\":\"every selected file must be a readable MP3\"}");
        for (size_t prior = 0; prior < i; prior++) {
            if (strcmp(names[prior], names[i]) == 0)
                return queue_json(connection, 400,
                                  "{\"ok\":false,\"error\":\"selected MP3 files must have unique filenames\"}");
        }
        if (upload->size > (UINT64_MAX - required) / 2u)
            return queue_json(connection, 400, "{\"ok\":false,\"error\":\"upload size overflow\"}");
        required += (uint64_t)upload->size * 2u;
    }

    if (!mp_asset_has_free_space(MP_MUSIC_DIR, required, g_disk_reserve_bytes))
        return queue_json(connection, 507, "{\"ok\":false,\"error\":\"insufficient storage\"}");

    size_t staged_count = 0;
    for (size_t i = 0; i < context->upload_count; i++) {
        snprintf(staged[i], sizeof(staged[i]), "%s/.source-XXXXXX", MP_MUSIC_PROCESS_DIR);
        int source_fd = mkstemp(staged[i]);
        if (source_fd < 0) break;
        close(source_fd);
        unlink(staged[i]);
        if (mp_asset_move_file(context->uploads[i].temp_path, staged[i]) != 0) break;
        context->uploads[i].temp_path[0] = '\0';
        requests[i].source_path = staged[i];
        requests[i].output_name = names[i];
        staged_count++;
    }
    if (staged_count != context->upload_count) {
        for (size_t i = 0; i < staged_count; i++) (void)unlink(staged[i]);
        return queue_json(connection, 500,
                          "{\"ok\":false,\"error\":\"one or more music files could not be staged\"}");
    }

    int queue_result = mp_music_jobs_queue_batch(requests, context->upload_count,
                                                  &settings, job_ids);
    if (queue_result != 0) {
        for (size_t i = 0; i < staged_count; i++) (void)unlink(staged[i]);
        if (queue_result == 1)
            return queue_json(connection, 409,
                              "{\"ok\":false,\"error\":\"wait for all selected songs to finish before uploading more\"}");
        if (queue_result == 2)
            return queue_json(connection, 409,
                              "{\"ok\":false,\"error\":\"music processing queue does not have enough free slots\"}");
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"music could not be queued\"}");
    }

    struct mp_buffer body;
    if (mp_buffer_init(&body, 512, 8192) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    mp_buffer_appendf(&body,
        "{\"ok\":true,\"queued\":%zu,\"skipped\":0,\"settings\":{\"bitrate_kbps\":%d,"
        "\"sample_rate_hz\":%ld,\"lowpass_hz\":%d},\"job_ids\":[",
        context->upload_count, settings.bitrate_kbps, settings.sample_rate_hz, settings.lowpass_hz);
    for (size_t i = 0; i < context->upload_count; i++)
        mp_buffer_appendf(&body, "%s%llu", i ? "," : "", (unsigned long long)job_ids[i]);
    mp_buffer_append(&body, "]}");
    return queue_json_builder(connection, 202, &body);
}

static enum MHD_Result upload_story(struct MHD_Connection *connection,
                                    struct request_context *context) {
    if (mp_asset_ensure_dir(MP_STORY_DIR) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"story directory unavailable\"}");
    if (context->upload_count != 1)
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"upload one story MP3 at a time\"}");

    struct upload_file *upload = &context->uploads[0];
    char name[MP_ASSET_NAME_MAX];
    mp_asset_sanitize_filename(upload->filename, name, sizeof(name), "story.mp3");
    if (!mp_asset_safe_filename(name) || !mp_asset_has_mp3_ext(name) || upload->size == 0 ||
        upload->size > MP_STORY_UPLOAD_MAX_BYTES || mp_asset_validate_mp3(upload->temp_path) != 0)
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"uploaded file is not a readable MP3\"}");

    char target[768];
    int target_len = snprintf(target, sizeof(target), "%s/%.*s", MP_STORY_DIR,
                              MP_ASSET_NAME_MAX - 1, name);
    if (target_len <= 0 || (size_t)target_len >= sizeof(target))
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"story filename is too long\"}");

    uint64_t current = mp_asset_directory_bytes(MP_STORY_DIR, MP_ASSET_SCAN_MUSIC_MP3);
    uint64_t replaced = 0;
    struct stat existing;
    if (stat(target, &existing) == 0 && S_ISREG(existing.st_mode) && existing.st_size > 0)
        replaced = (uint64_t)existing.st_size;
    uint64_t retained = current >= replaced ? current - replaced : 0;
    uint64_t incoming = (uint64_t)upload->size;
    if (incoming > g_story_quota_bytes || retained > g_story_quota_bytes - incoming)
        return queue_json(connection, 507, "{\"ok\":false,\"error\":\"story library quota would be exceeded\"}");
    if (!mp_asset_has_free_space(MP_STORY_DIR, incoming, g_disk_reserve_bytes))
        return queue_json(connection, 507, "{\"ok\":false,\"error\":\"insufficient storage\"}");

    if (mp_asset_move_file(upload->temp_path, target) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"story could not be saved\"}");
    upload->temp_path[0] = '\0';
    (void)chmod(target, 0640);
    return notify_or_saved_warning(connection, MP_IPC_ASSET_STORY, MP_IPC_ASSET_UPLOADED,
                                   1, name, "{\"ok\":true,\"uploaded\":1}");
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

static enum MHD_Result delete_image(struct MHD_Connection *connection, const struct request_context *context, int bedtime) {
    const char *file = form_value(context, "file");
    if (!mp_asset_safe_image_filename(file))
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"invalid image filename\"}");
    const char *dir = bedtime ? MP_BEDTIME_IMAGE_DIR : MP_IMAGE_DIR;
    (void)mp_asset_delete_file(dir, file);
    char source[768];
    if (mp_asset_image_source_path(file, bedtime, source, sizeof(source)) == 0) (void)unlink(source);
    if (notify_asset(bedtime ? MP_IPC_ASSET_BEDTIME_IMAGE : MP_IPC_ASSET_IMAGE,
                     MP_IPC_ASSET_DELETED, 1, file) != 0)
        return queue_json(connection, 503, "{\"ok\":false,\"deleted\":true,\"error\":\"clock core unavailable\"}");
    return queue_json(connection, 200, "{\"ok\":true,\"deleted\":true}");
}

static enum MHD_Result delete_all_images(struct MHD_Connection *connection, int bedtime) {
    int deleted = mp_asset_delete_images(bedtime);
    if (notify_asset(bedtime ? MP_IPC_ASSET_BEDTIME_IMAGE : MP_IPC_ASSET_IMAGE,
                     MP_IPC_ASSET_DELETED_ALL, (uint32_t)deleted, NULL) != 0)
        return queue_json(connection, 503, "{\"ok\":false,\"deleted\":true,\"error\":\"clock core unavailable\"}");
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":true,\"deleted\":%d,\"kind\":\"%s\"}",
             deleted, bedtime ? "bedtime" : "normal");
    return queue_json(connection, 200, json);
}

static enum MHD_Result delete_music(struct MHD_Connection *connection,
                                    const struct request_context *context) {
    const char *file = form_value(context, "file");
    if (!mp_asset_safe_filename(file) || !mp_asset_has_mp3_ext(file))
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"invalid music filename\"}");
    if (mp_music_jobs_file_active(file))
        return queue_json(connection, 409, "{\"ok\":false,\"error\":\"this music file is still processing\"}");
    if (mp_asset_delete_file(MP_MUSIC_DIR, file) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"music file could not be deleted\"}");
    if (notify_asset(MP_IPC_ASSET_MUSIC, MP_IPC_ASSET_DELETED, 1, file) != 0)
        return queue_json(connection, 503,
                          "{\"ok\":false,\"deleted\":true,\"error\":\"music deleted but clock core unavailable\"}");
    return queue_json(connection, 200, "{\"ok\":true,\"deleted\":true}");
}

static enum MHD_Result delete_all_music(struct MHD_Connection *connection) {
    if (mp_music_jobs_active() > 0)
        return queue_json(connection, 409,
                          "{\"ok\":false,\"error\":\"wait for music processing to finish before deleting all music\"}");
    int deleted = mp_asset_delete_music();
    if (notify_asset(MP_IPC_ASSET_MUSIC, MP_IPC_ASSET_DELETED_ALL, (uint32_t)deleted, NULL) != 0)
        return queue_json(connection, 503, "{\"ok\":false,\"deleted\":true,\"error\":\"clock core unavailable\"}");
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":true,\"deleted_music\":%d}", deleted);
    return queue_json(connection, 200, json);
}

static enum MHD_Result delete_story(struct MHD_Connection *connection,
                                    const struct request_context *context) {
    const char *file = form_value(context, "file");
    if (!mp_asset_safe_filename(file) || !mp_asset_has_mp3_ext(file))
        return queue_json(connection, 400, "{\"ok\":false,\"error\":\"invalid story filename\"}");
    if (mp_asset_delete_file(MP_STORY_DIR, file) != 0)
        return queue_json(connection, 500, "{\"ok\":false,\"error\":\"story file could not be deleted\"}");
    if (notify_asset(MP_IPC_ASSET_STORY, MP_IPC_ASSET_DELETED, 1, file) != 0)
        return queue_json(connection, 503,
                          "{\"ok\":false,\"deleted\":true,\"error\":\"story deleted but clock core unavailable\"}");
    return queue_json(connection, 200, "{\"ok\":true,\"deleted\":true}");
}

static enum MHD_Result delete_all_stories(struct MHD_Connection *connection) {
    int deleted = mp_asset_delete_stories();
    if (notify_asset(MP_IPC_ASSET_STORY, MP_IPC_ASSET_DELETED_ALL, (uint32_t)deleted, NULL) != 0)
        return queue_json(connection, 503, "{\"ok\":false,\"deleted\":true,\"error\":\"clock core unavailable\"}");
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":true,\"deleted_stories\":%d}", deleted);
    return queue_json(connection, 200, json);
}

static enum MHD_Result clear_music_queue(struct MHD_Connection *connection) {
    int removed = mp_music_jobs_clear_queued();
    int processing_active = mp_music_jobs_active() > 0;
    char json[160];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"queued_removed\":%d,\"processing_active\":%s}",
             removed, processing_active ? "true" : "false");
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
        case ROUTE_AUTH_STATUS:
            return serve_auth_status(connection);
        case ROUTE_AUTH_LOGIN:
            return serve_auth_login(connection, context);
        case ROUTE_AUTH_PASSWORD:
            return configure_web_password(connection, context);
        case ROUTE_STATUS:
            return call_core(connection, MP_IPC_OP_STATUS, NULL, 0);
        case ROUTE_HEALTH:
        case ROUTE_DIAGNOSTICS:
            return serve_diagnostics(connection);
        case ROUTE_DIAGNOSTIC_REPORT:
            return serve_diagnostic_report(connection);
        case ROUTE_BACKUP_DOWNLOAD:
            return download_backup(connection);
        case ROUTE_BACKUP_RESTORE:
            return restore_backup(connection, context);
        case ROUTE_FACTORY_RESET:
            return factory_reset(connection, context);
        case ROUTE_IMAGES_LIST:
            return serve_images_list(connection, 0);
        case ROUTE_BEDTIME_IMAGES_LIST:
            return serve_images_list(connection, 1);
        case ROUTE_IMAGES_DOWNLOAD:
            return download_images(connection, 0);
        case ROUTE_BEDTIME_IMAGES_DOWNLOAD:
            return download_images(connection, 1);
        case ROUTE_FONTS_LIST:
            return serve_fonts_list(connection);
        case ROUTE_MUSIC_LIST:
            return serve_music_list(connection);
        case ROUTE_STORIES_LIST:
            return serve_stories_list(connection);
        case ROUTE_MUSIC_JOBS:
            return serve_music_jobs(connection);
        case ROUTE_CLEAR_MUSIC_QUEUE:
            return clear_music_queue(connection);
        case ROUTE_IMAGE_SOURCE:
            return serve_image_source(connection, 0);
        case ROUTE_BEDTIME_IMAGE_SOURCE:
            return serve_image_source(connection, 1);
        case ROUTE_FONT_FILE:
            return serve_font_file(connection);
        case ROUTE_UPLOAD_IMAGE:
            return upload_images(connection, context, 0);
        case ROUTE_UPLOAD_BEDTIME_IMAGE:
            return upload_images(connection, context, 1);
        case ROUTE_UPLOAD_MUSIC:
            return upload_music(connection, context);
        case ROUTE_UPLOAD_STORY:
            return upload_story(connection, context);
        case ROUTE_UPLOAD_FONT:
            return upload_font(connection, context);
        case ROUTE_DELETE_IMAGE:
            return delete_image(connection, context, 0);
        case ROUTE_DELETE_BEDTIME_IMAGE:
            return delete_image(connection, context, 1);
        case ROUTE_DELETE_ALL_IMAGES:
            return delete_all_images(connection, 0);
        case ROUTE_DELETE_ALL_BEDTIME_IMAGES:
            return delete_all_images(connection, 1);
        case ROUTE_DELETE_MUSIC:
            return delete_music(connection, context);
        case ROUTE_DELETE_ALL_MUSIC:
            return delete_all_music(connection);
        case ROUTE_DELETE_STORY:
            return delete_story(connection, context);
        case ROUTE_DELETE_ALL_STORIES:
            return delete_all_stories(connection);
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
            else if (action && strcmp(action, "play-story") == 0) request.action = MP_IPC_ACTION_PLAY_STORY;
            else return queue_json(connection, 400, "{\"ok\":false,\"error\":\"unknown display action\"}");
            mp_safe_str(request.file, sizeof(request.file), form_value(context, "file"));
            return call_core(connection, MP_IPC_OP_DISPLAY_ACTION, &request, sizeof(request));
        }
        case ROUTE_DISPLAY_PREVIEW:
            return call_core(connection, MP_IPC_OP_DISPLAY_PREVIEW, NULL, 0);
        case ROUTE_BRIGHTNESS_PREVIEW: {
            int percent = form_int(context, "brightness_percent", 35);
            int hold_seconds = form_int(context, "hold_seconds", 8);
            if (percent < 0 || percent > 100 || hold_seconds < 1 || hold_seconds > 30)
                return queue_json(connection, 400, "{\"ok\":false,\"error\":\"invalid brightness preview\"}");
            struct mp_ipc_brightness_preview request;
            memset(&request, 0, sizeof(request));
            request.percent = (uint8_t)percent;
            request.hold_seconds = (uint8_t)hold_seconds;
            return call_core(connection, MP_IPC_OP_BRIGHTNESS_PREVIEW, &request, sizeof(request));
        }
        case ROUTE_DISPLAY_MESSAGE: {
            struct mp_ipc_display_message request;
            memset(&request, 0, sizeof(request));
            const char *image = form_value(context, "image_file");
            const char *text = form_value(context, "message_text");
            int image_bedtime = form_int(context, "image_bedtime", 0);
            int notification_sound = form_int(context, "notification_sound", 0);
            int delay_seconds = form_int(context, "delay_seconds", 0);
            uint64_t scheduled_at = form_u64(context, "scheduled_at", 0);
            if (image_bedtime != 0 && image_bedtime != 1)
                return queue_json(connection, 400,
                    "{\"ok\":false,\"error\":\"image_bedtime must be 0 or 1\"}");
            if (notification_sound != 0 && notification_sound != 1)
                return queue_json(connection, 400,
                    "{\"ok\":false,\"error\":\"notification_sound must be 0 or 1\"}");
            if (delay_seconds != 0 && delay_seconds != 10 && delay_seconds != 30 && delay_seconds != 60)
                return queue_json(connection, 400,
                    "{\"ok\":false,\"error\":\"delay_seconds must be 0, 10, 30, or 60\"}");
            if (scheduled_at && delay_seconds)
                return queue_json(connection, 400,
                    "{\"ok\":false,\"error\":\"use either Send After or a specific time, not both\"}");
            time_t now = time(NULL);
            if (scheduled_at && (scheduled_at <= (uint64_t)now ||
                scheduled_at > (uint64_t)now + 30u * 24u * 60u * 60u))
                return queue_json(connection, 400,
                    "{\"ok\":false,\"error\":\"specific time must be within the next 30 days\"}");
            request.delay_seconds = (uint16_t)delay_seconds;
            request.image_bedtime = (uint8_t)image_bedtime;
            request.notification_sound = (uint8_t)notification_sound;
            request.scheduled_at = scheduled_at;
            mp_safe_str(request.image_file, sizeof(request.image_file), image);
            mp_safe_str(request.text, sizeof(request.text), text);
            return call_core(connection, MP_IPC_OP_DISPLAY_MESSAGE, &request, sizeof(request));
        }
        case ROUTE_MESSAGE_PREVIEW: {
            struct mp_ipc_display_message request;
            memset(&request, 0, sizeof(request));
            int image_bedtime = form_int(context, "image_bedtime", 0);
            if (image_bedtime != 0 && image_bedtime != 1)
                return queue_json(connection, 400,
                    "{\"ok\":false,\"error\":\"image_bedtime must be 0 or 1\"}");
            request.image_bedtime = (uint8_t)image_bedtime;
            mp_safe_str(request.image_file, sizeof(request.image_file), form_value(context, "image_file"));
            mp_safe_str(request.text, sizeof(request.text), form_value(context, "message_text"));
            return call_core(connection, MP_IPC_OP_MESSAGE_PREVIEW, &request, sizeof(request));
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
            if (form_value(context, "story_enabled") != NULL) {
                request.present_mask |= MP_IPC_AUDIO_STORY_ENABLED;
                request.story_enabled = (uint8_t)(form_int(context, "story_enabled", 0) != 0);
            }
            if (form_value(context, "story_volume") != NULL) {
                request.present_mask |= MP_IPC_AUDIO_STORY_VOLUME;
                request.story_volume = (uint8_t)form_int(context, "story_volume", 55);
            }
            if (form_value(context, "story_message") != NULL) {
                request.present_mask |= MP_IPC_AUDIO_STORY_MESSAGE;
                mp_safe_str(request.story_message, sizeof(request.story_message), form_value(context, "story_message"));
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
            if (form_value(context, "bedtime_music_enabled")) {
                request.present_mask |= MP_IPC_DISPLAY_BEDTIME_MUSIC;
                request.bedtime_music_enabled =
                    (uint8_t)(form_int(context, "bedtime_music_enabled", 1) != 0);
            }
            if (form_value(context, "clock_24h_mode")) {
                request.present_mask |= MP_IPC_DISPLAY_CLOCK_MODE;
                request.clock_24h_mode = (uint8_t)(form_int(context, "clock_24h_mode", 0) != 0);
            }
            if (form_value(context, "oled_color")) {
                const char *color = form_value(context, "oled_color");
                request.present_mask |= MP_IPC_DISPLAY_OLED_COLOR;
                request.oled_color = (uint8_t)(strcmp(color, "yellow") == 0 ? MP_OLED_COLOR_YELLOW :
                    strcmp(color, "white") == 0 ? MP_OLED_COLOR_WHITE : MP_OLED_COLOR_GREEN);
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
        case ROUTE_CONFIG_LED: {
            struct mp_ipc_led_config request;
            memset(&request, 0, sizeof(request));
            int scene = -1;
            if (build_led_profile(context, &request.profile, &scene) != 0)
                return queue_json(connection, 400, "{\"ok\":false,\"error\":\"invalid LED settings\"}");
            request.scene = (uint8_t)scene;
            return call_core(connection, MP_IPC_OP_CONFIG_LED, &request, sizeof(request));
        }
        case ROUTE_CONFIG_LED_GLOBAL: {
            struct mp_ipc_led_global_config request;
            memset(&request, 0, sizeof(request));
            int enabled = form_int(context, "enabled", 1);
            int max_brightness = form_int(context, "max_brightness", 100);
            int red_gain = form_int(context, "red_gain", 100);
            int green_gain = form_int(context, "green_gain", 65);
            int blue_gain = form_int(context, "blue_gain", 80);
            int idle_off = form_int(context, "idle_off", 0);
            int bedtime_fade_minutes = form_int(context, "bedtime_fade_minutes", 15);
            int touch_blink_enabled = form_int(context, "touch_blink_enabled", 1);
            int touch_blink_brightness = form_int(context, "touch_blink_brightness", 60);
            const char *touch_blink_color = form_value(context, "touch_blink_color");
            if (!touch_blink_color) touch_blink_color = "#ffffff";
            if ((enabled != 0 && enabled != 1) || max_brightness < 0 || max_brightness > 100 ||
                red_gain < 0 || red_gain > 100 || green_gain < 0 || green_gain > 100 ||
                blue_gain < 0 || blue_gain > 100 || (idle_off != 0 && idle_off != 1) ||
                bedtime_fade_minutes < 0 || bedtime_fade_minutes > 120 ||
                (touch_blink_enabled != 0 && touch_blink_enabled != 1) ||
                touch_blink_brightness < 0 || touch_blink_brightness > 100 ||
                parse_hex_colour(touch_blink_color,
                                 &request.settings.touch_blink_red,
                                 &request.settings.touch_blink_green,
                                 &request.settings.touch_blink_blue) != 0)
                return queue_json(connection, 400,
                    "{\"ok\":false,\"error\":\"invalid global LED settings\"}");
            request.settings.enabled = (uint8_t)enabled;
            request.settings.max_brightness = (uint8_t)max_brightness;
            request.settings.red_gain = (uint8_t)red_gain;
            request.settings.green_gain = (uint8_t)green_gain;
            request.settings.blue_gain = (uint8_t)blue_gain;
            request.settings.idle_off = (uint8_t)idle_off;
            request.settings.bedtime_fade_minutes = (uint8_t)bedtime_fade_minutes;
            request.settings.touch_blink_enabled = (uint8_t)touch_blink_enabled;
            request.settings.touch_blink_brightness = (uint8_t)touch_blink_brightness;
            return call_core(connection, MP_IPC_OP_CONFIG_LED_GLOBAL, &request, sizeof(request));
        }
        case ROUTE_LED_PREVIEW: {
            struct mp_ipc_led_preview request;
            memset(&request, 0, sizeof(request));
            int scene = -1;
            if (build_led_profile(context, &request.profile, &scene) != 0)
                return queue_json(connection, 400, "{\"ok\":false,\"error\":\"invalid LED preview settings\"}");
            int hold_seconds = form_int(context, "hold_seconds", 10);
            if (hold_seconds < 1 || hold_seconds > 30)
                return queue_json(connection, 400, "{\"ok\":false,\"error\":\"preview must be 1 to 30 seconds\"}");
            request.scene = (uint8_t)scene;
            request.hold_seconds = (uint8_t)hold_seconds;
            request.bypass_master = (uint8_t)(form_int(context, "bypass_master", 0) != 0);
            request.raw_output = (uint8_t)(form_int(context, "raw_output", 0) != 0);
            return call_core(connection, MP_IPC_OP_LED_PREVIEW, &request, sizeof(request));
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
            "{\"name\":\"mk-piclock API\",\"api_version\":\"" API_VERSION "\","
            "\"product_version\":\"%s\",\"http_engine\":\"libmicrohttpd\","
            "\"core_protocol\":\"binary-ipc-v%u\","
            "\"status\":\"/api/v1/status\",\"auth\":\"/api/v1/auth/status\","
            "\"capabilities\":\"/api/v1/capabilities\","
            "\"openapi\":\"/api/v1/openapi.json\"}",
            PRODUCT_VERSION, (unsigned int)MP_IPC_VERSION);
        return queue_json(connection, 200, discovery);
    }
    if (strcmp(path, "/api/v1/capabilities") == 0) {
        *handled = 1;
        return queue_json(connection, 200,
            "{\"ok\":true,\"api_version\":\"" API_VERSION "\",\"capabilities\":["
            "\"status.read\",\"display.control\",\"display.preview\",\"display.brightness.preview\","
            "\"display.message\",\"display.message.delay\",\"message.notification-sound\","
            "\"alarm.configure\",\"audio.configure\","
            "\"audio.metadata\",\"audio.optimize\",\"audio.processing-status\",\"audio.queue-clear\","
            "\"stories.library\",\"stories.touch-gesture\",\"stories.intro\","
            "\"touch.input\",\"assets.images\",\"assets.bedtime-images\","
            "\"assets.read\",\"assets.download\",\"assets.upload\",\"assets.delete\",\"display.color\","
            "\"message.schedule\",\"lighting.configure\",\"lighting.global-configure\",\"lighting.preview\","
            "\"auth.optional-plaintext\",\"network.open-controls\",\"logs.read\",\"diagnostics.read\",\"diagnostics.oled\","
            "\"diagnostics.download\",\"backup.download\",\"backup.restore\",\"factory-reset\"]}");
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

    int public_api = strcmp(url, "/api/v1") == 0 || strcmp(url, "/api/v1/") == 0 ||
                     strcmp(url, "/api/v1/auth/status") == 0 ||
                     strcmp(url, "/api/v1/auth/login") == 0;
    if (!public_api && strncmp(url, "/api/v1", 7) == 0) {
        char password[WEB_PASSWORD_MAX + 1];
        if (read_web_password(password, sizeof(password)) &&
            !auth_cookie_matches(connection, password))
            return queue_json(connection, MHD_HTTP_UNAUTHORIZED,
                "{\"ok\":false,\"password_required\":true,\"error\":\"Password required\"}");
    }

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
    g_story_quota_bytes = bytes_from_env("MK_PICLOCK_STORY_QUOTA_BYTES", DEFAULT_STORY_QUOTA_BYTES);
    g_disk_reserve_bytes = bytes_from_env("MK_PICLOCK_DISK_RESERVE_BYTES", DISK_RESERVE_BYTES);

    (void)mp_asset_ensure_dir(MP_IMAGE_DIR);
    (void)mp_asset_ensure_dir(MP_BEDTIME_IMAGE_DIR);
    (void)mp_asset_ensure_dir(MP_MUSIC_DIR);
    (void)mp_asset_ensure_dir(MP_MUSIC_PROCESS_DIR);
    (void)mp_asset_ensure_dir(MP_STORY_DIR);
    (void)mp_asset_ensure_dir(MP_FONT_DIR);
    if (mp_music_jobs_start(g_music_quota_bytes, g_disk_reserve_bytes,
                            notify_processed_music, NULL) != 0) {
        fprintf(stderr, "%s: music processing worker could not start\n", API_NAME);
        return 1;
    }

    int public_port = public_port_from_env();
    if (public_port < 0) {
        fprintf(stderr, "Invalid MK_PICLOCK_API_PORT\n");
        mp_music_jobs_stop();
        return 1;
    }
    const char *public_bind = public_bind_from_env();
    struct sockaddr_in bind_address;
    memset(&bind_address, 0, sizeof(bind_address));
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons((uint16_t)public_port);
    if (inet_pton(AF_INET, public_bind, &bind_address.sin_addr) != 1) {
        fprintf(stderr, "Invalid MK_PICLOCK_API_BIND IPv4 address: %s\n", public_bind);
        mp_music_jobs_stop();
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
        mp_music_jobs_stop();
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
    mp_music_jobs_stop();
    return 0;
}
