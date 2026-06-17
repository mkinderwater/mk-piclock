/*
  mk-piclock.c

  Single-file Raspberry Pi alarm clock daemon.

  Features:
    - SSD1322 256x64 OLED over /dev/spidev0.0
    - libgpiod GPIO for DC/RST
    - Mobile-first built-in HTTP server
    - Separate web HTML, CSS, and JS assets served by C
    - PNG face upload, converted to 64x64 packed 4-bit grayscale .raw
    - MP3 upload/play/stop using libmpg123 + ALSA PCM
    - Web-configured alarms, fonts, faces, messages, bedtime dimming, and config file

  Compile:
    gcc -Wall -Wextra -O2 $(pkg-config --cflags freetype2 alsa libmpg123) mk-piclock.c -lgpiod -lpng $(pkg-config --libs freetype2 alsa libmpg123) -pthread -o mk-piclock

  Install deps:
    sudo apt install --no-install-recommends -y build-essential pkg-config libgpiod-dev gpiod libpng-dev libfreetype-dev libasound2-dev libmpg123-dev alsa-utils
*/

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <netinet/in.h>
#include <png.h>
#include <poll.h>
#include <alsa/asoundlib.h>
#include <mpg123.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <gpiod.h>

#define APP_NAME "mk-piclock"
#define APP_VERSION "1.5.23"
#define DEFAULT_CLOCK_NAME "Rylie"
#define STARTUP_GREETING_SECONDS 3
#define APP_ROOT "/opt/mk-piclock"
#define FACE_DIR APP_ROOT "/assets/faces"
#define BEDTIME_FACE_DIR APP_ROOT "/assets/bedtime-faces"
#define MUSIC_DIR APP_ROOT "/assets/music"
#define FONT_DIR APP_ROOT "/assets/fonts"
#define WEB_DIR APP_ROOT "/web"
#define CONFIG_DIR APP_ROOT "/config"
#define CONFIG_FILE CONFIG_DIR "/clock.conf"
#define LOG_FILE CONFIG_DIR "/event.log"
#define LOG_MAX_BYTES 65536
#define LOG_KEEP_BYTES 32768
#define LOG_VIEW_LINES 200


#define OLED_SPI_DEV "/dev/spidev0.0"
#define OLED_W 256
#define OLED_H 64
#define OLED_ROW_BYTES (OLED_W / 2)
#define OLED_FB_BYTES ((OLED_W * OLED_H) / 2)
#define SPI_SPEED_HZ 4000000
#define SPI_CHUNK 4096
#define OLED_CLOCK_FULL_FLUSH 1

#define MESSAGE_TEXT_X 70
#define MESSAGE_TEXT_W (OLED_W - MESSAGE_TEXT_X - 4)
#define MESSAGE_MAX_LINES 3
#define MESSAGE_LINE_CHARS 64
#define MESSAGE_INPUT_MAX_CHARS 180
#define MESSAGE_FALLBACK_SCALE 2
#define MESSAGE_TTF_MIN_PX 12
#define MESSAGE_TTF_MAX_PX 18
#define MESSAGE_DEFAULT_DURATION_SECONDS 30


#define GPIO_CHIP "/dev/gpiochip0"
#define GPIO_DC 25
#define GPIO_RST 27

#define FACE_W 64
#define FACE_H 64
#define FACE_RAW_BYTES ((FACE_W * FACE_H) / 2)
#define FACE_COUNT 64
#define MAX_ALARMS 7
#define MUSIC_FILE_MAX 256
#define FACE_FILE_MAX 128
#define WIFI_SSID_MAX 64
#define CLOCK_FACE_CHANGE_MAX_SECONDS 300
#define BEDTIME_FACE_CHANGE_SECONDS 300
#define CLOCK_TIME_Y_OFFSET -7
#define CLOCK_FACE_Y_NUDGE -2
#define CLOCK_SIDE_WIDGET_SIZE 54
#define CLOCK_SIDE_WIDGET_X 4
#define CLOCK_FACE_PREVIEW_SECONDS 15
#define CLOCK_STATUS_PILL_H 11


#define HTTP_PORT 8080
#define HTTP_MAX_REQUEST (12 * 1024 * 1024)
#define HTTP_SOCKET_TIMEOUT_SEC 5
#define HTTP_ACCEPT_POLL_MS 500
#define HTTP_MAX_WORKERS 8
#define ASSET_LIST_MAX_FILES 256
#define ASSET_LIST_NAME_MAX MUSIC_FILE_MAX

static volatile sig_atomic_t g_running = 1;
static time_t g_start_time = 0;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static char g_clock_face_cached[FACE_FILE_MAX] = "";
static time_t g_clock_face_next_change = 0;
static char g_bedtime_face_cached[FACE_FILE_MAX] = "";
static time_t g_bedtime_face_next_change = 0;

struct face_raw_cache {
    char file[FACE_FILE_MAX];
    int bedtime;
    uint8_t raw[FACE_RAW_BYTES];
    int valid;
    unsigned long generation;
};

struct asset_list_cache {
    pthread_mutex_t lock;
    int valid;
    int count;
    char files[ASSET_LIST_MAX_FILES][ASSET_LIST_NAME_MAX];
};

static pthread_mutex_t g_face_raw_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct face_raw_cache g_face_raw_cache = { .file = "", .bedtime = 0, .valid = 0, .generation = 0 };
static struct asset_list_cache g_face_list_cache = { .lock = PTHREAD_MUTEX_INITIALIZER, .valid = 0, .count = 0 };
static struct asset_list_cache g_bedtime_face_list_cache = { .lock = PTHREAD_MUTEX_INITIALIZER, .valid = 0, .count = 0 };
static struct asset_list_cache g_music_list_cache = { .lock = PTHREAD_MUTEX_INITIALIZER, .valid = 0, .count = 0 };
static struct asset_list_cache g_font_list_cache = { .lock = PTHREAD_MUTEX_INITIALIZER, .valid = 0, .count = 0 };

static pthread_mutex_t g_http_worker_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_http_active_workers = 0;

struct alarm_slot {
    int enabled;
    int hour;
    int min;
    int weekdays;          /* bit 0 Sunday through bit 6 Saturday */
    int start_volume;      /* 0..100 */
    int end_volume;        /* 0..100 */
    int fired_yday;
    char music_file[MUSIC_FILE_MAX]; /* empty = random uploaded MP3 */
};

struct app_state {
    int display_mode;       /* 0 clock, 1 clear, 2 face, 3 message */
    int display_dirty;      /* set by web thread; drawn by main OLED loop */
    int current_face;       /* legacy numeric face */
    int message_face;       /* legacy numeric face */
    char current_face_file[FACE_FILE_MAX];
    char preview_face_file[FACE_FILE_MAX];
    int preview_face_bedtime;
    time_t preview_face_until;
    char message_face_file[FACE_FILE_MAX];
    time_t message_until;   /* unix time; 0 = no active message */
    char message_text[192];
    int alarm_enabled;      /* legacy compatibility, mirrors alarm 1 */
    int alarm_hour;         /* legacy compatibility, mirrors alarm 1 */
    int alarm_min;          /* legacy compatibility, mirrors alarm 1 */
    int alarm_fired_yday;   /* legacy compatibility */
    int global_volume;
    int bedtime_enabled;
    int bedtime_start_hour;
    int bedtime_start_min;
    int bedtime_end_hour;
    int bedtime_end_min;
    int bedtime_dim_percent;
    int oled_contrast_current;
    int oled_master_current;
    int oled_brightness_current;
    int audio_playing;
    char audio_file[MUSIC_FILE_MAX];
    int alarm_active;             /* 1 only while an alarm MP3 is currently playing */
    int alarm_volume_percent;     /* current alarm ramp volume, 0..100 */
    struct alarm_slot alarms[MAX_ALARMS];
    int oled_ok;
    int http_port;
    char clock_name[64];
    int oled_font;         /* 0 seven, 1 seven thin, 2 pixel, 3 pixel bold */
    char oled_font_file[128]; /* uploaded .ttf/.otf filename, empty = built-in */
    int oled_font_size;    /* TrueType pixel size */
    int clock_24h_mode;    /* 0 = 12-hour, 1 = 24-hour */
    FT_Library ft_library;
    FT_Face ft_face;
    char ft_loaded_file[128];
    int ft_loaded_size;
    pthread_mutex_t lock;
};

static struct app_state g_state = {
    .display_mode = 0,
    .display_dirty = 1,
    .current_face = 1,
    .message_face = 1,
    .current_face_file = "",
    .preview_face_file = "",
    .preview_face_bedtime = 0,
    .preview_face_until = 0,
    .message_face_file = "",
    .message_until = 0,
    .message_text = "",
    .alarm_enabled = 0,
    .alarm_hour = 7,
    .alarm_min = 0,
    .alarm_fired_yday = -1,
    .global_volume = 80,
    .bedtime_enabled = 0,
    .bedtime_start_hour = 21,
    .bedtime_start_min = 0,
    .bedtime_end_hour = 7,
    .bedtime_end_min = 0,
    .bedtime_dim_percent = 35,
    .oled_contrast_current = -1,
    .oled_master_current = -1,
    .oled_brightness_current = -1,
    .audio_playing = 0,
    .audio_file = "",
    .alarm_active = 0,
    .alarm_volume_percent = 0,
    .oled_ok = 0,
    .http_port = HTTP_PORT,
    .clock_name = DEFAULT_CLOCK_NAME,
    .oled_font = 0,
    .oled_font_file = "",
    .oled_font_size = 48,
    .clock_24h_mode = 0,
    .ft_library = NULL,
    .ft_face = NULL,
    .ft_loaded_file = "",
    .ft_loaded_size = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

struct audio_player_state {
    pthread_mutex_t lock;
    int running;
    int stop_requested;
    char file[MUSIC_FILE_MAX];
};

static struct audio_player_state g_audio = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .running = 0,
    .stop_requested = 0,
    .file = ""
};


struct oled_dev {
    int spi_fd;
    struct gpiod_chip *chip;
    struct gpiod_line_request *gpio_req;
    uint8_t fb[OLED_FB_BYTES];
    uint8_t prev_fb[OLED_FB_BYTES];
    int prev_valid;
};

static struct oled_dev g_oled = {
    .spi_fd = -1,
    .chip = NULL,
    .gpio_req = NULL,
    .prev_valid = 0
};

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static int ensure_dir(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void safe_str(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    if (!src) src = "";

    size_t i = 0;
    while (i + 1 < dst_len && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}


static void log_trim_if_needed_locked(void) {
    struct stat st;
    if (stat(LOG_FILE, &st) != 0 || st.st_size <= LOG_MAX_BYTES) return;

    FILE *in = fopen(LOG_FILE, "rb");
    if (!in) return;

    long keep = LOG_KEEP_BYTES;
    if (st.st_size < keep) keep = (long)st.st_size;
    if (fseek(in, -keep, SEEK_END) != 0) {
        fclose(in);
        return;
    }

    char *buf = (char *)malloc((size_t)keep + 1);
    if (!buf) {
        fclose(in);
        return;
    }

    size_t got = fread(buf, 1, (size_t)keep, in);
    fclose(in);
    buf[got] = '\0';

    char *start = buf;
    if (got == (size_t)keep && st.st_size > keep) {
        char *nl = strchr(buf, '\n');
        if (nl && nl[1]) start = nl + 1;
    }

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", LOG_FILE);
    FILE *out = fopen(tmp_path, "wb");
    if (out) {
        fwrite(start, 1, strlen(start), out);
        fclose(out);
        rename(tmp_path, LOG_FILE);
    }
    free(buf);
}

static void app_log(const char *category, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    for (char *p = msg; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == '\t') *p = ' ';
    }

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    pthread_mutex_lock(&g_log_lock);
    ensure_dir(CONFIG_DIR);
    log_trim_if_needed_locked();

    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "%s [%s] %s\n", ts, category && *category ? category : "info", msg);
        fclose(f);
    }
    pthread_mutex_unlock(&g_log_lock);
}

static void sanitize_clock_name(char *s) {
    if (!s) return;

    char out[64];
    size_t j = 0;
    int last_space = 1;

    for (const unsigned char *p = (const unsigned char *)s; *p && j + 1 < sizeof(out); p++) {
        unsigned char ch = *p;
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
        if (ch < 32 || ch > 126) continue;
        if (ch == '<' || ch == '>' || ch == '&' || ch == '"') continue;
        if (ch == ' ') {
            if (last_space) continue;
            last_space = 1;
        } else {
            last_space = 0;
        }
        out[j++] = (char)ch;
    }

    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';

    if (!out[0]) snprintf(out, sizeof(out), "%s", DEFAULT_CLOCK_NAME);
    safe_str(s, 64, out);
}

static int safe_asset_filename(const char *name);
static void sanitize_asset_filename(const char *in, char *out, size_t out_len, const char *fallback);
static int save_bytes(const char *path, const uint8_t *data, size_t len);

static int face_id_valid_int(int id) {
    return id >= 1 && id <= FACE_COUNT;
}

static int has_raw_ext(const char *name) {
    const char *dot = strrchr(name ? name : "", '.');
    return dot && strcasecmp(dot, ".raw") == 0;
}

static int has_png_ext(const char *name) {
    const char *dot = strrchr(name ? name : "", '.');
    return dot && strcasecmp(dot, ".png") == 0;
}

static int safe_face_filename(const char *name) {
    return safe_asset_filename(name) && has_raw_ext(name);
}

static int make_face_path_by_file(const char *file, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    if (!safe_face_filename(file)) return -1;
    snprintf(out, out_len, FACE_DIR "/%s", file);
    return 0;
}

static int make_bedtime_face_path_by_file(const char *file, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    if (!safe_face_filename(file)) return -1;
    snprintf(out, out_len, BEDTIME_FACE_DIR "/%s", file);
    return 0;
}

static int safe_face_source_png_filename(const char *name) {
    return safe_asset_filename(name) && has_png_ext(name);
}

static int raw_face_to_source_png_name(const char *raw_file, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    if (!safe_face_filename(raw_file)) return -1;

    char tmp[FACE_FILE_MAX];
    safe_str(tmp, sizeof(tmp), raw_file);
    char *dot = strrchr(tmp, '.');
    if (!dot || strcasecmp(dot, ".raw") != 0) return -1;
    *dot = '\0';

    if (snprintf(out, out_len, "%s.png", tmp) >= (int)out_len) return -1;
    return safe_face_source_png_filename(out) ? 0 : -1;
}

static int make_source_face_png_path_by_raw(const char *raw_file, int bedtime, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';

    char png_name[FACE_FILE_MAX];
    if (raw_face_to_source_png_name(raw_file, png_name, sizeof(png_name)) != 0) return -1;

    snprintf(out, out_len, "%s/%s", bedtime ? BEDTIME_FACE_DIR : FACE_DIR, png_name);
    return 0;
}

static int source_face_png_exists(const char *raw_file, int bedtime, char *png_name, size_t png_name_len) {
    char path[512];
    if (raw_face_to_source_png_name(raw_file, png_name, png_name_len) != 0) return 0;
    if (make_source_face_png_path_by_raw(raw_file, bedtime, path, sizeof(path)) != 0) return 0;
    return access(path, R_OK) == 0;
}

static int save_source_face_png_if_missing(const char *raw_file, int bedtime, const uint8_t *data, size_t len) {
    char path[512];
    if (make_source_face_png_path_by_raw(raw_file, bedtime, path, sizeof(path)) != 0) return -1;
    if (access(path, F_OK) == 0) return 0;
    return save_bytes(path, data, len);
}

static void face_title_from_file(const char *file, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!file || !*file) {
        safe_str(out, out_len, "Face");
        return;
    }

    char tmp[FACE_FILE_MAX];
    safe_str(tmp, sizeof(tmp), file);
    char *dot = strrchr(tmp, '.');
    if (dot) *dot = '\0';

    size_t j = 0;
    int cap_next = 1;
    for (char *p = tmp; *p && j + 1 < out_len; p++) {
        char ch = *p;
        if (ch == '_' || ch == '-') {
            if (j > 0 && out[j - 1] != ' ') out[j++] = ' ';
            cap_next = 1;
            continue;
        }
        if (cap_next && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        out[j++] = ch;
        cap_next = 0;
    }
    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';
    if (!out[0]) safe_str(out, out_len, "Face");
}

static void make_raw_face_filename_from_upload(const char *upload_name, char *out, size_t out_len) {
    char clean[FACE_FILE_MAX];
    sanitize_asset_filename(upload_name, clean, sizeof(clean), "face.png");

    char *dot = strrchr(clean, '.');
    if (dot) *dot = '\0';
    if (!clean[0]) safe_str(clean, sizeof(clean), "face");

    size_t max_base = out_len > 5 ? out_len - 5 : 0;
    if (strlen(clean) > max_base) clean[max_base] = '\0';
    snprintf(out, out_len, "%s.raw", clean);
}

static int parse_int_query(const char *query, const char *key, int def) {
    if (!query || !key) return def;
    size_t key_len = strlen(key);
    const char *p = query;

    while (*p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            return atoi(p + key_len + 1);
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return def;
}

static int has_font_ext(const char *name) {
    const char *dot = strrchr(name ? name : "", '.');
    if (!dot) return 0;
    return strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0;
}

static int safe_asset_filename(const char *name) {
    if (!name || !*name || strlen(name) >= 120) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    for (const char *p = name; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (!(isalnum(ch) || ch == '.' || ch == '_' || ch == '-')) return 0;
    }
    return 1;
}

static void sanitize_asset_filename(const char *in, char *out, size_t out_len, const char *fallback) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!fallback || !*fallback) fallback = "upload.bin";

    const char *base = in && *in ? in : fallback;
    for (const char *p = base; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    size_t j = 0;
    for (const char *p = base; *p && j + 1 < out_len; p++) {
        unsigned char ch = (unsigned char)*p;
        if (isalnum(ch) || ch == '.' || ch == '_' || ch == '-') out[j++] = (char)ch;
        else out[j++] = '_';
    }
    out[j] = '\0';

    if (!out[0] || strcmp(out, ".") == 0 || strcmp(out, "..") == 0) {
        snprintf(out, out_len, "%s", fallback);
    }
}

static void make_font_path(const char *file, char *out, size_t out_len) {
    snprintf(out, out_len, FONT_DIR "/%s", file && *file ? file : "");
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int has_mp3_ext(const char *name) {
    if (!name) return 0;
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".mp3") == 0;
}

static void make_music_path(const char *file, char *out, size_t out_len) {
    snprintf(out, out_len, MUSIC_DIR "/%s", file && *file ? file : "");
}


enum asset_list_kind {
    ASSET_LIST_FACE_RAW = 1,
    ASSET_LIST_MUSIC_MP3 = 2,
    ASSET_LIST_FONT_FILE = 3
};

static void invalidate_asset_list_cache(struct asset_list_cache *cache) {
    if (!cache) return;
    pthread_mutex_lock(&cache->lock);
    cache->valid = 0;
    cache->count = 0;
    pthread_mutex_unlock(&cache->lock);
}

static void invalidate_face_raw_cache(void) {
    pthread_mutex_lock(&g_face_raw_cache_lock);
    g_face_raw_cache.valid = 0;
    g_face_raw_cache.file[0] = '\0';
    g_face_raw_cache.generation++;
    pthread_mutex_unlock(&g_face_raw_cache_lock);
}

static void invalidate_normal_face_assets(void) {
    invalidate_asset_list_cache(&g_face_list_cache);
    invalidate_face_raw_cache();
    g_clock_face_cached[0] = '\0';
    g_clock_face_next_change = 0;
}

static void invalidate_bedtime_face_assets(void) {
    invalidate_asset_list_cache(&g_bedtime_face_list_cache);
    invalidate_face_raw_cache();
    g_bedtime_face_cached[0] = '\0';
    g_bedtime_face_next_change = 0;
}

static void invalidate_all_face_assets(void) {
    invalidate_normal_face_assets();
    invalidate_bedtime_face_assets();
}

static int asset_file_matches_kind(const char *dir, const char *name, int kind) {
    if (!dir || !name) return 0;

    if (kind == ASSET_LIST_FACE_RAW) {
        if (!safe_face_filename(name)) return 0;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        struct stat st;
        if (stat(path, &st) != 0) return 0;
        return st.st_size == FACE_RAW_BYTES;
    }

    if (!safe_asset_filename(name)) return 0;
    if (kind == ASSET_LIST_MUSIC_MP3) return has_mp3_ext(name);
    if (kind == ASSET_LIST_FONT_FILE) return has_font_ext(name);
    return 0;
}

static int scan_asset_files(const char *dir, int kind, char files[][ASSET_LIST_NAME_MAX], int max_files) {
    if (!dir || !files || max_files <= 0) return 0;

    DIR *d = opendir(dir);
    if (!d) return 0;

    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < max_files) {
        if (!asset_file_matches_kind(dir, de->d_name, kind)) continue;
        safe_str(files[count], ASSET_LIST_NAME_MAX, de->d_name);
        count++;
    }
    closedir(d);

    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcasecmp(files[i], files[j]) > 0) {
                char tmp[ASSET_LIST_NAME_MAX];
                safe_str(tmp, sizeof(tmp), files[i]);
                safe_str(files[i], ASSET_LIST_NAME_MAX, files[j]);
                safe_str(files[j], ASSET_LIST_NAME_MAX, tmp);
            }
        }
    }

    return count;
}

static int collect_cached_asset_files(struct asset_list_cache *cache, const char *dir, int kind,
                                      char files[][ASSET_LIST_NAME_MAX], int max_files) {
    if (!cache || !files || max_files <= 0) return 0;

    pthread_mutex_lock(&cache->lock);
    if (!cache->valid) {
        cache->count = scan_asset_files(dir, kind, cache->files, ASSET_LIST_MAX_FILES);
        cache->valid = 1;
    }

    int count = cache->count;
    if (count > max_files) count = max_files;
    for (int i = 0; i < count; i++) {
        safe_str(files[i], ASSET_LIST_NAME_MAX, cache->files[i]);
    }
    pthread_mutex_unlock(&cache->lock);

    return count;
}

static void init_alarm_defaults(void) {
    for (int i = 0; i < MAX_ALARMS; i++) {
        g_state.alarms[i].enabled = 0;
        g_state.alarms[i].hour = 7;
        g_state.alarms[i].min = 0;
        g_state.alarms[i].weekdays = 0x7F;
        g_state.alarms[i].start_volume = 20;
        g_state.alarms[i].end_volume = 80;
        g_state.alarms[i].fired_yday = -1;
        g_state.alarms[i].music_file[0] = '\0';
    }

    g_state.alarm_enabled = g_state.alarms[0].enabled;
    g_state.alarm_hour = g_state.alarms[0].hour;
    g_state.alarm_min = g_state.alarms[0].min;
    g_state.alarm_fired_yday = g_state.alarms[0].fired_yday;
}

static void sync_legacy_alarm_fields_locked(void) {
    g_state.alarm_enabled = g_state.alarms[0].enabled;
    g_state.alarm_hour = g_state.alarms[0].hour;
    g_state.alarm_min = g_state.alarms[0].min;
    g_state.alarm_fired_yday = g_state.alarms[0].fired_yday;
}

#define CONFIG_INT_FIELDS(X) \
    X("alarm_enabled", g_state.alarms[0].enabled, "%d") \
    X("alarm_hour", g_state.alarms[0].hour, "%02d") \
    X("alarm_min", g_state.alarms[0].min, "%02d") \
    X("global_volume", g_state.global_volume, "%d") \
    X("bedtime_enabled", g_state.bedtime_enabled, "%d") \
    X("bedtime_start_hour", g_state.bedtime_start_hour, "%02d") \
    X("bedtime_start_min", g_state.bedtime_start_min, "%02d") \
    X("bedtime_end_hour", g_state.bedtime_end_hour, "%02d") \
    X("bedtime_end_min", g_state.bedtime_end_min, "%02d") \
    X("bedtime_dim_percent", g_state.bedtime_dim_percent, "%d") \
    X("oled_font", g_state.oled_font, "%d") \
    X("oled_font_size", g_state.oled_font_size, "%d") \
    X("clock_24h_mode", g_state.clock_24h_mode, "%d")

#define CONFIG_STRING_FIELDS(X) \
    X("clock_name", g_state.clock_name, sizeof(g_state.clock_name)) \
    X("oled_font_file", g_state.oled_font_file, sizeof(g_state.oled_font_file))

#define CONFIG_ALARM_INT_FIELDS(X) \
    for (int i = 0; i < MAX_ALARMS; i++) { \
        X(i, "enabled", g_state.alarms[i].enabled, "%d", atoi(val) ? 1 : 0) \
        X(i, "hour", g_state.alarms[i].hour, "%02d", atoi(val)) \
        X(i, "min", g_state.alarms[i].min, "%02d", atoi(val)) \
        X(i, "weekdays", g_state.alarms[i].weekdays, "%d", atoi(val) & 0x7F) \
        X(i, "start_volume", g_state.alarms[i].start_volume, "%d", atoi(val)) \
        X(i, "end_volume", g_state.alarms[i].end_volume, "%d", atoi(val)) \
    }

#define CONFIG_ALARM_STRING_FIELDS(X) \
    for (int i = 0; i < MAX_ALARMS; i++) { \
        X(i, "music_file", g_state.alarms[i].music_file, sizeof(g_state.alarms[i].music_file)) \
    }

static void save_config(void) {
    ensure_dir(CONFIG_DIR);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), CONFIG_FILE ".tmp");

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;

    pthread_mutex_lock(&g_state.lock);
    sync_legacy_alarm_fields_locked();
#define SAVE_CONFIG_INT(cfg_key, field, fmt) fprintf(f, cfg_key "=" fmt "\n", field);
    CONFIG_INT_FIELDS(SAVE_CONFIG_INT)
#undef SAVE_CONFIG_INT
#define SAVE_CONFIG_ALARM_INT(idx, field_str, field, fmt, conversion) \
    fprintf(f, "alarm%d_%s=" fmt "\n", (idx) + 1, field_str, field);
    CONFIG_ALARM_INT_FIELDS(SAVE_CONFIG_ALARM_INT)
#undef SAVE_CONFIG_ALARM_INT
#define SAVE_CONFIG_ALARM_STRING(idx, field_str, field, field_size) \
    fprintf(f, "alarm%d_%s=%s\n", (idx) + 1, field_str, field);
    CONFIG_ALARM_STRING_FIELDS(SAVE_CONFIG_ALARM_STRING)
#undef SAVE_CONFIG_ALARM_STRING
#define SAVE_CONFIG_STRING(cfg_key, field, field_size) fprintf(f, cfg_key "=%s\n", field);
    CONFIG_STRING_FIELDS(SAVE_CONFIG_STRING)
#undef SAVE_CONFIG_STRING
    pthread_mutex_unlock(&g_state.lock);

    int ok = 1;
    if (fflush(f) != 0) ok = 0;
    if (ok && fsync(fileno(f)) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;

    if (ok) {
        if (rename(tmp_path, CONFIG_FILE) != 0) {
            unlink(tmp_path);
        }
    } else {
        unlink(tmp_path);
    }
}

static void load_config(void) {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return;

    char line[384];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';

        int matched = 0;
        char tmp_key[128];
#define LOAD_CONFIG_ALARM_INT(idx, field_str, field, fmt, conversion) \
        do { \
            snprintf(tmp_key, sizeof(tmp_key), "alarm%d_%s", (idx) + 1, field_str); \
            if (!matched && strcmp(key, tmp_key) == 0) { \
                field = conversion; \
                matched = 1; \
            } \
        } while (0);
        CONFIG_ALARM_INT_FIELDS(LOAD_CONFIG_ALARM_INT)
#undef LOAD_CONFIG_ALARM_INT
#define LOAD_CONFIG_ALARM_STRING(idx, field_str, field, field_size) \
        do { \
            snprintf(tmp_key, sizeof(tmp_key), "alarm%d_%s", (idx) + 1, field_str); \
            if (!matched && strcmp(key, tmp_key) == 0) { \
                safe_str(field, field_size, val); \
                matched = 1; \
            } \
        } while (0);
        CONFIG_ALARM_STRING_FIELDS(LOAD_CONFIG_ALARM_STRING)
#undef LOAD_CONFIG_ALARM_STRING

#define LOAD_CONFIG_INT(cfg_key, field, fmt) \
        do { \
            if (!matched && strcmp(key, cfg_key) == 0) { \
                field = atoi(val); \
                matched = 1; \
            } \
        } while (0);
        CONFIG_INT_FIELDS(LOAD_CONFIG_INT)
#undef LOAD_CONFIG_INT
#define LOAD_CONFIG_STRING(cfg_key, field, field_size) \
        do { \
            if (!matched && strcmp(key, cfg_key) == 0) { \
                safe_str(field, field_size, val); \
                matched = 1; \
            } \
        } while (0);
        CONFIG_STRING_FIELDS(LOAD_CONFIG_STRING)
#undef LOAD_CONFIG_STRING
        if (!matched && strcmp(key, "web_font") == 0) g_state.oled_font = atoi(val); /* old config compatibility */
    }

    fclose(f);

    g_state.global_volume = clamp_int(g_state.global_volume, 0, 100);
    g_state.bedtime_enabled = g_state.bedtime_enabled ? 1 : 0;
    g_state.bedtime_start_hour = clamp_int(g_state.bedtime_start_hour, 0, 23);
    g_state.bedtime_start_min = clamp_int(g_state.bedtime_start_min, 0, 59);
    g_state.bedtime_end_hour = clamp_int(g_state.bedtime_end_hour, 0, 23);
    g_state.bedtime_end_min = clamp_int(g_state.bedtime_end_min, 0, 59);
    g_state.bedtime_dim_percent = clamp_int(g_state.bedtime_dim_percent, 0, 100);
    g_state.clock_24h_mode = g_state.clock_24h_mode ? 1 : 0;

    for (int i = 0; i < MAX_ALARMS; i++) {
        struct alarm_slot *a = &g_state.alarms[i];
        a->enabled = a->enabled ? 1 : 0;
        a->hour = clamp_int(a->hour, 0, 23);
        a->min = clamp_int(a->min, 0, 59);
        a->weekdays &= 0x7F;
        if (a->weekdays == 0) a->weekdays = 0x7F;
        a->start_volume = clamp_int(a->start_volume, 0, 100);
        a->end_volume = clamp_int(a->end_volume, 0, 100);
        a->fired_yday = -1;
        if (a->music_file[0] && (!safe_asset_filename(a->music_file) || !has_mp3_ext(a->music_file))) {
            a->music_file[0] = '\0';
        }
    }

    sanitize_clock_name(g_state.clock_name);
    if (g_state.oled_font < 0 || g_state.oled_font > 3) g_state.oled_font = 0;
    if (g_state.oled_font_size < 18 || g_state.oled_font_size > 54) g_state.oled_font_size = 48;
    sync_legacy_alarm_fields_locked();
}

/* ---------------- OLED low level ---------------- */

static int gpio_set(unsigned int offset, int value) {
    if (!g_oled.gpio_req) return -1;
    return gpiod_line_request_set_value(
        g_oled.gpio_req,
        offset,
        value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE
    );
}

static int spi_write_bytes(const uint8_t *data, size_t len) {
    if (g_oled.spi_fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > SPI_CHUNK) n = SPI_CHUNK;

        struct spi_ioc_transfer tr;
        memset(&tr, 0, sizeof(tr));
        tr.tx_buf = (unsigned long)(uintptr_t)(data + off);
        tr.len = (uint32_t)n;
        tr.speed_hz = SPI_SPEED_HZ;
        tr.bits_per_word = 8;

        if (ioctl(g_oled.spi_fd, SPI_IOC_MESSAGE(1), &tr) < 1) return -1;
        off += n;
    }
    return 0;
}

static int oled_cmd(uint8_t cmd) {
    gpio_set(GPIO_DC, 0);
    return spi_write_bytes(&cmd, 1);
}

static int oled_data(const uint8_t *data, size_t len) {
    gpio_set(GPIO_DC, 1);
    return spi_write_bytes(data, len);
}

static int oled_cmd1(uint8_t cmd, uint8_t a) {
    if (oled_cmd(cmd) != 0) return -1;
    return oled_data(&a, 1);
}

static int oled_cmd2(uint8_t cmd, uint8_t a, uint8_t b) {
    uint8_t d[2] = {a, b};
    if (oled_cmd(cmd) != 0) return -1;
    return oled_data(d, 2);
}

static int oled_set_brightness_percent(int percent) {
    percent = clamp_int(percent, 0, 100);
    if (g_oled.spi_fd < 0) return -1;

    /* SSD1322 brightness is much more obvious when both current controls move.
       0xC1 = contrast current, 0xC7 = master current. */
    int hardware_percent = percent <= 0 ? 1 : percent;
    int contrast = (127 * hardware_percent + 50) / 100;
    int master = (15 * hardware_percent + 50) / 100;
    contrast = clamp_int(contrast, 1, 127);
    master = clamp_int(master, 1, 15);

    if (oled_cmd1(0xC1, (uint8_t)contrast) != 0) return -1;
    if (oled_cmd1(0xC7, (uint8_t)master) != 0) return -1;

    pthread_mutex_lock(&g_state.lock);
    g_state.oled_contrast_current = contrast;
    g_state.oled_master_current = master;
    g_state.oled_brightness_current = percent;
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);
    g_oled.prev_valid = 0;

    fprintf(stderr, "OLED brightness set: %d%% contrast=%d master=%d software-scale=%d%%\n", percent, contrast, master, percent);
    return 0;
}

static int time_in_window_minutes(int now_min, int start_min, int end_min) {
    if (start_min == end_min) return 0;
    if (start_min < end_min) return now_min >= start_min && now_min < end_min;
    return now_min >= start_min || now_min < end_min;
}

static void apply_bedtime_brightness(void) {
    int bedtime_enabled, sh, sm, eh, em, dim_pct, current_pct;
    pthread_mutex_lock(&g_state.lock);
    bedtime_enabled = g_state.bedtime_enabled;
    sh = g_state.bedtime_start_hour;
    sm = g_state.bedtime_start_min;
    eh = g_state.bedtime_end_hour;
    em = g_state.bedtime_end_min;
    dim_pct = g_state.bedtime_dim_percent;
    current_pct = g_state.oled_brightness_current;
    pthread_mutex_unlock(&g_state.lock);

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    int now_min = tmv.tm_hour * 60 + tmv.tm_min;
    int start_min = sh * 60 + sm;
    int end_min = eh * 60 + em;
    int in_bedtime = bedtime_enabled && time_in_window_minutes(now_min, start_min, end_min);

    /* bedtime_dim_percent means percent of normal brightness during bedtime.
       Example: 20 = very dim, 35 = dim, 100 = no dimming. */
    int target_pct = in_bedtime ? clamp_int(dim_pct, 0, 100) : 100;
    if (target_pct != current_pct) {
        oled_set_brightness_percent(target_pct);
    }
}

static int is_bedtime_now(void) {
    int bedtime_enabled, sh, sm, eh, em;
    pthread_mutex_lock(&g_state.lock);
    bedtime_enabled = g_state.bedtime_enabled;
    sh = g_state.bedtime_start_hour;
    sm = g_state.bedtime_start_min;
    eh = g_state.bedtime_end_hour;
    em = g_state.bedtime_end_min;
    pthread_mutex_unlock(&g_state.lock);

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    int now_min = tmv.tm_hour * 60 + tmv.tm_min;
    return bedtime_enabled && time_in_window_minutes(now_min, sh * 60 + sm, eh * 60 + em);
}

static void oled_clear_fb(uint8_t gray4) {
    gray4 &= 0x0F;
    memset(g_oled.fb, (gray4 << 4) | gray4, sizeof(g_oled.fb));
}

static void oled_set_px(int x, int y, uint8_t gray4) {
    if (x < 0 || y < 0 || x >= OLED_W || y >= OLED_H) return;
    gray4 &= 0x0F;
    size_t idx = (size_t)(y * OLED_W + x) / 2;
    if ((x & 1) == 0) {
        g_oled.fb[idx] = (g_oled.fb[idx] & 0x0F) | (gray4 << 4);
    } else {
        g_oled.fb[idx] = (g_oled.fb[idx] & 0xF0) | gray4;
    }
}

static uint8_t oled_get_px(int x, int y) {
    if (x < 0 || y < 0 || x >= OLED_W || y >= OLED_H) return 0;
    size_t idx = (size_t)(y * OLED_W + x) / 2;
    if ((x & 1) == 0) return (g_oled.fb[idx] >> 4) & 0x0F;
    return g_oled.fb[idx] & 0x0F;
}

/*
 * FreeType gives us linear 0..255 coverage, but the SSD1322 OLED and human
 * perception make mid-gray anti-aliased pixels look too bright/fat.
 *
 * Quantize coverage to 4-bit, then apply a gamma-style LUT before blending.
 * This keeps full pixels fully bright while pulling edge pixels down so uploaded
 * TrueType/OpenType fonts look sharper on the 16-level grayscale OLED.
 *
 * Index:  linear 4-bit coverage from FreeType, 0..15
 * Value:  OLED-corrected 4-bit coverage, approx gamma 2.2
 */
static const uint8_t oled_coverage_gamma_lut[16] = {
    0, 0, 0, 1, 1, 2, 2, 3,
    4, 5, 6, 8, 9, 11, 13, 15
};

static void oled_blend_px(int x, int y, uint8_t coverage, uint8_t gray4) {
    if (coverage == 0) return;

    uint8_t fg = gray4 & 0x0F;
    if (fg == 0) return;

    uint8_t cov4 = (uint8_t)(((int)coverage * 15 + 127) / 255);
    uint8_t corrected = oled_coverage_gamma_lut[cov4];
    if (corrected == 0) return;

    uint8_t v = (uint8_t)(((int)corrected * (int)fg + 7) / 15);
    if (v > oled_get_px(x, y)) oled_set_px(x, y, v);
}

static void oled_fill_rect(int x, int y, int w, int h, uint8_t gray4) {
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            oled_set_px(xx, yy, gray4);
        }
    }
}

static int pill_row_inset(int row, int h) {
    if (h < 7) return 0;
    int mid = h / 2;
    int d = row - mid;
    if (d < 0) d = -d;

    if (d >= mid) return 4;
    if (d == mid - 1) return 2;
    if (d == mid - 2) return 1;
    return 0;
}

static void oled_draw_pill(int x, int y, int w, int h, uint8_t bg, uint8_t border) {
    if (w <= 0 || h <= 0) return;
    if (h < 7) {
        oled_fill_rect(x, y, w, h, bg);
        return;
    }

    for (int row = 0; row < h; row++) {
        int inset = pill_row_inset(row, h);
        int x1 = x + inset;
        int x2 = x + w - inset - 1;
        if (x2 < x1) continue;

        for (int xx = x1; xx <= x2; xx++) {
            oled_set_px(xx, y + row, bg);
        }

        if (row == 0 || row == h - 1) {
            for (int xx = x1; xx <= x2; xx++) oled_set_px(xx, y + row, border);
        } else {
            oled_set_px(x1, y + row, border);
            oled_set_px(x2, y + row, border);
        }
    }
}

static int oled_flush_region_bytes(int byte_start, int byte_end, int row_start, int row_end) {
    static uint8_t tmp[OLED_FB_BYTES];

    if (g_oled.spi_fd < 0) return -1;

    if (byte_start < 0) byte_start = 0;
    if (byte_end >= OLED_ROW_BYTES) byte_end = OLED_ROW_BYTES - 1;
    if (row_start < 0) row_start = 0;
    if (row_end >= OLED_H) row_end = OLED_H - 1;
    if (byte_end < byte_start || row_end < row_start) return 0;

    /* SSD1322 column addressing is effectively 4 pixels wide on these modules,
       so align dirty byte ranges to 2-byte boundaries. */
    byte_start &= ~1;
    byte_end |= 1;
    if (byte_end >= OLED_ROW_BYTES) byte_end = OLED_ROW_BYTES - 1;

    int width_bytes = byte_end - byte_start + 1;
    int height = row_end - row_start + 1;
    size_t tmp_len = (size_t)width_bytes * (size_t)height;
    if (tmp_len > sizeof(tmp)) return -1;

    for (int y = 0; y < height; y++) {
        memcpy(tmp + ((size_t)y * (size_t)width_bytes),
               g_oled.fb + ((size_t)(row_start + y) * OLED_ROW_BYTES) + byte_start,
               (size_t)width_bytes);
    }

    int brightness_percent = 100;
    pthread_mutex_lock(&g_state.lock);
    brightness_percent = g_state.oled_brightness_current;
    pthread_mutex_unlock(&g_state.lock);
    brightness_percent = clamp_int(brightness_percent < 0 ? 100 : brightness_percent, 0, 100);
    if (brightness_percent < 100) {
        for (size_t i = 0; i < tmp_len; i++) {
            uint8_t hi = (tmp[i] >> 4) & 0x0F;
            uint8_t lo = tmp[i] & 0x0F;
            hi = (uint8_t)((hi * brightness_percent + 50) / 100);
            lo = (uint8_t)((lo * brightness_percent + 50) / 100);
            if (hi > 15) hi = 15;
            if (lo > 15) lo = 15;
            tmp[i] = (uint8_t)((hi << 4) | lo);
        }
    }

    int col_start = 0x1C + (byte_start / 2);
    int col_end = 0x1C + (byte_end / 2);
    int rc = 0;

    if (oled_cmd2(0x15, (uint8_t)col_start, (uint8_t)col_end) != 0) rc = -1;
    else if (oled_cmd2(0x75, (uint8_t)row_start, (uint8_t)row_end) != 0) rc = -1;
    else if (oled_cmd(0x5C) != 0) rc = -1;
    else if (oled_data(tmp, tmp_len) != 0) rc = -1;

    return rc;
}

static int oled_flush_full(void) {
    int rc = oled_flush_region_bytes(0, OLED_ROW_BYTES - 1, 0, OLED_H - 1);
    if (rc == 0) {
        memcpy(g_oled.prev_fb, g_oled.fb, sizeof(g_oled.fb));
        g_oled.prev_valid = 1;
    }
    return rc;
}

static int oled_flush(void) {
    if (g_oled.spi_fd < 0) return -1;
    if (!g_oled.prev_valid) return oled_flush_full();

    int min_row = OLED_H;
    int max_row = -1;
    int min_byte = OLED_ROW_BYTES;
    int max_byte = -1;

    for (int y = 0; y < OLED_H; y++) {
        const uint8_t *cur = g_oled.fb + ((size_t)y * OLED_ROW_BYTES);
        const uint8_t *old = g_oled.prev_fb + ((size_t)y * OLED_ROW_BYTES);
        for (int bx = 0; bx < OLED_ROW_BYTES; bx++) {
            if (cur[bx] != old[bx]) {
                if (y < min_row) min_row = y;
                if (y > max_row) max_row = y;
                if (bx < min_byte) min_byte = bx;
                if (bx > max_byte) max_byte = bx;
            }
        }
    }

    if (max_row < min_row || max_byte < min_byte) return 0;

    int rc = oled_flush_region_bytes(min_byte, max_byte, min_row, max_row);
    if (rc == 0) memcpy(g_oled.prev_fb, g_oled.fb, sizeof(g_oled.fb));
    return rc;
}

static int oled_init(void) {
    g_oled.spi_fd = open(OLED_SPI_DEV, O_RDWR);
    if (g_oled.spi_fd < 0) {
        perror("open spi");
        return -1;
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = SPI_SPEED_HZ;

    if (ioctl(g_oled.spi_fd, SPI_IOC_WR_MODE, &mode) < 0) perror("SPI_IOC_WR_MODE");
    if (ioctl(g_oled.spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) perror("SPI bits");
    if (ioctl(g_oled.spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) perror("SPI speed");

    g_oled.chip = gpiod_chip_open(GPIO_CHIP);
    if (!g_oled.chip) {
        perror("gpiod_chip_open");
        return -1;
    }

    unsigned int offsets[2] = { GPIO_DC, GPIO_RST };

    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();

    if (!settings || !line_cfg || !req_cfg) {
        fprintf(stderr, "failed to allocate gpio request config\n");
        if (settings) gpiod_line_settings_free(settings);
        if (line_cfg) gpiod_line_config_free(line_cfg);
        if (req_cfg) gpiod_request_config_free(req_cfg);
        return -1;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);

    if (gpiod_line_config_add_line_settings(line_cfg, offsets, 2, settings) != 0) {
        perror("gpiod_line_config_add_line_settings");
        gpiod_line_settings_free(settings);
        gpiod_line_config_free(line_cfg);
        gpiod_request_config_free(req_cfg);
        return -1;
    }

    gpiod_request_config_set_consumer(req_cfg, APP_NAME);

    g_oled.gpio_req = gpiod_chip_request_lines(g_oled.chip, req_cfg, line_cfg);

    gpiod_line_settings_free(settings);
    gpiod_line_config_free(line_cfg);
    gpiod_request_config_free(req_cfg);

    if (!g_oled.gpio_req) {
        perror("gpiod_chip_request_lines");
        return -1;
    }

    gpio_set(GPIO_RST, 0);
    usleep(100000);
    gpio_set(GPIO_RST, 1);
    usleep(100000);

    oled_cmd1(0xFD, 0x12);       /* unlock */
    oled_cmd(0xAE);              /* display off */
    oled_cmd1(0xB3, 0x91);       /* clock */
    oled_cmd1(0xCA, 0x3F);       /* mux ratio 64 */
    oled_cmd1(0xA2, 0x00);       /* display offset */
    oled_cmd1(0xA1, 0x00);       /* start line */
    oled_cmd2(0xA0, 0x14, 0x11); /* remap */
    oled_cmd1(0xAB, 0x01);       /* function select */
    oled_cmd1(0xB4, 0xA0);       /* display enhancement A */
    oled_data((uint8_t[]){0xFD}, 1);
    oled_cmd1(0xC1, 0x7F);       /* contrast */
    oled_cmd1(0xC7, 0x0F);       /* master contrast */
    oled_cmd1(0xB1, 0xE2);       /* phase length */
    oled_cmd1(0xD1, 0x82);       /* display enhancement B */
    oled_cmd1(0xBB, 0x1F);       /* precharge voltage */
    oled_cmd1(0xB6, 0x08);       /* second precharge */
    oled_cmd1(0xBE, 0x07);       /* VCOMH */
    oled_cmd(0xA6);              /* normal display */
    oled_cmd(0xAF);              /* display on */

    oled_clear_fb(0);
    oled_flush();
    return 0;
}

static void oled_close(void) {
    if (g_oled.spi_fd >= 0) close(g_oled.spi_fd);
    g_oled.spi_fd = -1;
    if (g_oled.gpio_req) gpiod_line_request_release(g_oled.gpio_req);
    g_oled.gpio_req = NULL;
    if (g_oled.chip) gpiod_chip_close(g_oled.chip);
}

/* ---------------- Clock drawing ---------------- */

static void draw_seg_digit(int x, int y, int w, int h, int t, int digit, uint8_t c) {
    static const uint8_t segs[10] = {
        0x3F, /* 0 abcdef */
        0x06, /* 1 bc */
        0x5B, /* 2 abged */
        0x4F, /* 3 abgcd */
        0x66, /* 4 fgbc */
        0x6D, /* 5 afgcd */
        0x7D, /* 6 afgcde */
        0x07, /* 7 abc */
        0x7F, /* 8 */
        0x6F  /* 9 */
    };
    if (digit < 0 || digit > 9) return;
    if (t < 1) t = 1;
    uint8_t s = segs[digit];
    int mid = y + h / 2;

    if (s & 0x01) oled_fill_rect(x + t, y, w - 2 * t, t, c);              /* a */
    if (s & 0x02) oled_fill_rect(x + w - t, y + t, t, h / 2 - t, c);      /* b */
    if (s & 0x04) oled_fill_rect(x + w - t, mid, t, h / 2 - t, c);        /* c */
    if (s & 0x08) oled_fill_rect(x + t, y + h - t, w - 2 * t, t, c);      /* d */
    if (s & 0x10) oled_fill_rect(x, mid, t, h / 2 - t, c);                /* e */
    if (s & 0x20) oled_fill_rect(x, y + t, t, h / 2 - t, c);              /* f */
    if (s & 0x40) oled_fill_rect(x + t, mid - t / 2, w - 2 * t, t, c);    /* g */
}

static void draw_seg_colon(int x, int y, int dot, uint8_t c) {
    oled_fill_rect(x, y + 17, dot, dot, c);
    oled_fill_rect(x, y + 36, dot, dot, c);
}

static const uint8_t font5x7_digits[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}, /* 5 */
    {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}  /* 9 */
};


static const uint8_t font5x7_unknown[7] = {0x1F,0x11,0x01,0x02,0x04,0x00,0x04};

static const uint8_t font5x7_table[128][7] = {
    [' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['-'] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    ['.'] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
    [','] = {0x00,0x00,0x00,0x00,0x00,0x04,0x08},
    ['%'] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    [':'] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    ['\''] = {0x04,0x04,0x08,0x00,0x00,0x00,0x00},
    ['!'] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
    ['?'] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    ['_'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    ['/'] = {0x01,0x02,0x02,0x04,0x08,0x08,0x10},
    ['0'] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    ['1'] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    ['2'] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    ['3'] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
    ['4'] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    ['5'] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
    ['6'] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},
    ['7'] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    ['8'] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    ['9'] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E},
    ['A'] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['B'] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    ['C'] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    ['D'] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    ['E'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    ['F'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    ['G'] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
    ['H'] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['I'] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    ['J'] = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C},
    ['K'] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    ['L'] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    ['M'] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    ['N'] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    ['O'] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['P'] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    ['Q'] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    ['R'] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    ['S'] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    ['T'] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    ['U'] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['V'] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    ['W'] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    ['X'] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    ['Y'] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    ['Z'] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}
};

static const uint8_t font5x7_known[128] = {
    [' '] = 1, ['-'] = 1, ['.'] = 1, [','] = 1, ['%'] = 1,
    [':'] = 1, ['\''] = 1, ['!'] = 1, ['?'] = 1, ['_'] = 1, ['/'] = 1,
    ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1,
    ['5'] = 1, ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1,
    ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1, ['E'] = 1,
    ['F'] = 1, ['G'] = 1, ['H'] = 1, ['I'] = 1, ['J'] = 1,
    ['K'] = 1, ['L'] = 1, ['M'] = 1, ['N'] = 1, ['O'] = 1,
    ['P'] = 1, ['Q'] = 1, ['R'] = 1, ['S'] = 1, ['T'] = 1,
    ['U'] = 1, ['V'] = 1, ['W'] = 1, ['X'] = 1, ['Y'] = 1, ['Z'] = 1
};

static const uint8_t *font5x7_glyph(char ch) {
    unsigned char idx = (unsigned char)ch;
    if (idx >= 'a' && idx <= 'z') idx = (unsigned char)(idx - 'a' + 'A');
    if (idx < 128 && font5x7_known[idx]) return font5x7_table[idx];
    return font5x7_unknown;
}

static int text5x7_width(const char *text, int scale) {
    if (!text || !*text) return 0;
    if (scale < 1) scale = 1;
    int n = 0;
    for (const char *p = text; *p; p++) n++;
    return n * 5 * scale + (n - 1) * scale;
}

static void draw_char5x7(int x, int y, int scale, char ch, uint8_t c) {
    const uint8_t *g = font5x7_glyph(ch);
    if (scale < 1) scale = 1;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (1 << (4 - col))) {
                oled_fill_rect(x + col * scale, y + row * scale, scale, scale, c);
            }
        }
    }
}

static void draw_text5x7(int x, int y, int scale, const char *text, uint8_t c) {
    if (!text) return;
    if (scale < 1) scale = 1;
    for (const char *p = text; *p; p++) {
        draw_char5x7(x, y, scale, *p, c);
        x += 6 * scale;
    }
}

static void draw_version_corner(void) {
    char ver[24];
    snprintf(ver, sizeof(ver), "v%s", APP_VERSION);
    int w = text5x7_width(ver, 1);
    draw_text5x7(OLED_W - w - 2, OLED_H - 8, 1, ver, 8);
}


static void trim_ascii_line(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
    char *start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static int read_first_line_from_file(const char *path, char *out, size_t out_len) {
    if (!path || !out || out_len == 0) return -1;
    out[0] = '\0';

    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    if (!fgets(out, (int)out_len, fp)) {
        fclose(fp);
        out[0] = '\0';
        return -1;
    }

    fclose(fp);
    trim_ascii_line(out);
    return out[0] ? 0 : -1;
}

static int wifi_connected_kernel(void) {
    char line[256];

    if (read_first_line_from_file("/sys/class/net/wlan0/operstate", line, sizeof(line)) == 0) {
        if (strcmp(line, "up") == 0) return 1;
        if (strcmp(line, "down") == 0 || strcmp(line, "dormant") == 0 || strcmp(line, "notpresent") == 0) return 0;
    }

    FILE *fp = fopen("/proc/net/wireless", "r");
    if (!fp) return 0;

    int connected = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *name = strstr(line, "wlan0:");
        if (!name) continue;

        char *colon = strchr(name, ':');
        if (!colon) break;

        unsigned int status = 0;
        double link = 0.0;
        if (sscanf(colon + 1, " %x %lf", &status, &link) >= 2 && link > 0.0) {
            connected = 1;
        }
        break;
    }

    fclose(fp);
    return connected;
}

static void get_wifi_ssid_cached(char *out, size_t out_len) {
    static pthread_mutex_t wifi_lock = PTHREAD_MUTEX_INITIALIZER;
    static int cached_connected = -1;
    static time_t last_read = 0;

    if (!out || out_len == 0) return;
    out[0] = '\0';

    time_t now = time(NULL);

    pthread_mutex_lock(&wifi_lock);
    if (cached_connected >= 0 && now - last_read < 60) {
        safe_str(out, out_len, cached_connected ? "WIFI" : "NO WIFI");
        pthread_mutex_unlock(&wifi_lock);
        return;
    }

    cached_connected = wifi_connected_kernel();
    last_read = now;
    safe_str(out, out_len, cached_connected ? "WIFI" : "NO WIFI");
    pthread_mutex_unlock(&wifi_lock);
}

static void draw_wifi_icon_small(int x, int y, uint8_t c) {
    static const uint16_t rows[7] = {
        0x07C, /*  .#####.  */
        0x082, /* #.....#  */
        0x038, /*  .###.   */
        0x044, /* .#...#.  */
        0x010, /*   .#.    */
        0x028, /*  .#.#.   */
        0x010  /*   .#.    */
    };

    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 9; col++) {
            if (rows[row] & (1 << (8 - col))) {
                oled_set_px(x + col, y + row, c);
            }
        }
    }
}

static void draw_music_note_icon_small(int x, int y, uint8_t c) {
    /* Simple 7x9 eighth-note style icon, OLED-safe at 1px strokes. */
    for (int yy = 0; yy < 7; yy++) oled_set_px(x + 4, y + yy, c);
    for (int xx = 4; xx <= 7; xx++) oled_set_px(x + xx, y, c);
    for (int xx = 5; xx <= 8; xx++) oled_set_px(x + xx, y + 1, c);
    oled_set_px(x + 8, y + 2, c);

    oled_set_px(x + 1, y + 6, c);
    oled_set_px(x + 2, y + 5, c);
    oled_set_px(x + 3, y + 5, c);
    oled_set_px(x + 4, y + 6, c);
    oled_set_px(x + 3, y + 7, c);
    oled_set_px(x + 2, y + 7, c);
}

static void draw_status_pills(int alarm_on, int alarm_active, int alarm_volume_percent) {
    char ssid[WIFI_SSID_MAX];
    get_wifi_ssid_cached(ssid, sizeof(ssid));

    const int pill_h = CLOCK_STATUS_PILL_H;
    const int pad_x = 5;
    const int icon_w = 9;
    const int icon_pill_w = icon_w + pad_x * 2;
    const int pill_gap = 3;
    const int bottom_y = OLED_H - pill_h;

    /* Bottom-left status group. Wi-Fi first, alarm/music immediately to its right.
       Keeping all status pills left leaves the clock/date zone clean. */
    int connected = (ssid[0] && strcmp(ssid, "NO WIFI") != 0);
    int wifi_x = 2;
    int wifi_y = bottom_y;
    oled_draw_pill(wifi_x, wifi_y, icon_pill_w, pill_h, connected ? 2 : 1, connected ? 8 : 5);
    draw_wifi_icon_small(wifi_x + pad_x, wifi_y + 2, connected ? 15 : 6);

    if (!connected) {
        /* Small diagonal slash for disconnected state. */
        for (int i = 0; i < 9; i++) {
            oled_set_px(wifi_x + pad_x + i, wifi_y + 2 + i / 2, 9);
        }
    }

    int next_x = wifi_x + icon_pill_w + pill_gap;
    if (alarm_active) {
        char label[12];
        snprintf(label, sizeof(label), "%d%%", clamp_int(alarm_volume_percent, 0, 100));
        int text_w = text5x7_width(label, 1);
        int note_pill_w = pad_x + icon_w + 3 + text_w + pad_x;
        if (next_x + note_pill_w < OLED_W - 2) {
            oled_draw_pill(next_x, bottom_y, note_pill_w, pill_h, 2, 8);
            draw_music_note_icon_small(next_x + pad_x, bottom_y + 2, 15);
            draw_text5x7(next_x + pad_x + icon_w + 3, bottom_y + 2, 1, label, 15);
        }
    } else if (alarm_on) {
        int note_pill_w = icon_w + pad_x * 2;
        if (next_x + note_pill_w < OLED_W - 2) {
            oled_draw_pill(next_x, bottom_y, note_pill_w, pill_h, 2, 8);
            draw_music_note_icon_small(next_x + pad_x, bottom_y + 2, 15);
        }
    }
}
static void draw_startup_screen(void) {
    char name[64];
    pthread_mutex_lock(&g_state.lock);
    safe_str(name, sizeof(name), g_state.clock_name);
    pthread_mutex_unlock(&g_state.lock);
    sanitize_clock_name(name);

    char msg[96];
    snprintf(msg, sizeof(msg), "Hi %s", name);

    int scale = 3;
    while (scale > 1 && text5x7_width(msg, scale) > OLED_W - 14) scale--;

    int max_chars = (OLED_W - 14 + scale) / (6 * scale);
    if (max_chars < 4) max_chars = 4;
    if ((int)strlen(msg) > max_chars) {
        if (max_chars > 3) {
            msg[max_chars - 3] = '.';
            msg[max_chars - 2] = '.';
            msg[max_chars - 1] = '.';
            msg[max_chars] = '\0';
        } else {
            msg[max_chars] = '\0';
        }
    }

    oled_clear_fb(0);
    int w = text5x7_width(msg, scale);
    int x = (OLED_W - w) / 2;
    if (x < 0) x = 0;
    int y = (OLED_H - (7 * scale)) / 2 - 4;
    if (y < 0) y = 0;
    draw_text5x7(x, y, scale, msg, 15);
    draw_version_corner();
    oled_flush_full();
}

static void draw_pixel_digit(int x, int y, int sx, int sy, int digit, uint8_t c, int bold) {
    if (digit < 0 || digit > 9) return;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = font5x7_digits[digit][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (1 << (4 - col))) {
                int bw = sx + (bold ? 1 : 0);
                oled_fill_rect(x + col * sx, y + row * sy, bw, sy, c);
            }
        }
    }
}

static void draw_pixel_colon(int x, int y, int sx, int sy, uint8_t c, int bold) {
    int dot_w = sx + (bold ? 1 : 0);
    oled_fill_rect(x, y + sy * 2, dot_w, sy, c);
    oled_fill_rect(x, y + sy * 4, dot_w, sy, c);
}

static void font_cache_close_locked(void) {
    if (g_state.ft_face) {
        FT_Done_Face(g_state.ft_face);
        g_state.ft_face = NULL;
    }
    if (g_state.ft_library) {
        FT_Done_FreeType(g_state.ft_library);
        g_state.ft_library = NULL;
    }
    g_state.ft_loaded_file[0] = '\0';
    g_state.ft_loaded_size = 0;
}

static int font_cache_ensure_locked(const char *font_file, const char *font_path, int px_size) {
    if (!font_file || !*font_file || !font_path || !*font_path) return -1;
    if (px_size < 8) px_size = 8;
    if (px_size > 72) px_size = 72;

    if (g_state.ft_face &&
        strcmp(g_state.ft_loaded_file, font_file) == 0 &&
        g_state.ft_loaded_size == px_size) {
        return 0;
    }

    if (g_state.ft_face) {
        FT_Done_Face(g_state.ft_face);
        g_state.ft_face = NULL;
    }

    if (!g_state.ft_library) {
        if (FT_Init_FreeType(&g_state.ft_library) != 0) {
            g_state.ft_library = NULL;
            return -1;
        }
    }

    if (FT_New_Face(g_state.ft_library, font_path, 0, &g_state.ft_face) != 0) {
        g_state.ft_face = NULL;
        g_state.ft_loaded_file[0] = '\0';
        g_state.ft_loaded_size = 0;
        return -1;
    }

    if (FT_Set_Pixel_Sizes(g_state.ft_face, 0, (FT_UInt)px_size) != 0) {
        FT_Done_Face(g_state.ft_face);
        g_state.ft_face = NULL;
        g_state.ft_loaded_file[0] = '\0';
        g_state.ft_loaded_size = 0;
        return -1;
    }

    safe_str(g_state.ft_loaded_file, sizeof(g_state.ft_loaded_file), font_file);
    g_state.ft_loaded_size = px_size;
    return 0;
}


static int draw_clock_truetype_time_fixed_centered(const char *font_file, int px_size, int hour, int minute, int center_x, int show_leading_zero, uint8_t colon_level);
static int clock_colon_blink_phase(void) {
    /* Simple 1-second blink: one second on, one second off. */
    time_t now = time(NULL);
    return (int)(now & 1);
}

static uint8_t clock_colon_blink_level(void) {
    /* Fully visible for one second, fully hidden for one second. */
    return clock_colon_blink_phase() ? 15 : 0;
}

/*
   Draw the main clock time using fixed slots.

   This is deliberately different from normal centered text rendering. With
   proportional TrueType fonts, "1:01" and "10:01" have different widths, so
   the clock visibly jumps left/right. We reserve the full HH:MM layout every
   time. For single-digit hours, the leading zero is drawn as black/invisible,
   but it still occupies its slot:

       visible 1:01 is laid out as invisible-0 + 1:01

   Result: the left and right distances stay constant without shrinking the
   font.
*/
static int draw_clock_truetype_time_fixed_centered(const char *font_file, int px_size, int hour, int minute, int center_x, int show_leading_zero, uint8_t colon_level) {
    if (!font_file || !*font_file) return -1;

    char font_path[512];
    make_font_path(font_file, font_path, sizeof(font_path));

    pthread_mutex_lock(&g_state.lock);
    if (font_cache_ensure_locked(font_file, font_path, px_size) != 0 || !g_state.ft_face) {
        pthread_mutex_unlock(&g_state.lock);
        return -1;
    }

    FT_Face face = g_state.ft_face;

    int digit_slot = 0;
    int colon_slot = 0;
    int min_y = 99999;
    int max_y = -99999;

    for (char c = '0'; c <= '9'; c++) {
        if (FT_Load_Char(face, c, FT_LOAD_RENDER) != 0) continue;
        FT_GlyphSlot g = face->glyph;
        int adv = (int)((g->advance.x + 32) >> 6);
        int vis = (int)g->bitmap.width + abs(g->bitmap_left);
        if (adv < vis) adv = vis;
        if (adv > digit_slot) digit_slot = adv;

        int gy1 = g->bitmap_top;
        int gy0 = gy1 - (int)g->bitmap.rows;
        if (gy0 < min_y) min_y = gy0;
        if (gy1 > max_y) max_y = gy1;
    }

    if (FT_Load_Char(face, ':', FT_LOAD_RENDER) == 0) {
        FT_GlyphSlot g = face->glyph;
        colon_slot = (int)((g->advance.x + 32) >> 6);
        int vis = (int)g->bitmap.width + abs(g->bitmap_left);
        if (colon_slot < vis) colon_slot = vis;

        int gy1 = g->bitmap_top;
        int gy0 = gy1 - (int)g->bitmap.rows;
        if (gy0 < min_y) min_y = gy0;
        if (gy1 > max_y) max_y = gy1;
    }

    if (digit_slot <= 0 || colon_slot <= 0 || max_y <= min_y) {
        pthread_mutex_unlock(&g_state.lock);
        return -1;
    }

    const int slot_gap = 1;

    /*
       In 24-hour mode we keep a full HH:MM footprint.

       In 12-hour mode, a single-digit hour should not reserve a visible-width
       leading slot. The earlier invisible-leading-zero trick kept the digits
       stable, but it also made 1:05 look shifted right and left too much blank
       space between the face and the time. Instead, keep each displayed digit
       in a fixed digit slot, but center the actual visible H:MM or HH:MM
       footprint.
    */
    if (hour < 0) hour = 0;
    else if (hour > 99) hour = 99;
    if (minute < 0) minute = 0;
    else if (minute > 99) minute = 99;

    int visible_hour_digits = (show_leading_zero || hour >= 10) ? 2 : 1;
    int total_w = digit_slot * (visible_hour_digits + 2) + colon_slot + slot_gap * (visible_hour_digits + 2);

    char time_text[8];
    if (visible_hour_digits == 1) snprintf(time_text, sizeof(time_text), "%d:%02d", hour, minute);
    else snprintf(time_text, sizeof(time_text), "%02d:%02d", hour, minute);

    /*
       Centre the actual visible glyph bounds, not just the logical slot width.
       Some TrueType fonts have uneven side bearings, especially around "1" and
       ":". Slot-only centring can look too far left even when the math is
       technically centred. Measuring the rendered bounds here keeps 1-digit and
       2-digit 12-hour times visually centred while retaining fixed digit slots.
    */
    int rel_min_x = 99999;
    int rel_max_x = -99999;
    int measure_slot_x = 0;
    for (int i = 0; time_text[i]; i++) {
        char c = time_text[i];
        int slot_w = (c == ':') ? colon_slot : digit_slot;
        if (FT_Load_Char(face, c, FT_LOAD_RENDER) == 0) {
            FT_GlyphSlot g = face->glyph;
            FT_Bitmap *bm = &g->bitmap;
            int adv = (int)((g->advance.x + 32) >> 6);
            if (adv <= 0) adv = (int)bm->width;
            int pen_x = measure_slot_x + ((slot_w - adv) / 2);
            int gx0 = pen_x + g->bitmap_left;
            int gx1 = gx0 + (int)bm->width;
            if (bm->width > 0) {
                if (gx0 < rel_min_x) rel_min_x = gx0;
                if (gx1 > rel_max_x) rel_max_x = gx1;
            }
        }
        measure_slot_x += slot_w + slot_gap;
    }

    int start_x;
    if (rel_max_x > rel_min_x) {
        int visual_center_twice = rel_min_x + rel_max_x;
        start_x = center_x - ((visual_center_twice + 1) / 2);
    } else {
        start_x = center_x - (total_w / 2);
    }

    int text_h = max_y - min_y;
    const int time_band_top = 0;
    const int time_band_bottom = OLED_H - 11; /* fixed date line lives below */
    int top_y = ((OLED_H - text_h) / 2) + CLOCK_TIME_Y_OFFSET;
    if (top_y < time_band_top) top_y = time_band_top;
    if (top_y + text_h > time_band_bottom) top_y = time_band_bottom - text_h;
    if (top_y < 0) top_y = 0;
    int baseline_y = top_y + max_y;

    int slot_x = start_x;
    for (int i = 0; time_text[i]; i++) {
        char c = time_text[i];
        int slot_w = (c == ':') ? colon_slot : digit_slot;
        int visible = 1;

        if (FT_Load_Char(face, c, FT_LOAD_RENDER) == 0) {
            FT_GlyphSlot g = face->glyph;
            FT_Bitmap *bm = &g->bitmap;
            int adv = (int)((g->advance.x + 32) >> 6);
            if (adv <= 0) adv = (int)bm->width;

            int pen_x = slot_x + ((slot_w - adv) / 2);
            int gx = pen_x + g->bitmap_left;
            int gy = baseline_y - g->bitmap_top;
            uint8_t fg = (c == ':') ? colon_level : (visible ? 15 : 0);

            if (fg > 0) {
                for (unsigned int row = 0; row < bm->rows; row++) {
                    for (unsigned int col = 0; col < bm->width; col++) {
                        uint8_t coverage = 0;
                        if (bm->pixel_mode == FT_PIXEL_MODE_GRAY) {
                            coverage = bm->buffer[row * bm->pitch + col];
                        } else if (bm->pixel_mode == FT_PIXEL_MODE_MONO) {
                            uint8_t byte = bm->buffer[row * bm->pitch + (col >> 3)];
                            coverage = (byte & (0x80 >> (col & 7))) ? 255 : 0;
                        }
                        oled_blend_px(gx + (int)col, gy + (int)row, coverage, fg);
                    }
                }
            }
        }

        slot_x += slot_w + slot_gap;
    }

    pthread_mutex_unlock(&g_state.lock);
    return 0;
}


static const char *date_ordinal_suffix(int day) {
    int mod100 = day % 100;
    if (mod100 >= 11 && mod100 <= 13) return "th";

    switch (day % 10) {
        case 1: return "st";
        case 2: return "nd";
        case 3: return "rd";
        default: return "th";
    }
}

static void format_long_date(const struct tm *tmv, char *out, size_t out_len) {
    if (!tmv || !out || out_len == 0) return;

    static const char *days[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
    };
    static const char *months[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    int wday = tmv->tm_wday;
    int mon = tmv->tm_mon;
    if (wday < 0 || wday > 6) wday = 0;
    if (mon < 0 || mon > 11) mon = 0;

    snprintf(
        out,
        out_len,
        "%s, %s %d%s %04d",
        days[wday],
        months[mon],
        tmv->tm_mday,
        date_ordinal_suffix(tmv->tm_mday),
        tmv->tm_year + 1900
    );
}

static void draw_long_date_centered_at(const struct tm *tmv, const char *font_file, int center_x) {
    (void)font_file;
    if (!tmv) return;

    char datestr[96];
    format_long_date(tmv, datestr, sizeof(datestr));

    /*
       Keep the date/status text on the fixed 5x7 OLED font.
       User-uploaded TrueType fonts are only used for the main time/message text.
       This prevents tall fonts from clipping the date line.

       The date is centred under the visible clock anchor, not the raw OLED
       centre. With the invisible leading-zero trick, single-digit hours are
       visually shifted right, so the date follows that perceived centre.
    */
    char upper[96];
    size_t i = 0;
    for (; datestr[i] && i + 1 < sizeof(upper); i++) {
        upper[i] = (char)toupper((unsigned char)datestr[i]);
    }
    upper[i] = '\0';

    int w = text5x7_width(upper, 1);
    int x = center_x - (w / 2);
    if (x < 0) x = 0;
    if (x + w >= OLED_W) x = OLED_W - w - 1;
    if (x < 0) x = 0;
    draw_text5x7(x, OLED_H - 8, 1, upper, 9);
}

static int face_raw_pixel(const uint8_t *raw, int x, int y) {
    if (!raw || x < 0 || y < 0 || x >= FACE_W || y >= FACE_H) return 0;
    uint8_t b = raw[(y * FACE_W + x) / 2];
    return (x & 1) ? (b & 0x0F) : ((b >> 4) & 0x0F);
}

static int load_face_raw_uncached_by_file(const char *file, int bedtime, uint8_t *raw, size_t raw_len) {
    if (!raw || raw_len < FACE_RAW_BYTES) return -1;
    char path[512];
    int ok = bedtime
        ? make_bedtime_face_path_by_file(file, path, sizeof(path))
        : make_face_path_by_file(file, path, sizeof(path));
    if (ok != 0) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(raw, 1, FACE_RAW_BYTES, f);
    fclose(f);
    return n == FACE_RAW_BYTES ? 0 : -1;
}

static int load_face_raw_cached_by_file(const char *file, int bedtime, uint8_t *raw, size_t raw_len) {
    if (!raw || raw_len < FACE_RAW_BYTES || !safe_face_filename(file)) return -1;

    pthread_mutex_lock(&g_face_raw_cache_lock);
    unsigned long generation = g_face_raw_cache.generation;
    if (g_face_raw_cache.valid && g_face_raw_cache.bedtime == bedtime && strcmp(g_face_raw_cache.file, file) == 0) {
        memcpy(raw, g_face_raw_cache.raw, FACE_RAW_BYTES);
        pthread_mutex_unlock(&g_face_raw_cache_lock);
        return 0;
    }
    pthread_mutex_unlock(&g_face_raw_cache_lock);

    uint8_t tmp[FACE_RAW_BYTES];
    if (load_face_raw_uncached_by_file(file, bedtime, tmp, sizeof(tmp)) != 0) return -1;

    pthread_mutex_lock(&g_face_raw_cache_lock);
    if (g_face_raw_cache.generation == generation) {
        safe_str(g_face_raw_cache.file, sizeof(g_face_raw_cache.file), file);
        g_face_raw_cache.bedtime = bedtime ? 1 : 0;
        memcpy(g_face_raw_cache.raw, tmp, FACE_RAW_BYTES);
        g_face_raw_cache.valid = 1;
    }
    pthread_mutex_unlock(&g_face_raw_cache_lock);

    memcpy(raw, tmp, FACE_RAW_BYTES);
    return 0;
}

static int load_face_raw_by_file(const char *file, uint8_t *raw, size_t raw_len) {
    return load_face_raw_cached_by_file(file, 0, raw, raw_len);
}

static int load_bedtime_face_raw_by_file(const char *file, uint8_t *raw, size_t raw_len) {
    return load_face_raw_cached_by_file(file, 1, raw, raw_len);
}

static int collect_uploaded_face_files(char files[][ASSET_LIST_NAME_MAX], int max_files) {
    return collect_cached_asset_files(&g_face_list_cache, FACE_DIR, ASSET_LIST_FACE_RAW, files, max_files);
}

static int collect_bedtime_face_files(char files[][ASSET_LIST_NAME_MAX], int max_files) {
    return collect_cached_asset_files(&g_bedtime_face_list_cache, BEDTIME_FACE_DIR, ASSET_LIST_FACE_RAW, files, max_files);
}

static int collect_music_files(char files[][ASSET_LIST_NAME_MAX], int max_files) {
    return collect_cached_asset_files(&g_music_list_cache, MUSIC_DIR, ASSET_LIST_MUSIC_MP3, files, max_files);
}

static int collect_font_files(char files[][ASSET_LIST_NAME_MAX], int max_files) {
    return collect_cached_asset_files(&g_font_list_cache, FONT_DIR, ASSET_LIST_FONT_FILE, files, max_files);
}

static int random_uploaded_face_file(char *out, size_t out_len) {
    char files[ASSET_LIST_MAX_FILES][ASSET_LIST_NAME_MAX];
    int count = collect_uploaded_face_files(files, ASSET_LIST_MAX_FILES);
    if (count <= 0) return -1;
    safe_str(out, out_len, files[rand() % count]);
    return 0;
}

static int random_bedtime_face_file(char *out, size_t out_len) {
    char files[ASSET_LIST_MAX_FILES][ASSET_LIST_NAME_MAX];
    int count = collect_bedtime_face_files(files, ASSET_LIST_MAX_FILES);
    if (count <= 0) return -1;
    safe_str(out, out_len, files[rand() % count]);
    return 0;
}

static int sticky_clock_face_file(char *out, size_t out_len) {
    time_t now = time(NULL);

    if (!out || out_len == 0) return -1;
    out[0] = '\0';

    if (g_clock_face_cached[0] && now < g_clock_face_next_change) {
        uint8_t test[FACE_RAW_BYTES];
        if (load_face_raw_by_file(g_clock_face_cached, test, sizeof(test)) == 0) {
            safe_str(out, out_len, g_clock_face_cached);
            return 0;
        }
    }

    if (random_uploaded_face_file(g_clock_face_cached, sizeof(g_clock_face_cached)) == 0) {
        g_clock_face_next_change = now + (rand() % (CLOCK_FACE_CHANGE_MAX_SECONDS + 1));
        safe_str(out, out_len, g_clock_face_cached);
        return 0;
    }

    g_clock_face_cached[0] = '\0';
    g_clock_face_next_change = now + 60;
    return -1;
}

static int sticky_bedtime_face_file(char *out, size_t out_len) {
    time_t now = time(NULL);

    if (!out || out_len == 0) return -1;
    out[0] = '\0';

    if (g_bedtime_face_cached[0] && now < g_bedtime_face_next_change) {
        uint8_t test[FACE_RAW_BYTES];
        if (load_bedtime_face_raw_by_file(g_bedtime_face_cached, test, sizeof(test)) == 0) {
            safe_str(out, out_len, g_bedtime_face_cached);
            return 0;
        }
    }

    if (random_bedtime_face_file(g_bedtime_face_cached, sizeof(g_bedtime_face_cached)) == 0) {
        g_bedtime_face_next_change = now + BEDTIME_FACE_CHANGE_SECONDS;
        safe_str(out, out_len, g_bedtime_face_cached);
        return 0;
    }

    g_bedtime_face_cached[0] = '\0';
    g_bedtime_face_next_change = now + 60;
    return -1;
}

static int clock_face_refresh_due(void) {
    time_t now = time(NULL);
    if (is_bedtime_now()) return now >= g_bedtime_face_next_change;
    return now >= g_clock_face_next_change;
}

static int draw_face_thumb_raw(const uint8_t *raw, int ox, int oy, int size) {
    if (!raw || size <= 0) return -1;

    for (int y = 0; y < size; y++) {
        int sy = (y * FACE_H) / size;
        for (int x = 0; x < size; x++) {
            int sx = (x * FACE_W) / size;
            oled_set_px(ox + x, oy + y, (uint8_t)face_raw_pixel(raw, sx, sy));
        }
    }

    return 0;
}

static int draw_face_thumb_by_file(const char *file, int ox, int oy, int size) {
    uint8_t raw[FACE_RAW_BYTES];
    if (load_face_raw_by_file(file, raw, sizeof(raw)) != 0) return -1;
    return draw_face_thumb_raw(raw, ox, oy, size);
}

static int draw_bedtime_face_thumb_by_file(const char *file, int ox, int oy, int size) {
    uint8_t raw[FACE_RAW_BYTES];
    if (load_bedtime_face_raw_by_file(file, raw, sizeof(raw)) != 0) return -1;
    return draw_face_thumb_raw(raw, ox, oy, size);
}

static void draw_clock_screen(void) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    int raw_hour = tmv.tm_hour;
    int minute = tmv.tm_min;
    int clock_24h_mode = 0;
    int hour = raw_hour;

    pthread_mutex_lock(&g_state.lock);
    int alarm_on = 0;
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (g_state.alarms[i].enabled) {
            alarm_on = 1;
            break;
        }
    }
    int oled_font = g_state.oled_font;
    int alarm_active = g_state.alarm_active;
    int alarm_volume_percent = g_state.alarm_volume_percent;
    clock_24h_mode = g_state.clock_24h_mode;
    char preview_face_file[FACE_FILE_MAX];
    int preview_face_bedtime = g_state.preview_face_bedtime;
    time_t preview_face_until = g_state.preview_face_until;
    safe_str(preview_face_file, sizeof(preview_face_file), g_state.preview_face_file);
    char oled_font_file[128];
    int oled_font_size = g_state.oled_font_size;
    int clock_font_size = clamp_int(oled_font_size, 18, 54);
    safe_str(oled_font_file, sizeof(oled_font_file), g_state.oled_font_file);
    pthread_mutex_unlock(&g_state.lock);

    if (!clock_24h_mode) {
        hour = raw_hour;
        if (hour == 0) hour = 12;
        else if (hour > 12) hour -= 12;
    } else {
        hour = raw_hour;
    }

    int h1 = hour / 10;
    int h2 = hour % 10;
    int m1 = minute / 10;
    int m2 = minute % 10;

    oled_clear_fb(0);
    uint8_t colon_level = clock_colon_blink_level();

    int bedtime = is_bedtime_now();
    int face_x = CLOCK_SIDE_WIDGET_X;
    /* Keep the face visually centered in the usable area above the bottom status pills.
       The small negative nudge compensates for most face art having more visual weight
       in the lower half. */
    int face_area_h = OLED_H - CLOCK_STATUS_PILL_H;
    int face_y = ((face_area_h - CLOCK_SIDE_WIDGET_SIZE) / 2) + CLOCK_FACE_Y_NUDGE;
    int face_right = face_x + CLOCK_SIDE_WIDGET_SIZE;
    int clock_center_x = (face_right + OLED_W) / 2;
    char random_face_file[FACE_FILE_MAX];
    time_t now_ts = time(NULL);
    if (preview_face_file[0] && now_ts < preview_face_until) {
        if (preview_face_bedtime) draw_bedtime_face_thumb_by_file(preview_face_file, face_x, face_y, CLOCK_SIDE_WIDGET_SIZE);
        else draw_face_thumb_by_file(preview_face_file, face_x, face_y, CLOCK_SIDE_WIDGET_SIZE);
    } else if (preview_face_file[0] && now_ts >= preview_face_until) {
        pthread_mutex_lock(&g_state.lock);
        g_state.preview_face_file[0] = '\0';
        g_state.preview_face_until = 0;
        g_state.preview_face_bedtime = 0;
        pthread_mutex_unlock(&g_state.lock);
    } else if (bedtime) {
        if (sticky_bedtime_face_file(random_face_file, sizeof(random_face_file)) == 0) {
            draw_bedtime_face_thumb_by_file(random_face_file, face_x, face_y, CLOCK_SIDE_WIDGET_SIZE);
        }
    } else {
        if (sticky_clock_face_file(random_face_file, sizeof(random_face_file)) == 0) {
            draw_face_thumb_by_file(random_face_file, face_x, face_y, CLOCK_SIDE_WIDGET_SIZE);
        }
    }

    int used_ttf = 0;
    if (oled_font_file[0]) {
        if (draw_clock_truetype_time_fixed_centered(oled_font_file, clock_font_size, hour, minute, clock_center_x, clock_24h_mode, colon_level) == 0) used_ttf = 1;
    }

    if (!used_ttf && (oled_font == 2 || oled_font == 3)) {
        int bold = (oled_font == 3);
        int sx = bold ? 6 : 5;
        int sy = 6;
        int digit_w = 5 * sx;
        int gap = bold ? 7 : 6;
        int colon_w = sx + (bold ? 1 : 0);
        int visible_hour_digits = (clock_24h_mode || h1 > 0) ? 2 : 1;
        int total_w = digit_w * (visible_hour_digits + 2) + gap * (visible_hour_digits + 2) + colon_w;
        int x = clock_center_x - (total_w / 2);
        int y = 0;

        if (visible_hour_digits == 2) {
            draw_pixel_digit(x, y, sx, sy, h1, 15, bold);
            x += digit_w + gap;
        }
        draw_pixel_digit(x, y, sx, sy, h2, 15, bold);
        x += digit_w + gap;
        draw_pixel_colon(x, y, sx, sy, colon_level, bold);
        x += colon_w + gap;
        draw_pixel_digit(x, y, sx, sy, m1, 15, bold);
        x += digit_w + gap;
        draw_pixel_digit(x, y, sx, sy, m2, 15, bold);
    } else if (!used_ttf) {
        int y = 0;
        int w = 24;
        int h = 43;
        int t = (oled_font == 1) ? 3 : 4;
        int visible_hour_digits = (clock_24h_mode || h1 > 0) ? 2 : 1;
        int total_w = (visible_hour_digits == 2) ? 126 : 97;
        int x = clock_center_x - (total_w / 2);

        if (visible_hour_digits == 2) {
            draw_seg_digit(x, y, w, h, t, h1, 15);
            x += 29;
        }
        draw_seg_digit(x, y, w, h, t, h2, 15);
        x += 31;
        draw_seg_colon(x, y, (oled_font == 1) ? 4 : 5, colon_level);
        x += 12;
        draw_seg_digit(x, y, w, h, t, m1, 15);
        x += 29;
        draw_seg_digit(x, y, w, h, t, m2, 15);
    }

    draw_long_date_centered_at(&tmv, oled_font_file, clock_center_x);
    draw_status_pills(alarm_on, alarm_active, alarm_volume_percent);

    /*
       The clock screen contains a 54x54 face beside the time, a blinking colon, bottom-left Wi-Fi/alarm status group. On SSD1322 modules,
       small partial updates around packed 4-bit graphics can occasionally leave
       edge noise, so a full flush is the cleaner and safer choice.
    */
#if OLED_CLOCK_FULL_FLUSH
    oled_flush_full();
#else
    oled_flush();
#endif
}

static int draw_face_screen_file(const char *file) {
    uint8_t raw[FACE_RAW_BYTES];
    if (load_face_raw_by_file(file, raw, sizeof(raw)) != 0) return -1;

    oled_clear_fb(0);
    int ox = 96;
    int oy = 0;
    for (int y = 0; y < FACE_H; y++) {
        for (int x = 0; x < FACE_W; x += 2) {
            uint8_t b = raw[(y * FACE_W + x) / 2];
            oled_set_px(ox + x, oy + y, (b >> 4) & 0x0F);
            oled_set_px(ox + x + 1, oy + y, b & 0x0F);
        }
    }
    oled_flush();
    return 0;
}

static int draw_face_screen(int id) {
    char file[FACE_FILE_MAX];
    snprintf(file, sizeof(file), "face_%03d.raw", id);
    return draw_face_screen_file(file);
}

static void sanitize_message_text(char *s) {
    if (!s) return;

    char out[192];
    size_t j = 0;
    int last_space = 1;

    for (const unsigned char *p = (const unsigned char *)s; *p && j + 1 < sizeof(out); p++) {
        unsigned char ch = *p;
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
        if (ch < 32 || ch > 126) continue;
        if (ch == '<' || ch == '>' || ch == '&' || ch == '"') continue;
        if (ch == ' ') {
            if (last_space) continue;
            last_space = 1;
        } else {
            last_space = 0;
        }
        out[j++] = (char)ch;
    }

    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';
    safe_str(s, 192, out);
}

static int measure_ttf_line_locked(FT_Face face, const char *text) {
    if (!face || !text || !*text) return 0;

    int pen_x = 0;
    int min_x = 99999;
    int max_x = -99999;

    for (const unsigned char *ch = (const unsigned char *)text; *ch; ch++) {
        if (FT_Load_Char(face, *ch, FT_LOAD_RENDER) != 0) continue;
        FT_GlyphSlot g = face->glyph;
        int gx0 = (pen_x >> 6) + g->bitmap_left;
        int gx1 = gx0 + (int)g->bitmap.width;
        if (gx0 < min_x) min_x = gx0;
        if (gx1 > max_x) max_x = gx1;
        pen_x += (int)g->advance.x;
    }

    return max_x > min_x ? max_x - min_x : (pen_x >> 6);
}


static int ttf_message_metrics_cached(const char *font_file, int px_size, int *ascent, int *descent) {
    if (ascent) *ascent = 0;
    if (descent) *descent = 0;
    if (!font_file || !*font_file) return -1;

    char font_path[512];
    make_font_path(font_file, font_path, sizeof(font_path));

    pthread_mutex_lock(&g_state.lock);
    if (font_cache_ensure_locked(font_file, font_path, px_size) != 0 || !g_state.ft_face) {
        pthread_mutex_unlock(&g_state.lock);
        return -1;
    }

    FT_Face face = g_state.ft_face;
    int a = 0;
    int d = 0;
    const char *sample = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.,:;!?()-_";
    for (const unsigned char *ch = (const unsigned char *)sample; *ch; ch++) {
        if (FT_Load_Char(face, *ch, FT_LOAD_RENDER) != 0) continue;
        FT_GlyphSlot g = face->glyph;
        int top = g->bitmap_top;
        int bottom = (int)g->bitmap.rows - g->bitmap_top;
        if (top > a) a = top;
        if (bottom > d) d = bottom;
    }
    pthread_mutex_unlock(&g_state.lock);

    if (a <= 0) a = px_size;
    if (d < 0) d = 0;
    if (ascent) *ascent = a;
    if (descent) *descent = d;
    return 0;
}

static int draw_truetype_line_cached_baseline(const char *font_file, int px_size, const char *text, int x, int baseline_y) {
    if (!font_file || !*font_file || !text || !*text) return -1;

    char font_path[512];
    make_font_path(font_file, font_path, sizeof(font_path));

    pthread_mutex_lock(&g_state.lock);
    if (font_cache_ensure_locked(font_file, font_path, px_size) != 0 || !g_state.ft_face) {
        pthread_mutex_unlock(&g_state.lock);
        return -1;
    }

    FT_Face face = g_state.ft_face;
    int pen_x = 0;
    int min_x = 99999;

    for (const unsigned char *ch = (const unsigned char *)text; *ch; ch++) {
        if (FT_Load_Char(face, *ch, FT_LOAD_RENDER) != 0) continue;
        FT_GlyphSlot g = face->glyph;
        int gx0 = (pen_x >> 6) + g->bitmap_left;
        if (gx0 < min_x) min_x = gx0;
        pen_x += (int)g->advance.x;
    }

    if (min_x == 99999) {
        pthread_mutex_unlock(&g_state.lock);
        return -1;
    }

    int origin_x = x - min_x;
    pen_x = 0;
    for (const unsigned char *ch = (const unsigned char *)text; *ch; ch++) {
        if (FT_Load_Char(face, *ch, FT_LOAD_RENDER) != 0) continue;
        FT_GlyphSlot g = face->glyph;
        FT_Bitmap *bm = &g->bitmap;
        int gx = origin_x + (pen_x >> 6) + g->bitmap_left;
        int gy = baseline_y - g->bitmap_top;

        for (unsigned int row = 0; row < bm->rows; row++) {
            for (unsigned int col = 0; col < bm->width; col++) {
                uint8_t coverage = 0;
                if (bm->pixel_mode == FT_PIXEL_MODE_GRAY) {
                    coverage = bm->buffer[row * bm->pitch + col];
                } else if (bm->pixel_mode == FT_PIXEL_MODE_MONO) {
                    uint8_t byte = bm->buffer[row * bm->pitch + (col >> 3)];
                    coverage = (byte & (0x80 >> (col & 7))) ? 255 : 0;
                }
                oled_blend_px(gx + (int)col, gy + (int)row, coverage, 15);
            }
        }
        pen_x += (int)g->advance.x;
    }

    pthread_mutex_unlock(&g_state.lock);
    return 0;
}

static int ttf_line_width_cached(const char *font_file, int px_size, const char *text) {
    if (!font_file || !*font_file || !text || !*text) return 0;

    char font_path[512];
    make_font_path(font_file, font_path, sizeof(font_path));

    pthread_mutex_lock(&g_state.lock);
    if (font_cache_ensure_locked(font_file, font_path, px_size) != 0 || !g_state.ft_face) {
        pthread_mutex_unlock(&g_state.lock);
        return 0;
    }
    int w = measure_ttf_line_locked(g_state.ft_face, text);
    pthread_mutex_unlock(&g_state.lock);
    return w;
}

static int message_line_width(const char *font_file, int px_size, const char *text, int scale) {
    if (font_file && *font_file) {
        int w = ttf_line_width_cached(font_file, px_size, text);
        if (w > 0) return w;
    }
    return text5x7_width(text, scale);
}

static int wrap_message_lines(const char *font_file, int px_size, const char *text, int max_w, char lines[][MESSAGE_LINE_CHARS], int max_lines, int scale) {
    if (!text || !*text || !lines || max_lines <= 0) return 0;

    char work[192];
    safe_str(work, sizeof(work), text);

    int count = 0;
    char line[MESSAGE_LINE_CHARS] = "";

    char *saveptr = NULL;
    for (char *tok = strtok_r(work, " ", &saveptr); tok; tok = strtok_r(NULL, " ", &saveptr)) {
        char test[MESSAGE_LINE_CHARS];
        if (line[0]) snprintf(test, sizeof(test), "%s %s", line, tok);
        else snprintf(test, sizeof(test), "%s", tok);

        if (message_line_width(font_file, px_size, test, scale) <= max_w || !line[0]) {
            safe_str(line, sizeof(line), test);
        } else {
            safe_str(lines[count++], MESSAGE_LINE_CHARS, line);
            if (count >= max_lines) return count;
            safe_str(line, sizeof(line), tok);
        }
    }

    if (line[0] && count < max_lines) safe_str(lines[count++], MESSAGE_LINE_CHARS, line);
    return count;
}

static int message_layout_params(char *font_file, size_t font_file_len, int *px_size, int *scale, int *use_ttf) {
    if (font_file && font_file_len > 0) font_file[0] = '\0';
    int font_size = 18;

    pthread_mutex_lock(&g_state.lock);
    if (font_file && font_file_len > 0) safe_str(font_file, font_file_len, g_state.oled_font_file);
    font_size = g_state.oled_font_size;
    pthread_mutex_unlock(&g_state.lock);

    if (font_size > MESSAGE_TTF_MAX_PX) font_size = MESSAGE_TTF_MAX_PX;
    if (font_size < MESSAGE_TTF_MIN_PX) font_size = MESSAGE_TTF_MIN_PX;

    if (px_size) *px_size = font_size;
    if (scale) *scale = MESSAGE_FALLBACK_SCALE;
    if (use_ttf) *use_ttf = (font_file && font_file[0] != '\0');
    return 0;
}

static int message_char_capacity_for_current_font(void) {
    char font_file[128];
    int px_size = 18;
    int scale = MESSAGE_FALLBACK_SCALE;
    int use_ttf = 0;
    message_layout_params(font_file, sizeof(font_file), &px_size, &scale, &use_ttf);

    int widest = 0;
    const char *sample = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    char one[2] = {0, 0};
    for (const char *p = sample; *p; p++) {
        one[0] = *p;
        int w = message_line_width(use_ttf ? font_file : "", px_size, one, scale);
        if (w > widest) widest = w;
    }
    if (widest <= 0) widest = 12;

    int per_line = MESSAGE_TEXT_W / widest;
    if (per_line < 6) per_line = 6;

    return per_line * MESSAGE_MAX_LINES;
}

static int message_fits_display(const char *message, char *reason, size_t reason_len) {
    if (reason && reason_len > 0) reason[0] = '\0';
    if (!message || !*message) return 1;

    int len = (int)strlen(message);
    if (len > MESSAGE_INPUT_MAX_CHARS) {
        if (reason && reason_len > 0) snprintf(reason, reason_len, "Message is too long. Maximum is %d characters.", MESSAGE_INPUT_MAX_CHARS);
        return 0;
    }

    char font_file[128];
    int px_size = 18;
    int scale = MESSAGE_FALLBACK_SCALE;
    int use_ttf = 0;
    message_layout_params(font_file, sizeof(font_file), &px_size, &scale, &use_ttf);

    char work[192];
    safe_str(work, sizeof(work), message);

    int count = 0;
    char line[MESSAGE_LINE_CHARS] = "";

    char *saveptr = NULL;
    for (char *tok = strtok_r(work, " ", &saveptr); tok; tok = strtok_r(NULL, " ", &saveptr)) {
        if (message_line_width(use_ttf ? font_file : "", px_size, tok, scale) > MESSAGE_TEXT_W) {
            if (reason && reason_len > 0) snprintf(reason, reason_len, "A word is too long to fit on the OLED.");
            return 0;
        }

        char test[MESSAGE_LINE_CHARS];
        if (line[0]) snprintf(test, sizeof(test), "%s %s", line, tok);
        else snprintf(test, sizeof(test), "%s", tok);

        if (message_line_width(use_ttf ? font_file : "", px_size, test, scale) <= MESSAGE_TEXT_W || !line[0]) {
            safe_str(line, sizeof(line), test);
        } else {
            count++;
            if (count >= MESSAGE_MAX_LINES) {
                if (reason && reason_len > 0) snprintf(reason, reason_len, "Message needs more than %d OLED lines.", MESSAGE_MAX_LINES);
                return 0;
            }
            safe_str(line, sizeof(line), tok);
        }
    }

    if (line[0]) count++;
    if (count > MESSAGE_MAX_LINES) {
        if (reason && reason_len > 0) snprintf(reason, reason_len, "Message needs more than %d OLED lines.", MESSAGE_MAX_LINES);
        return 0;
    }

    return 1;
}

static int draw_message_screen_file(const char *face_file, const char *message) {
    if (!safe_face_filename(face_file)) face_file = "";
    if (!message || !*message) message = "Hello";

    pthread_mutex_lock(&g_state.lock);
    char font_file[128];
    int font_size = g_state.oled_font_size;
    safe_str(font_file, sizeof(font_file), g_state.oled_font_file);
    pthread_mutex_unlock(&g_state.lock);

    oled_clear_fb(0);

    /* Full 64x64 face at left; text gets the remaining 188 px. */
    if (!face_file[0] || draw_face_thumb_by_file(face_file, 0, 0, 64) != 0) {
        draw_text5x7(8, 27, 1, "NO FACE", 8);
    }

    const int text_x = MESSAGE_TEXT_X;
    const int text_w = MESSAGE_TEXT_W;
    const int max_lines = MESSAGE_MAX_LINES;
    char lines[MESSAGE_MAX_LINES][MESSAGE_LINE_CHARS];
    memset(lines, 0, sizeof(lines));

    int ttf_px = font_size;
    if (ttf_px > MESSAGE_TTF_MAX_PX) ttf_px = MESSAGE_TTF_MAX_PX;
    if (ttf_px < MESSAGE_TTF_MIN_PX) ttf_px = MESSAGE_TTF_MIN_PX;

    int scale = MESSAGE_FALLBACK_SCALE;
    int use_ttf = font_file[0] != '\0';
    int line_h = use_ttf ? (ttf_px + 2) : (7 * scale + 3);
    int n = wrap_message_lines(use_ttf ? font_file : "", ttf_px, message, text_w, lines, max_lines, scale);
    if (n <= 0) {
        safe_str(lines[0], sizeof(lines[0]), "Hello");
        n = 1;
    }

    int ascent = ttf_px;
    int descent = 0;
    if (use_ttf) {
        ttf_message_metrics_cached(font_file, ttf_px, &ascent, &descent);
        int min_line_h = ascent + descent + 2;
        if (line_h < min_line_h) line_h = min_line_h;
    }

    int total_h = n * line_h - 2;
    int y = (OLED_H - total_h) / 2;
    if (y < 0) y = 0;

    for (int i = 0; i < n; i++) {
        int line_y = y + i * line_h;
        if (use_ttf) {
            int baseline_y = line_y + ascent;
            if (draw_truetype_line_cached_baseline(font_file, ttf_px, lines[i], text_x, baseline_y) != 0) {
                draw_text5x7(text_x, line_y, 1, lines[i], 15);
            }
        } else {
            draw_text5x7(text_x, line_y, scale, lines[i], 15);
        }
    }

    oled_flush_full();
    return 0;
}

static int draw_message_screen(int face_id, const char *message) {
    char file[FACE_FILE_MAX];
    snprintf(file, sizeof(file), "face_%03d.raw", face_id_valid_int(face_id) ? face_id : 1);
    return draw_message_screen_file(file, message);
}

/* ---------------- PNG to raw face ---------------- */

static int save_face_raw_from_png_memory(const uint8_t *png_data, size_t png_size, const char *out_path) {
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_memory(&image, png_data, png_size)) {
        fprintf(stderr, "png read failed: %s\n", image.message);
        return -1;
    }

    image.format = PNG_FORMAT_RGBA;
    size_t stride = PNG_IMAGE_ROW_STRIDE(image);
    size_t buf_size = PNG_IMAGE_SIZE(image);
    uint8_t *rgba = malloc(buf_size);
    if (!rgba) {
        png_image_free(&image);
        return -1;
    }

    if (!png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        fprintf(stderr, "png finish failed: %s\n", image.message);
        free(rgba);
        png_image_free(&image);
        return -1;
    }

    int src_w = (int)image.width;
    int src_h = (int)image.height;
    int crop = src_w < src_h ? src_w : src_h;
    int crop_x = (src_w - crop) / 2;
    int crop_y = (src_h - crop) / 2;

    uint8_t raw[FACE_RAW_BYTES];
    memset(raw, 0, sizeof(raw));

    for (int y = 0; y < FACE_H; y++) {
        for (int x = 0; x < FACE_W; x += 2) {
            uint8_t packed = 0;
            for (int p = 0; p < 2; p++) {
                int dx = x + p;
                int sx = crop_x + (dx * crop) / FACE_W;
                int sy = crop_y + (y * crop) / FACE_H;
                uint8_t *px = rgba + ((size_t)sy * stride) + ((size_t)sx * 4);

                int r = px[0];
                int g = px[1];
                int b = px[2];
                int a = px[3];

                r = (r * a + 128) >> 8;
                g = (g * a + 128) >> 8;
                b = (b * a + 128) >> 8;

                int gray = (r * 77 + g * 150 + b * 29) >> 8;
                int g4 = gray >> 4;

                if (p == 0) packed |= (uint8_t)((g4 & 0x0F) << 4);
                else packed |= (uint8_t)(g4 & 0x0F);
            }
            raw[(y * FACE_W + x) / 2] = packed;
        }
    }

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        perror("fopen face raw");
        free(rgba);
        png_image_free(&image);
        return -1;
    }
    size_t written = fwrite(raw, 1, sizeof(raw), f);
    fclose(f);

    free(rgba);
    png_image_free(&image);

    return written == sizeof(raw) ? 0 : -1;
}

/* ---------------- Audio ---------------- */

struct audio_play_request {
    char path[512];
    char file[MUSIC_FILE_MAX];
    int start_volume;
    int end_volume;
    int use_ramp;
};

static int choose_random_music_file(char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';

    char files[ASSET_LIST_MAX_FILES][ASSET_LIST_NAME_MAX];
    int count = collect_music_files(files, ASSET_LIST_MAX_FILES);
    if (count <= 0) return -1;

    safe_str(out, out_len, files[rand() % count]);
    return 0;
}

static int audio_should_stop(void) {
    int stop;
    pthread_mutex_lock(&g_audio.lock);
    stop = g_audio.stop_requested;
    pthread_mutex_unlock(&g_audio.lock);
    return stop;
}

static void alarm_volume_state_set(int active, int volume_percent) {
    static int last_active = -1;
    static int last_volume = -999;

    volume_percent = clamp_int(volume_percent, 0, 100);

    pthread_mutex_lock(&g_state.lock);
    g_state.alarm_active = active ? 1 : 0;
    g_state.alarm_volume_percent = active ? volume_percent : 0;

    if (g_state.display_mode == 0 &&
        (last_active != g_state.alarm_active || last_volume != g_state.alarm_volume_percent)) {
        g_state.display_dirty = 1;
    }

    last_active = g_state.alarm_active;
    last_volume = g_state.alarm_volume_percent;
    pthread_mutex_unlock(&g_state.lock);
}

static void audio_scale_s16(unsigned char *buf, size_t bytes, int volume_percent) {
    volume_percent = clamp_int(volume_percent, 0, 100);
    int gain_q15 = (volume_percent * 32768) / 100;
    int16_t *samples = (int16_t *)buf;
    size_t count = bytes / sizeof(int16_t);

    for (size_t i = 0; i < count; i++) {
        int32_t v = ((int32_t)samples[i] * gain_q15) >> 15;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        samples[i] = (int16_t)v;
    }
}

static int audio_write_pcm(snd_pcm_t *pcm, unsigned char *buf, size_t bytes, int channels) {
    const int frame_bytes = channels * (int)sizeof(int16_t);
    if (frame_bytes <= 0) return -1;

    size_t offset = 0;
    snd_pcm_sframes_t frames_left = (snd_pcm_sframes_t)(bytes / (size_t)frame_bytes);

    while (frames_left > 0 && !audio_should_stop()) {
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, buf + offset, frames_left);
        if (written == -EPIPE) {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (written < 0) {
            written = snd_pcm_recover(pcm, (int)written, 1);
            if (written < 0) return -1;
            continue;
        }
        if (written == 0) continue;
        frames_left -= written;
        offset += (size_t)written * (size_t)frame_bytes;
    }
    return 0;
}

static void *audio_thread_main(void *arg) {
    struct audio_play_request *req = (struct audio_play_request *)arg;
    mpg123_handle *mh = NULL;
    snd_pcm_t *pcm = NULL;
    unsigned char *buf = NULL;
    int err = MPG123_OK;
    long rate = 0;
    int channels = 0;
    int enc = 0;
    off_t total_samples = 0;
    off_t played_samples = 0;
    double ramp_seconds = 0.0;
    struct timespec ramp_start_ts;
    memset(&ramp_start_ts, 0, sizeof(ramp_start_ts));

    mh = mpg123_new(NULL, &err);
    if (!mh) goto done;

    mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0.0);

    if (mpg123_open(mh, req->path) != MPG123_OK) goto done;
    if (mpg123_getformat(mh, &rate, &channels, &enc) != MPG123_OK) goto done;
    if (channels < 1 || channels > 2 || rate <= 0) goto done;

    mpg123_format_none(mh);
    mpg123_format(mh, rate, channels, MPG123_ENC_SIGNED_16);

    /*
       Scan once so VBR files get a useful sample length for the alarm ramp.
       We compute seconds from decoded PCM frames, then ramp by real playback time.
       That is more reliable than tying volume changes only to decoder position.
    */
    if (req->use_ramp) {
        if (mpg123_scan(mh) == MPG123_OK) {
            total_samples = mpg123_length(mh);
            mpg123_seek(mh, 0, SEEK_SET);
        } else {
            total_samples = mpg123_length(mh);
        }
        if (total_samples < 0) total_samples = 0;
        if (total_samples > 0 && rate > 0) {
            ramp_seconds = (double)total_samples / (double)rate;
        }
        if (ramp_seconds < 1.0) ramp_seconds = 60.0;
        clock_gettime(CLOCK_MONOTONIC, &ramp_start_ts);
        alarm_volume_state_set(1, req->start_volume);
        fprintf(stderr,
                "alarm ramp: file=%s start=%d end=%d duration=%.1fs alarm-volume-overrides-global\n",
                req->file,
                req->start_volume,
                req->end_volume,
                ramp_seconds);
    } else {
        total_samples = mpg123_length(mh);
        if (total_samples < 0) total_samples = 0;
    }

    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) goto done;
    if (snd_pcm_set_params(pcm,
                           SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED,
                           (unsigned int)channels,
                           (unsigned int)rate,
                           1,
                           300000) < 0) {
        goto done;
    }

    const size_t buf_size = 8192;
    buf = malloc(buf_size);
    if (!buf) goto done;

    while (!audio_should_stop()) {
        size_t done_bytes = 0;
        int r = mpg123_read(mh, buf, buf_size, &done_bytes);
        if (done_bytes > 0) {
            int frame_bytes = channels * (int)sizeof(int16_t);
            off_t frames = (off_t)(done_bytes / (size_t)frame_bytes);
            int volume = req->start_volume;

            if (req->use_ramp && ramp_seconds > 0.0) {
                struct timespec now_ts;
                clock_gettime(CLOCK_MONOTONIC, &now_ts);
                double elapsed = (double)(now_ts.tv_sec - ramp_start_ts.tv_sec) +
                                 ((double)(now_ts.tv_nsec - ramp_start_ts.tv_nsec) / 1000000000.0);
                if (elapsed < 0.0) elapsed = 0.0;
                double t = elapsed / ramp_seconds;
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;
                volume = req->start_volume + (int)((double)(req->end_volume - req->start_volume) * t + 0.5);
            } else if (req->use_ramp && total_samples > 0) {
                off_t pos = played_samples;
                if (pos < 0) pos = 0;
                if (pos > total_samples) pos = total_samples;
                volume = req->start_volume + (int)(((long long)(req->end_volume - req->start_volume) * (long long)pos) / (long long)total_samples);
            }

            if (req->use_ramp) alarm_volume_state_set(1, volume);
            audio_scale_s16(buf, done_bytes, volume);
            if (audio_write_pcm(pcm, buf, done_bytes, channels) != 0) break;
            played_samples += frames;
        }
        if (r == MPG123_DONE) break;
        if (r == MPG123_NEW_FORMAT) continue;
        if (r != MPG123_OK) break;
    }

    if (pcm) {
        if (audio_should_stop()) snd_pcm_drop(pcm);
        else snd_pcm_drain(pcm);
    }

done:
    if (buf) free(buf);
    if (pcm) snd_pcm_close(pcm);
    if (mh) {
        mpg123_close(mh);
        mpg123_delete(mh);
    }

    pthread_mutex_lock(&g_audio.lock);
    g_audio.running = 0;
    g_audio.stop_requested = 0;
    g_audio.file[0] = '\0';
    pthread_mutex_unlock(&g_audio.lock);

    if (req->use_ramp) alarm_volume_state_set(0, 0);

    pthread_mutex_lock(&g_state.lock);
    g_state.audio_playing = 0;
    g_state.audio_file[0] = '\0';
    pthread_mutex_unlock(&g_state.lock);

    free(req);
    return NULL;
}

static void audio_stop(void) {
    int was_running;

    pthread_mutex_lock(&g_audio.lock);
    was_running = g_audio.running;
    if (was_running) g_audio.stop_requested = 1;
    pthread_mutex_unlock(&g_audio.lock);

    for (int i = 0; i < 300; i++) {
        pthread_mutex_lock(&g_audio.lock);
        was_running = g_audio.running;
        pthread_mutex_unlock(&g_audio.lock);
        if (!was_running) break;
        usleep(10000);
    }

    alarm_volume_state_set(0, 0);

    pthread_mutex_lock(&g_state.lock);
    g_state.audio_playing = 0;
    g_state.audio_file[0] = '\0';
    pthread_mutex_unlock(&g_state.lock);
}

static void audio_play_music_file(const char *music_file, int start_volume, int end_volume, int use_ramp) {
    char safe_file[MUSIC_FILE_MAX];
    char path[512];
    int global_volume;

    safe_file[0] = '\0';
    if (music_file && *music_file && safe_asset_filename(music_file) && has_mp3_ext(music_file)) {
        safe_str(safe_file, sizeof(safe_file), music_file);
    } else if (choose_random_music_file(safe_file, sizeof(safe_file)) != 0) {
        safe_str(safe_file, sizeof(safe_file), "alarm.mp3");
    }

    make_music_path(safe_file, path, sizeof(path));
    if (access(path, R_OK) != 0) return;

    start_volume = clamp_int(start_volume, 0, 100);
    end_volume = clamp_int(end_volume, 0, 100);

    pthread_mutex_lock(&g_state.lock);
    global_volume = clamp_int(g_state.global_volume, 0, 100);
    pthread_mutex_unlock(&g_state.lock);

    /*
       Alarm playback intentionally ignores global volume.
       Per-alarm start/end volumes are absolute 0..100 values so the OLED
       indicator and the actual PCM sample scaling always match.
       Normal/manual music playback still passes global_volume as start=end.
    */
    if (!use_ramp) {
        start_volume = global_volume;
        end_volume = global_volume;
    }

    audio_stop();

    pthread_mutex_lock(&g_audio.lock);
    int still_running = g_audio.running;
    pthread_mutex_unlock(&g_audio.lock);
    if (still_running) return;

    struct audio_play_request *req = calloc(1, sizeof(*req));
    if (!req) return;
    safe_str(req->path, sizeof(req->path), path);
    safe_str(req->file, sizeof(req->file), safe_file);
    req->start_volume = clamp_int(start_volume, 0, 100);
    req->end_volume = clamp_int(end_volume, 0, 100);
    req->use_ramp = use_ramp ? 1 : 0;

    pthread_mutex_lock(&g_audio.lock);
    g_audio.running = 1;
    g_audio.stop_requested = 0;
    safe_str(g_audio.file, sizeof(g_audio.file), safe_file);
    pthread_mutex_unlock(&g_audio.lock);

    pthread_t tid;
    if (pthread_create(&tid, NULL, audio_thread_main, req) != 0) {
        pthread_mutex_lock(&g_audio.lock);
        g_audio.running = 0;
        g_audio.stop_requested = 0;
        g_audio.file[0] = '\0';
        pthread_mutex_unlock(&g_audio.lock);
        free(req);
        return;
    }
    pthread_detach(tid);

    pthread_mutex_lock(&g_state.lock);
    g_state.audio_playing = 1;
    safe_str(g_state.audio_file, sizeof(g_state.audio_file), safe_file);
    pthread_mutex_unlock(&g_state.lock);
}

/* ---------------- HTTP helpers ---------------- */

static void http_send(int client, const char *status, const char *ctype, const char *body) {
    char hdr[512];
    size_t len = body ? strlen(body) : 0;
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n",
        status, ctype, len);
    write_all(client, hdr, (size_t)n);
    if (body && len) write_all(client, body, len);
}

static void http_redirect(int client, const char *path) {
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 303 See Other\r\n"
        "Location: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n", path);
    write_all(client, hdr, (size_t)n);
}


static void http_send_file(int client, const char *path, const char *ctype) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        http_send(client, "404 Not Found", "text/plain", "File not found");
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        http_send(client, "500 Internal Server Error", "text/plain", "Could not read file");
        return;
    }

    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        http_send(client, "500 Internal Server Error", "text/plain", "Could not read file size");
        return;
    }
    rewind(f);

    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n",
        ctype, size);
    write_all(client, hdr, (size_t)n);

    char buf[4096];
    while (!feof(f)) {
        size_t got = fread(buf, 1, sizeof(buf), f);
        if (got > 0) write_all(client, buf, got);
        if (ferror(f)) break;
    }

    fclose(f);
}

static void http_send_json(int client, const char *body) {
    http_send(client, "200 OK", "application/json; charset=utf-8", body ? body : "{}");
}

static const char *oled_font_name_for_id(int id) {
    switch (id) {
        case 1: return "Seven Thin";
        case 2: return "Pixel";
        case 3: return "Pixel Bold";
        case 0:
        default: return "Seven Segment";
    }
}

static const char *display_mode_name(int mode) {
    switch (mode) {
        case 1: return "clear";
        case 2: return "face";
        case 3: return "message";
        case 0:
        default: return "clock";
    }
}

static void json_append_escaped(char *buf, size_t cap, const char *sval) {
    if (!buf || cap == 0) return;
    size_t used = strlen(buf);
    if (used >= cap) return;

    if (!sval) sval = "";
    for (const unsigned char *p = (const unsigned char *)sval; *p && used + 8 < cap; p++) {
        unsigned char ch = *p;
        if (ch == '"' || ch == '\\') {
            buf[used++] = '\\';
            buf[used++] = (char)ch;
        } else if (ch == '\n') {
            buf[used++] = '\\';
            buf[used++] = 'n';
        } else if (ch == '\r') {
            buf[used++] = '\\';
            buf[used++] = 'r';
        } else if (ch == '\t') {
            buf[used++] = '\\';
            buf[used++] = 't';
        } else if (ch < 32) {
            int n = snprintf(buf + used, cap - used, "\\u%04x", ch);
            if (n < 0) break;
            used += (size_t)n;
        } else {
            buf[used++] = (char)ch;
        }
        buf[used] = '\0';
    }
}

static void json_escape_to_buffer(char *out, size_t cap, const char *sval) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    json_append_escaped(out, cap, sval);
}

static void json_append(char *buf, size_t cap, const char *fmt, ...) {
    size_t used = strlen(buf);
    if (used >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);
}


static void url_decode_inplace(char *s);

static int parse_string_query(const char *query, const char *key, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    if (!query || !key || !*key) return -1;

    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', pair_len);

        if (eq) {
            size_t key_len = (size_t)(eq - p);
            char decoded_key[96];
            if (key_len < sizeof(decoded_key)) {
                memcpy(decoded_key, p, key_len);
                decoded_key[key_len] = '\0';
                url_decode_inplace(decoded_key);

                if (strcmp(decoded_key, key) == 0) {
                    size_t val_len = pair_len - key_len - 1;
                    size_t copy_len = val_len < out_len - 1 ? val_len : out_len - 1;
                    memcpy(out, eq + 1, copy_len);
                    out[copy_len] = '\0';
                    url_decode_inplace(out);
                    return 0;
                }
            }
        }

        if (!amp) break;
        p = amp + 1;
    }

    return -1;
}

static void send_static_page(int client, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), WEB_DIR "/%s", name);
    http_send_file(client, path, "text/html; charset=utf-8");
}

static void send_status_json(int client) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    char timestr[64];
    char datestr[96];
    strftime(timestr, sizeof(timestr), "%I:%M %p", &tmv);
    if (timestr[0] == '0') memmove(timestr, timestr + 1, strlen(timestr));
    strftime(datestr, sizeof(datestr), "%A %B %e, %Y", &tmv);

    long uptime_seconds = 0;
    if (g_start_time > 0 && now >= g_start_time) {
        uptime_seconds = (long)(now - g_start_time);
    }

    pthread_mutex_lock(&g_state.lock);
    int audio = g_state.audio_playing;
    int alarm_active = g_state.alarm_active;
    int alarm_volume_percent = g_state.alarm_volume_percent;
    int mode = g_state.display_mode;
    int oled_ok = g_state.oled_ok;
    int font = g_state.oled_font;
    int font_size = g_state.oled_font_size;
    int current_face = g_state.current_face;
    int global_volume = g_state.global_volume;
    int bedtime_enabled = g_state.bedtime_enabled;
    int bsh = g_state.bedtime_start_hour;
    int bsm = g_state.bedtime_start_min;
    int beh = g_state.bedtime_end_hour;
    int bem = g_state.bedtime_end_min;
    int bedtime_dim = g_state.bedtime_dim_percent;
    int clock_24h_mode = g_state.clock_24h_mode;
    int oled_brightness = g_state.oled_brightness_current;
    char font_file[128];
    char clock_name[64];
    char audio_file[MUSIC_FILE_MAX];
    struct alarm_slot alarms[MAX_ALARMS];
    safe_str(font_file, sizeof(font_file), g_state.oled_font_file);
    safe_str(clock_name, sizeof(clock_name), g_state.clock_name);
    safe_str(audio_file, sizeof(audio_file), g_state.audio_file);
    memcpy(alarms, g_state.alarms, sizeof(alarms));
    pthread_mutex_unlock(&g_state.lock);

    int bedtime_active = is_bedtime_now();

    char e_time[128];
    char e_date[192];
    char e_clock_name[160];
    char e_app_version[64];
    char e_audio_file[512];
    char e_font_file[256];
    char e_font_name[256];
    json_escape_to_buffer(e_time, sizeof(e_time), timestr);
    json_escape_to_buffer(e_date, sizeof(e_date), datestr);
    json_escape_to_buffer(e_clock_name, sizeof(e_clock_name), clock_name);
    json_escape_to_buffer(e_app_version, sizeof(e_app_version), APP_VERSION);
    json_escape_to_buffer(e_audio_file, sizeof(e_audio_file), audio_file);
    json_escape_to_buffer(e_font_file, sizeof(e_font_file), font_file);
    json_escape_to_buffer(e_font_name, sizeof(e_font_name), font_file[0] ? font_file : oled_font_name_for_id(font));

    char body[16384];
    snprintf(body, sizeof(body),
        "{\"time\":\"%s\",\"date\":\"%s\",\"clock_name\":\"%s\",\"app_version\":\"%s\","
        "\"uptime_seconds\":%ld,\"audio_file\":\"%s\",\"global_volume\":%d,\"bedtime_enabled\":%d,"
        "\"bedtime_start_hour\":%d,\"bedtime_start_min\":%d,\"bedtime_end_hour\":%d,\"bedtime_end_min\":%d,"
        "\"bedtime_dim_percent\":%d,\"clock_24h_mode\":%d,\"bedtime_active\":%d,\"oled_brightness_percent\":%d,"
        "\"audio_playing\":%d,\"alarm_active\":%d,\"alarm_volume_percent\":%d,\"display_mode\":\"%s\",\"oled_ok\":%d,"
        "\"current_face\":%d,\"oled_font\":%d,\"oled_font_size\":%d,\"oled_font_file\":\"%s\",\"oled_font_name\":\"%s\",\"alarms\":[",
        e_time, e_date, e_clock_name, e_app_version, uptime_seconds, e_audio_file,
        global_volume, bedtime_enabled, bsh, bsm, beh, bem, bedtime_dim, clock_24h_mode,
        bedtime_active, oled_brightness, audio, alarm_active, alarm_volume_percent,
        display_mode_name(mode), oled_ok, current_face, font, font_size, e_font_file, e_font_name);

    for (int i = 0; i < MAX_ALARMS; i++) {
        char e_music_file[512];
        json_escape_to_buffer(e_music_file, sizeof(e_music_file), alarms[i].music_file);
        json_append(body, sizeof(body),
            "%s{\"id\":%d,\"enabled\":%d,\"hour\":%d,\"min\":%d,\"weekdays\":%d,\"start_volume\":%d,\"end_volume\":%d,\"music_file\":\"%s\"}",
            i ? "," : "", i + 1, alarms[i].enabled, alarms[i].hour, alarms[i].min,
            alarms[i].weekdays, alarms[i].start_volume, alarms[i].end_volume, e_music_file);
    }
    json_append(body, sizeof(body), "]}");

    http_send_json(client, body);
}


static void send_log_json(int client) {
    pthread_mutex_lock(&g_log_lock);

    FILE *f = fopen(LOG_FILE, "rb");
    if (!f) {
        pthread_mutex_unlock(&g_log_lock);
        http_send_json(client, "{\"ok\":true,\"log_file\":\"" LOG_FILE "\",\"entries\":[]}");
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        pthread_mutex_unlock(&g_log_lock);
        http_send_json(client, "{\"ok\":false,\"entries\":[]}");
        return;
    }

    long size = ftell(f);
    if (size < 0) size = 0;
    long start = size > LOG_MAX_BYTES ? size - LOG_MAX_BYTES : 0;
    if (fseek(f, start, SEEK_SET) != 0) start = 0;

    size_t cap = (size_t)(size - start);
    char *buf = (char *)malloc(cap + 1);
    if (!buf) {
        fclose(f);
        pthread_mutex_unlock(&g_log_lock);
        http_send_json(client, "{\"ok\":false,\"entries\":[]}");
        return;
    }

    size_t got = fread(buf, 1, cap, f);
    fclose(f);
    buf[got] = '\0';
    pthread_mutex_unlock(&g_log_lock);

    char *first = buf;
    if (start > 0) {
        char *nl = strchr(buf, '\n');
        if (nl && nl[1]) first = nl + 1;
    }

    char *lines[LOG_VIEW_LINES];
    int count = 0;
    char *save = NULL;
    for (char *line = strtok_r(first, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line) continue;
        lines[count % LOG_VIEW_LINES] = line;
        count++;
    }

    char body[98304];
    body[0] = '\0';
    json_append(body, sizeof(body), "{\"ok\":true,\"log_file\":\"");
    json_append_escaped(body, sizeof(body), LOG_FILE);
    json_append(body, sizeof(body), "\",\"entries\":[");

    int n = count < LOG_VIEW_LINES ? count : LOG_VIEW_LINES;
    int start_idx = count > LOG_VIEW_LINES ? count % LOG_VIEW_LINES : 0;
    for (int i = 0; i < n; i++) {
        int idx = (start_idx + i) % LOG_VIEW_LINES;
        json_append(body, sizeof(body), "%s\"", i ? "," : "");
        json_append_escaped(body, sizeof(body), lines[idx]);
        json_append(body, sizeof(body), "\"");
    }
    json_append(body, sizeof(body), "]}");
    free(buf);
    http_send_json(client, body);
}

static void handle_clear_log(int client) {
    pthread_mutex_lock(&g_log_lock);
    ensure_dir(CONFIG_DIR);
    FILE *f = fopen(LOG_FILE, "w");
    if (f) fclose(f);
    pthread_mutex_unlock(&g_log_lock);
    app_log("log", "Log cleared from web GUI");
    http_send_json(client, "{\"ok\":true}");
}

static void send_faces_json(int client, const char *query) {
    int all = parse_int_query(query, "all", 0);
    int page = parse_int_query(query, "page", 1);
    char kind[32] = "";
    parse_string_query(query, "kind", kind, sizeof(kind));
    int bedtime = (strcmp(kind, "bedtime") == 0);
    if (page < 1) page = 1;

    char files[ASSET_LIST_MAX_FILES][ASSET_LIST_NAME_MAX];
    int count = bedtime ? collect_bedtime_face_files(files, ASSET_LIST_MAX_FILES) : collect_uploaded_face_files(files, ASSET_LIST_MAX_FILES);
    int per_page = all ? (count > 0 ? count : 1) : 8;
    int max_page = all ? 1 : ((count + per_page - 1) / per_page);
    if (max_page < 1) max_page = 1;
    if (page > max_page) page = max_page;

    int start = all ? 0 : ((page - 1) * per_page);
    int end = all ? count : (start + per_page);
    if (end > count) end = count;

    char body[65536];
    snprintf(body, sizeof(body),
        "{\"kind\":\"%s\",\"page\":%d,\"max_page\":%d,\"per_page\":%d,\"count\":%d,\"faces\":[",
        bedtime ? "bedtime" : "normal", page, max_page, per_page, count);

    for (int i = start; i < end; i++) {
        char title[FACE_FILE_MAX];
        char source_png[FACE_FILE_MAX];
        char preview_url[256];
        int has_source = source_face_png_exists(files[i], bedtime, source_png, sizeof(source_png));
        face_title_from_file(files[i], title, sizeof(title));
        if (has_source) {
            snprintf(preview_url, sizeof(preview_url), "/face-source?kind=%s&file=%s", bedtime ? "bedtime" : "normal", files[i]);
        } else {
            preview_url[0] = '\0';
            source_png[0] = '\0';
        }

        char e_file[256];
        char e_title[256];
        char e_source_png[256];
        char e_preview_url[512];
        json_escape_to_buffer(e_file, sizeof(e_file), files[i]);
        json_escape_to_buffer(e_title, sizeof(e_title), title);
        json_escape_to_buffer(e_source_png, sizeof(e_source_png), source_png);
        json_escape_to_buffer(e_preview_url, sizeof(e_preview_url), preview_url);

        json_append(body, sizeof(body),
            "%s{\"id\":%d,\"file\":\"%s\",\"title\":\"%s\",\"source_png\":\"%s\",\"preview_url\":\"%s\",\"source_exists\":%d,\"exists\":1}",
            i == start ? "" : ",", i + 1, e_file, e_title, e_source_png, e_preview_url, has_source ? 1 : 0);
    }

    json_append(body, sizeof(body), "]}");
    http_send_json(client, body);
}

static void send_fonts_json(int client) {
    pthread_mutex_lock(&g_state.lock);
    int font = g_state.oled_font;
    int font_size = g_state.oled_font_size;
    char selected[128];
    safe_str(selected, sizeof(selected), g_state.oled_font_file);
    pthread_mutex_unlock(&g_state.lock);

    char esc_selected[256];
    json_escape_to_buffer(esc_selected, sizeof(esc_selected), selected);

    char body[16384];
    size_t len = 0;
    int n = snprintf(body, sizeof(body),
        "{\"selected\":\"%s\",\"builtin\":%d,\"font_size\":%d,\"builtin_fonts\":[",
        esc_selected, font, font_size);
    if (n < 0) return;
    len = (size_t)n < sizeof(body) ? (size_t)n : sizeof(body) - 1;

    for (int i = 0; i < 4 && len < sizeof(body) - 1; i++) {
        char esc_name[128];
        json_escape_to_buffer(esc_name, sizeof(esc_name), oled_font_name_for_id(i));
        n = snprintf(body + len, sizeof(body) - len,
                     "%s{\"id\":%d,\"name\":\"%s\"}",
                     i ? "," : "", i, esc_name);
        if (n < 0) break;
        size_t avail = sizeof(body) - len;
        len += ((size_t)n < avail) ? (size_t)n : avail - 1;
    }

    if (len < sizeof(body) - 1) {
        n = snprintf(body + len, sizeof(body) - len, "],\"uploaded_fonts\":[");
        if (n > 0) {
            size_t avail = sizeof(body) - len;
            len += ((size_t)n < avail) ? (size_t)n : avail - 1;
        }
    }

    char font_files[ASSET_LIST_MAX_FILES][ASSET_LIST_NAME_MAX];
    int font_count = collect_font_files(font_files, ASSET_LIST_MAX_FILES);
    int count = 0;
    for (int i = 0; i < font_count && len < sizeof(body) - 1; i++) {
        char esc_file[256];
        json_escape_to_buffer(esc_file, sizeof(esc_file), font_files[i]);
        n = snprintf(body + len, sizeof(body) - len,
                     "%s\"%s\"", count ? "," : "", esc_file);
        if (n < 0) break;
        size_t avail = sizeof(body) - len;
        len += ((size_t)n < avail) ? (size_t)n : avail - 1;
        count++;
    }

    if (len < sizeof(body) - 1) {
        snprintf(body + len, sizeof(body) - len, "]}");
    } else {
        body[sizeof(body) - 1] = '\0';
    }
    http_send_json(client, body);
}

static void send_music_json(int client) {
    pthread_mutex_lock(&g_state.lock);
    int global_volume = g_state.global_volume;
    char current[MUSIC_FILE_MAX];
    safe_str(current, sizeof(current), g_state.audio_file);
    pthread_mutex_unlock(&g_state.lock);

    char body[16384];
    body[0] = '\0';
    json_append(body, sizeof(body), "{\"global_volume\":%d,\"current\":\"", global_volume);
    json_append_escaped(body, sizeof(body), current);
    json_append(body, sizeof(body), "\",\"files\":[");

    char music_files[ASSET_LIST_MAX_FILES][ASSET_LIST_NAME_MAX];
    int music_count = collect_music_files(music_files, ASSET_LIST_MAX_FILES);
    for (int i = 0; i < music_count; i++) {
        json_append(body, sizeof(body), "%s\"", i ? "," : "");
        json_append_escaped(body, sizeof(body), music_files[i]);
        json_append(body, sizeof(body), "\"");
    }
    json_append(body, sizeof(body), "]}");
    http_send_json(client, body);
}

static void send_message_limit_json(int client) {
    int advisory_chars = message_char_capacity_for_current_font();
    char body[512];
    snprintf(body, sizeof(body),
        "{\"max_chars\":%d,\"advisory_chars\":%d,\"max_lines\":%d,\"text_width\":%d,\"duration_seconds\":%d,\"wrap_check\":1}",
        MESSAGE_INPUT_MAX_CHARS, advisory_chars, MESSAGE_MAX_LINES, MESSAGE_TEXT_W, MESSAGE_DEFAULT_DURATION_SECONDS);
    http_send_json(client, body);
}

static void send_message_fit_json(int client, const char *query) {
    char text[192] = "";
    char reason[160] = "";
    parse_string_query(query, "text", text, sizeof(text));
    sanitize_message_text(text);

    int advisory_chars = message_char_capacity_for_current_font();
    int ok = message_fits_display(text, reason, sizeof(reason));

    char font_file[128];
    int px_size = 18;
    int scale = MESSAGE_FALLBACK_SCALE;
    int use_ttf = 0;
    message_layout_params(font_file, sizeof(font_file), &px_size, &scale, &use_ttf);

    char lines[MESSAGE_MAX_LINES][MESSAGE_LINE_CHARS];
    memset(lines, 0, sizeof(lines));
    int line_count = wrap_message_lines(use_ttf ? font_file : "", px_size, text, MESSAGE_TEXT_W, lines, MESSAGE_MAX_LINES, scale);
    if (!text[0]) line_count = 0;

    char body[4096];
    body[0] = '\0';
    json_append(body, sizeof(body),
        "{\"ok\":%d,\"chars\":%d,\"max_chars\":%d,\"advisory_chars\":%d,\"wrap_check\":1,\"max_lines\":%d,\"line_count\":%d,\"text_x\":%d,\"text_w\":%d,\"face_x\":0,\"face_y\":0,\"face_size\":64,\"reason\":\"",
        ok,
        (int)strlen(text),
        MESSAGE_INPUT_MAX_CHARS,
        advisory_chars,
        MESSAGE_MAX_LINES,
        line_count,
        MESSAGE_TEXT_X,
        MESSAGE_TEXT_W);
    json_append_escaped(body, sizeof(body), reason);
    json_append(body, sizeof(body), "\",\"lines\":[");
    for (int i = 0; i < line_count && i < MESSAGE_MAX_LINES; i++) {
        json_append(body, sizeof(body), "%s\"", i ? "," : "");
        json_append_escaped(body, sizeof(body), lines[i]);
        json_append(body, sizeof(body), "\"");
    }
    json_append(body, sizeof(body), "]}");
    http_send_json(client, body);
}

static char *memmem_simple(const char *hay, size_t hay_len, const char *needle, size_t needle_len) {
    if (!hay || !needle || needle_len == 0 || hay_len < needle_len) return NULL;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) return (char *)(hay + i);
    }
    return NULL;
}

static int extract_multipart_file(char *body, size_t body_len, const char *boundary, uint8_t **file_data, size_t *file_len) {
    *file_data = NULL;
    *file_len = 0;
    if (!body || !boundary) return -1;

    char marker[256];
    snprintf(marker, sizeof(marker), "--%s", boundary);

    char *part = memmem_simple(body, body_len, marker, strlen(marker));
    if (!part) return -1;

    char *headers_end = memmem_simple(part, body_len - (size_t)(part - body), "\r\n\r\n", 4);
    if (!headers_end) return -1;

    char *data = headers_end + 4;
    size_t data_avail = body_len - (size_t)(data - body);

    char end_marker[300];
    snprintf(end_marker, sizeof(end_marker), "\r\n--%s", boundary);
    char *end = memmem_simple(data, data_avail, end_marker, strlen(end_marker));
    if (!end) return -1;

    *file_data = (uint8_t *)data;
    *file_len = (size_t)(end - data);
    return *file_len > 0 ? 0 : -1;
}

static int extract_multipart_filename(char *body, size_t body_len, const char *boundary, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    if (!body || !boundary) return -1;

    char marker[256];
    snprintf(marker, sizeof(marker), "--%s", boundary);

    char *part = memmem_simple(body, body_len, marker, strlen(marker));
    if (!part) return -1;

    char *headers_end = memmem_simple(part, body_len - (size_t)(part - body), "\r\n\r\n", 4);
    if (!headers_end) return -1;

    char *fn = memmem_simple(part, (size_t)(headers_end - part), "filename=\"", 10);
    if (!fn) return -1;
    fn += 10;
    char *end = memchr(fn, '"', (size_t)(headers_end - fn));
    if (!end) return -1;

    const char *base = fn;
    for (char *p = fn; p < end; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    size_t n = (size_t)(end - base);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, base, n);
    out[n] = '\0';
    return out[0] ? 0 : -1;
}

struct multipart_file_part {
    char filename[MUSIC_FILE_MAX];
    uint8_t *data;
    size_t len;
};

typedef int (*multipart_file_cb)(const struct multipart_file_part *part, void *user);

static int foreach_multipart_file(char *body, size_t body_len, const char *boundary, multipart_file_cb cb, void *user) {
    if (!body || !boundary || !cb) return -1;

    char marker[256];
    snprintf(marker, sizeof(marker), "--%s", boundary);
    size_t marker_len = strlen(marker);
    char end_marker[300];
    snprintf(end_marker, sizeof(end_marker), "\r\n--%s", boundary);
    size_t end_marker_len = strlen(end_marker);

    char *pos = body;
    size_t remaining = body_len;
    int count = 0;

    while (remaining > marker_len) {
        char *part = memmem_simple(pos, remaining, marker, marker_len);
        if (!part) break;
        if ((size_t)(part - body) + marker_len + 2 <= body_len &&
            part[marker_len] == '-' && part[marker_len + 1] == '-') break;

        size_t part_avail = body_len - (size_t)(part - body);
        char *headers_end = memmem_simple(part, part_avail, "\r\n\r\n", 4);
        if (!headers_end) break;

        char filename[FACE_FILE_MAX] = "";
        char *fn = memmem_simple(part, (size_t)(headers_end - part), "filename=\"", 10);
        if (fn) {
            fn += 10;
            char *q = memchr(fn, '"', (size_t)(headers_end - fn));
            if (q) {
                const char *base = fn;
                for (char *p = fn; p < q; p++) {
                    if (*p == '/' || *p == '\\') base = p + 1;
                }
                size_t n = (size_t)(q - base);
                if (n >= sizeof(filename)) n = sizeof(filename) - 1;
                memcpy(filename, base, n);
                filename[n] = '\0';
            }
        }

        char *data = headers_end + 4;
        size_t data_avail = body_len - (size_t)(data - body);
        char *end = memmem_simple(data, data_avail, end_marker, end_marker_len);
        if (!end) break;

        if (filename[0] && end > data) {
            struct multipart_file_part part_info;
            safe_str(part_info.filename, sizeof(part_info.filename), filename);
            part_info.data = (uint8_t *)data;
            part_info.len = (size_t)(end - data);
            if (part_info.len > 0) {
                if (cb(&part_info, user) == 0) count++;
            }
        }

        pos = end + 2;
        remaining = body_len - (size_t)(pos - body);
    }

    return count > 0 ? count : -1;
}

static int save_bytes(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

static void url_decode_inplace(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') *o++ = ' ';
        else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

typedef void (*form_field_cb_t)(const char *key, const char *val, void *user);

static int for_each_form_field(char *body, form_field_cb_t cb, void *user) {
    if (!cb) return -1;
    if (!body || !*body) return 0;

    char *saveptr = NULL;
    for (char *tok = strtok_r(body, "&", &saveptr); tok; tok = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = tok;
        char *val = eq + 1;
        url_decode_inplace(key);
        url_decode_inplace(val);
        cb(key, val, user);
    }

    return 0;
}

struct alarm_form_ctx {
    int id;
    struct alarm_slot *alarm;
};

static void parse_alarm_form_field(const char *key, const char *val, void *user) {
    struct alarm_form_ctx *ctx = (struct alarm_form_ctx *)user;
    struct alarm_slot *a = ctx->alarm;

    if (strcmp(key, "id") == 0) ctx->id = atoi(val);
    else if (strcmp(key, "enabled") == 0) a->enabled = atoi(val) ? 1 : 0;
    else if (strcmp(key, "time") == 0) sscanf(val, "%d:%d", &a->hour, &a->min);
    else if (strcmp(key, "start_volume") == 0) a->start_volume = atoi(val);
    else if (strcmp(key, "end_volume") == 0) a->end_volume = atoi(val);
    else if (strcmp(key, "music_file") == 0) safe_str(a->music_file, sizeof(a->music_file), val);
    else if (strncmp(key, "day", 3) == 0) {
        int d = atoi(key + 3);
        if (d >= 0 && d <= 6) a->weekdays |= (1 << d);
    }
}

struct audio_form_ctx {
    int global_volume;
};

static void parse_audio_form_field(const char *key, const char *val, void *user) {
    struct audio_form_ctx *ctx = (struct audio_form_ctx *)user;
    if (strcmp(key, "global_volume") == 0) ctx->global_volume = atoi(val);
}

struct personalization_form_ctx {
    char *clock_name;
    size_t clock_name_len;
};

static void parse_personalization_form_field(const char *key, const char *val, void *user) {
    struct personalization_form_ctx *ctx = (struct personalization_form_ctx *)user;
    if (strcmp(key, "clock_name") == 0) safe_str(ctx->clock_name, ctx->clock_name_len, val);
}

struct display_form_ctx {
    int font;
    int font_size;
    int bedtime_enabled;
    int bedtime_dim_percent;
    int clock_24h_mode;
    int bsh;
    int bsm;
    int beh;
    int bem;
    char *font_file;
    size_t font_file_len;
};

static void parse_display_form_field(const char *key, const char *val, void *user) {
    struct display_form_ctx *ctx = (struct display_form_ctx *)user;

    if (strcmp(key, "oled_font") == 0) ctx->font = atoi(val);
    else if (strcmp(key, "oled_font_size") == 0) ctx->font_size = atoi(val);
    else if (strcmp(key, "oled_font_file") == 0) safe_str(ctx->font_file, ctx->font_file_len, val);
    else if (strcmp(key, "bedtime_enabled") == 0) ctx->bedtime_enabled = atoi(val) ? 1 : 0;
    else if (strcmp(key, "bedtime_dim_percent") == 0) ctx->bedtime_dim_percent = atoi(val);
    else if (strcmp(key, "clock_24h_mode") == 0) ctx->clock_24h_mode = atoi(val) ? 1 : 0;
    else if (strcmp(key, "bedtime_start") == 0) sscanf(val, "%d:%d", &ctx->bsh, &ctx->bsm);
    else if (strcmp(key, "bedtime_end") == 0) sscanf(val, "%d:%d", &ctx->beh, &ctx->bem);
}

struct delete_face_form_ctx {
    char *face_file;
    size_t face_file_len;
    char *kind;
    size_t kind_len;
    char *format;
    size_t format_len;
};

static void parse_delete_face_form_field(const char *key, const char *val, void *user) {
    struct delete_face_form_ctx *ctx = (struct delete_face_form_ctx *)user;
    if (strcmp(key, "file") == 0) safe_str(ctx->face_file, ctx->face_file_len, val);
    else if (strcmp(key, "kind") == 0) safe_str(ctx->kind, ctx->kind_len, val);
    else if (strcmp(key, "format") == 0) safe_str(ctx->format, ctx->format_len, val);
}

struct format_form_ctx {
    char *format;
    size_t format_len;
};

static void parse_format_form_field(const char *key, const char *val, void *user) {
    struct format_form_ctx *ctx = (struct format_form_ctx *)user;
    if (strcmp(key, "format") == 0) safe_str(ctx->format, ctx->format_len, val);
}

struct font_delete_form_ctx {
    char *font_file;
    size_t font_file_len;
};

static void parse_font_delete_form_field(const char *key, const char *val, void *user) {
    struct font_delete_form_ctx *ctx = (struct font_delete_form_ctx *)user;
    if (strcmp(key, "font") == 0) safe_str(ctx->font_file, ctx->font_file_len, val);
}

struct message_form_ctx {
    int face_id;
    char *face_file;
    size_t face_file_len;
    char *message;
    size_t message_len;
    char *format;
    size_t format_len;
};

static void parse_message_form_field(const char *key, const char *val, void *user) {
    struct message_form_ctx *ctx = (struct message_form_ctx *)user;
    if (strcmp(key, "face_id") == 0) {
        ctx->face_id = atoi(val);
    } else if ((strcmp(key, "face_file") == 0 || strcmp(key, "file") == 0) && safe_face_filename(val)) {
        safe_str(ctx->face_file, ctx->face_file_len, val);
    } else if (strcmp(key, "message_text") == 0 || strcmp(key, "message") == 0) {
        safe_str(ctx->message, ctx->message_len, val);
    } else if (strcmp(key, "format") == 0) {
        safe_str(ctx->format, ctx->format_len, val);
    }
}

static void handle_save_alarm(int client, char *body) {
    int id = 1;
    struct alarm_slot a;
    a.enabled = 0;
    a.hour = 7;
    a.min = 0;
    a.weekdays = 0;
    a.start_volume = 20;
    a.end_volume = 80;
    a.fired_yday = -1;
    a.music_file[0] = '\0';

    struct alarm_form_ctx form = { .id = id, .alarm = &a };
    if (for_each_form_field(body, parse_alarm_form_field, &form) != 0) {
        http_redirect(client, "/alarm");
        return;
    }
    id = form.id;

    if (id < 1 || id > MAX_ALARMS) id = 1;
    a.hour = clamp_int(a.hour, 0, 23);
    a.min = clamp_int(a.min, 0, 59);
    a.start_volume = clamp_int(a.start_volume, 0, 100);
    a.end_volume = clamp_int(a.end_volume, 0, 100);
    if (a.weekdays == 0) a.weekdays = 0x7F;
    if (a.music_file[0]) {
        char music_path[512];
        if (!safe_asset_filename(a.music_file) || !has_mp3_ext(a.music_file)) {
            a.music_file[0] = '\0';
        } else {
            make_music_path(a.music_file, music_path, sizeof(music_path));
            if (access(music_path, R_OK) != 0) a.music_file[0] = '\0';
        }
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.alarms[id - 1] = a;
    sync_legacy_alarm_fields_locked();
    pthread_mutex_unlock(&g_state.lock);

    save_config();
    app_log("alarm", "Saved alarm %d", id);
    http_redirect(client, "/alarm");
}

static void handle_save_audio(int client, char *body) {
    int global_volume;
    pthread_mutex_lock(&g_state.lock);
    global_volume = g_state.global_volume;
    pthread_mutex_unlock(&g_state.lock);

    struct audio_form_ctx form = { .global_volume = global_volume };
    if (for_each_form_field(body, parse_audio_form_field, &form) != 0) {
        http_redirect(client, "/music");
        return;
    }
    global_volume = form.global_volume;

    global_volume = clamp_int(global_volume, 0, 100);
    pthread_mutex_lock(&g_state.lock);
    g_state.global_volume = global_volume;
    pthread_mutex_unlock(&g_state.lock);
    save_config();
    app_log("music", "Saved global volume %d%%", global_volume);
    http_redirect(client, "/music");
}

static void handle_save_personalization(int client, char *body) {
    char clock_name[64];

    pthread_mutex_lock(&g_state.lock);
    safe_str(clock_name, sizeof(clock_name), g_state.clock_name);
    pthread_mutex_unlock(&g_state.lock);

    struct personalization_form_ctx form = {
        .clock_name = clock_name,
        .clock_name_len = sizeof(clock_name)
    };
    if (for_each_form_field(body, parse_personalization_form_field, &form) != 0) {
        http_redirect(client, "/display");
        return;
    }

    sanitize_clock_name(clock_name);

    pthread_mutex_lock(&g_state.lock);
    safe_str(g_state.clock_name, sizeof(g_state.clock_name), clock_name);
    pthread_mutex_unlock(&g_state.lock);

    save_config();
    app_log("settings", "Saved clock name %s", clock_name);
    http_redirect(client, "/display");
}

static void handle_save_display(int client, char *body) {
    int font = 0;
    int font_size = 42;
    int bedtime_enabled = 0;
    int bedtime_dim_percent = 35;
    int clock_24h_mode = 0;
    int bsh = 21, bsm = 0, beh = 7, bem = 0;
    char font_file[128] = "";

    pthread_mutex_lock(&g_state.lock);
    font = g_state.oled_font;
    font_size = g_state.oled_font_size;
    bedtime_enabled = g_state.bedtime_enabled;
    bedtime_dim_percent = g_state.bedtime_dim_percent;
    clock_24h_mode = g_state.clock_24h_mode;
    bsh = g_state.bedtime_start_hour;
    bsm = g_state.bedtime_start_min;
    beh = g_state.bedtime_end_hour;
    bem = g_state.bedtime_end_min;
    safe_str(font_file, sizeof(font_file), g_state.oled_font_file);
    pthread_mutex_unlock(&g_state.lock);

    struct display_form_ctx form = {
        .font = font,
        .font_size = font_size,
        .bedtime_enabled = bedtime_enabled,
        .bedtime_dim_percent = bedtime_dim_percent,
        .clock_24h_mode = clock_24h_mode,
        .bsh = bsh,
        .bsm = bsm,
        .beh = beh,
        .bem = bem,
        .font_file = font_file,
        .font_file_len = sizeof(font_file)
    };
    if (for_each_form_field(body, parse_display_form_field, &form) != 0) {
        http_redirect(client, "/display");
        return;
    }

    font = form.font;
    font_size = form.font_size;
    bedtime_enabled = form.bedtime_enabled;
    bedtime_dim_percent = form.bedtime_dim_percent;
    clock_24h_mode = form.clock_24h_mode;
    bsh = form.bsh;
    bsm = form.bsm;
    beh = form.beh;
    bem = form.bem;

    if (font < 0 || font > 3) font = 0;
    if (font_size < 18) font_size = 18;
    if (font_size > 54) font_size = 54;
    bedtime_enabled = bedtime_enabled ? 1 : 0;
    clock_24h_mode = clock_24h_mode ? 1 : 0;
    bedtime_dim_percent = clamp_int(bedtime_dim_percent, 0, 100);
    bsh = clamp_int(bsh, 0, 23);
    bsm = clamp_int(bsm, 0, 59);
    beh = clamp_int(beh, 0, 23);
    bem = clamp_int(bem, 0, 59);

    if (font_file[0]) {
        char font_path[512];
        if (!safe_asset_filename(font_file) || !has_font_ext(font_file)) {
            font_file[0] = '\0';
        } else {
            make_font_path(font_file, font_path, sizeof(font_path));
            if (access(font_path, R_OK) != 0) font_file[0] = '\0';
        }
    }

    pthread_mutex_lock(&g_state.lock);
    int changed = (g_state.oled_font_size != font_size) || (strcmp(g_state.oled_font_file, font_file) != 0);
    g_state.oled_font = font;
    g_state.oled_font_size = font_size;
    g_state.clock_24h_mode = clock_24h_mode;
    g_state.bedtime_enabled = bedtime_enabled;
    g_state.bedtime_dim_percent = bedtime_dim_percent;
    g_state.bedtime_start_hour = bsh;
    g_state.bedtime_start_min = bsm;
    g_state.bedtime_end_hour = beh;
    g_state.bedtime_end_min = bem;
    safe_str(g_state.oled_font_file, sizeof(g_state.oled_font_file), font_file);
    g_state.display_dirty = 1;
    if (changed && g_state.ft_face) {
        FT_Done_Face(g_state.ft_face);
        g_state.ft_face = NULL;
        g_state.ft_loaded_file[0] = '\0';
        g_state.ft_loaded_size = 0;
    }
    pthread_mutex_unlock(&g_state.lock);

    save_config();
    app_log("display", "Saved display settings");
    http_redirect(client, "/display");
}

static void handle_delete_font(int client, char *body) {
    char font_file[128] = "";
    struct font_delete_form_ctx form = {
        .font_file = font_file,
        .font_file_len = sizeof(font_file)
    };
    (void)for_each_form_field(body, parse_font_delete_form_field, &form);

    if (!safe_asset_filename(font_file) || !has_font_ext(font_file)) {
        http_send(client, "400 Bad Request", "text/plain", "Invalid font filename");
        return;
    }

    char path[512];
    make_font_path(font_file, path, sizeof(path));
    unlink(path);
    invalidate_asset_list_cache(&g_font_list_cache);

    pthread_mutex_lock(&g_state.lock);
    if (strcmp(g_state.oled_font_file, font_file) == 0) {
        g_state.oled_font_file[0] = '\0';
    }
    if (strcmp(g_state.ft_loaded_file, font_file) == 0) {
        if (g_state.ft_face) {
            FT_Done_Face(g_state.ft_face);
            g_state.ft_face = NULL;
        }
        g_state.ft_loaded_file[0] = '\0';
        g_state.ft_loaded_size = 0;
    }
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);

    save_config();
    app_log("fonts", "Deleted font %s", font_file);
    http_redirect(client, "/display");
}


static void handle_delete_face(int client, char *body) {
    char face_file[FACE_FILE_MAX] = "";
    char kind[32] = "normal";
    char format[16] = "";

    struct delete_face_form_ctx form = {
        .face_file = face_file,
        .face_file_len = sizeof(face_file),
        .kind = kind,
        .kind_len = sizeof(kind),
        .format = format,
        .format_len = sizeof(format)
    };
    (void)for_each_form_field(body, parse_delete_face_form_field, &form);

    int bedtime = (strcmp(kind, "bedtime") == 0);
    if (!safe_face_filename(face_file)) {
        if (strcmp(format, "json") == 0) http_send(client, "400 Bad Request", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"Invalid face filename\"}");
        else http_send(client, "400 Bad Request", "text/plain", "Invalid face filename");
        return;
    }

    char raw_path[512];
    int raw_ok = bedtime
        ? (make_bedtime_face_path_by_file(face_file, raw_path, sizeof(raw_path)) == 0)
        : (make_face_path_by_file(face_file, raw_path, sizeof(raw_path)) == 0);
    if (!raw_ok) {
        if (strcmp(format, "json") == 0) http_send(client, "400 Bad Request", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"Invalid face path\"}");
        else http_send(client, "400 Bad Request", "text/plain", "Invalid face path");
        return;
    }

    char png_path[512];
    int has_source_path = (make_source_face_png_path_by_raw(face_file, bedtime, png_path, sizeof(png_path)) == 0);

    unlink(raw_path);
    if (has_source_path) unlink(png_path);
    if (bedtime) invalidate_bedtime_face_assets();
    else invalidate_normal_face_assets();

    pthread_mutex_lock(&g_state.lock);
    if (!bedtime) {
        if (strcmp(g_state.current_face_file, face_file) == 0) g_state.current_face_file[0] = '\0';
        if (strcmp(g_state.message_face_file, face_file) == 0) g_state.message_face_file[0] = '\0';
        if (strcmp(g_clock_face_cached, face_file) == 0) g_clock_face_cached[0] = '\0';
        g_clock_face_next_change = 0;
    } else {
        if (strcmp(g_bedtime_face_cached, face_file) == 0) g_bedtime_face_cached[0] = '\0';
        g_bedtime_face_next_change = 0;
    }
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);
    app_log("faces", "Deleted %s face %s", bedtime ? "bedtime" : "normal", face_file);

    if (strcmp(format, "json") == 0) {
        http_send_json(client, "{\"ok\":true,\"deleted\":true}");
    } else {
        http_redirect(client, "/faces");
    }
}


static int delete_matching_assets_in_dir(const char *dir, int raw_files, int png_files, int mp3_files) {
    if (!dir) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;

    int deleted = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (!safe_asset_filename(name)) continue;

        int match = 0;
        if (raw_files && has_raw_ext(name)) match = 1;
        if (png_files && has_png_ext(name)) match = 1;
        if (mp3_files && has_mp3_ext(name)) match = 1;
        if (!match) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (unlink(path) == 0) deleted++;
    }

    closedir(d);
    return deleted;
}

static void parse_form_format(char *body, char *format, size_t format_len) {
    if (!format || format_len == 0) return;
    format[0] = '\0';
    struct format_form_ctx ctx = { .format = format, .format_len = format_len };
    (void)for_each_form_field(body, parse_format_form_field, &ctx);
}

static void handle_delete_assets(int client, char *body, const char *asset_type) {
    char format[16] = "";
    parse_form_format(body, format, sizeof(format));

    if (strcmp(asset_type, "faces") == 0) {
        int deleted_normal = delete_matching_assets_in_dir(FACE_DIR, 1, 1, 0);
        int deleted_bedtime = delete_matching_assets_in_dir(BEDTIME_FACE_DIR, 1, 1, 0);
        invalidate_all_face_assets();

        pthread_mutex_lock(&g_state.lock);
        g_state.current_face_file[0] = '\0';
        g_state.message_face_file[0] = '\0';
        g_state.preview_face_file[0] = '\0';
        g_state.preview_face_until = 0;
        g_state.display_dirty = 1;
        g_clock_face_cached[0] = '\0';
        g_clock_face_next_change = 0;
        g_bedtime_face_cached[0] = '\0';
        g_bedtime_face_next_change = 0;
        pthread_mutex_unlock(&g_state.lock);

        app_log("faces", "Deleted all faces, removed %d normal and %d bedtime", deleted_normal, deleted_bedtime);

        if (strcmp(format, "json") == 0) {
            char reply[160];
            snprintf(reply, sizeof(reply), "{\"ok\":true,\"deleted_normal\":%d,\"deleted_bedtime\":%d}", deleted_normal, deleted_bedtime);
            http_send_json(client, reply);
        } else {
            http_redirect(client, "/faces");
        }
        return;
    }

    if (strcmp(asset_type, "music") == 0) {
        audio_stop();
        int deleted_music = delete_matching_assets_in_dir(MUSIC_DIR, 0, 0, 1);
        invalidate_asset_list_cache(&g_music_list_cache);

        pthread_mutex_lock(&g_state.lock);
        g_state.audio_playing = 0;
        g_state.audio_file[0] = '\0';
        for (int i = 0; i < MAX_ALARMS; i++) {
            g_state.alarms[i].music_file[0] = '\0';
        }
        g_state.display_dirty = 1;
        pthread_mutex_unlock(&g_state.lock);

        save_config();
        app_log("music", "Deleted all music, removed %d files", deleted_music);

        if (strcmp(format, "json") == 0) {
            char reply[128];
            snprintf(reply, sizeof(reply), "{\"ok\":true,\"deleted_music\":%d}", deleted_music);
            http_send_json(client, reply);
        } else {
            http_redirect(client, "/music");
        }
        return;
    }

    http_send(client, "400 Bad Request", "text/plain", "Unknown asset type");
}

static void handle_show_message(int client, char *body) {
    char face_file[FACE_FILE_MAX] = "";
    char message[192] = "";
    char format[16] = "";
    struct message_form_ctx form = {
        .face_id = 1,
        .face_file = face_file,
        .face_file_len = sizeof(face_file),
        .message = message,
        .message_len = sizeof(message),
        .format = format,
        .format_len = sizeof(format)
    };
    (void)for_each_form_field(body, parse_message_form_field, &form);

    if (!face_file[0]) {
        if (face_id_valid_int(form.face_id)) snprintf(face_file, sizeof(face_file), "face_%03d.raw", form.face_id);
        else if (random_uploaded_face_file(face_file, sizeof(face_file)) != 0) face_file[0] = '\0';
    }
    sanitize_message_text(message);
    if (!message[0]) safe_str(message, sizeof(message), "Hello");

    char fit_reason[160];
    if (!message_fits_display(message, fit_reason, sizeof(fit_reason))) {
        app_log("message", "Rejected message: %s", fit_reason[0] ? fit_reason : "too long for OLED");
        if (strcmp(format, "json") == 0) {
            char body_json[256];
            snprintf(body_json, sizeof(body_json), "{\"ok\":false,\"error\":\"");
            json_append_escaped(body_json, sizeof(body_json), fit_reason[0] ? fit_reason : "Message is too long for the OLED");
            json_append(body_json, sizeof(body_json), "\"}");
            http_send(client, "400 Bad Request", "application/json; charset=utf-8", body_json);
        } else {
            http_send(client, "400 Bad Request", "text/plain", fit_reason[0] ? fit_reason : "Message is too long for the OLED");
        }
        return;
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.display_mode = 3;
    g_state.message_face = face_id_valid_int(form.face_id) ? form.face_id : 1;
    safe_str(g_state.message_face_file, sizeof(g_state.message_face_file), face_file);
    g_state.message_until = time(NULL) + MESSAGE_DEFAULT_DURATION_SECONDS;
    safe_str(g_state.message_text, sizeof(g_state.message_text), message);
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);
    app_log("message", "Sent message to OLED with face %s: %.120s", face_file[0] ? face_file : "default", message);

    if (strcmp(format, "json") == 0) {
        http_send_json(client, "{\"ok\":true,\"mode\":\"message\"}");
    } else {
        http_redirect(client, "/");
    }
}

static void handle_action(int client, const char *query) {
    char action[64] = "";
    char format[16] = "";
    parse_string_query(query, "do", action, sizeof(action));
    parse_string_query(query, "format", format, sizeof(format));

    if (action[0]) app_log("action", "Web action requested: %s", action);

    if (strcmp(action, "clock") == 0) {
        pthread_mutex_lock(&g_state.lock);
        g_state.display_mode = 0;
        g_state.display_dirty = 1;
        pthread_mutex_unlock(&g_state.lock);
    } else if (strcmp(action, "clear") == 0) {
        pthread_mutex_lock(&g_state.lock);
        g_state.display_mode = 1;
        g_state.display_dirty = 1;
        pthread_mutex_unlock(&g_state.lock);
    } else if (strcmp(action, "stop") == 0) {
        audio_stop();
    } else if (strcmp(action, "play-music") == 0) {
        char file[MUSIC_FILE_MAX] = "";
        int vol;
        parse_string_query(query, "file", file, sizeof(file));
        pthread_mutex_lock(&g_state.lock);
        vol = g_state.global_volume;
        pthread_mutex_unlock(&g_state.lock);
        audio_play_music_file(file, vol, vol, 0);
    }

    if (strcmp(format, "json") == 0) {
        http_send_json(client, "{\"ok\":true}");
    } else {
        http_redirect(client, "/");
    }
}

static void handle_face_source_png(int client, const char *query) {
    char kind[32] = "";
    char file[FACE_FILE_MAX] = "";
    parse_string_query(query, "kind", kind, sizeof(kind));
    parse_string_query(query, "file", file, sizeof(file));

    int bedtime = strcmp(kind, "bedtime") == 0;
    if (!safe_face_filename(file)) {
        http_send(client, "400 Bad Request", "text/plain", "Invalid face file");
        return;
    }

    char path[512];
    if (make_source_face_png_path_by_raw(file, bedtime, path, sizeof(path)) != 0) {
        http_send(client, "400 Bad Request", "text/plain", "Invalid source PNG path");
        return;
    }

    http_send_file(client, path, "image/png");
}


static void handle_font_file(int client, const char *query) {
    char file[128] = "";
    parse_string_query(query, "file", file, sizeof(file));

    if (!safe_asset_filename(file) || !has_font_ext(file)) {
        http_send(client, "400 Bad Request", "text/plain", "Invalid font file");
        return;
    }

    char path[512];
    make_font_path(file, path, sizeof(path));

    const char *dot = strrchr(file, '.');
    const char *ctype = (dot && strcasecmp(dot, ".otf") == 0) ? "font/otf" : "font/ttf";
    http_send_file(client, path, ctype);
}

static void handle_show_face(int client, const char *query) {
    int id = parse_int_query(query, "id", 1);
    char file[FACE_FILE_MAX] = "";
    char format[16] = "";
    parse_string_query(query, "file", file, sizeof(file));
    parse_string_query(query, "format", format, sizeof(format));

    if (!safe_face_filename(file)) {
        if (!face_id_valid_int(id)) id = 1;
        snprintf(file, sizeof(file), "face_%03d.raw", id);
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.display_mode = 0;
    g_state.current_face = face_id_valid_int(id) ? id : 1;
    safe_str(g_state.current_face_file, sizeof(g_state.current_face_file), file);
    safe_str(g_state.preview_face_file, sizeof(g_state.preview_face_file), file);
    g_state.preview_face_bedtime = 0;
    g_state.preview_face_until = time(NULL) + CLOCK_FACE_PREVIEW_SECONDS;
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);
    app_log("faces", "Show face requested: %s", file);

    if (strcmp(format, "json") == 0) {
        http_send_json(client, "{\"ok\":true,\"mode\":\"face\"}");
    } else {
        http_redirect(client, "/faces");
    }
}

struct face_upload_result {
    int ok;
    int fail;
    char first_file[FACE_FILE_MAX];
};

static int upload_face_part_cb(const struct multipart_file_part *part, void *user) {
    struct face_upload_result *res = (struct face_upload_result *)user;
    if (!part || !res || !part->filename[0] || !has_png_ext(part->filename)) {
        if (res) res->fail++;
        return -1;
    }

    char raw_name[FACE_FILE_MAX];
    make_raw_face_filename_from_upload(part->filename, raw_name, sizeof(raw_name));
    if (!safe_face_filename(raw_name)) {
        res->fail++;
        return -1;
    }

    char path[512];
    if (make_face_path_by_file(raw_name, path, sizeof(path)) != 0) {
        res->fail++;
        return -1;
    }

    if (save_face_raw_from_png_memory(part->data, part->len, path) != 0) {
        res->fail++;
        return -1;
    }

    if (save_source_face_png_if_missing(raw_name, 0, part->data, part->len) != 0) {
        fprintf(stderr, "warning: could not save source PNG for face %s\n", raw_name);
    }

    if (!res->first_file[0]) safe_str(res->first_file, sizeof(res->first_file), raw_name);
    res->ok++;
    return 0;
}

static void handle_upload_face(int client, const char *query, char *body, size_t body_len, const char *ctype) {
    (void)query;
    const char *b = strstr(ctype ? ctype : "", "boundary=");
    if (!b) {
        http_send(client, "400 Bad Request", "text/plain", "Missing multipart boundary");
        return;
    }
    b += 9;

    struct face_upload_result res;
    memset(&res, 0, sizeof(res));

    if (foreach_multipart_file(body, body_len, b, upload_face_part_cb, &res) < 0 || res.ok <= 0) {
        http_send(client, "400 Bad Request", "text/plain", "No PNG face files were uploaded or conversion failed");
        return;
    }

    invalidate_normal_face_assets();

    pthread_mutex_lock(&g_state.lock);
    g_state.display_mode = 0;
    safe_str(g_state.current_face_file, sizeof(g_state.current_face_file), res.first_file);
    safe_str(g_state.preview_face_file, sizeof(g_state.preview_face_file), res.first_file);
    g_state.preview_face_bedtime = 0;
    g_state.preview_face_until = time(NULL) + CLOCK_FACE_PREVIEW_SECONDS;
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);
    app_log("faces", "Uploaded %d normal face file(s), first %s", res.ok, res.first_file);

    http_redirect(client, "/faces");
}

static int upload_bedtime_face_part_cb(const struct multipart_file_part *part, void *user) {
    struct face_upload_result *res = (struct face_upload_result *)user;
    if (!part || !res || !part->filename[0] || !has_png_ext(part->filename)) {
        if (res) res->fail++;
        return -1;
    }

    char raw_name[FACE_FILE_MAX];
    make_raw_face_filename_from_upload(part->filename, raw_name, sizeof(raw_name));
    if (!safe_face_filename(raw_name)) {
        res->fail++;
        return -1;
    }

    char path[512];
    if (make_bedtime_face_path_by_file(raw_name, path, sizeof(path)) != 0) {
        res->fail++;
        return -1;
    }

    if (save_face_raw_from_png_memory(part->data, part->len, path) != 0) {
        res->fail++;
        return -1;
    }

    if (save_source_face_png_if_missing(raw_name, 1, part->data, part->len) != 0) {
        fprintf(stderr, "warning: could not save source PNG for bedtime face %s\n", raw_name);
    }

    if (!res->first_file[0]) safe_str(res->first_file, sizeof(res->first_file), raw_name);
    res->ok++;
    return 0;
}

static void handle_upload_bedtime_face(int client, const char *query, char *body, size_t body_len, const char *ctype) {
    (void)query;
    const char *b = strstr(ctype ? ctype : "", "boundary=");
    if (!b) {
        http_send(client, "400 Bad Request", "text/plain", "Missing multipart boundary");
        return;
    }
    b += 9;

    struct face_upload_result res;
    memset(&res, 0, sizeof(res));

    if (foreach_multipart_file(body, body_len, b, upload_bedtime_face_part_cb, &res) < 0 || res.ok <= 0) {
        http_send(client, "400 Bad Request", "text/plain", "No bedtime PNG face files were uploaded or conversion failed");
        return;
    }

    invalidate_bedtime_face_assets();
    pthread_mutex_lock(&g_state.lock);
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);
    app_log("faces", "Uploaded %d bedtime face file(s), first %s", res.ok, res.first_file);

    http_redirect(client, "/faces");
}

struct upload_music_result {
    int ok;
    int failed;
    char first_file[MUSIC_FILE_MAX];
    char last_error[160];
};

static void set_upload_music_error(struct upload_music_result *res, const char *name, const char *suffix) {
    char shown_name[96];
    if (name && *name) safe_str(shown_name, sizeof(shown_name), name);
    else safe_str(shown_name, sizeof(shown_name), "uploaded file");
    snprintf(res->last_error, sizeof(res->last_error), "%s %s", shown_name, suffix ? suffix : "failed");
}

static int upload_music_part_cb(const struct multipart_file_part *part, void *user) {
    struct upload_music_result *res = (struct upload_music_result *)user;
    char safe_name[MUSIC_FILE_MAX] = "";
    sanitize_asset_filename(part->filename, safe_name, sizeof(safe_name), "alarm.mp3");

    if (!has_mp3_ext(safe_name)) {
        res->failed++;
        set_upload_music_error(res, safe_name[0] ? safe_name : part->filename, "is not an MP3");
        return -1;
    }
    if (!safe_asset_filename(safe_name)) {
        res->failed++;
        set_upload_music_error(res, safe_name, "has an invalid filename");
        return -1;
    }

    char path[512];
    make_music_path(safe_name, path, sizeof(path));
    if (save_bytes(path, part->data, part->len) != 0) {
        res->failed++;
        set_upload_music_error(res, safe_name, "could not be saved");
        return -1;
    }

    if (!res->first_file[0]) safe_str(res->first_file, sizeof(res->first_file), safe_name);
    res->ok++;
    return 0;
}

static void handle_upload_music(int client, char *body, size_t body_len, const char *ctype) {
    const char *b = strstr(ctype ? ctype : "", "boundary=");
    if (!b) {
        http_send(client, "400 Bad Request", "text/plain", "Missing multipart boundary");
        return;
    }
    b += 9;

    ensure_dir(MUSIC_DIR);
    struct upload_music_result res;
    memset(&res, 0, sizeof(res));

    int scanned = foreach_multipart_file(body, body_len, b, upload_music_part_cb, &res);
    if (scanned < 0 || res.ok <= 0) {
        http_send(client, "400 Bad Request", "text/plain", res.last_error[0] ? res.last_error : "No uploaded MP3 file found");
        return;
    }

    invalidate_asset_list_cache(&g_music_list_cache);

    if (res.failed > 0) {
        app_log("music", "Uploaded %d MP3 file(s), %d skipped, first %s", res.ok, res.failed, res.first_file);
    } else {
        app_log("music", "Uploaded %d MP3 file(s), first %s", res.ok, res.first_file);
    }
    http_redirect(client, "/music");
}

static void handle_upload_font(int client, char *body, size_t body_len, const char *ctype) {
    const char *b = strstr(ctype ? ctype : "", "boundary=");
    if (!b) {
        http_send(client, "400 Bad Request", "text/plain", "Missing multipart boundary");
        return;
    }
    b += 9;

    uint8_t *file_data = NULL;
    size_t file_len = 0;
    if (extract_multipart_file(body, body_len, b, &file_data, &file_len) != 0) {
        http_send(client, "400 Bad Request", "text/plain", "No uploaded font found");
        return;
    }

    char upload_name[160] = "";
    char safe_name[128] = "";
    extract_multipart_filename(body, body_len, b, upload_name, sizeof(upload_name));
    sanitize_asset_filename(upload_name, safe_name, sizeof(safe_name), "uploaded_font.ttf");

    if (!has_font_ext(safe_name)) {
        http_send(client, "400 Bad Request", "text/plain", "Font must be .ttf or .otf");
        return;
    }

    ensure_dir(FONT_DIR);
    char path[512];
    make_font_path(safe_name, path, sizeof(path));

    if (save_bytes(path, file_data, file_len) != 0) {
        http_send(client, "500 Internal Server Error", "text/plain", "Could not save font");
        return;
    }
    invalidate_asset_list_cache(&g_font_list_cache);

    char font_path[512];
    make_font_path(safe_name, font_path, sizeof(font_path));
    FT_Face test_face = NULL;
    int ok = 0;

    pthread_mutex_lock(&g_state.lock);
    if (!g_state.ft_library) {
        if (FT_Init_FreeType(&g_state.ft_library) != 0) g_state.ft_library = NULL;
    }
    if (g_state.ft_library && FT_New_Face(g_state.ft_library, font_path, 0, &test_face) == 0) {
        ok = 1;
        FT_Done_Face(test_face);
    }
    if (ok) {
        safe_str(g_state.oled_font_file, sizeof(g_state.oled_font_file), safe_name);
        if (g_state.ft_face) {
            FT_Done_Face(g_state.ft_face);
            g_state.ft_face = NULL;
        }
        g_state.ft_loaded_file[0] = '\0';
        g_state.ft_loaded_size = 0;
    }
    pthread_mutex_unlock(&g_state.lock);

    if (!ok) {
        unlink(path);
        http_send(client, "400 Bad Request", "text/plain", "Uploaded file is not a readable TrueType/OpenType font");
        return;
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);

    save_config();
    app_log("fonts", "Uploaded font %s", safe_name);
    http_redirect(client, "/display");
}

static char *read_http_request(int client, size_t *out_len) {
    size_t cap = 16384;
    size_t len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) return NULL;

    size_t header_end_pos = 0;
    int content_length = 0;

    while (len < HTTP_MAX_REQUEST) {
        if (len == cap) {
            cap *= 2;
            if (cap > HTTP_MAX_REQUEST) cap = HTTP_MAX_REQUEST;
            char *nb = realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }

        ssize_t n = read(client, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return NULL;
        }
        if (n == 0) break;
        len += (size_t)n;
        buf[len] = '\0';

        if (header_end_pos == 0) {
            char *he = strstr(buf, "\r\n\r\n");
            if (he) {
                header_end_pos = (size_t)(he - buf) + 4;
                char *cl = strcasestr(buf, "Content-Length:");
                if (cl && (size_t)(cl - buf) < header_end_pos) {
                    cl += strlen("Content-Length:");
                    while (*cl == ' ' || *cl == '\t') cl++;
                    content_length = atoi(cl);
                    if (content_length < 0 || content_length > HTTP_MAX_REQUEST) {
                        free(buf);
                        return NULL;
                    }
                }
            }
        }

        if (header_end_pos > 0) {
            size_t need = header_end_pos + (size_t)content_length;
            if (len >= need) break;
        }
    }

    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static void handle_client(int client);

static void set_client_socket_timeouts(int client) {
    struct timeval tv;
    tv.tv_sec = HTTP_SOCKET_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int http_worker_try_acquire(void) {
    int ok = 0;
    pthread_mutex_lock(&g_http_worker_lock);
    if (g_http_active_workers < HTTP_MAX_WORKERS) {
        g_http_active_workers++;
        ok = 1;
    }
    pthread_mutex_unlock(&g_http_worker_lock);
    return ok;
}

static void http_worker_release(void) {
    pthread_mutex_lock(&g_http_worker_lock);
    if (g_http_active_workers > 0) g_http_active_workers--;
    pthread_mutex_unlock(&g_http_worker_lock);
}

static void *http_client_thread_main(void *arg) {
    int client = (int)(intptr_t)arg;

    if (client >= 0) {
        handle_client(client);
        close(client);
    }

    http_worker_release();
    return NULL;
}

typedef struct {
    int client;
    const char *method;
    const char *path;
    const char *query;
    char *body;
    size_t body_len;
    const char *ctype;
} http_request_t;

typedef void (*route_handler_t)(const http_request_t *req);

struct http_route {
    const char *method;
    const char *path;
    route_handler_t handler;
};

/* --- Pure Client Socket Adapters --- */
static void adapter_client(const http_request_t *req, void (*real_handler)(int)) {
    real_handler(req->client);
}

static void route_status_json(const http_request_t *req)        { adapter_client(req, send_status_json); }
static void route_fonts_json(const http_request_t *req)         { adapter_client(req, send_fonts_json); }
static void route_music_json(const http_request_t *req)         { adapter_client(req, send_music_json); }
static void route_message_limit(const http_request_t *req)      { adapter_client(req, send_message_limit_json); }
static void route_log_json(const http_request_t *req)           { adapter_client(req, send_log_json); }
static void route_clear_log(const http_request_t *req)          { adapter_client(req, handle_clear_log); }

/* --- Query Parameter Adapters --- */
static void adapter_query(const http_request_t *req, void (*real_handler)(int, const char *)) {
    real_handler(req->client, req->query);
}

static void route_faces_json(const http_request_t *req)         { adapter_query(req, send_faces_json); }
static void route_face_source(const http_request_t *req)        { adapter_query(req, handle_face_source_png); }
static void route_font_file(const http_request_t *req)          { adapter_query(req, handle_font_file); }
static void route_message_fit(const http_request_t *req)        { adapter_query(req, send_message_fit_json); }
static void route_handle_action(const http_request_t *req)      { adapter_query(req, handle_action); }
static void route_handle_show_face(const http_request_t *req)   { adapter_query(req, handle_show_face); }

/* --- POST/Form Body Adapters --- */
static void adapter_body(const http_request_t *req, void (*real_handler)(int, char *)) {
    real_handler(req->client, req->body);
}

static void route_save_alarm(const http_request_t *req)         { adapter_body(req, handle_save_alarm); }
static void route_save_audio(const http_request_t *req)         { adapter_body(req, handle_save_audio); }
static void route_save_personalization(const http_request_t *req) { adapter_body(req, handle_save_personalization); }
static void route_save_display(const http_request_t *req)       { adapter_body(req, handle_save_display); }
static void route_delete_font(const http_request_t *req)        { adapter_body(req, handle_delete_font); }
static void route_delete_face(const http_request_t *req)        { adapter_body(req, handle_delete_face); }
static void route_show_message(const http_request_t *req)       { adapter_body(req, handle_show_message); }

/* --- Consolidated Asset Deletion Adapters --- */
static void route_delete_all_faces(const http_request_t *req)   { handle_delete_assets(req->client, req->body, "faces"); }
static void route_delete_all_music(const http_request_t *req)   { handle_delete_assets(req->client, req->body, "music"); }

/* --- Multipart/Binary Upload Adapters --- */
static void adapter_upload_query(const http_request_t *req, void (*real_handler)(int, const char *, char *, size_t, const char *)) {
    real_handler(req->client, req->query, req->body, req->body_len, req->ctype);
}

static void adapter_upload_body(const http_request_t *req, void (*real_handler)(int, char *, size_t, const char *)) {
    real_handler(req->client, req->body, req->body_len, req->ctype);
}

static void route_upload_face(const http_request_t *req)        { adapter_upload_query(req, handle_upload_face); }
static void route_upload_bedtime(const http_request_t *req)     { adapter_upload_query(req, handle_upload_bedtime_face); }
static void route_upload_music(const http_request_t *req)       { adapter_upload_body(req, handle_upload_music); }
static void route_upload_font(const http_request_t *req)        { adapter_upload_body(req, handle_upload_font); }

static const struct http_route routes[] = {
    /* Status & Asset API JSON targets */
    {"GET",  "/api/status",         route_status_json},
    {"GET",  "/api/faces",          route_faces_json},
    {"GET",  "/api/fonts",          route_fonts_json},
    {"GET",  "/api/music",          route_music_json},
    {"GET",  "/api/message-limit",  route_message_limit},
    {"GET",  "/api/message-fit",    route_message_fit},
    {"GET",  "/api/log",            route_log_json},
    {"GET",  "/face-source",        route_face_source},
    {"GET",  "/font-file",          route_font_file},

    /* Config & Device Interactions */
    {"GET",  "/action",             route_handle_action},
    {"GET",  "/face",               route_handle_show_face},
    {"POST", "/save-alarm",         route_save_alarm},
    {"POST", "/save-audio",         route_save_audio},
    {"POST", "/save-personalization", route_save_personalization},
    {"POST", "/save-display",       route_save_display},
    {"POST", "/show-message",       route_show_message},
    {"POST", "/message",            route_show_message},
    {"POST", "/clear-log",          route_clear_log},

    /* Destruction & Deletions */
    {"POST", "/delete-font",        route_delete_font},
    {"POST", "/delete-face",        route_delete_face},
    {"POST", "/delete-all-faces",   route_delete_all_faces},
    {"POST", "/delete-all-music",   route_delete_all_music},

    /* Asset Binary Storage Uploads */
    {"POST", "/upload-face",         route_upload_face},
    {"POST", "/upload-bedtime-face", route_upload_bedtime},
    {"POST", "/upload-music",        route_upload_music},
    {"POST", "/upload-font",         route_upload_font}
};

struct static_file_route {
    const char *path;
    const char *file;
    const char *ctype;
};

struct static_view {
    const char *path;
    const char *file;
};

static const struct static_file_route static_files[] = {
    {"/style.css", "style.css", "text/css; charset=utf-8"},
    {"/app.js",    "app.js",    "application/javascript; charset=utf-8"}
};

static const struct static_view static_views[] = {
    {"/",              "index.html"},
    {"/index.html",    "index.html"},
    {"/faces",         "index.html"},
    {"/faces.html",    "index.html"},
    {"/fonts",         "index.html"},
    {"/fonts.html",    "index.html"},
    {"/music",         "index.html"},
    {"/music.html",    "index.html"},
    {"/alarm",         "index.html"},
    {"/alarm.html",    "index.html"},
    {"/display",       "index.html"},
    {"/display.html",  "index.html"},
    {"/message",       "index.html"},
    {"/messages",      "index.html"},
    {"/message.html",  "index.html"},
    {"/messages.html", "index.html"},
    {"/log",           "index.html"},
    {"/log.html",      "index.html"}
};

static int send_static_ui_route(int client, const char *method, const char *path) {
    if (strcmp(method, "GET") != 0) return 0;

    size_t num_files = sizeof(static_files) / sizeof(static_files[0]);
    for (size_t i = 0; i < num_files; i++) {
        if (strcmp(path, static_files[i].path) == 0) {
            char file_path[512];
            snprintf(file_path, sizeof(file_path), WEB_DIR "/%s", static_files[i].file);
            http_send_file(client, file_path, static_files[i].ctype);
            return 1;
        }
    }

    size_t num_views = sizeof(static_views) / sizeof(static_views[0]);
    for (size_t i = 0; i < num_views; i++) {
        if (strcmp(path, static_views[i].path) == 0) {
            send_static_page(client, static_views[i].file);
            return 1;
        }
    }

    return 0;
}

static void handle_client(int client) {
    size_t req_len = 0;
    char *req = read_http_request(client, &req_len);
    if (!req) return;

    char method[8] = "";
    char target[512] = "";
    sscanf(req, "%7s %511s", method, target);

    char *headers_end = strstr(req, "\r\n\r\n");
    char *body = headers_end ? headers_end + 4 : NULL;
    size_t body_len = headers_end ? req_len - (size_t)(body - req) : 0;

    char ctype[512] = "";
    char *ct = strcasestr(req, "Content-Type:");
    if (ct && headers_end && ct < headers_end) {
        ct += strlen("Content-Type:");
        while (*ct == ' ' || *ct == '\t') ct++;
        char *eol = strstr(ct, "\r\n");
        if (eol) {
            size_t n = (size_t)(eol - ct);
            if (n >= sizeof(ctype)) n = sizeof(ctype) - 1;
            memcpy(ctype, ct, n);
            ctype[n] = 0;
        }
    }

    char path[512];
    char *query = NULL;
    safe_str(path, sizeof(path), target);
    char *q = strchr(path, '?');
    if (q) {
        *q = '\0';
        query = q + 1;
    }

    http_request_t request = {
        .client = client,
        .method = method,
        .path = path,
        .query = query,
        .body = body,
        .body_len = body_len,
        .ctype = ctype
    };

    int route_matched = 0;
    size_t num_routes = sizeof(routes) / sizeof(routes[0]);
    for (size_t i = 0; i < num_routes; i++) {
        if (strcmp(request.method, routes[i].method) == 0 && strcmp(request.path, routes[i].path) == 0) {
            routes[i].handler(&request);
            route_matched = 1;
            break;
        }
    }

    if (!route_matched && !send_static_ui_route(client, method, path)) {
        http_send(client, "404 Not Found", "text/plain", "Not Found");
    }

    free(req);
}

static void *http_thread_main(void *arg) {
    (void)arg;
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        return NULL;
    }

    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)HTTP_PORT);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(server);
        return NULL;
    }
    if (listen(server, 8) != 0) {
        perror("listen");
        close(server);
        return NULL;
    }

    fprintf(stderr, "web gui listening on port %d\n", HTTP_PORT);

    while (g_running) {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = server;
        pfd.events = POLLIN;

        int pr = poll(&pfd, 1, HTTP_ACCEPT_POLL_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (pr == 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client = accept(server, (struct sockaddr *)&peer, &peer_len);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("accept");
            break;
        }

        set_client_socket_timeouts(client);

        if (!http_worker_try_acquire()) {
            app_log("http", "Rejected client: worker limit reached");
            close(client);
            continue;
        }

        pthread_t client_thread;
        if (pthread_create(&client_thread, NULL, http_client_thread_main, (void *)(intptr_t)client) != 0) {
            http_worker_release();
            close(client);
            continue;
        }
        pthread_detach(client_thread);
    }

    close(server);
    return NULL;
}

/* ---------------- Main loop ---------------- */

static void check_alarm(void) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    struct alarm_slot fire_alarm;
    int fire = 0;

    pthread_mutex_lock(&g_state.lock);
    for (int i = 0; i < MAX_ALARMS; i++) {
        struct alarm_slot *a = &g_state.alarms[i];
        int weekday_ok = (a->weekdays & (1 << tmv.tm_wday)) != 0;
        if (a->enabled && weekday_ok && tmv.tm_hour == a->hour && tmv.tm_min == a->min && a->fired_yday != tmv.tm_yday) {
            a->fired_yday = tmv.tm_yday;
            fire_alarm = *a;
            fire = 1;
            break;
        }
    }
    sync_legacy_alarm_fields_locked();
    if (fire) g_state.display_mode = 0;
    pthread_mutex_unlock(&g_state.lock);

    if (fire) {
        app_log("alarm", "Alarm fired at %02d:%02d", fire_alarm.hour, fire_alarm.min);
        audio_play_music_file(fire_alarm.music_file, fire_alarm.start_volume, fire_alarm.end_volume, 1);
    }
}

int main(void) {
    g_start_time = time(NULL);

    if (mpg123_init() != MPG123_OK) {
        fprintf(stderr, "mpg123_init failed\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    srand((unsigned int)(time(NULL) ^ getpid()));

    ensure_dir(APP_ROOT);
    ensure_dir(FACE_DIR);
    ensure_dir(BEDTIME_FACE_DIR);
    ensure_dir(MUSIC_DIR);
    ensure_dir(FONT_DIR);
    ensure_dir(WEB_DIR);
    ensure_dir(CONFIG_DIR);
    init_alarm_defaults();
    load_config();
    app_log("system", "mk-piclock %s starting", APP_VERSION);

    if (oled_init() == 0) {
        pthread_mutex_lock(&g_state.lock);
        g_state.oled_ok = 1;
        pthread_mutex_unlock(&g_state.lock);
        app_log("system", "OLED initialized");
        draw_startup_screen();
        sleep(STARTUP_GREETING_SECONDS);
        draw_clock_screen();
    } else {
        app_log("system", "OLED init failed, web server will still start");
        fprintf(stderr, "OLED init failed, web server will still start\n");
    }

    pthread_t http_thread;
    app_log("system", "Web GUI listening on port %d", HTTP_PORT);

    if (pthread_create(&http_thread, NULL, http_thread_main, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }

    int last_min = -1;
    int last_mode = -1;
    int last_face = -1;
    int last_colon_phase = -1;
    char last_face_file[FACE_FILE_MAX] = "";
    while (g_running) {
        check_alarm();
        apply_bedtime_brightness();

        pthread_mutex_lock(&g_state.lock);
        int mode = g_state.display_mode;
        int dirty = g_state.display_dirty;
        g_state.display_dirty = 0;
        int face = g_state.current_face;
        char face_file[FACE_FILE_MAX];
        safe_str(face_file, sizeof(face_file), g_state.current_face_file);
        int oled_ok = g_state.oled_ok;
        pthread_mutex_unlock(&g_state.lock);

        if (oled_ok) {
            if (mode == 3) {
                time_t until;
                int msg_face;
                char msg_face_file[FACE_FILE_MAX];
                char msg_text[192];
                pthread_mutex_lock(&g_state.lock);
                until = g_state.message_until;
                msg_face = g_state.message_face;
                safe_str(msg_face_file, sizeof(msg_face_file), g_state.message_face_file);
                safe_str(msg_text, sizeof(msg_text), g_state.message_text);
                pthread_mutex_unlock(&g_state.lock);

                if (time(NULL) >= until) {
                    pthread_mutex_lock(&g_state.lock);
                    g_state.display_mode = 0;
                    g_state.message_until = 0;
                    pthread_mutex_unlock(&g_state.lock);
                    mode = 0;
                    last_mode = -1;
                } else if (dirty || mode != last_mode) {
                    if (msg_face_file[0]) draw_message_screen_file(msg_face_file, msg_text);
                    else draw_message_screen(msg_face, msg_text);
                }
            }

            if (mode == 0) {
                time_t now = time(NULL);
                struct tm tmv;
                localtime_r(&now, &tmv);
                int colon_phase = clock_colon_blink_phase();
                if (dirty || mode != last_mode || tmv.tm_min != last_min || colon_phase != last_colon_phase || clock_face_refresh_due()) {
                    last_min = tmv.tm_min;
                    last_colon_phase = colon_phase;
                    draw_clock_screen();
                }
            } else if (mode == 1) {
                if (dirty || mode != last_mode) {
                    oled_clear_fb(0);
                    oled_flush_full();
                }
            } else if (mode == 2) {
                if (dirty || mode != last_mode || face != last_face || strcmp(face_file, last_face_file) != 0) {
                    if (face_file[0]) draw_face_screen_file(face_file);
                    else draw_face_screen(face);
                }
            }
        }

        last_mode = mode;
        last_face = face;
        safe_str(last_face_file, sizeof(last_face_file), face_file);
        usleep(250000);
    }

    audio_stop();
    pthread_mutex_lock(&g_state.lock);
    font_cache_close_locked();
    pthread_mutex_unlock(&g_state.lock);
    oled_close();
    mpg123_exit();
    return 0;
}
