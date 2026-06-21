/*
  mk-piclock.c

  Private Raspberry Pi alarm clock core daemon.

  Features:
    - SSD1322 256x64 OLED over /dev/spidev0.0
    - libgpiod GPIO for OLED DC/RST and TTP223B touch input
    - Private Unix socket control service for mk-piclock-api
    - MP3 playback using libmpg123 + ALSA PCM
    - Alarms, fonts, faces, messages, bedtime dimming, touch input, and config

  Build both services with the supplied Makefile.
*/

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <pwd.h>
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
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <gpiod.h>

#include "ipc_protocol.h"
#include "asset_format.h"
#include "util.h"

#define safe_str mp_safe_str
#define write_all mp_write_full

#define APP_NAME "mk-piclock-core"
#define APP_VERSION "1.6.10"
#define DEFAULT_CLOCK_NAME "Rylie"
#define STARTUP_GREETING_SECONDS 3
#define APP_ROOT "/opt/mk-piclock"
#define FACE_DIR APP_ROOT "/assets/faces"
#define BEDTIME_FACE_DIR APP_ROOT "/assets/bedtime-faces"
#define MUSIC_DIR APP_ROOT "/assets/music"
#define FONT_DIR APP_ROOT "/assets/fonts"
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

#define MESSAGE_TEXT_X 70
#define MESSAGE_TEXT_W (OLED_W - MESSAGE_TEXT_X - 4)
#define MESSAGE_MAX_LINES 3
#define MESSAGE_LINE_CHARS 64
#define MESSAGE_INPUT_MAX_CHARS 180
#define MESSAGE_FALLBACK_SCALE 2
#define MESSAGE_TTF_MIN_PX 12
#define MESSAGE_TTF_MAX_PX 18
#define MESSAGE_DEFAULT_DURATION_SECONDS 30
#define MESSAGE_DELAY_MAX_SECONDS 60


#define GPIO_CHIP "/dev/gpiochip0"
#define GPIO_DC 25
#define GPIO_RST 27
#define GPIO_TOUCH 20
#define TOUCH_POLL_MS 20u
#define TOUCH_DEBOUNCE_MS 50u
#define TOUCH_LONG_PRESS_MS 3000u

#define FACE_COUNT 64
#define MAX_ALARMS 7
#define MUSIC_FILE_MAX 256
#define FACE_FILE_MAX 128
#define CLOCK_FACE_CHANGE_MAX_SECONDS 300
#define BEDTIME_FACE_CHANGE_SECONDS 300
#define CLOCK_TIME_Y_OFFSET -7
#define CLOCK_FACE_Y_NUDGE -2
#define CLOCK_SIDE_WIDGET_SIZE 54
#define CLOCK_SIDE_WIDGET_X 4
#define CLOCK_FACE_PREVIEW_SECONDS 15
#define CLOCK_STATUS_PILL_H 11
#define SONG_METADATA_X 74
#define SONG_METADATA_W (OLED_W - SONG_METADATA_X - 2)
#define SONG_METADATA_TEXT_MAX (MP_ID3_TEXT_MAX * 2 + 4)
#define SONG_SCROLL_PAUSE_MS 1250u
#define SONG_SCROLL_SPEED_PX_PER_SEC 18u


#define CORE_RUNTIME_DIR "/run/mk-piclock"
#define CORE_SOCKET_PATH CORE_RUNTIME_DIR "/core.sock"
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
    uint8_t raw[MP_FACE_RAW_BYTES];
    int valid;
    unsigned long generation;
};

static pthread_mutex_t g_face_raw_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct face_raw_cache g_face_raw_cache = { .file = "", .bedtime = 0, .valid = 0, .generation = 0 };

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
    int display_dirty;      /* set by API IPC thread; drawn by main OLED loop */
    int current_face;       /* legacy numeric face */
    int message_face;       /* legacy numeric face */
    char current_face_file[FACE_FILE_MAX];
    char preview_face_file[FACE_FILE_MAX];
    int preview_face_bedtime;
    time_t preview_face_until;
    char message_face_file[FACE_FILE_MAX];
    time_t message_until;   /* unix time; 0 = no active message */
    char message_text[192];
    int pending_message_face;
    char pending_message_face_file[FACE_FILE_MAX];
    char pending_message_text[192];
    time_t pending_message_at;
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
    char audio_title[MP_ID3_TEXT_MAX];
    char audio_artist[MP_ID3_TEXT_MAX];
    char audio_display[SONG_METADATA_TEXT_MAX];
    uint64_t audio_scroll_started_ms;
    int show_song_metadata;
    int alarm_active;             /* 1 only while an alarm MP3 is currently playing */
    int alarm_volume_percent;     /* current alarm ramp volume, 0..100 */
    struct alarm_slot alarms[MAX_ALARMS];
    int oled_ok;
    char clock_name[64];
    int oled_font;         /* 0 seven, 1 seven thin, 2 pixel, 3 pixel bold */
    char oled_font_file[128]; /* uploaded .ttf/.otf filename, empty = built-in */
    int oled_font_size;    /* TrueType pixel size */
    int clock_24h_mode;    /* 0 = 12-hour, 1 = 24-hour */
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
    .pending_message_face = 1,
    .pending_message_face_file = "",
    .pending_message_text = "",
    .pending_message_at = 0,
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
    .audio_title = "",
    .audio_artist = "",
    .audio_display = "",
    .audio_scroll_started_ms = 0,
    .show_song_metadata = 1,
    .alarm_active = 0,
    .alarm_volume_percent = 0,
    .oled_ok = 0,
    .clock_name = DEFAULT_CLOCK_NAME,
    .oled_font = 0,
    .oled_font_file = "",
    .oled_font_size = 48,
    .clock_24h_mode = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

struct font_cache_state {
    pthread_mutex_t lock;
    FT_Library library;
    FT_Face face;
    char loaded_file[128];
    int loaded_size;
};

static struct font_cache_state g_font = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .library = NULL,
    .face = NULL,
    .loaded_file = "",
    .loaded_size = 0
};

struct audio_player_state {
    pthread_mutex_t lock;
    pthread_cond_t stopped;
    int running;
    int stop_requested;
    char file[MUSIC_FILE_MAX];
};

static struct audio_player_state g_audio = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .stopped = PTHREAD_COND_INITIALIZER,
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

struct touch_dev {
    struct gpiod_chip *chip;
    struct gpiod_line_request *request;
    pthread_mutex_t lock;
    int ready;
    int pressed;
};

static struct touch_dev g_touch = {
    .chip = NULL,
    .request = NULL,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .ready = 0,
    .pressed = 0
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

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void config_encode_to_buffer(char *out, size_t out_len, const char *in) {
    static const char hex[] = "0123456789ABCDEF";
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!in) in = "";

    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && j + 1 < out_len; p++) {
        unsigned char ch = *p;
        int plain = isalnum(ch) || ch == '.' || ch == '_' || ch == '-';
        if (plain) {
            out[j++] = (char)ch;
        } else {
            if (j + 3 >= out_len) break;
            out[j++] = '%';
            out[j++] = hex[(ch >> 4) & 0x0F];
            out[j++] = hex[ch & 0x0F];
        }
    }
    out[j] = '\0';
}

static void config_decode_inplace(char *s) {
    if (!s) return;
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = hex_value(p[1]);
            int lo = hex_value(p[2]);
            if (hi >= 0 && lo >= 0) {
                *o++ = (char)((hi << 4) | lo);
                p += 2;
                continue;
            }
        }
        *o++ = *p;
    }
    *o = '\0';
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

static int face_id_valid_int(int id) {
    return id >= 1 && id <= FACE_COUNT;
}

static int has_raw_ext(const char *name) {
    const char *dot = strrchr(name ? name : "", '.');
    return dot && strcasecmp(dot, ".raw") == 0;
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
    ASSET_LIST_MUSIC_MP3 = 2
};

static void invalidate_face_raw_cache(void) {
    pthread_mutex_lock(&g_face_raw_cache_lock);
    g_face_raw_cache.valid = 0;
    g_face_raw_cache.file[0] = '\0';
    g_face_raw_cache.generation++;
    pthread_mutex_unlock(&g_face_raw_cache_lock);
}

static void invalidate_normal_face_assets(void) {
    invalidate_face_raw_cache();
    g_clock_face_cached[0] = '\0';
    g_clock_face_next_change = 0;
}

static void invalidate_bedtime_face_assets(void) {
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
        return stat(path, &st) == 0 && st.st_size == MP_FACE_RAW_BYTES;
    }
    return safe_asset_filename(name) && kind == ASSET_LIST_MUSIC_MP3 && has_mp3_ext(name);
}

static int scan_asset_files(const char *dir, int kind, char files[][ASSET_LIST_NAME_MAX], int max_files) {
    if (!dir || !files || max_files <= 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < max_files) {
        if (!asset_file_matches_kind(dir, de->d_name, kind)) continue;
        safe_str(files[count++], ASSET_LIST_NAME_MAX, de->d_name);
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
    X("show_song_metadata", g_state.show_song_metadata, "%d") \
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
    do { \
        char enc[(field_size) * 3]; \
        config_encode_to_buffer(enc, sizeof(enc), field); \
        fprintf(f, "alarm%d_%s=%s\n", (idx) + 1, field_str, enc); \
    } while (0);
    CONFIG_ALARM_STRING_FIELDS(SAVE_CONFIG_ALARM_STRING)
#undef SAVE_CONFIG_ALARM_STRING
#define SAVE_CONFIG_STRING(cfg_key, field, field_size) \
    do { \
        char enc[(field_size) * 3]; \
        config_encode_to_buffer(enc, sizeof(enc), field); \
        fprintf(f, cfg_key "=%s\n", enc); \
    } while (0);
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
                char dec[384]; \
                safe_str(dec, sizeof(dec), val); \
                config_decode_inplace(dec); \
                safe_str(field, field_size, dec); \
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
                char dec[384]; \
                safe_str(dec, sizeof(dec), val); \
                config_decode_inplace(dec); \
                safe_str(field, field_size, dec); \
                matched = 1; \
            } \
        } while (0);
        CONFIG_STRING_FIELDS(LOAD_CONFIG_STRING)
#undef LOAD_CONFIG_STRING
        if (!matched && strcmp(key, "web_font") == 0) g_state.oled_font = atoi(val); /* old config compatibility */
    }

    fclose(f);

    g_state.global_volume = clamp_int(g_state.global_volume, 0, 100);
    g_state.show_song_metadata = g_state.show_song_metadata ? 1 : 0;
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


static uint64_t monotonic_millis(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void oled_ascii_text(const char *input, char *output, size_t output_len) {
    if (!output || output_len == 0) return;
    output[0] = '\0';
    if (!input) return;
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p && used + 1 < output_len;) {
        if (*p < 0x80) {
            unsigned char ch = *p++;
            if (ch < 32 || ch == 127) ch = ' ';
            output[used++] = (char)toupper(ch);
            continue;
        }
        unsigned char first = *p++;
        int continuation = (first & 0xe0) == 0xc0 ? 1 : (first & 0xf0) == 0xe0 ? 2 : (first & 0xf8) == 0xf0 ? 3 : 0;
        while (continuation-- > 0 && (*p & 0xc0) == 0x80) p++;
        output[used++] = '?';
    }
    output[used] = '\0';

    char *src = output;
    while (*src == ' ') src++;
    if (src != output) memmove(output, src, strlen(src) + 1);
    size_t len = strlen(output);
    while (len && output[len - 1] == ' ') output[--len] = '\0';
}

static void draw_char5x7_clipped(int x, int y, char ch, uint8_t c, int clip_x0, int clip_x1) {
    const uint8_t *glyph = font5x7_glyph(ch);
    for (int row = 0; row < 7; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 5; col++) {
            int px = x + col;
            if (px < clip_x0 || px >= clip_x1) continue;
            if (bits & (1 << (4 - col))) oled_set_px(px, y + row, c);
        }
    }
}

static void draw_text5x7_clipped(int x, int y, const char *text, uint8_t c, int clip_x0, int clip_x1) {
    if (!text || clip_x1 <= clip_x0) return;
    for (const char *p = text; *p; p++, x += 6) {
        if (x >= clip_x1) break;
        if (x + 5 > clip_x0) draw_char5x7_clipped(x, y, *p, c, clip_x0, clip_x1);
    }
}

static int song_scroll_offset(int text_width, int viewport_width, uint64_t started_ms) {
    int travel = text_width - viewport_width;
    if (travel <= 0) return 0;
    uint64_t travel_ms = ((uint64_t)travel * 1000u + SONG_SCROLL_SPEED_PX_PER_SEC - 1u) /
                         SONG_SCROLL_SPEED_PX_PER_SEC;
    uint64_t cycle = SONG_SCROLL_PAUSE_MS + travel_ms + SONG_SCROLL_PAUSE_MS + travel_ms;
    if (cycle == 0) return 0;
    uint64_t now_ms = monotonic_millis();
    uint64_t elapsed = now_ms >= started_ms ? now_ms - started_ms : 0;
    uint64_t phase = elapsed % cycle;
    if (phase < SONG_SCROLL_PAUSE_MS) return 0;
    phase -= SONG_SCROLL_PAUSE_MS;
    if (phase < travel_ms) return (int)((phase * (uint64_t)travel) / travel_ms);
    phase -= travel_ms;
    if (phase < SONG_SCROLL_PAUSE_MS) return travel;
    phase -= SONG_SCROLL_PAUSE_MS;
    return travel - (int)((phase * (uint64_t)travel) / travel_ms);
}

static void draw_song_metadata_line(const char *text, uint64_t started_ms) {
    if (!text || !*text) return;
    int width = text5x7_width(text, 1);
    int x = SONG_METADATA_X;
    if (width <= SONG_METADATA_W) x += (SONG_METADATA_W - width) / 2;
    else x -= song_scroll_offset(width, SONG_METADATA_W, started_ms);
    draw_text5x7_clipped(x, OLED_H - 8, text, 11,
                         SONG_METADATA_X, SONG_METADATA_X + SONG_METADATA_W);
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

static int wifi_connected_cached(void) {
    static pthread_mutex_t wifi_lock = PTHREAD_MUTEX_INITIALIZER;
    static int cached_connected = -1;
    static time_t last_read = 0;
    time_t now = time(NULL);

    pthread_mutex_lock(&wifi_lock);
    if (cached_connected < 0 || now - last_read >= 60) {
        cached_connected = wifi_connected_kernel();
        last_read = now;
    }
    int connected = cached_connected;
    pthread_mutex_unlock(&wifi_lock);
    return connected;
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
    int connected = wifi_connected_cached();

    const int pill_h = CLOCK_STATUS_PILL_H;
    const int pad_x = 5;
    const int icon_w = 9;
    const int icon_pill_w = icon_w + pad_x * 2;
    const int pill_gap = 3;
    const int bottom_y = OLED_H - pill_h;

    /* Bottom-left status group. Wi-Fi first, alarm/music immediately to its right.
       Keeping all status pills left leaves the clock/date zone clean. */
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
    if (g_font.face) {
        FT_Done_Face(g_font.face);
        g_font.face = NULL;
    }
    if (g_font.library) {
        FT_Done_FreeType(g_font.library);
        g_font.library = NULL;
    }
    g_font.loaded_file[0] = '\0';
    g_font.loaded_size = 0;
}

static void font_cache_reset(void) {
    pthread_mutex_lock(&g_font.lock);
    if (g_font.face) {
        FT_Done_Face(g_font.face);
        g_font.face = NULL;
    }
    g_font.loaded_file[0] = '\0';
    g_font.loaded_size = 0;
    pthread_mutex_unlock(&g_font.lock);
}

static int font_cache_ensure_locked(const char *font_file, const char *font_path, int px_size) {
    if (!font_file || !*font_file || !font_path || !*font_path) return -1;
    if (px_size < 8) px_size = 8;
    if (px_size > 72) px_size = 72;

    if (g_font.face &&
        strcmp(g_font.loaded_file, font_file) == 0 &&
        g_font.loaded_size == px_size) {
        return 0;
    }

    if (g_font.face) {
        FT_Done_Face(g_font.face);
        g_font.face = NULL;
    }

    if (!g_font.library) {
        if (FT_Init_FreeType(&g_font.library) != 0) {
            g_font.library = NULL;
            return -1;
        }
    }

    if (FT_New_Face(g_font.library, font_path, 0, &g_font.face) != 0) {
        g_font.face = NULL;
        g_font.loaded_file[0] = '\0';
        g_font.loaded_size = 0;
        return -1;
    }

    if (FT_Set_Pixel_Sizes(g_font.face, 0, (FT_UInt)px_size) != 0) {
        FT_Done_Face(g_font.face);
        g_font.face = NULL;
        g_font.loaded_file[0] = '\0';
        g_font.loaded_size = 0;
        return -1;
    }

    safe_str(g_font.loaded_file, sizeof(g_font.loaded_file), font_file);
    g_font.loaded_size = px_size;
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

    pthread_mutex_lock(&g_font.lock);
    if (font_cache_ensure_locked(font_file, font_path, px_size) != 0 || !g_font.face) {
        pthread_mutex_unlock(&g_font.lock);
        return -1;
    }

    FT_Face face = g_font.face;

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
        pthread_mutex_unlock(&g_font.lock);
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

    pthread_mutex_unlock(&g_font.lock);
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

static void draw_clock_footer_contents(const struct tm *tmv, int center_x,
                                       int alarm_on, int alarm_active, int alarm_volume_percent,
                                       int audio_playing, int show_song_metadata,
                                       const char *audio_display, uint64_t scroll_started_ms) {
    if (audio_playing && show_song_metadata && audio_display && audio_display[0])
        draw_song_metadata_line(audio_display, scroll_started_ms);
    else
        draw_long_date_centered_at(tmv, NULL, center_x);
    draw_status_pills(alarm_on, alarm_active, alarm_volume_percent);
}

static int song_metadata_scroll_active(void) {
    int active = 0;
    pthread_mutex_lock(&g_state.lock);
    if (g_state.audio_playing && g_state.show_song_metadata && g_state.audio_display[0])
        active = text5x7_width(g_state.audio_display, 1) > SONG_METADATA_W;
    pthread_mutex_unlock(&g_state.lock);
    return active;
}

static void refresh_clock_footer(void) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    int alarm_on = 0;
    int alarm_active;
    int alarm_volume_percent;
    int audio_playing;
    int show_song_metadata;
    uint64_t scroll_started_ms;
    char audio_display[SONG_METADATA_TEXT_MAX];

    pthread_mutex_lock(&g_state.lock);
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (g_state.alarms[i].enabled) {
            alarm_on = 1;
            break;
        }
    }
    alarm_active = g_state.alarm_active;
    alarm_volume_percent = g_state.alarm_volume_percent;
    audio_playing = g_state.audio_playing;
    show_song_metadata = g_state.show_song_metadata;
    scroll_started_ms = g_state.audio_scroll_started_ms;
    safe_str(audio_display, sizeof(audio_display), g_state.audio_display);
    pthread_mutex_unlock(&g_state.lock);

    oled_fill_rect(0, OLED_H - CLOCK_STATUS_PILL_H, OLED_W, CLOCK_STATUS_PILL_H, 0);
    int clock_center_x = (CLOCK_SIDE_WIDGET_X + CLOCK_SIDE_WIDGET_SIZE + OLED_W) / 2;
    draw_clock_footer_contents(&tmv, clock_center_x, alarm_on, alarm_active,
                               alarm_volume_percent, audio_playing, show_song_metadata,
                               audio_display, scroll_started_ms);
    (void)oled_flush_region_bytes(0, OLED_ROW_BYTES - 1,
                                  OLED_H - CLOCK_STATUS_PILL_H, OLED_H - 1);
}

static int face_raw_pixel(const uint8_t *raw, int x, int y) {
    if (!raw || x < 0 || y < 0 || x >= MP_FACE_WIDTH || y >= MP_FACE_HEIGHT) return 0;
    uint8_t b = raw[(y * MP_FACE_WIDTH + x) / 2];
    return (x & 1) ? (b & 0x0F) : ((b >> 4) & 0x0F);
}

static int load_face_raw_uncached_by_file(const char *file, int bedtime, uint8_t *raw, size_t raw_len) {
    if (!raw || raw_len < MP_FACE_RAW_BYTES) return -1;
    char path[512];
    int ok = bedtime
        ? make_bedtime_face_path_by_file(file, path, sizeof(path))
        : make_face_path_by_file(file, path, sizeof(path));
    if (ok != 0) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(raw, 1, MP_FACE_RAW_BYTES, f);
    fclose(f);
    return n == MP_FACE_RAW_BYTES ? 0 : -1;
}

static int load_face_raw_cached_by_file(const char *file, int bedtime, uint8_t *raw, size_t raw_len) {
    if (!raw || raw_len < MP_FACE_RAW_BYTES || !safe_face_filename(file)) return -1;

    pthread_mutex_lock(&g_face_raw_cache_lock);
    unsigned long generation = g_face_raw_cache.generation;
    if (g_face_raw_cache.valid && g_face_raw_cache.bedtime == bedtime && strcmp(g_face_raw_cache.file, file) == 0) {
        memcpy(raw, g_face_raw_cache.raw, MP_FACE_RAW_BYTES);
        pthread_mutex_unlock(&g_face_raw_cache_lock);
        return 0;
    }
    pthread_mutex_unlock(&g_face_raw_cache_lock);

    uint8_t tmp[MP_FACE_RAW_BYTES];
    if (load_face_raw_uncached_by_file(file, bedtime, tmp, sizeof(tmp)) != 0) return -1;

    pthread_mutex_lock(&g_face_raw_cache_lock);
    if (g_face_raw_cache.generation == generation) {
        safe_str(g_face_raw_cache.file, sizeof(g_face_raw_cache.file), file);
        g_face_raw_cache.bedtime = bedtime ? 1 : 0;
        memcpy(g_face_raw_cache.raw, tmp, MP_FACE_RAW_BYTES);
        g_face_raw_cache.valid = 1;
    }
    pthread_mutex_unlock(&g_face_raw_cache_lock);

    memcpy(raw, tmp, MP_FACE_RAW_BYTES);
    return 0;
}

static int load_face_raw_by_file(const char *file, uint8_t *raw, size_t raw_len) {
    return load_face_raw_cached_by_file(file, 0, raw, raw_len);
}

static int load_bedtime_face_raw_by_file(const char *file, uint8_t *raw, size_t raw_len) {
    return load_face_raw_cached_by_file(file, 1, raw, raw_len);
}

static int collect_uploaded_face_files(char files[][ASSET_LIST_NAME_MAX], int max_files) {
    return scan_asset_files(FACE_DIR, ASSET_LIST_FACE_RAW, files, max_files);
}

static int collect_bedtime_face_files(char files[][ASSET_LIST_NAME_MAX], int max_files) {
    return scan_asset_files(BEDTIME_FACE_DIR, ASSET_LIST_FACE_RAW, files, max_files);
}

static int collect_music_files(char files[][ASSET_LIST_NAME_MAX], int max_files) {
    return scan_asset_files(MUSIC_DIR, ASSET_LIST_MUSIC_MP3, files, max_files);
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
        uint8_t test[MP_FACE_RAW_BYTES];
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
        uint8_t test[MP_FACE_RAW_BYTES];
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
        int sy = (y * MP_FACE_HEIGHT) / size;
        for (int x = 0; x < size; x++) {
            int sx = (x * MP_FACE_WIDTH) / size;
            oled_set_px(ox + x, oy + y, (uint8_t)face_raw_pixel(raw, sx, sy));
        }
    }

    return 0;
}

static int draw_face_thumb_by_file(const char *file, int ox, int oy, int size) {
    uint8_t raw[MP_FACE_RAW_BYTES];
    if (load_face_raw_by_file(file, raw, sizeof(raw)) != 0) return -1;
    return draw_face_thumb_raw(raw, ox, oy, size);
}

static int draw_bedtime_face_thumb_by_file(const char *file, int ox, int oy, int size) {
    uint8_t raw[MP_FACE_RAW_BYTES];
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
    int audio_playing = g_state.audio_playing;
    int show_song_metadata = g_state.show_song_metadata;
    uint64_t audio_scroll_started_ms = g_state.audio_scroll_started_ms;
    char audio_display[SONG_METADATA_TEXT_MAX];
    safe_str(audio_display, sizeof(audio_display), g_state.audio_display);
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

    draw_clock_footer_contents(&tmv, clock_center_x, alarm_on, alarm_active,
                               alarm_volume_percent, audio_playing, show_song_metadata,
                               audio_display, audio_scroll_started_ms);

    /*
       The clock screen contains a 54x54 face beside the time, a blinking colon, bottom-left Wi-Fi/alarm status group. On SSD1322 modules,
       small partial updates around packed 4-bit graphics can occasionally leave
       edge noise, so a full flush is the cleaner and safer choice.
    */
oled_flush_full();
}

static int draw_face_screen_file(const char *file) {
    uint8_t raw[MP_FACE_RAW_BYTES];
    if (load_face_raw_by_file(file, raw, sizeof(raw)) != 0) return -1;

    oled_clear_fb(0);
    int ox = 96;
    int oy = 0;
    for (int y = 0; y < MP_FACE_HEIGHT; y++) {
        for (int x = 0; x < MP_FACE_WIDTH; x += 2) {
            uint8_t b = raw[(y * MP_FACE_WIDTH + x) / 2];
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

    pthread_mutex_lock(&g_font.lock);
    if (font_cache_ensure_locked(font_file, font_path, px_size) != 0 || !g_font.face) {
        pthread_mutex_unlock(&g_font.lock);
        return -1;
    }

    FT_Face face = g_font.face;
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
    pthread_mutex_unlock(&g_font.lock);

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

    pthread_mutex_lock(&g_font.lock);
    if (font_cache_ensure_locked(font_file, font_path, px_size) != 0 || !g_font.face) {
        pthread_mutex_unlock(&g_font.lock);
        return -1;
    }

    FT_Face face = g_font.face;
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
        pthread_mutex_unlock(&g_font.lock);
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

    pthread_mutex_unlock(&g_font.lock);
    return 0;
}

static int ttf_line_width_cached(const char *font_file, int px_size, const char *text) {
    if (!font_file || !*font_file || !text || !*text) return 0;

    char font_path[512];
    make_font_path(font_file, font_path, sizeof(font_path));

    pthread_mutex_lock(&g_font.lock);
    if (font_cache_ensure_locked(font_file, font_path, px_size) != 0 || !g_font.face) {
        pthread_mutex_unlock(&g_font.lock);
        return 0;
    }
    int w = measure_ttf_line_locked(g_font.face, text);
    pthread_mutex_unlock(&g_font.lock);
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

static void clear_audio_metadata_locked(void);

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

    if (req->use_ramp) alarm_volume_state_set(0, 0);

    pthread_mutex_lock(&g_state.lock);
    g_state.audio_playing = 0;
    g_state.audio_file[0] = '\0';
    clear_audio_metadata_locked();
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);

    /* Publish completion only after all old playback state is cleared.
       A waiting replacement track can then start without the exiting thread
       erasing the new track's metadata. */
    pthread_mutex_lock(&g_audio.lock);
    g_audio.running = 0;
    g_audio.stop_requested = 0;
    g_audio.file[0] = '\0';
    pthread_cond_broadcast(&g_audio.stopped);
    pthread_mutex_unlock(&g_audio.lock);

    free(req);
    return NULL;
}

static void song_metadata_for_file(const char *path, const char *file,
                                   char *title, size_t title_len,
                                   char *artist, size_t artist_len,
                                   char *display, size_t display_len) {
    struct mp_id3_metadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    (void)mp_read_id3_metadata(path, &metadata);
    if (!metadata.title[0]) mp_title_from_filename(file, metadata.title, sizeof(metadata.title));
    safe_str(title, title_len, metadata.title);
    safe_str(artist, artist_len, metadata.artist);

    char combined[MP_ID3_TEXT_MAX * 2 + 4];
    if (metadata.title[0] && metadata.artist[0])
        snprintf(combined, sizeof(combined), "%s - %s", metadata.title, metadata.artist);
    else if (metadata.title[0])
        safe_str(combined, sizeof(combined), metadata.title);
    else if (metadata.artist[0])
        safe_str(combined, sizeof(combined), metadata.artist);
    else
        mp_title_from_filename(file, combined, sizeof(combined));
    oled_ascii_text(combined, display, display_len);
}

static void clear_audio_metadata_locked(void) {
    g_state.audio_title[0] = '\0';
    g_state.audio_artist[0] = '\0';
    g_state.audio_display[0] = '\0';
    g_state.audio_scroll_started_ms = 0;
}

static void audio_clear_visible_state(void) {
    alarm_volume_state_set(0, 0);

    pthread_mutex_lock(&g_state.lock);
    g_state.audio_playing = 0;
    g_state.audio_file[0] = '\0';
    clear_audio_metadata_locked();
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);
}

static void audio_request_stop(void) {
    pthread_mutex_lock(&g_audio.lock);
    if (g_audio.running) g_audio.stop_requested = 1;
    pthread_mutex_unlock(&g_audio.lock);
    audio_clear_visible_state();
}

static int audio_wait_stopped(unsigned int timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000u);
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&g_audio.lock);
    int wait_rc = 0;
    while (g_audio.running) {
        wait_rc = pthread_cond_timedwait(&g_audio.stopped, &g_audio.lock, &deadline);
        if (wait_rc == ETIMEDOUT || wait_rc != 0) break;
    }
    int stopped = !g_audio.running;
    pthread_mutex_unlock(&g_audio.lock);
    return stopped ? 0 : -1;
}

/*
 * Request-only stop for latency-sensitive callers such as IPC and touch.
 * The decoder thread publishes completion through g_audio.stopped.
 */
static void audio_stop(void) {
    audio_request_stop();
}

/*
 * Use only where the caller must know the old decoder has exited before it
 * can safely continue, such as track replacement and daemon shutdown.
 */
static int audio_stop_and_wait(unsigned int timeout_ms) {
    audio_request_stop();
    return audio_wait_stopped(timeout_ms);
}

static void audio_play_music_file(const char *music_file, int start_volume, int end_volume, int use_ramp) {
    char safe_file[MUSIC_FILE_MAX];
    char path[512];
    char song_title[MP_ID3_TEXT_MAX];
    char song_artist[MP_ID3_TEXT_MAX];
    char song_display[SONG_METADATA_TEXT_MAX];
    int global_volume;

    safe_file[0] = '\0';
    if (music_file && *music_file && safe_asset_filename(music_file) && has_mp3_ext(music_file)) {
        safe_str(safe_file, sizeof(safe_file), music_file);
    } else if (choose_random_music_file(safe_file, sizeof(safe_file)) != 0) {
        safe_str(safe_file, sizeof(safe_file), "alarm.mp3");
    }

    make_music_path(safe_file, path, sizeof(path));
    if (access(path, R_OK) != 0) return;
    song_metadata_for_file(path, safe_file,
                           song_title, sizeof(song_title),
                           song_artist, sizeof(song_artist),
                           song_display, sizeof(song_display));

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

    if (audio_stop_and_wait(3000u) != 0) return;

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

    pthread_mutex_lock(&g_state.lock);
    g_state.audio_playing = 1;
    safe_str(g_state.audio_file, sizeof(g_state.audio_file), safe_file);
    safe_str(g_state.audio_title, sizeof(g_state.audio_title), song_title);
    safe_str(g_state.audio_artist, sizeof(g_state.audio_artist), song_artist);
    safe_str(g_state.audio_display, sizeof(g_state.audio_display), song_display);
    g_state.audio_scroll_started_ms = monotonic_millis();
    /* Every playback path uses this function. Return to the clock so enabled
       Title - Artist metadata is visible for alarms, GUI/API play, and touch. */
    if (g_state.show_song_metadata) g_state.display_mode = 0;
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);

    pthread_t tid;
    if (pthread_create(&tid, NULL, audio_thread_main, req) != 0) {
        pthread_mutex_lock(&g_audio.lock);
        g_audio.running = 0;
        g_audio.stop_requested = 0;
        g_audio.file[0] = '\0';
        pthread_cond_broadcast(&g_audio.stopped);
        pthread_mutex_unlock(&g_audio.lock);
        pthread_mutex_lock(&g_state.lock);
        g_state.audio_playing = 0;
        g_state.audio_file[0] = '\0';
        clear_audio_metadata_locked();
        g_state.display_dirty = 1;
        pthread_mutex_unlock(&g_state.lock);
        free(req);
        return;
    }
    pthread_detach(tid);

}

/* ---------------- TTP223B touch input ---------------- */

static void touch_set_state(int ready, int pressed) {
    pthread_mutex_lock(&g_touch.lock);
    g_touch.ready = ready ? 1 : 0;
    g_touch.pressed = pressed ? 1 : 0;
    pthread_mutex_unlock(&g_touch.lock);
}

static void touch_get_state(int *ready, int *pressed) {
    pthread_mutex_lock(&g_touch.lock);
    if (ready) *ready = g_touch.ready;
    if (pressed) *pressed = g_touch.pressed;
    pthread_mutex_unlock(&g_touch.lock);
}

static int touch_read_active(void) {
    if (!g_touch.request) return -1;
    enum gpiod_line_value value =
        gpiod_line_request_get_value(g_touch.request, GPIO_TOUCH);
    if (value == GPIOD_LINE_VALUE_ACTIVE) return 1;
    if (value == GPIOD_LINE_VALUE_INACTIVE) return 0;
    return -1;
}

static int touch_init(void) {
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_cfg = NULL;
    struct gpiod_request_config *req_cfg = NULL;
    unsigned int offset = GPIO_TOUCH;
    int rc = -1;

    g_touch.chip = gpiod_chip_open(GPIO_CHIP);
    if (!g_touch.chip) {
        perror("touch gpiod_chip_open");
        return -1;
    }

    settings = gpiod_line_settings_new();
    line_cfg = gpiod_line_config_new();
    req_cfg = gpiod_request_config_new();
    if (!settings || !line_cfg || !req_cfg) {
        fprintf(stderr, "failed to allocate touch GPIO request config\n");
        goto done;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) != 0) {
        perror("touch gpiod_line_config_add_line_settings");
        goto done;
    }

    gpiod_request_config_set_consumer(req_cfg, APP_NAME "-touch");
    g_touch.request = gpiod_chip_request_lines(g_touch.chip, req_cfg, line_cfg);
    if (!g_touch.request) {
        perror("touch gpiod_chip_request_lines");
        goto done;
    }

    int initial = touch_read_active();
    if (initial < 0) {
        perror("touch gpiod_line_request_get_value");
        goto done;
    }

    touch_set_state(1, initial);
    rc = 0;

done:
    if (settings) gpiod_line_settings_free(settings);
    if (line_cfg) gpiod_line_config_free(line_cfg);
    if (req_cfg) gpiod_request_config_free(req_cfg);
    if (rc != 0) {
        if (g_touch.request) {
            gpiod_line_request_release(g_touch.request);
            g_touch.request = NULL;
        }
        if (g_touch.chip) {
            gpiod_chip_close(g_touch.chip);
            g_touch.chip = NULL;
        }
        touch_set_state(0, 0);
    }
    return rc;
}

static void touch_close(void) {
    touch_set_state(0, 0);
    if (g_touch.request) {
        gpiod_line_request_release(g_touch.request);
        g_touch.request = NULL;
    }
    if (g_touch.chip) {
        gpiod_chip_close(g_touch.chip);
        g_touch.chip = NULL;
    }
}

static int audio_is_playing(void) {
    int playing;
    pthread_mutex_lock(&g_state.lock);
    playing = g_state.audio_playing;
    pthread_mutex_unlock(&g_state.lock);
    return playing;
}

static void *touch_thread_main(void *arg) {
    (void)arg;
    int raw = touch_read_active();
    if (raw < 0) raw = 0;
    int last_raw = raw;
    int stable = raw;
    int long_press_fired = 0;
    uint64_t now_ms = monotonic_millis();
    uint64_t raw_changed_ms = now_ms;
    uint64_t pressed_ms = stable ? now_ms : 0;
    touch_set_state(1, stable);

    while (g_running) {
        usleep(TOUCH_POLL_MS * 1000u);
        int current = touch_read_active();
        if (current < 0) continue;
        now_ms = monotonic_millis();

        if (current != last_raw) {
            last_raw = current;
            raw_changed_ms = now_ms;
        }

        if (current != stable && now_ms - raw_changed_ms >= TOUCH_DEBOUNCE_MS) {
            stable = current;
            touch_set_state(1, stable);
            if (stable) {
                pressed_ms = now_ms;
                long_press_fired = 0;
            } else {
                if (!long_press_fired && audio_is_playing()) {
                    app_log("touch", "Short press requested audio stop");
                    audio_request_stop();
                }
                pressed_ms = 0;
            }
        }

        if (stable && !long_press_fired &&
            now_ms - pressed_ms >= TOUCH_LONG_PRESS_MS) {
            long_press_fired = 1;
            app_log("touch", "Long press requested random music");
            audio_play_music_file("", 0, 0, 0);
        }
    }

    touch_set_state(0, 0);
    return NULL;
}

/* ---------------- Private binary IPC ---------------- */

static const char *oled_font_name_for_id(int id) {
    switch (id) {
        case 1: return "Seven Thin";
        case 2: return "Pixel";
        case 3: return "Pixel Bold";
        default: return "Seven Segment";
    }
}

static const char *display_mode_name(int mode) {
    switch (mode) {
        case 1: return "clear";
        case 2: return "face";
        case 3: return "message";
        default: return "clock";
    }
}

static int ipc_send_response(int client, unsigned int status, unsigned int content_type,
                             const void *body, size_t body_len) {
    if (body_len > MP_IPC_MAX_PAYLOAD) return -1;
    struct mp_ipc_response_header header = {
        .magic = MP_IPC_MAGIC,
        .version = MP_IPC_VERSION,
        .status = (uint16_t)status,
        .body_len = (uint32_t)body_len,
        .content_type = (uint16_t)content_type,
        .reserved = 0
    };
    return mp_send_packet(client, &header, sizeof(header), body, body_len);
}

static int ipc_send_json(int client, unsigned int status, const char *json) {
    const char *body = json ? json : "{}";
    return ipc_send_response(client, status, MP_IPC_CONTENT_JSON, body, strlen(body));
}

static int ipc_send_builder(int client, unsigned int status, struct mp_buffer *buffer) {
    size_t length = 0;
    char *body = mp_buffer_steal(buffer, &length);
    mp_buffer_free(buffer);
    if (!body) return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"JSON response exceeded its limit\"}");
    int rc = ipc_send_response(client, status, MP_IPC_CONTENT_JSON, body, length);
    free(body);
    return rc;
}

static int ipc_bad_payload(int client) {
    return ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"invalid IPC payload\"}");
}

static int ipc_status(int client) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    char timestr[64];
    char datestr[96];
    strftime(timestr, sizeof(timestr), "%I:%M %p", &tmv);
    if (timestr[0] == '0') memmove(timestr, timestr + 1, strlen(timestr));
    strftime(datestr, sizeof(datestr), "%A %B %e, %Y", &tmv);
    long uptime_seconds = (g_start_time > 0 && now >= g_start_time) ? (long)(now - g_start_time) : 0;

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
    int show_song_metadata = g_state.show_song_metadata;
    time_t pending_message_at = g_state.pending_message_at;
    int bedtime_enabled = g_state.bedtime_enabled;
    int bsh = g_state.bedtime_start_hour;
    int bsm = g_state.bedtime_start_min;
    int beh = g_state.bedtime_end_hour;
    int bem = g_state.bedtime_end_min;
    int bedtime_dim = g_state.bedtime_dim_percent;
    int clock_24h_mode = g_state.clock_24h_mode;
    int oled_brightness = g_state.oled_brightness_current;
    int touch_ok = 0;
    int touch_pressed = 0;
    char font_file[128];
    char clock_name[64];
    char audio_file[MUSIC_FILE_MAX];
    char audio_title[MP_ID3_TEXT_MAX];
    char audio_artist[MP_ID3_TEXT_MAX];
    char audio_display[SONG_METADATA_TEXT_MAX];
    struct alarm_slot alarms[MAX_ALARMS];
    mp_safe_str(font_file, sizeof(font_file), g_state.oled_font_file);
    mp_safe_str(clock_name, sizeof(clock_name), g_state.clock_name);
    mp_safe_str(audio_file, sizeof(audio_file), g_state.audio_file);
    mp_safe_str(audio_title, sizeof(audio_title), g_state.audio_title);
    mp_safe_str(audio_artist, sizeof(audio_artist), g_state.audio_artist);
    mp_safe_str(audio_display, sizeof(audio_display), g_state.audio_display);
    memcpy(alarms, g_state.alarms, sizeof(alarms));
    pthread_mutex_unlock(&g_state.lock);
    touch_get_state(&touch_ok, &touch_pressed);

    char e_time[128], e_date[192], e_clock_name[160], e_audio_file[512];
    char e_audio_title[384], e_audio_artist[384], e_audio_display[768];
    char e_font_file[256], e_font_name[256];
    mp_json_escape(e_time, sizeof(e_time), timestr);
    mp_json_escape(e_date, sizeof(e_date), datestr);
    mp_json_escape(e_clock_name, sizeof(e_clock_name), clock_name);
    mp_json_escape(e_audio_file, sizeof(e_audio_file), audio_file);
    mp_json_escape(e_audio_title, sizeof(e_audio_title), audio_title);
    mp_json_escape(e_audio_artist, sizeof(e_audio_artist), audio_artist);
    mp_json_escape(e_audio_display, sizeof(e_audio_display), audio_display);
    mp_json_escape(e_font_file, sizeof(e_font_file), font_file);
    mp_json_escape(e_font_name, sizeof(e_font_name), font_file[0] ? font_file : oled_font_name_for_id(font));

    long long message_send_in = pending_message_at > now ? (long long)(pending_message_at - now) : 0LL;
    struct mp_buffer body;
    if (mp_buffer_init(&body, 4096, MP_IPC_MAX_PAYLOAD) != 0)
        return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    mp_buffer_appendf(&body,
        "{\"time\":\"%s\",\"date\":\"%s\",\"clock_name\":\"%s\",\"app_version\":\"%s\","
        "\"uptime_seconds\":%ld,\"audio_file\":\"%s\",\"audio_title\":\"%s\",\"audio_artist\":\"%s\","
        "\"audio_display\":\"%s\",\"global_volume\":%d,\"show_song_metadata\":%d,"
        "\"message_pending\":%d,\"message_send_in_seconds\":%lld,\"bedtime_enabled\":%d,"
        "\"bedtime_start_hour\":%d,\"bedtime_start_min\":%d,\"bedtime_end_hour\":%d,\"bedtime_end_min\":%d,"
        "\"bedtime_dim_percent\":%d,\"clock_24h_mode\":%d,\"bedtime_active\":%d,\"oled_brightness_percent\":%d,"
        "\"audio_playing\":%d,\"alarm_active\":%d,\"alarm_volume_percent\":%d,\"display_mode\":\"%s\",\"oled_ok\":%d,"
        "\"touch_ok\":%d,\"touch_pressed\":%d,\"touch_gpio\":%d,"
        "\"current_face\":%d,\"oled_font\":%d,\"oled_font_size\":%d,\"oled_font_file\":\"%s\",\"oled_font_name\":\"%s\",\"alarms\":[",
        e_time, e_date, e_clock_name, APP_VERSION, uptime_seconds, e_audio_file,
        e_audio_title, e_audio_artist, e_audio_display, global_volume, show_song_metadata,
        message_send_in > 0 ? 1 : 0, message_send_in, bedtime_enabled,
        bsh, bsm, beh, bem, bedtime_dim, clock_24h_mode,
        is_bedtime_now(), oled_brightness, audio, alarm_active, alarm_volume_percent,
        display_mode_name(mode), oled_ok, touch_ok, touch_pressed, GPIO_TOUCH,
        current_face, font, font_size, e_font_file, e_font_name);

    for (int i = 0; i < MAX_ALARMS && !body.failed; i++) {
        mp_buffer_appendf(&body,
            "%s{\"id\":%d,\"enabled\":%d,\"hour\":%d,\"min\":%d,\"weekdays\":%d,\"start_volume\":%d,\"end_volume\":%d,\"music_file\":\"",
            i ? "," : "", i + 1, alarms[i].enabled, alarms[i].hour, alarms[i].min,
            alarms[i].weekdays, alarms[i].start_volume, alarms[i].end_volume);
        mp_buffer_append_json_string(&body, alarms[i].music_file);
        mp_buffer_append(&body, "\"}");
    }
    mp_buffer_append(&body, "]}");
    return ipc_send_builder(client, 200, &body);
}

static int ipc_logs_get(int client) {
    pthread_mutex_lock(&g_log_lock);
    FILE *f = fopen(LOG_FILE, "rb");
    if (!f) {
        pthread_mutex_unlock(&g_log_lock);
        return ipc_send_json(client, 200, "{\"ok\":true,\"log_file\":\"" LOG_FILE "\",\"entries\":[]}");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        pthread_mutex_unlock(&g_log_lock);
        return ipc_send_json(client, 500, "{\"ok\":false,\"entries\":[]}");
    }
    long size = ftell(f);
    if (size < 0) size = 0;
    long start = size > LOG_MAX_BYTES ? size - LOG_MAX_BYTES : 0;
    if (fseek(f, start, SEEK_SET) != 0) start = 0;
    size_t cap = (size_t)(size - start);
    char *buf = malloc(cap + 1);
    if (!buf) {
        fclose(f);
        pthread_mutex_unlock(&g_log_lock);
        return ipc_send_json(client, 500, "{\"ok\":false,\"entries\":[]}");
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
        if (*line) lines[count++ % LOG_VIEW_LINES] = line;
    }
    struct mp_buffer body;
    if (mp_buffer_init(&body, 4096, MP_IPC_MAX_PAYLOAD) != 0) {
        free(buf);
        return ipc_send_json(client, 500, "{\"ok\":false,\"entries\":[]}");
    }
    mp_buffer_append(&body, "{\"ok\":true,\"log_file\":\"");
    mp_buffer_append_json_string(&body, LOG_FILE);
    mp_buffer_append(&body, "\",\"entries\":[");
    int n = count < LOG_VIEW_LINES ? count : LOG_VIEW_LINES;
    int start_idx = count > LOG_VIEW_LINES ? count % LOG_VIEW_LINES : 0;
    for (int i = 0; i < n && !body.failed; i++) {
        int idx = (start_idx + i) % LOG_VIEW_LINES;
        mp_buffer_appendf(&body, "%s\"", i ? "," : "");
        mp_buffer_append_json_string(&body, lines[idx]);
        mp_buffer_append(&body, "\"");
    }
    mp_buffer_append(&body, "]}");
    free(buf);
    return ipc_send_builder(client, 200, &body);
}

static int ipc_logs_clear(int client) {
    pthread_mutex_lock(&g_log_lock);
    ensure_dir(CONFIG_DIR);
    FILE *f = fopen(LOG_FILE, "w");
    if (f) fclose(f);
    pthread_mutex_unlock(&g_log_lock);
    app_log("log", "Log cleared through API");
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int ipc_message_limits(int client) {
    char body[512];
    snprintf(body, sizeof(body),
        "{\"max_chars\":%d,\"advisory_chars\":%d,\"max_lines\":%d,\"text_width\":%d,\"duration_seconds\":%d,\"wrap_check\":1}",
        MESSAGE_INPUT_MAX_CHARS, message_char_capacity_for_current_font(), MESSAGE_MAX_LINES,
        MESSAGE_TEXT_W, MESSAGE_DEFAULT_DURATION_SECONDS);
    return ipc_send_json(client, 200, body);
}

static int ipc_message_fit(int client, const struct mp_ipc_message_fit *request) {
    char text[192];
    mp_safe_str(text, sizeof(text), request->text);
    sanitize_message_text(text);
    char reason[160] = "";
    int ok = message_fits_display(text, reason, sizeof(reason));
    char font_file[128];
    int px_size = 18, scale = MESSAGE_FALLBACK_SCALE, use_ttf = 0;
    message_layout_params(font_file, sizeof(font_file), &px_size, &scale, &use_ttf);
    char lines[MESSAGE_MAX_LINES][MESSAGE_LINE_CHARS];
    memset(lines, 0, sizeof(lines));
    int line_count = wrap_message_lines(use_ttf ? font_file : "", px_size, text,
                                        MESSAGE_TEXT_W, lines, MESSAGE_MAX_LINES, scale);
    if (!text[0]) line_count = 0;
    struct mp_buffer body;
    if (mp_buffer_init(&body, 1024, 8192) != 0)
        return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    mp_buffer_appendf(&body,
        "{\"ok\":%d,\"chars\":%d,\"max_chars\":%d,\"advisory_chars\":%d,\"wrap_check\":1,\"max_lines\":%d,\"line_count\":%d,\"text_x\":%d,\"text_w\":%d,\"face_x\":0,\"face_y\":0,\"face_size\":64,\"reason\":\"",
        ok, (int)strlen(text), MESSAGE_INPUT_MAX_CHARS, message_char_capacity_for_current_font(),
        MESSAGE_MAX_LINES, line_count, MESSAGE_TEXT_X, MESSAGE_TEXT_W);
    mp_buffer_append_json_string(&body, reason);
    mp_buffer_append(&body, "\",\"lines\":[");
    for (int i = 0; i < line_count && i < MESSAGE_MAX_LINES && !body.failed; i++) {
        mp_buffer_appendf(&body, "%s\"", i ? "," : "");
        mp_buffer_append_json_string(&body, lines[i]);
        mp_buffer_append(&body, "\"");
    }
    mp_buffer_append(&body, "]}");
    return ipc_send_builder(client, 200, &body);
}

static int ipc_display_action(int client, const struct mp_ipc_display_action *request) {
    switch (request->action) {
        case MP_IPC_ACTION_CLOCK:
            pthread_mutex_lock(&g_state.lock);
            g_state.display_mode = 0;
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
            app_log("action", "Show clock requested");
            break;
        case MP_IPC_ACTION_CLEAR:
            pthread_mutex_lock(&g_state.lock);
            g_state.display_mode = 1;
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
            app_log("action", "Clear screen requested");
            break;
        case MP_IPC_ACTION_STOP_AUDIO:
            audio_request_stop();
            app_log("action", "Stop audio requested");
            break;
        case MP_IPC_ACTION_PLAY_MUSIC: {
            int volume;
            pthread_mutex_lock(&g_state.lock);
            volume = g_state.global_volume;
            pthread_mutex_unlock(&g_state.lock);
            audio_play_music_file(request->file, volume, volume, 0);
            app_log("action", "Play music requested: %s", request->file);
            break;
        }
        default:
            return ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"unknown display action\"}");
    }
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int ipc_display_face(int client, const struct mp_ipc_display_face *request) {
    int id = face_id_valid_int(request->id) ? request->id : 1;
    char file[FACE_FILE_MAX];
    if (safe_face_filename(request->file)) mp_safe_str(file, sizeof(file), request->file);
    else snprintf(file, sizeof(file), "face_%03d.raw", id);
    pthread_mutex_lock(&g_state.lock);
    g_state.display_mode = 0;
    g_state.current_face = id;
    mp_safe_str(g_state.current_face_file, sizeof(g_state.current_face_file), file);
    mp_safe_str(g_state.preview_face_file, sizeof(g_state.preview_face_file), file);
    g_state.preview_face_bedtime = 0;
    g_state.preview_face_until = time(NULL) + CLOCK_FACE_PREVIEW_SECONDS;
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);
    app_log("faces", "Show face requested: %s", file);
    return ipc_send_json(client, 200, "{\"ok\":true,\"mode\":\"face\"}");
}

static int valid_message_delay(unsigned int seconds) {
    return seconds == 0 || seconds == 10 || seconds == 30 || seconds == MESSAGE_DELAY_MAX_SECONDS;
}

static void activate_pending_message_if_due(void) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_state.lock);
    if (g_state.pending_message_at > 0 && now >= g_state.pending_message_at) {
        g_state.display_mode = 3;
        g_state.message_face = g_state.pending_message_face;
        safe_str(g_state.message_face_file, sizeof(g_state.message_face_file),
                 g_state.pending_message_face_file);
        safe_str(g_state.message_text, sizeof(g_state.message_text),
                 g_state.pending_message_text);
        g_state.message_until = now + MESSAGE_DEFAULT_DURATION_SECONDS;
        g_state.pending_message_at = 0;
        g_state.pending_message_face_file[0] = '\0';
        g_state.pending_message_text[0] = '\0';
        g_state.display_dirty = 1;
    }
    pthread_mutex_unlock(&g_state.lock);
}

static int ipc_display_message(int client, const struct mp_ipc_display_message *request) {
    char face_file[FACE_FILE_MAX] = "";
    char message[192];
    unsigned int delay_seconds = request->delay_seconds;
    if (!valid_message_delay(delay_seconds))
        return ipc_send_json(client, 400,
            "{\"ok\":false,\"error\":\"delay_seconds must be 0, 10, 30, or 60\"}");
    if (safe_face_filename(request->face_file)) mp_safe_str(face_file, sizeof(face_file), request->face_file);
    if (!face_file[0]) {
        if (face_id_valid_int(request->face_id)) snprintf(face_file, sizeof(face_file), "face_%03d.raw", request->face_id);
        else if (random_uploaded_face_file(face_file, sizeof(face_file)) != 0) face_file[0] = '\0';
    }
    mp_safe_str(message, sizeof(message), request->text);
    sanitize_message_text(message);
    if (!message[0]) mp_safe_str(message, sizeof(message), "Hello");
    char reason[160] = "";
    if (!message_fits_display(message, reason, sizeof(reason))) {
        struct mp_buffer body;
        if (mp_buffer_init(&body, 256, 1024) != 0)
            return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
        mp_buffer_append(&body, "{\"ok\":false,\"error\":\"");
        mp_buffer_append_json_string(&body, reason[0] ? reason : "Message is too long for the OLED");
        mp_buffer_append(&body, "\"}");
        app_log("message", "Rejected message: %s", reason);
        return ipc_send_builder(client, 400, &body);
    }

    time_t now = time(NULL);
    pthread_mutex_lock(&g_state.lock);
    if (delay_seconds == 0) {
        g_state.display_mode = 3;
        g_state.message_face = face_id_valid_int(request->face_id) ? request->face_id : 1;
        mp_safe_str(g_state.message_face_file, sizeof(g_state.message_face_file), face_file);
        g_state.message_until = now + MESSAGE_DEFAULT_DURATION_SECONDS;
        mp_safe_str(g_state.message_text, sizeof(g_state.message_text), message);
        g_state.pending_message_at = 0;
        g_state.pending_message_face_file[0] = '\0';
        g_state.pending_message_text[0] = '\0';
        g_state.display_dirty = 1;
    } else {
        g_state.pending_message_face = face_id_valid_int(request->face_id) ? request->face_id : 1;
        mp_safe_str(g_state.pending_message_face_file, sizeof(g_state.pending_message_face_file), face_file);
        mp_safe_str(g_state.pending_message_text, sizeof(g_state.pending_message_text), message);
        g_state.pending_message_at = now + (time_t)delay_seconds;
    }
    pthread_mutex_unlock(&g_state.lock);

    if (delay_seconds == 0) {
        app_log("message", "Sent message with face %s: %.120s", face_file[0] ? face_file : "default", message);
        return ipc_send_json(client, 200, "{\"ok\":true,\"mode\":\"message\",\"delay_seconds\":0}");
    }

    app_log("message", "Scheduled message in %u seconds with face %s: %.120s",
            delay_seconds, face_file[0] ? face_file : "default", message);
    char response[192];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"mode\":\"scheduled-message\",\"delay_seconds\":%u,\"scheduled_for\":%lld}",
             delay_seconds, (long long)(now + (time_t)delay_seconds));
    return ipc_send_json(client, 200, response);
}

static int ipc_config_alarm(int client, const struct mp_ipc_alarm_config *request) {
    int id = clamp_int(request->id, 1, MAX_ALARMS);
    struct alarm_slot alarm = {
        .enabled = request->enabled ? 1 : 0,
        .hour = clamp_int(request->hour, 0, 23),
        .min = clamp_int(request->minute, 0, 59),
        .weekdays = request->weekdays ? request->weekdays : 0x7f,
        .start_volume = clamp_int(request->start_volume, 0, 100),
        .end_volume = clamp_int(request->end_volume, 0, 100),
        .fired_yday = -1,
        .music_file = ""
    };
    if (safe_asset_filename(request->music_file) && has_mp3_ext(request->music_file)) {
        char path[512];
        make_music_path(request->music_file, path, sizeof(path));
        if (access(path, R_OK) == 0) mp_safe_str(alarm.music_file, sizeof(alarm.music_file), request->music_file);
    }
    pthread_mutex_lock(&g_state.lock);
    g_state.alarms[id - 1] = alarm;
    sync_legacy_alarm_fields_locked();
    pthread_mutex_unlock(&g_state.lock);
    save_config();
    app_log("alarm", "Saved alarm %d", id);
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int ipc_config_audio(int client, const struct mp_ipc_audio_config *request) {
    int changed = 0;
    int volume = -1;
    int show_metadata = -1;
    pthread_mutex_lock(&g_state.lock);
    if (request->present_mask & MP_IPC_AUDIO_GLOBAL_VOLUME) {
        volume = clamp_int(request->global_volume, 0, 100);
        g_state.global_volume = volume;
        changed = 1;
    }
    if (request->present_mask & MP_IPC_AUDIO_SHOW_METADATA) {
        show_metadata = request->show_song_metadata ? 1 : 0;
        g_state.show_song_metadata = show_metadata;
        g_state.display_dirty = 1;
        changed = 1;
    }
    pthread_mutex_unlock(&g_state.lock);
    if (!changed)
        return ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"no audio settings supplied\"}");
    save_config();
    if (volume >= 0) app_log("music", "Saved global volume %d%%", volume);
    if (show_metadata >= 0) app_log("music", "Song metadata display %s", show_metadata ? "enabled" : "disabled");
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int ipc_config_personalization(int client, const struct mp_ipc_personalization_config *request) {
    char name[64];
    mp_safe_str(name, sizeof(name), request->clock_name);
    sanitize_clock_name(name);
    pthread_mutex_lock(&g_state.lock);
    mp_safe_str(g_state.clock_name, sizeof(g_state.clock_name), name);
    pthread_mutex_unlock(&g_state.lock);
    save_config();
    app_log("settings", "Saved clock name %s", name);
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int ipc_config_display(int client, const struct mp_ipc_display_config *request) {
    pthread_mutex_lock(&g_state.lock);
    int font = g_state.oled_font;
    int font_size = g_state.oled_font_size;
    int bedtime_enabled = g_state.bedtime_enabled;
    int bedtime_dim = g_state.bedtime_dim_percent;
    int clock_mode = g_state.clock_24h_mode;
    int bsh = g_state.bedtime_start_hour;
    int bsm = g_state.bedtime_start_min;
    int beh = g_state.bedtime_end_hour;
    int bem = g_state.bedtime_end_min;
    char font_file[128];
    mp_safe_str(font_file, sizeof(font_file), g_state.oled_font_file);
    pthread_mutex_unlock(&g_state.lock);

    if (request->present_mask & MP_IPC_DISPLAY_FONT) font = clamp_int(request->oled_font, 0, 3);
    if (request->present_mask & MP_IPC_DISPLAY_FONT_SIZE) font_size = clamp_int(request->oled_font_size, 18, 54);
    if (request->present_mask & MP_IPC_DISPLAY_BEDTIME_ENABLED) bedtime_enabled = request->bedtime_enabled ? 1 : 0;
    if (request->present_mask & MP_IPC_DISPLAY_BEDTIME_DIM) bedtime_dim = clamp_int(request->bedtime_dim_percent, 0, 100);
    if (request->present_mask & MP_IPC_DISPLAY_CLOCK_MODE) clock_mode = request->clock_24h_mode ? 1 : 0;
    if (request->present_mask & MP_IPC_DISPLAY_BEDTIME_START) {
        bsh = clamp_int(request->bedtime_start_hour, 0, 23);
        bsm = clamp_int(request->bedtime_start_minute, 0, 59);
    }
    if (request->present_mask & MP_IPC_DISPLAY_BEDTIME_END) {
        beh = clamp_int(request->bedtime_end_hour, 0, 23);
        bem = clamp_int(request->bedtime_end_minute, 0, 59);
    }
    if (request->present_mask & MP_IPC_DISPLAY_FONT_FILE) {
        font_file[0] = '\0';
        if (request->oled_font_file[0] && safe_asset_filename(request->oled_font_file) && has_font_ext(request->oled_font_file)) {
            char path[512];
            make_font_path(request->oled_font_file, path, sizeof(path));
            if (access(path, R_OK) == 0) mp_safe_str(font_file, sizeof(font_file), request->oled_font_file);
        }
    }

    pthread_mutex_lock(&g_state.lock);
    int changed = g_state.oled_font_size != font_size || strcmp(g_state.oled_font_file, font_file) != 0;
    g_state.oled_font = font;
    g_state.oled_font_size = font_size;
    g_state.clock_24h_mode = clock_mode;
    g_state.bedtime_enabled = bedtime_enabled;
    g_state.bedtime_dim_percent = bedtime_dim;
    g_state.bedtime_start_hour = bsh;
    g_state.bedtime_start_min = bsm;
    g_state.bedtime_end_hour = beh;
    g_state.bedtime_end_min = bem;
    mp_safe_str(g_state.oled_font_file, sizeof(g_state.oled_font_file), font_file);
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);
    if (changed) font_cache_reset();
    save_config();
    app_log("display", "Saved display settings");
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int ipc_asset_event(int client, const struct mp_ipc_asset_event *event) {
    if (event->action == MP_IPC_ASSET_UPLOADED) {
        if (event->kind == MP_IPC_ASSET_FACE) {
            invalidate_normal_face_assets();
            pthread_mutex_lock(&g_state.lock);
            g_state.display_mode = 0;
            mp_safe_str(g_state.current_face_file, sizeof(g_state.current_face_file), event->file);
            mp_safe_str(g_state.preview_face_file, sizeof(g_state.preview_face_file), event->file);
            g_state.preview_face_bedtime = 0;
            g_state.preview_face_until = time(NULL) + CLOCK_FACE_PREVIEW_SECONDS;
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
        } else if (event->kind == MP_IPC_ASSET_BEDTIME_FACE) {
            invalidate_bedtime_face_assets();
            pthread_mutex_lock(&g_state.lock);
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
        } else if (event->kind == MP_IPC_ASSET_FONT) {
            pthread_mutex_lock(&g_state.lock);
            mp_safe_str(g_state.oled_font_file, sizeof(g_state.oled_font_file), event->file);
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
            font_cache_reset();
            save_config();
        }
        app_log("assets", "Uploaded %u asset(s), first %s", event->count, event->file);
    } else if (event->action == MP_IPC_ASSET_DELETED) {
        if (event->kind == MP_IPC_ASSET_FACE || event->kind == MP_IPC_ASSET_BEDTIME_FACE) {
            int bedtime = event->kind == MP_IPC_ASSET_BEDTIME_FACE;
            if (bedtime) invalidate_bedtime_face_assets(); else invalidate_normal_face_assets();
            pthread_mutex_lock(&g_state.lock);
            if (!bedtime) {
                if (strcmp(g_state.current_face_file, event->file) == 0) g_state.current_face_file[0] = '\0';
                if (strcmp(g_state.message_face_file, event->file) == 0) g_state.message_face_file[0] = '\0';
                if (strcmp(g_state.preview_face_file, event->file) == 0) g_state.preview_face_file[0] = '\0';
            }
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
        } else if (event->kind == MP_IPC_ASSET_FONT) {
            pthread_mutex_lock(&g_state.lock);
            if (strcmp(g_state.oled_font_file, event->file) == 0) g_state.oled_font_file[0] = '\0';
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
            font_cache_reset();
            save_config();
        }
        app_log("assets", "Deleted asset %s", event->file);
    } else if (event->action == MP_IPC_ASSET_DELETED_ALL) {
        if (event->kind == MP_IPC_ASSET_FACE) {
            invalidate_all_face_assets();
            pthread_mutex_lock(&g_state.lock);
            g_state.current_face_file[0] = '\0';
            g_state.message_face_file[0] = '\0';
            g_state.preview_face_file[0] = '\0';
            g_state.preview_face_until = 0;
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
        } else if (event->kind == MP_IPC_ASSET_MUSIC) {
            audio_stop();
            pthread_mutex_lock(&g_state.lock);
            g_state.audio_playing = 0;
            g_state.audio_file[0] = '\0';
            for (int i = 0; i < MAX_ALARMS; i++) g_state.alarms[i].music_file[0] = '\0';
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
            save_config();
        }
        app_log("assets", "Deleted all assets of kind %u (%u files)", event->kind, event->count);
    } else {
        return ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"invalid asset event\"}");
    }
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int ipc_asset_state(int client) {
    struct mp_ipc_asset_state state;
    memset(&state, 0, sizeof(state));
    pthread_mutex_lock(&g_state.lock);
    state.global_volume = g_state.global_volume;
    state.builtin_font = g_state.oled_font;
    state.font_size = g_state.oled_font_size;
    mp_safe_str(state.current_music, sizeof(state.current_music), g_state.audio_file);
    mp_safe_str(state.selected_font, sizeof(state.selected_font), g_state.oled_font_file);
    pthread_mutex_unlock(&g_state.lock);
    return ipc_send_response(client, 200, MP_IPC_CONTENT_BINARY, &state, sizeof(state));
}

static int ipc_dispatch(int client, uint16_t opcode, const void *payload, size_t payload_len) {
#define EXPECT(type) do { if (payload_len != sizeof(type)) return ipc_bad_payload(client); } while (0)
    switch (opcode) {
        case MP_IPC_OP_STATUS:
            if (payload_len != 0) return ipc_bad_payload(client);
            return ipc_status(client);
        case MP_IPC_OP_DISPLAY_ACTION:
            EXPECT(struct mp_ipc_display_action);
            return ipc_display_action(client, payload);
        case MP_IPC_OP_DISPLAY_FACE:
            EXPECT(struct mp_ipc_display_face);
            return ipc_display_face(client, payload);
        case MP_IPC_OP_DISPLAY_MESSAGE:
            EXPECT(struct mp_ipc_display_message);
            return ipc_display_message(client, payload);
        case MP_IPC_OP_MESSAGE_LIMITS:
            if (payload_len != 0) return ipc_bad_payload(client);
            return ipc_message_limits(client);
        case MP_IPC_OP_MESSAGE_FIT:
            EXPECT(struct mp_ipc_message_fit);
            return ipc_message_fit(client, payload);
        case MP_IPC_OP_CONFIG_ALARM:
            EXPECT(struct mp_ipc_alarm_config);
            return ipc_config_alarm(client, payload);
        case MP_IPC_OP_CONFIG_AUDIO:
            EXPECT(struct mp_ipc_audio_config);
            return ipc_config_audio(client, payload);
        case MP_IPC_OP_CONFIG_PERSONALIZATION:
            EXPECT(struct mp_ipc_personalization_config);
            return ipc_config_personalization(client, payload);
        case MP_IPC_OP_CONFIG_DISPLAY:
            EXPECT(struct mp_ipc_display_config);
            return ipc_config_display(client, payload);
        case MP_IPC_OP_LOGS_GET:
            if (payload_len != 0) return ipc_bad_payload(client);
            return ipc_logs_get(client);
        case MP_IPC_OP_LOGS_CLEAR:
            if (payload_len != 0) return ipc_bad_payload(client);
            return ipc_logs_clear(client);
        case MP_IPC_OP_ASSET_EVENT:
            EXPECT(struct mp_ipc_asset_event);
            return ipc_asset_event(client, payload);
        case MP_IPC_OP_ASSET_STATE:
            if (payload_len != 0) return ipc_bad_payload(client);
            return ipc_asset_state(client);
        case MP_IPC_OP_PING:
            if (payload_len != 0) return ipc_bad_payload(client);
            return ipc_send_json(client, 200, "{\"ok\":true}");
        default:
            return ipc_send_json(client, 404, "{\"ok\":false,\"error\":\"unknown IPC opcode\"}");
    }
#undef EXPECT
}

static void set_ipc_timeouts(int client) {
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int socket_buffer = (int)(MP_IPC_MAX_PAYLOAD * 2u);
    (void)setsockopt(client, SOL_SOCKET, SO_RCVBUF, &socket_buffer, sizeof(socket_buffer));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDBUF, &socket_buffer, sizeof(socket_buffer));
}

static uid_t expected_api_uid(void) {
    const char *value = getenv("MK_PICLOCK_API_USER");
    if (!value || !*value) value = "mk-piclock-api";
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (!errno && end != value && *end == '\0') return (uid_t)parsed;
    struct passwd *entry = getpwnam(value);
    return entry ? entry->pw_uid : (uid_t)-1;
}

static int ipc_peer_allowed(int client, uid_t api_uid) {
#ifdef SO_PEERCRED
    struct ucred credentials;
    socklen_t length = sizeof(credentials);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) return 0;
    return credentials.uid == 0 || (api_uid != (uid_t)-1 && credentials.uid == api_uid);
#else
    (void)client;
    (void)api_uid;
    return 0;
#endif
}

static ssize_t ipc_expected_payload_size(uint16_t opcode) {
    switch (opcode) {
        case MP_IPC_OP_STATUS:
        case MP_IPC_OP_MESSAGE_LIMITS:
        case MP_IPC_OP_LOGS_GET:
        case MP_IPC_OP_LOGS_CLEAR:
        case MP_IPC_OP_ASSET_STATE:
        case MP_IPC_OP_PING:
            return 0;
        case MP_IPC_OP_DISPLAY_ACTION: return sizeof(struct mp_ipc_display_action);
        case MP_IPC_OP_DISPLAY_FACE: return sizeof(struct mp_ipc_display_face);
        case MP_IPC_OP_DISPLAY_MESSAGE: return sizeof(struct mp_ipc_display_message);
        case MP_IPC_OP_MESSAGE_FIT: return sizeof(struct mp_ipc_message_fit);
        case MP_IPC_OP_CONFIG_ALARM: return sizeof(struct mp_ipc_alarm_config);
        case MP_IPC_OP_CONFIG_AUDIO: return sizeof(struct mp_ipc_audio_config);
        case MP_IPC_OP_CONFIG_PERSONALIZATION: return sizeof(struct mp_ipc_personalization_config);
        case MP_IPC_OP_CONFIG_DISPLAY: return sizeof(struct mp_ipc_display_config);
        case MP_IPC_OP_ASSET_EVENT: return sizeof(struct mp_ipc_asset_event);
        default: return -1;
    }
}

static void handle_ipc_client(int client) {
    void *packet = NULL;
    size_t packet_len = 0;
    if (mp_recv_packet_alloc(client, &packet,
                             sizeof(struct mp_ipc_request_header) + MP_IPC_MAX_PAYLOAD,
                             &packet_len) != 0 || packet_len < sizeof(struct mp_ipc_request_header)) {
        free(packet);
        return;
    }
    struct mp_ipc_request_header header;
    memcpy(&header, packet, sizeof(header));
    if (header.magic != MP_IPC_MAGIC) {
        (void)ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"invalid IPC magic\"}");
        free(packet);
        return;
    }
    if (header.version != MP_IPC_VERSION) {
        (void)ipc_send_json(client, 409, "{\"ok\":false,\"error\":\"IPC protocol version mismatch\"}");
        free(packet);
        return;
    }
    ssize_t expected = ipc_expected_payload_size(header.opcode);
    if (expected < 0) {
        (void)ipc_send_json(client, 404, "{\"ok\":false,\"error\":\"unknown IPC opcode\"}");
        free(packet);
        return;
    }
    if (header.payload_len != (uint32_t)expected ||
        packet_len != sizeof(header) + header.payload_len) {
        (void)ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"invalid IPC payload length\"}");
        free(packet);
        return;
    }
    const void *payload = header.payload_len
        ? (const unsigned char *)packet + sizeof(header) : NULL;
    (void)ipc_dispatch(client, header.opcode, payload, header.payload_len);
    free(packet);
}

static void *ipc_thread_main(void *arg) {
    (void)arg;
    if (mkdir(CORE_RUNTIME_DIR, 0770) != 0 && errno != EEXIST) {
        perror("mkdir core runtime");
        return NULL;
    }
    uid_t api_uid = expected_api_uid();
    if (api_uid == (uid_t)-1)
        fprintf(stderr, "warning: mk-piclock-api user not found; only root IPC peers will be accepted\n");
    int server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (server < 0) {
        perror("core socket");
        return NULL;
    }
    int socket_buffer = (int)(MP_IPC_MAX_PAYLOAD * 2u);
    (void)setsockopt(server, SOL_SOCKET, SO_RCVBUF, &socket_buffer, sizeof(socket_buffer));
    (void)setsockopt(server, SOL_SOCKET, SO_SNDBUF, &socket_buffer, sizeof(socket_buffer));
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(CORE_SOCKET_PATH) >= sizeof(addr.sun_path)) {
        close(server);
        return NULL;
    }
    mp_safe_str(addr.sun_path, sizeof(addr.sun_path), CORE_SOCKET_PATH);
    unlink(CORE_SOCKET_PATH);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        chmod(CORE_SOCKET_PATH, 0660) != 0 || listen(server, 8) != 0) {
        perror("bind/listen core IPC");
        close(server);
        unlink(CORE_SOCKET_PATH);
        return NULL;
    }
    fprintf(stderr, "private SOCK_SEQPACKET IPC listening on %s\n", CORE_SOCKET_PATH);
    while (g_running) {
        struct pollfd pfd = {.fd = server, .events = POLLIN};
        int ready = poll(&pfd, 1, 500);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0 || !(pfd.revents & POLLIN)) continue;
        int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        set_ipc_timeouts(client);
        if (!ipc_peer_allowed(client, api_uid)) {
            (void)ipc_send_json(client, 403, "{\"ok\":false,\"error\":\"IPC peer rejected\"}");
            close(client);
            continue;
        }
        handle_ipc_client(client);
        close(client);
    }
    close(server);
    unlink(CORE_SOCKET_PATH);
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
    ensure_dir(CONFIG_DIR);
    init_alarm_defaults();
    load_config();
    app_log("system", "mk-piclock %s starting", APP_VERSION);

    int touch_available = (touch_init() == 0);
    if (touch_available) app_log("system", "TTP223B touch input initialized on GPIO %d", GPIO_TOUCH);
    else app_log("system", "TTP223B touch input unavailable on GPIO %d", GPIO_TOUCH);

    if (oled_init() == 0) {
        pthread_mutex_lock(&g_state.lock);
        g_state.oled_ok = 1;
        pthread_mutex_unlock(&g_state.lock);
        app_log("system", "OLED initialized");
        draw_startup_screen();
        sleep(STARTUP_GREETING_SECONDS);
        draw_clock_screen();
    } else {
        app_log("system", "OLED init failed, core IPC will still start");
        fprintf(stderr, "OLED init failed, core IPC will still start\n");
    }

    pthread_t ipc_thread;
    pthread_t touch_thread;
    int touch_thread_started = 0;
    app_log("system", "Private core IPC listening on %s", CORE_SOCKET_PATH);

    if (touch_available) {
        if (pthread_create(&touch_thread, NULL, touch_thread_main, NULL) == 0) {
            touch_thread_started = 1;
        } else {
            app_log("system", "Unable to start touch input thread");
            touch_close();
        }
    }

    if (pthread_create(&ipc_thread, NULL, ipc_thread_main, NULL) != 0) {
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
        activate_pending_message_if_due();
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
                } else if (song_metadata_scroll_active()) {
                    refresh_clock_footer();
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

    if (touch_thread_started) pthread_join(touch_thread, NULL);
    touch_close();
    (void)audio_stop_and_wait(3000u);
    pthread_mutex_lock(&g_font.lock);
    font_cache_close_locked();
    pthread_mutex_unlock(&g_font.lock);
    oled_close();
    mpg123_exit();
    return 0;
}
