/*
  mk-piclock.c

  Private Raspberry Pi alarm clock core daemon.

  Features:
    - SSD1322 256x64 OLED over /dev/spidev0.0
    - libgpiod GPIO for OLED DC/RST and TTP223B touch input
    - Private Unix socket control service for mk-piclock-api
    - MP3 playback using libmpg123 + ALSA PCM
    - Alarms, fonts, images, messages, bedtime dimming, touch input, and config

  Build both services with the supplied Makefile.
*/

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/wireless.h>
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
#include "led_control.h"
#include "asset_format.h"
#include "util.h"

#define safe_str mp_safe_str
#define write_all mp_write_full

#define APP_NAME "mk-piclock-core"
#define APP_VERSION "1.7.6"

#define LED_PROFILE(fx, level, seconds, r1, g1, b1, r2, g2, b2) \
    { .effect = (fx), .brightness = (level), .cycle_seconds = (seconds), .reserved = 0, \
      .red1 = (r1), .green1 = (g1), .blue1 = (b1), \
      .red2 = (r2), .green2 = (g2), .blue2 = (b2) }
#define DEFAULT_CLOCK_NAME "Rylie"
#define STARTUP_GREETING_SECONDS 3
#define APP_ROOT "/opt/mk-piclock"
#define IMAGE_DIR APP_ROOT "/assets/images"
#define BEDTIME_IMAGE_DIR APP_ROOT "/assets/bedtime-images"
#define MUSIC_DIR APP_ROOT "/assets/music"
#define STORY_DIR APP_ROOT "/assets/stories"
#define DEFAULT_ALARM_PATH APP_ROOT "/assets/default-alarm.mp3"
#define DEFAULT_ALARM_LABEL "Built-in Alarm"
#define MESSAGE_CHIME_PATH APP_ROOT "/assets/message-chime.mp3"
#define MESSAGE_CHIME_LABEL "Message Chime"
#define MESSAGE_CHIME_VOLUME_MAX 55
#define ALARM_MAX_DURATION_SECONDS 1800
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
#define TOUCH_DIAGNOSTIC_PRESS_MS 8000u
#define DIAGNOSTIC_SCREEN_SECONDS 30u
#define DIAGNOSTIC_REFRESH_MS 2000u
#define STORY_PRESS_COUNT 10
#define STORY_PRESS_WINDOW_MS 8000u
#define STORY_MESSAGE_MAX 64
#define STORY_COLLAGE_MAX_IMAGES 5
#define STORY_COLLAGE_IMAGE_SIZE 48
#define STORY_INTRO_SECONDS 3u
#define STORY_TITLE_SECONDS 4u

#define MAX_ALARMS 7
#define MUSIC_FILE_MAX 256
#define IMAGE_FILE_MAX 128
#define CLOCK_IMAGE_CHANGE_MAX_SECONDS 300
#define BEDTIME_IMAGE_CHANGE_SECONDS 300
#define CLOCK_TIME_Y_OFFSET -7
#define CLOCK_IMAGE_Y_NUDGE -2
#define CLOCK_SIDE_WIDGET_SIZE 54
#define CLOCK_SIDE_WIDGET_X 4
#define CLOCK_STATUS_PILL_H 11
#define SONG_METADATA_X 74
#define SONG_METADATA_W (OLED_W - SONG_METADATA_X - 2)
#define SONG_METADATA_TEXT_MAX (MP_ID3_TEXT_MAX * 2 + 4)
#define SONG_SCROLL_SPEED_PX_PER_SEC 18u
#define SONG_SCROLL_GAP_PX 24
#define SONG_SCROLL_FRAME_US 75000u


#define CORE_RUNTIME_DIR "/run/mk-piclock"
#define CORE_SOCKET_PATH CORE_RUNTIME_DIR "/core.sock"
#define ASSET_LIST_MAX_FILES 256
#define ASSET_LIST_NAME_MAX MUSIC_FILE_MAX

static volatile sig_atomic_t g_running = 1;
static time_t g_start_time = 0;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

struct rotating_image_state {
    char file[IMAGE_FILE_MAX];
    time_t next_change;
};

static struct rotating_image_state g_rotating_images[2];

struct story_collage_state {
    char files[STORY_COLLAGE_MAX_IMAGES][IMAGE_FILE_MAX];
    int count;
};

static struct story_collage_state g_story_collage;

struct image_raw_cache {
    char file[IMAGE_FILE_MAX];
    int bedtime;
    uint8_t raw[MP_IMAGE_RAW_BYTES];
    int valid;
    unsigned long generation;
};

static pthread_mutex_t g_image_raw_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct image_raw_cache g_image_raw_cache = { .file = "", .bedtime = 0, .valid = 0, .generation = 0 };

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
    int display_mode;       /* 0 clock, 1 clear, 2 message, 3 diagnostics */
    int display_dirty;      /* set by API IPC thread; drawn by main OLED loop */
    int diagnostic_return_mode;
    time_t diagnostic_until;
    char message_image_file[IMAGE_FILE_MAX];
    int message_image_bedtime;
    time_t message_until;   /* unix time; 0 = no active message */
    char message_text[192];
    char pending_message_image_file[IMAGE_FILE_MAX];
    int pending_message_image_bedtime;
    int pending_message_notification_sound;
    char pending_message_text[192];
    time_t pending_message_at;
    int global_volume;
    int story_mode_enabled;
    int story_volume;
    char story_message[STORY_MESSAGE_MAX];
    int story_playing;
    uint64_t story_intro_until_ms;
    uint64_t story_title_until_ms;
    int bedtime_enabled;
    int bedtime_start_hour;
    int bedtime_start_min;
    int bedtime_end_hour;
    int bedtime_end_min;
    int bedtime_dim_percent;
    int bedtime_music_enabled;
    int oled_contrast_current;
    int oled_master_current;
    int oled_brightness_current;
    int brightness_preview_percent;
    time_t brightness_preview_until;
    int audio_playing;
    char audio_file[MUSIC_FILE_MAX];
    char audio_title[MP_ID3_TEXT_MAX];
    char audio_artist[MP_ID3_TEXT_MAX];
    char audio_display[SONG_METADATA_TEXT_MAX];
    uint64_t audio_scroll_started_ms;
    int show_song_metadata;
    int alarm_active;             /* 1 only while an alarm MP3 is currently playing */
    int alarm_volume_percent;     /* current alarm ramp volume, 0..100 */
    long long last_successful_alarm;
    struct alarm_slot alarms[MAX_ALARMS];
    struct mp_led_profile led_profiles[MP_LED_SCENE_COUNT];
    struct mp_led_global_settings led_settings;
    int led_bedtime_fade_active;
    int led_preview_scene;
    time_t led_preview_until;
    int led_preview_bypass_master;
    int led_preview_raw_output;
    struct mp_led_profile led_preview_profile;
    int oled_ok;
    char clock_name[64];
    int oled_font;         /* 0 seven, 1 seven thin, 2 pixel, 3 pixel bold */
    char oled_font_file[128]; /* uploaded .ttf/.otf filename, empty = built-in */
    int oled_font_size;    /* TrueType pixel size */
    int clock_24h_mode;    /* 0 = 12-hour, 1 = 24-hour */
    int oled_color;        /* GUI panel colour: yellow, green, or white */
    pthread_mutex_t lock;
};

static struct app_state g_state = {
    .display_mode = 0,
    .display_dirty = 1,
    .diagnostic_return_mode = 0,
    .diagnostic_until = 0,
    .message_image_file = "",
    .message_image_bedtime = 0,
    .message_until = 0,
    .message_text = "",
    .pending_message_image_file = "",
    .pending_message_image_bedtime = 0,
    .pending_message_notification_sound = 0,
    .pending_message_text = "",
    .pending_message_at = 0,
    .global_volume = 80,
    .story_mode_enabled = 0,
    .story_volume = 55,
    .story_message = "STORY MODE!",
    .story_playing = 0,
    .story_intro_until_ms = 0,
    .story_title_until_ms = 0,
    .bedtime_enabled = 0,
    .bedtime_start_hour = 21,
    .bedtime_start_min = 0,
    .bedtime_end_hour = 7,
    .bedtime_end_min = 0,
    .bedtime_dim_percent = 35,
    .bedtime_music_enabled = 1,
    .oled_contrast_current = -1,
    .oled_master_current = -1,
    .oled_brightness_current = -1,
    .brightness_preview_percent = -1,
    .brightness_preview_until = 0,
    .audio_playing = 0,
    .audio_file = "",
    .audio_title = "",
    .audio_artist = "",
    .audio_display = "",
    .audio_scroll_started_ms = 0,
    .show_song_metadata = 1,
    .alarm_active = 0,
    .alarm_volume_percent = 0,
    .last_successful_alarm = 0,
    .led_profiles = {
        [MP_LED_SCENE_ALARM] = LED_PROFILE(MP_LED_EFFECT_FADE, 70, 8, 255, 0, 0, 255, 160, 0),
        [MP_LED_SCENE_BEDTIME] = LED_PROFILE(MP_LED_EFFECT_SOLID, 3, 8, 255, 48, 0, 255, 48, 0),
        [MP_LED_SCENE_MESSAGE] = LED_PROFILE(MP_LED_EFFECT_FADE, 35, 8, 0, 150, 255, 255, 255, 255),
        [MP_LED_SCENE_MUSIC] = LED_PROFILE(MP_LED_EFFECT_RAINBOW, 40, 12, 255, 0, 0, 0, 0, 255),
        [MP_LED_SCENE_DAYTIME] = LED_PROFILE(MP_LED_EFFECT_SOLID, 5, 8, 255, 220, 160, 255, 220, 160),
        [MP_LED_SCENE_STORIES] = LED_PROFILE(MP_LED_EFFECT_FADE, 15, 8, 125, 45, 255, 0, 90, 255),
        [MP_LED_SCENE_TOUCH] = LED_PROFILE(MP_LED_EFFECT_FADE, 60, 2, 255, 255, 255, 0, 0, 0)
    },
    .led_settings = {1, 100, 100, 65, 80, 0, 15, 1, 60, 255, 255, 255},
    .led_bedtime_fade_active = 0,
    .led_preview_scene = -1,
    .led_preview_until = 0,
    .led_preview_bypass_master = 0,
    .led_preview_raw_output = 0,
    .led_preview_profile = {0},
    .oled_ok = 0,
    .clock_name = DEFAULT_CLOCK_NAME,
    .oled_font = 0,
    .oled_font_file = "",
    .oled_font_size = 48,
    .clock_24h_mode = 0,
    .oled_color = MP_OLED_COLOR_GREEN,
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
    int alarm_mode;
    int notification_mode;
    int timed_out;
    uint64_t alarm_deadline_ms;
    char file[MUSIC_FILE_MAX];
};

static struct audio_player_state g_audio = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .stopped = PTHREAD_COND_INITIALIZER,
    .running = 0,
    .stop_requested = 0,
    .alarm_mode = 0,
    .notification_mode = 0,
    .timed_out = 0,
    .alarm_deadline_ms = 0,
    .file = ""
};


struct oled_dev {
    int spi_fd;
    struct gpiod_chip *chip;
    struct gpiod_line_request *gpio_req;
    uint8_t fb[OLED_FB_BYTES];
    uint8_t prev_fb[OLED_FB_BYTES];
    uint8_t preview_fb[OLED_FB_BYTES];
    pthread_mutex_t preview_lock;
    int prev_valid;
};

static struct oled_dev g_oled = {
    .spi_fd = -1,
    .chip = NULL,
    .gpio_req = NULL,
    .preview_lock = PTHREAD_MUTEX_INITIALIZER,
    .prev_valid = 0
};

/* Drawing normally targets the physical OLED framebuffer. The message editor
 * can render into a thread-local scratch framebuffer so its browser preview is
 * produced by the exact same renderer without changing the physical screen. */
static _Thread_local uint8_t *g_oled_render_fb = NULL;

static uint8_t *oled_draw_fb(void) {
    return g_oled_render_fb ? g_oled_render_fb : g_oled.fb;
}

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

static void touch_get_state(int *ready, int *pressed);

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
static void sanitize_message_text(char *s, size_t s_len);


static int has_raw_ext(const char *name) {
    const char *dot = strrchr(name ? name : "", '.');
    return dot && strcasecmp(dot, ".raw") == 0;
}

static int safe_image_filename(const char *name) {
    return safe_asset_filename(name) && has_raw_ext(name);
}

static const char *image_dir(int bedtime) {
    return bedtime ? BEDTIME_IMAGE_DIR : IMAGE_DIR;
}

static int make_image_path_by_file(const char *file, int bedtime, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    if (!safe_image_filename(file)) return -1;
    int written = snprintf(out, out_len, "%s/%s", image_dir(bedtime), file);
    return written > 0 && (size_t)written < out_len ? 0 : -1;
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
    ASSET_LIST_IMAGE_RAW = 1,
    ASSET_LIST_MUSIC_MP3 = 2
};

static void invalidate_image_raw_cache(void) {
    pthread_mutex_lock(&g_image_raw_cache_lock);
    g_image_raw_cache.valid = 0;
    g_image_raw_cache.file[0] = '\0';
    g_image_raw_cache.generation++;
    pthread_mutex_unlock(&g_image_raw_cache_lock);
}

static void invalidate_image_assets(int bedtime) {
    invalidate_image_raw_cache();
    struct rotating_image_state *state = &g_rotating_images[bedtime ? 1 : 0];
    state->file[0] = '\0';
    state->next_change = 0;
}


static int asset_file_matches_kind(const char *dir, const char *name, int kind) {
    if (!dir || !name) return 0;
    if (kind == ASSET_LIST_IMAGE_RAW) {
        if (!safe_image_filename(name)) return 0;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        struct stat st;
        return stat(path, &st) == 0 && st.st_size == MP_IMAGE_RAW_BYTES;
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
}


static void reset_persistent_state_locked(void) {
    g_state.display_mode = 0;
    g_state.display_dirty = 1;
    g_state.diagnostic_return_mode = 0;
    g_state.diagnostic_until = 0;
    g_state.message_image_file[0] = '\0';
    g_state.message_image_bedtime = 0;
    g_state.message_until = 0;
    g_state.message_text[0] = '\0';
    g_state.pending_message_image_file[0] = '\0';
    g_state.pending_message_image_bedtime = 0;
    g_state.pending_message_notification_sound = 0;
    g_state.pending_message_text[0] = '\0';
    g_state.pending_message_at = 0;
    g_state.global_volume = 80;
    g_state.story_mode_enabled = 0;
    g_state.story_volume = 55;
    safe_str(g_state.story_message, sizeof(g_state.story_message), "STORY MODE!");
    g_state.story_playing = 0;
    g_state.story_intro_until_ms = 0;
    g_state.story_title_until_ms = 0;
    g_state.bedtime_enabled = 0;
    g_state.bedtime_start_hour = 21;
    g_state.bedtime_start_min = 0;
    g_state.bedtime_end_hour = 7;
    g_state.bedtime_end_min = 0;
    g_state.bedtime_dim_percent = 35;
    g_state.bedtime_music_enabled = 1;
    g_state.show_song_metadata = 1;
    g_state.last_successful_alarm = 0;
    safe_str(g_state.clock_name, sizeof(g_state.clock_name), DEFAULT_CLOCK_NAME);
    g_state.oled_font = 0;
    g_state.oled_font_file[0] = '\0';
    g_state.oled_font_size = 48;
    g_state.clock_24h_mode = 0;
    g_state.oled_color = MP_OLED_COLOR_GREEN;
    g_state.led_profiles[MP_LED_SCENE_ALARM] =
        (struct mp_led_profile)LED_PROFILE(MP_LED_EFFECT_FADE, 70, 8, 255, 0, 0, 255, 160, 0);
    g_state.led_profiles[MP_LED_SCENE_BEDTIME] =
        (struct mp_led_profile)LED_PROFILE(MP_LED_EFFECT_SOLID, 3, 8, 255, 48, 0, 255, 48, 0);
    g_state.led_profiles[MP_LED_SCENE_MESSAGE] =
        (struct mp_led_profile)LED_PROFILE(MP_LED_EFFECT_FADE, 35, 8, 0, 150, 255, 255, 255, 255);
    g_state.led_profiles[MP_LED_SCENE_MUSIC] =
        (struct mp_led_profile)LED_PROFILE(MP_LED_EFFECT_RAINBOW, 40, 12, 255, 0, 0, 0, 0, 255);
    g_state.led_profiles[MP_LED_SCENE_DAYTIME] =
        (struct mp_led_profile)LED_PROFILE(MP_LED_EFFECT_SOLID, 5, 8, 255, 220, 160, 255, 220, 160);
    g_state.led_profiles[MP_LED_SCENE_STORIES] =
        (struct mp_led_profile)LED_PROFILE(MP_LED_EFFECT_FADE, 15, 8, 125, 45, 255, 0, 90, 255);
    g_state.led_profiles[MP_LED_SCENE_TOUCH] =
        (struct mp_led_profile)LED_PROFILE(MP_LED_EFFECT_FADE, 60, 2, 255, 255, 255, 0, 0, 0);
    g_state.led_settings =
        (struct mp_led_global_settings){1, 100, 100, 65, 80, 0, 15, 1, 60, 255, 255, 255};
    g_state.led_bedtime_fade_active = 0;
    g_state.led_preview_scene = -1;
    g_state.led_preview_until = 0;
    g_state.led_preview_bypass_master = 0;
    g_state.led_preview_raw_output = 0;
    memset(&g_state.led_preview_profile, 0, sizeof(g_state.led_preview_profile));
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
}


#define CONFIG_INT_FIELDS(X) \
    X("global_volume", g_state.global_volume, "%d") \
    X("story_mode_enabled", g_state.story_mode_enabled, "%d") \
    X("story_volume", g_state.story_volume, "%d") \
    X("show_song_metadata", g_state.show_song_metadata, "%d") \
    X("bedtime_enabled", g_state.bedtime_enabled, "%d") \
    X("bedtime_start_hour", g_state.bedtime_start_hour, "%02d") \
    X("bedtime_start_min", g_state.bedtime_start_min, "%02d") \
    X("bedtime_end_hour", g_state.bedtime_end_hour, "%02d") \
    X("bedtime_end_min", g_state.bedtime_end_min, "%02d") \
    X("bedtime_dim_percent", g_state.bedtime_dim_percent, "%d") \
    X("bedtime_music_enabled", g_state.bedtime_music_enabled, "%d") \
    X("oled_font", g_state.oled_font, "%d") \
    X("oled_font_size", g_state.oled_font_size, "%d") \
    X("clock_24h_mode", g_state.clock_24h_mode, "%d") \
    X("oled_color", g_state.oled_color, "%d") \
    X("led_enabled", g_state.led_settings.enabled, "%d") \
    X("led_max_brightness", g_state.led_settings.max_brightness, "%d") \
    X("led_red_gain", g_state.led_settings.red_gain, "%d") \
    X("led_green_gain", g_state.led_settings.green_gain, "%d") \
    X("led_blue_gain", g_state.led_settings.blue_gain, "%d") \
    X("led_idle_off", g_state.led_settings.idle_off, "%d") \
    X("led_bedtime_fade_minutes", g_state.led_settings.bedtime_fade_minutes, "%d") \
    X("led_touch_blink_enabled", g_state.led_settings.touch_blink_enabled, "%d") \
    X("led_touch_blink_brightness", g_state.led_settings.touch_blink_brightness, "%d") \
    X("led_touch_blink_red", g_state.led_settings.touch_blink_red, "%d") \
    X("led_touch_blink_green", g_state.led_settings.touch_blink_green, "%d") \
    X("led_touch_blink_blue", g_state.led_settings.touch_blink_blue, "%d")

#define CONFIG_STRING_FIELDS(X) \
    X("clock_name", g_state.clock_name, sizeof(g_state.clock_name)) \
    X("story_message", g_state.story_message, sizeof(g_state.story_message)) \
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

static const char *led_scene_config_key(enum mp_led_scene scene) {
    switch (scene) {
        case MP_LED_SCENE_ALARM: return "alarm";
        case MP_LED_SCENE_BEDTIME: return "bedtime";
        case MP_LED_SCENE_MESSAGE: return "message";
        case MP_LED_SCENE_MUSIC: return "music";
        case MP_LED_SCENE_DAYTIME: return "daytime";
        case MP_LED_SCENE_STORIES: return "stories";
        case MP_LED_SCENE_TOUCH: return "touch";
        default: return "daytime";
    }
}

static void sanitize_led_profile(struct mp_led_profile *profile) {
    if (!profile) return;
    if (profile->effect > MP_LED_EFFECT_RAINBOW) profile->effect = MP_LED_EFFECT_SOLID;
    if (profile->brightness > 100) profile->brightness = 100;
    if (profile->cycle_seconds < 2) profile->cycle_seconds =
        profile->effect == MP_LED_EFFECT_RAINBOW ? 12 : 8;
    if (profile->cycle_seconds > 60) profile->cycle_seconds = 60;
    profile->reserved = 0;
}

static void sanitize_led_settings(struct mp_led_global_settings *settings) {
    if (!settings) return;
    settings->enabled = settings->enabled ? 1 : 0;
    if (settings->max_brightness > 100) settings->max_brightness = 100;
    if (settings->red_gain > 100) settings->red_gain = 100;
    if (settings->green_gain > 100) settings->green_gain = 100;
    if (settings->blue_gain > 100) settings->blue_gain = 100;
    settings->idle_off = settings->idle_off ? 1 : 0;
    if (settings->bedtime_fade_minutes > 120) settings->bedtime_fade_minutes = 120;
    settings->touch_blink_enabled = settings->touch_blink_enabled ? 1 : 0;
    if (settings->touch_blink_brightness > 100) settings->touch_blink_brightness = 100;
}

static int load_led_config_value(const char *key, const char *val) {
    char expected[96];
    for (int i = 0; i < MP_LED_SCENE_COUNT; i++) {
        struct mp_led_profile *profile = &g_state.led_profiles[i];
        const char *name = led_scene_config_key((enum mp_led_scene)i);
#define MATCH_LED_FIELD(suffix, target, maximum) \
        do { \
            snprintf(expected, sizeof(expected), "led_%s_%s", name, suffix); \
            if (strcmp(key, expected) == 0) { \
                target = (uint8_t)clamp_int(atoi(val), 0, maximum); \
                return 1; \
            } \
        } while (0)
        MATCH_LED_FIELD("effect", profile->effect, MP_LED_EFFECT_RAINBOW);
        MATCH_LED_FIELD("brightness", profile->brightness, 100);
        MATCH_LED_FIELD("cycle_seconds", profile->cycle_seconds, 60);
        MATCH_LED_FIELD("red1", profile->red1, 255);
        MATCH_LED_FIELD("green1", profile->green1, 255);
        MATCH_LED_FIELD("blue1", profile->blue1, 255);
        MATCH_LED_FIELD("red2", profile->red2, 255);
        MATCH_LED_FIELD("green2", profile->green2, 255);
        MATCH_LED_FIELD("blue2", profile->blue2, 255);
#undef MATCH_LED_FIELD
    }
    return 0;
}

static void save_config(void) {
    ensure_dir(CONFIG_DIR);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), CONFIG_FILE ".tmp");

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;

    pthread_mutex_lock(&g_state.lock);
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
    for (int i = 0; i < MP_LED_SCENE_COUNT; i++) {
        const struct mp_led_profile *profile = &g_state.led_profiles[i];
        const char *name = led_scene_config_key((enum mp_led_scene)i);
        fprintf(f, "led_%s_effect=%u\n", name, profile->effect);
        fprintf(f, "led_%s_brightness=%u\n", name, profile->brightness);
        fprintf(f, "led_%s_cycle_seconds=%u\n", name, profile->cycle_seconds);
        fprintf(f, "led_%s_red1=%u\n", name, profile->red1);
        fprintf(f, "led_%s_green1=%u\n", name, profile->green1);
        fprintf(f, "led_%s_blue1=%u\n", name, profile->blue1);
        fprintf(f, "led_%s_red2=%u\n", name, profile->red2);
        fprintf(f, "led_%s_green2=%u\n", name, profile->green2);
        fprintf(f, "led_%s_blue2=%u\n", name, profile->blue2);
    }
    fprintf(f, "last_successful_alarm=%lld\n", g_state.last_successful_alarm);
    fprintf(f, "pending_message_at=%lld\n", (long long)g_state.pending_message_at);
    fprintf(f, "pending_message_image_bedtime=%d\n",
            g_state.pending_message_image_bedtime ? 1 : 0);
    fprintf(f, "pending_message_notification_sound=%d\n",
            g_state.pending_message_notification_sound ? 1 : 0);
    {
        char enc_image[sizeof(g_state.pending_message_image_file) * 3];
        char enc_text[sizeof(g_state.pending_message_text) * 3];
        config_encode_to_buffer(enc_image, sizeof(enc_image), g_state.pending_message_image_file);
        config_encode_to_buffer(enc_text, sizeof(enc_text), g_state.pending_message_text);
        fprintf(f, "pending_message_image_file=%s\n", enc_image);
        fprintf(f, "pending_message_text=%s\n", enc_text);
    }
    pthread_mutex_unlock(&g_state.lock);

    int ok = 1;
    if (fflush(f) != 0) ok = 0;
    if (ok && fsync(fileno(f)) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;

    if (ok) {
        if (rename(tmp_path, CONFIG_FILE) != 0) unlink(tmp_path);
    } else {
        unlink(tmp_path);
    }
}

static void load_config(void) {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';

        int matched = load_led_config_value(key, val);
        char tmp_key[128];
        if (!matched && strcmp(key, "last_successful_alarm") == 0) {
            char *end = NULL;
            errno = 0;
            long long parsed = strtoll(val, &end, 10);
            if (!errno && end != val && *end == '\0' && parsed > 0)
                g_state.last_successful_alarm = parsed;
            matched = 1;
        } else if (strcmp(key, "pending_message_at") == 0) {
            char *end = NULL;
            errno = 0;
            long long parsed = strtoll(val, &end, 10);
            if (!errno && end != val && *end == '\0' && parsed > 0)
                g_state.pending_message_at = (time_t)parsed;
            matched = 1;
        } else if (strcmp(key, "pending_message_image_bedtime") == 0) {
            g_state.pending_message_image_bedtime = atoi(val) ? 1 : 0;
            matched = 1;
        } else if (strcmp(key, "pending_message_notification_sound") == 0) {
            g_state.pending_message_notification_sound = atoi(val) ? 1 : 0;
            matched = 1;
        } else if (strcmp(key, "pending_message_image_file") == 0) {
            char decoded[384];
            safe_str(decoded, sizeof(decoded), val);
            config_decode_inplace(decoded);
            safe_str(g_state.pending_message_image_file, sizeof(g_state.pending_message_image_file), decoded);
            matched = 1;
        } else if (strcmp(key, "pending_message_text") == 0) {
            char decoded[768];
            safe_str(decoded, sizeof(decoded), val);
            config_decode_inplace(decoded);
            safe_str(g_state.pending_message_text, sizeof(g_state.pending_message_text), decoded);
            matched = 1;
        }
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
    }

    fclose(f);

    g_state.global_volume = clamp_int(g_state.global_volume, 0, 100);
    g_state.story_mode_enabled = g_state.story_mode_enabled ? 1 : 0;
    g_state.story_volume = clamp_int(g_state.story_volume, 0, 100);
    sanitize_message_text(g_state.story_message, sizeof(g_state.story_message));
    if (!g_state.story_message[0]) safe_str(g_state.story_message, sizeof(g_state.story_message), "STORY MODE!");
    g_state.show_song_metadata = g_state.show_song_metadata ? 1 : 0;
    g_state.bedtime_enabled = g_state.bedtime_enabled ? 1 : 0;
    g_state.bedtime_start_hour = clamp_int(g_state.bedtime_start_hour, 0, 23);
    g_state.bedtime_start_min = clamp_int(g_state.bedtime_start_min, 0, 59);
    g_state.bedtime_end_hour = clamp_int(g_state.bedtime_end_hour, 0, 23);
    g_state.bedtime_end_min = clamp_int(g_state.bedtime_end_min, 0, 59);
    g_state.bedtime_dim_percent = clamp_int(g_state.bedtime_dim_percent, 0, 100);
    g_state.bedtime_music_enabled = g_state.bedtime_music_enabled ? 1 : 0;
    if (g_state.last_successful_alarm < 0) g_state.last_successful_alarm = 0;
    g_state.clock_24h_mode = g_state.clock_24h_mode ? 1 : 0;
    g_state.oled_color = clamp_int(g_state.oled_color, MP_OLED_COLOR_YELLOW, MP_OLED_COLOR_WHITE);
    for (int i = 0; i < MP_LED_SCENE_COUNT; i++)
        sanitize_led_profile(&g_state.led_profiles[i]);
    sanitize_led_settings(&g_state.led_settings);

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

    time_t now = time(NULL);
    if (g_state.pending_message_at > now + (time_t)(30 * 24 * 60 * 60)) {
        g_state.pending_message_at = 0;
        g_state.pending_message_image_file[0] = '\0';
        g_state.pending_message_image_bedtime = 0;
        g_state.pending_message_notification_sound = 0;
        g_state.pending_message_text[0] = '\0';
    } else if (g_state.pending_message_at > 0) {
        g_state.pending_message_image_bedtime = g_state.pending_message_image_bedtime ? 1 : 0;
        g_state.pending_message_notification_sound = g_state.pending_message_notification_sound ? 1 : 0;
        int pending_image_ok = 0;
        if (safe_image_filename(g_state.pending_message_image_file)) {
            char pending_path[512];
            pending_image_ok = make_image_path_by_file(
                g_state.pending_message_image_file,
                g_state.pending_message_image_bedtime,
                pending_path, sizeof(pending_path)) == 0 &&
                access(pending_path, R_OK) == 0;
        }
        if (!pending_image_ok)
            g_state.pending_message_image_file[0] = '\0';
        sanitize_message_text(g_state.pending_message_text, sizeof(g_state.pending_message_text));
        if (!g_state.pending_message_text[0] && !pending_image_ok) {
            g_state.pending_message_at = 0;
            g_state.pending_message_image_bedtime = 0;
            g_state.pending_message_notification_sound = 0;
        }
    } else {
        g_state.pending_message_image_file[0] = '\0';
        g_state.pending_message_image_bedtime = 0;
        g_state.pending_message_notification_sound = 0;
        g_state.pending_message_text[0] = '\0';
    }

    sanitize_clock_name(g_state.clock_name);
    if (g_state.oled_font < 0 || g_state.oled_font > 3) g_state.oled_font = 0;
    if (g_state.oled_font_size < 18 || g_state.oled_font_size > 54) g_state.oled_font_size = 48;
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
    /* Truncate instead of rounding at the low end. At 10%, rounding made
       both the master current and a white pixel level 2, causing a large
       jump above 0%. Truncation gives level 1 and minimum master current. */
    int hardware_percent = percent <= 0 ? 1 : percent;
    int contrast = (127 * hardware_percent) / 100;
    int master = (15 * hardware_percent) / 100;
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
    int preview_pct;
    time_t preview_until;
    time_t now = time(NULL);

    pthread_mutex_lock(&g_state.lock);
    bedtime_enabled = g_state.bedtime_enabled;
    sh = g_state.bedtime_start_hour;
    sm = g_state.bedtime_start_min;
    eh = g_state.bedtime_end_hour;
    em = g_state.bedtime_end_min;
    dim_pct = g_state.bedtime_dim_percent;
    current_pct = g_state.oled_brightness_current;
    preview_pct = g_state.brightness_preview_percent;
    preview_until = g_state.brightness_preview_until;
    if (preview_until > 0 && preview_until <= now) {
        g_state.brightness_preview_percent = -1;
        g_state.brightness_preview_until = 0;
        preview_pct = -1;
        preview_until = 0;
    }
    pthread_mutex_unlock(&g_state.lock);

    struct tm tmv;
    localtime_r(&now, &tmv);
    int now_min = tmv.tm_hour * 60 + tmv.tm_min;
    int start_min = sh * 60 + sm;
    int end_min = eh * 60 + em;
    int in_bedtime = bedtime_enabled && time_in_window_minutes(now_min, start_min, end_min);

    /* A live GUI preview temporarily overrides the schedule. Once it expires,
       bedtime_dim_percent applies during bedtime and daytime returns to 100%. */
    int target_pct = preview_until > now && preview_pct >= 0
        ? clamp_int(preview_pct, 0, 100)
        : (in_bedtime ? clamp_int(dim_pct, 0, 100) : 100);
    if (target_pct != current_pct) oled_set_brightness_percent(target_pct);
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

static void update_led_scene(void) {
    time_t now = time(NULL);
    enum mp_led_scene scene = MP_LED_SCENE_DAYTIME;
    struct mp_led_profile profile;
    struct mp_led_profile preview_profile;
    struct mp_led_profile daytime_profile;
    struct mp_led_profile bedtime_profile;
    struct mp_led_global_settings settings;
    int preview_scene = -1;
    int preview_active = 0;
    int preview_bypass_master = 0;
    int preview_raw_output = 0;
    int alarm_active;
    int display_mode;
    time_t message_until;
    int story_playing;
    int audio_playing;
    int bedtime_enabled;
    int bedtime_start_hour;
    int bedtime_start_min;
    int touch_ok;
    int touch_pressed;

    pthread_mutex_lock(&g_state.lock);
    if (g_state.led_preview_until > 0 && g_state.led_preview_until <= now) {
        g_state.led_preview_until = 0;
        g_state.led_preview_scene = -1;
        g_state.led_preview_bypass_master = 0;
        g_state.led_preview_raw_output = 0;
    }
    if (g_state.led_preview_until > now &&
        g_state.led_preview_scene >= 0 &&
        g_state.led_preview_scene < MP_LED_SCENE_COUNT) {
        preview_scene = g_state.led_preview_scene;
        preview_profile = g_state.led_preview_profile;
        preview_bypass_master = g_state.led_preview_bypass_master;
        preview_raw_output = g_state.led_preview_raw_output;
        preview_active = 1;
    }
    alarm_active = g_state.alarm_active;
    display_mode = g_state.display_mode;
    message_until = g_state.message_until;
    story_playing = g_state.story_playing;
    audio_playing = g_state.audio_playing;
    bedtime_enabled = g_state.bedtime_enabled;
    bedtime_start_hour = g_state.bedtime_start_hour;
    bedtime_start_min = g_state.bedtime_start_min;
    daytime_profile = g_state.led_profiles[MP_LED_SCENE_DAYTIME];
    bedtime_profile = g_state.led_profiles[MP_LED_SCENE_BEDTIME];
    settings = g_state.led_settings;
    g_state.led_bedtime_fade_active = 0;
    pthread_mutex_unlock(&g_state.lock);
    touch_get_state(&touch_ok, &touch_pressed);

    /* Touch feedback is immediate. Alarm still wins once the press is released. */
    if (touch_ok && touch_pressed && settings.touch_blink_enabled) {
        scene = MP_LED_SCENE_TOUCH;
        profile = (struct mp_led_profile)LED_PROFILE(
            MP_LED_EFFECT_FADE, settings.touch_blink_brightness, 2,
            settings.touch_blink_red, settings.touch_blink_green, settings.touch_blink_blue,
            0, 0, 0);
    } else if (alarm_active) {
        scene = MP_LED_SCENE_ALARM;
        pthread_mutex_lock(&g_state.lock);
        profile = g_state.led_profiles[scene];
        pthread_mutex_unlock(&g_state.lock);
    } else if (preview_active) {
        scene = (enum mp_led_scene)preview_scene;
        profile = preview_profile;
    } else {
        if (display_mode == 2 && message_until > now) scene = MP_LED_SCENE_MESSAGE;
        else if (story_playing) scene = MP_LED_SCENE_STORIES;
        else if (audio_playing) scene = MP_LED_SCENE_MUSIC;
        else if (is_bedtime_now()) scene = MP_LED_SCENE_BEDTIME;
        else scene = MP_LED_SCENE_DAYTIME;

        pthread_mutex_lock(&g_state.lock);
        profile = g_state.led_profiles[scene];
        pthread_mutex_unlock(&g_state.lock);

        if (scene == MP_LED_SCENE_DAYTIME && settings.idle_off) {
            profile.brightness = 0;
        } else if (scene == MP_LED_SCENE_DAYTIME && bedtime_enabled &&
                   settings.bedtime_fade_minutes > 0) {
            struct tm tmv;
            localtime_r(&now, &tmv);
            int current_minute = tmv.tm_hour * 60 + tmv.tm_min;
            int start_minute = bedtime_start_hour * 60 + bedtime_start_min;
            int until_start = (start_minute - current_minute + 1440) % 1440;
            int fade_minutes = settings.bedtime_fade_minutes;
            if (until_start > 0 && until_start <= fade_minutes) {
                int elapsed = fade_minutes - until_start;
                int blended = ((int)daytime_profile.brightness * (fade_minutes - elapsed) +
                               (int)bedtime_profile.brightness * elapsed + fade_minutes / 2) /
                              fade_minutes;
                profile.brightness = (uint8_t)clamp_int(blended, 0, 100);
                pthread_mutex_lock(&g_state.lock);
                g_state.led_bedtime_fade_active = 1;
                pthread_mutex_unlock(&g_state.lock);
            }
        }
    }

    sanitize_led_profile(&profile);
    sanitize_led_settings(&settings);
    if (preview_active && preview_bypass_master) settings.enabled = 1;
    if (preview_active && preview_raw_output) {
        settings.enabled = 1;
        settings.max_brightness = 100;
        settings.red_gain = 100;
        settings.green_gain = 100;
        settings.blue_gain = 100;
    }
    mp_led_set_global(&settings);
    mp_led_set(scene, &profile);
}

static void oled_clear_fb(uint8_t gray4) {
    gray4 &= 0x0F;
    memset(oled_draw_fb(), (gray4 << 4) | gray4, OLED_FB_BYTES);
}

static void oled_set_px(int x, int y, uint8_t gray4) {
    if (x < 0 || y < 0 || x >= OLED_W || y >= OLED_H) return;
    uint8_t *fb = oled_draw_fb();
    gray4 &= 0x0F;
    size_t idx = (size_t)(y * OLED_W + x) / 2;
    if ((x & 1) == 0) {
        fb[idx] = (fb[idx] & 0x0F) | (gray4 << 4);
    } else {
        fb[idx] = (fb[idx] & 0xF0) | gray4;
    }
}

static uint8_t oled_get_px(int x, int y) {
    if (x < 0 || y < 0 || x >= OLED_W || y >= OLED_H) return 0;
    const uint8_t *fb = oled_draw_fb();
    size_t idx = (size_t)(y * OLED_W + x) / 2;
    if ((x & 1) == 0) return (fb[idx] >> 4) & 0x0F;
    return fb[idx] & 0x0F;
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

static void oled_apply_brightness_to_buffer(uint8_t *buffer, size_t len, int brightness_percent) {
    if (!buffer || len == 0) return;
    brightness_percent = clamp_int(brightness_percent < 0 ? 100 : brightness_percent, 0, 100);
    if (brightness_percent >= 100) return;

    for (size_t i = 0; i < len; i++) {
        uint8_t hi = (buffer[i] >> 4) & 0x0F;
        uint8_t lo = buffer[i] & 0x0F;
        uint8_t original_hi = hi;
        uint8_t original_lo = lo;
        hi = (uint8_t)((hi * brightness_percent) / 100);
        lo = (uint8_t)((lo * brightness_percent) / 100);
        /* Any non-zero slider value remains visible at the panel's minimum
           grayscale step instead of behaving like 0%. */
        if (brightness_percent > 0 && original_hi > 0 && hi == 0) hi = 1;
        if (brightness_percent > 0 && original_lo > 0 && lo == 0) lo = 1;
        if (hi > 15) hi = 15;
        if (lo > 15) lo = 15;
        buffer[i] = (uint8_t)((hi << 4) | lo);
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
    oled_apply_brightness_to_buffer(tmp, tmp_len, brightness_percent);

    int col_start = 0x1C + (byte_start / 2);
    int col_end = 0x1C + (byte_end / 2);
    int rc = 0;

    if (oled_cmd2(0x15, (uint8_t)col_start, (uint8_t)col_end) != 0) rc = -1;
    else if (oled_cmd2(0x75, (uint8_t)row_start, (uint8_t)row_end) != 0) rc = -1;
    else if (oled_cmd(0x5C) != 0) rc = -1;
    else if (oled_data(tmp, tmp_len) != 0) rc = -1;

    if (rc == 0) {
        pthread_mutex_lock(&g_oled.preview_lock);
        for (int y = 0; y < height; y++) {
            memcpy(g_oled.preview_fb + ((size_t)(row_start + y) * OLED_ROW_BYTES) + byte_start,
                   tmp + ((size_t)y * (size_t)width_bytes),
                   (size_t)width_bytes);
        }
        pthread_mutex_unlock(&g_oled.preview_lock);
    }

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


static uint32_t utf8_next_codepoint(const unsigned char **cursor);

static const uint8_t font5x7_unknown[7] = {0x1F,0x11,0x01,0x02,0x04,0x00,0x04};
static const uint8_t font5x7_a_umlaut[7] = {0x0A,0x00,0x0E,0x11,0x1F,0x11,0x11};
static const uint8_t font5x7_e_umlaut[7] = {0x0A,0x00,0x1F,0x10,0x1E,0x10,0x1F};
static const uint8_t font5x7_i_umlaut[7] = {0x0A,0x00,0x0E,0x04,0x04,0x04,0x0E};
static const uint8_t font5x7_o_umlaut[7] = {0x0A,0x00,0x0E,0x11,0x11,0x11,0x0E};
static const uint8_t font5x7_u_umlaut[7] = {0x0A,0x00,0x11,0x11,0x11,0x11,0x0E};
static const uint8_t font5x7_y_umlaut[7] = {0x0A,0x00,0x11,0x0A,0x04,0x04,0x04};

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

static const uint8_t *font5x7_glyph(uint32_t cp) {
    switch (cp) {
        case 0x00c4: case 0x00e4: return font5x7_a_umlaut;
        case 0x00cb: case 0x00eb: return font5x7_e_umlaut;
        case 0x00cf: case 0x00ef: return font5x7_i_umlaut;
        case 0x00d6: case 0x00f6: return font5x7_o_umlaut;
        case 0x00dc: case 0x00fc: return font5x7_u_umlaut;
        case 0x0178: case 0x00ff: return font5x7_y_umlaut;
        default: break;
    }

    unsigned char idx = 0;
    if (cp < 0x80) {
        idx = (unsigned char)cp;
    } else {
        switch (cp) {
            case 0x00c0: case 0x00c1: case 0x00c2: case 0x00c3: case 0x00c5:
            case 0x00e0: case 0x00e1: case 0x00e2: case 0x00e3: case 0x00e5:
            case 0x0100: case 0x0101: case 0x0102: case 0x0103: case 0x0104: case 0x0105:
            case 0x00c6: case 0x00e6:
                idx = 'A'; break;
            case 0x00c7: case 0x00e7: case 0x0106: case 0x0107: case 0x010c: case 0x010d:
                idx = 'C'; break;
            case 0x00d0: case 0x00f0: case 0x010e: case 0x010f:
                idx = 'D'; break;
            case 0x00c8: case 0x00c9: case 0x00ca:
            case 0x00e8: case 0x00e9: case 0x00ea:
            case 0x0112: case 0x0113: case 0x0116: case 0x0117: case 0x0118: case 0x0119:
                idx = 'E'; break;
            case 0x011e: case 0x011f:
                idx = 'G'; break;
            case 0x00cc: case 0x00cd: case 0x00ce:
            case 0x00ec: case 0x00ed: case 0x00ee:
            case 0x012a: case 0x012b: case 0x012e: case 0x012f:
                idx = 'I'; break;
            case 0x0139: case 0x013a: case 0x013d: case 0x013e: case 0x0141: case 0x0142:
                idx = 'L'; break;
            case 0x00d1: case 0x00f1: case 0x0143: case 0x0144: case 0x0147: case 0x0148:
                idx = 'N'; break;
            case 0x00d2: case 0x00d3: case 0x00d4: case 0x00d5: case 0x00d8:
            case 0x00f2: case 0x00f3: case 0x00f4: case 0x00f5: case 0x00f8:
            case 0x014c: case 0x014d: case 0x0150: case 0x0151: case 0x0152: case 0x0153:
                idx = 'O'; break;
            case 0x0154: case 0x0155: case 0x0158: case 0x0159:
                idx = 'R'; break;
            case 0x015a: case 0x015b: case 0x0160: case 0x0161: case 0x00df:
                idx = 'S'; break;
            case 0x0164: case 0x0165: case 0x00de: case 0x00fe:
                idx = 'T'; break;
            case 0x00d9: case 0x00da: case 0x00db:
            case 0x00f9: case 0x00fa: case 0x00fb:
            case 0x016a: case 0x016b: case 0x016e: case 0x016f: case 0x0170: case 0x0171:
                idx = 'U'; break;
            case 0x00dd: case 0x00fd:
                idx = 'Y'; break;
            case 0x0179: case 0x017a: case 0x017b: case 0x017c: case 0x017d: case 0x017e:
                idx = 'Z'; break;
            case 0x00a0:
                idx = ' '; break;
            case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212:
                idx = '-'; break;
            case 0x2018: case 0x2019: case 0x201a: case 0x201b:
            case 0x201c: case 0x201d: case 0x201e: case 0x201f:
                idx = '\''; break;
            default:
                return font5x7_unknown;
        }
    }

    if (idx >= 'a' && idx <= 'z') idx = (unsigned char)(idx - 'a' + 'A');
    if (idx < 128 && font5x7_known[idx]) return font5x7_table[idx];
    return font5x7_unknown;
}

static int text5x7_width(const char *text, int scale) {
    if (!text || !*text) return 0;
    if (scale < 1) scale = 1;
    int n = 0;
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor) {
        uint32_t cp = utf8_next_codepoint(&cursor);
        if (cp >= 0x0300 && cp <= 0x036f) continue;
        n++;
    }
    return n > 0 ? n * 5 * scale + (n - 1) * scale : 0;
}

static void draw_char5x7(int x, int y, int scale, uint32_t cp, uint8_t c) {
    const uint8_t *g = font5x7_glyph(cp);
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
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor) {
        uint32_t cp = utf8_next_codepoint(&cursor);
        if (cp >= 0x0300 && cp <= 0x036f) continue;
        draw_char5x7(x, y, scale, cp, c);
        x += 6 * scale;
    }
}


static uint64_t monotonic_millis(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t utf8_next_codepoint(const unsigned char **cursor) {
    const unsigned char *p = cursor ? *cursor : NULL;
    if (!p || !*p) return 0;

    uint32_t cp;
    size_t count;
    if (p[0] < 0x80) {
        cp = p[0];
        count = 1;
    } else if ((p[0] & 0xe0) == 0xc0 && p[1]) {
        cp = (uint32_t)(p[0] & 0x1f);
        count = 2;
    } else if ((p[0] & 0xf0) == 0xe0 && p[1] && p[2]) {
        cp = (uint32_t)(p[0] & 0x0f);
        count = 3;
    } else if ((p[0] & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) {
        cp = (uint32_t)(p[0] & 0x07);
        count = 4;
    } else {
        *cursor = p + 1;
        return 0xfffd;
    }

    for (size_t i = 1; i < count; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            *cursor = p + 1;
            return 0xfffd;
        }
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3f);
    }

    if ((count == 2 && cp < 0x80) ||
        (count == 3 && cp < 0x800) ||
        (count == 4 && cp < 0x10000) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        *cursor = p + 1;
        return 0xfffd;
    }

    *cursor = p + count;
    return cp;
}

static void draw_char5x7_clipped(int x, int y, uint32_t cp, uint8_t c, int clip_x0, int clip_x1) {
    const uint8_t *glyph = font5x7_glyph(cp);
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
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor) {
        uint32_t cp = utf8_next_codepoint(&cursor);
        if (cp >= 0x0300 && cp <= 0x036f) continue;
        if (x >= clip_x1) break;
        if (x + 5 > clip_x0) draw_char5x7_clipped(x, y, cp, c, clip_x0, clip_x1);
        x += 6;
    }
}

static int song_marquee_offset(int cycle_width, uint64_t started_ms) {
    if (cycle_width <= 0) return 0;
    uint64_t now_ms = monotonic_millis();
    uint64_t elapsed = now_ms >= started_ms ? now_ms - started_ms : 0;
    uint64_t pixels = (elapsed * SONG_SCROLL_SPEED_PX_PER_SEC) / 1000u;
    return (int)(pixels % (uint64_t)cycle_width);
}

static void draw_song_metadata_line(const char *text, uint64_t started_ms) {
    if (!text || !*text) return;

    int width = text5x7_width(text, 1);
    int clip_x1 = SONG_METADATA_X + SONG_METADATA_W;

    /* Keep metadata still and centred when the full Title - Artist fits. */
    if (width <= SONG_METADATA_W) {
        int x = SONG_METADATA_X + (SONG_METADATA_W - width) / 2;
        draw_text5x7_clipped(x, OLED_H - 8, text, 11,
                             SONG_METADATA_X, clip_x1);
        return;
    }

    int cycle_width = SONG_METADATA_W + width + SONG_SCROLL_GAP_PX;
    int x = clip_x1 - song_marquee_offset(cycle_width, started_ms);

    /* Overflowing metadata enters from the right, moves left, leaves fully,
       then repeats after the configured blank gap. */
    draw_text5x7_clipped(x, OLED_H - 8, text, 11,
                         SONG_METADATA_X, clip_x1);
    x += cycle_width;
    draw_text5x7_clipped(x, OLED_H - 8, text, 11,
                         SONG_METADATA_X, clip_x1);
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
       space between the image and the time. Instead, keep each displayed digit
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
                                       int audio_playing, int show_song_metadata, int story_playing,
                                       const char *audio_display, uint64_t scroll_started_ms) {
    if (audio_playing && show_song_metadata && audio_display && audio_display[0])
        draw_song_metadata_line(audio_display, scroll_started_ms);
    else
        draw_long_date_centered_at(tmv, NULL, center_x);

    /* Story playback returns to the normal clock after the intro/title sequence,
       but its small status pills stay hidden until the story ends. */
    if (!story_playing)
        draw_status_pills(alarm_on, alarm_active, alarm_volume_percent);
}

static int song_metadata_marquee_active(void) {
    int active = 0;
    uint64_t now_ms = monotonic_millis();
    pthread_mutex_lock(&g_state.lock);
    int show = g_state.show_song_metadata;
    if (g_state.story_playing) show = now_ms < g_state.story_title_until_ms;
    if (g_state.audio_playing && show && g_state.audio_display[0])
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
    int story_playing;
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
    story_playing = g_state.story_playing;
    if (story_playing)
        show_song_metadata = monotonic_millis() < g_state.story_title_until_ms;
    scroll_started_ms = g_state.audio_scroll_started_ms;
    safe_str(audio_display, sizeof(audio_display), g_state.audio_display);
    pthread_mutex_unlock(&g_state.lock);

    oled_fill_rect(0, OLED_H - CLOCK_STATUS_PILL_H, OLED_W, CLOCK_STATUS_PILL_H, 0);
    int clock_center_x = (CLOCK_SIDE_WIDGET_X + CLOCK_SIDE_WIDGET_SIZE + OLED_W) / 2;
    draw_clock_footer_contents(&tmv, clock_center_x, alarm_on, alarm_active,
                               alarm_volume_percent, audio_playing, show_song_metadata,
                               story_playing, audio_display, scroll_started_ms);
    (void)oled_flush_region_bytes(0, OLED_ROW_BYTES - 1,
                                  OLED_H - CLOCK_STATUS_PILL_H, OLED_H - 1);
}

static int image_raw_pixel(const uint8_t *raw, int x, int y) {
    if (!raw || x < 0 || y < 0 || x >= MP_IMAGE_WIDTH || y >= MP_IMAGE_HEIGHT) return 0;
    uint8_t b = raw[(y * MP_IMAGE_WIDTH + x) / 2];
    return (x & 1) ? (b & 0x0F) : ((b >> 4) & 0x0F);
}

static int load_image_raw_uncached_by_file(const char *file, int bedtime, uint8_t *raw, size_t raw_len) {
    if (!raw || raw_len < MP_IMAGE_RAW_BYTES) return -1;
    char path[512];
    if (make_image_path_by_file(file, bedtime, path, sizeof(path)) != 0) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(raw, 1, MP_IMAGE_RAW_BYTES, f);
    fclose(f);
    return n == MP_IMAGE_RAW_BYTES ? 0 : -1;
}

static int load_image_raw_cached_by_file(const char *file, int bedtime, uint8_t *raw, size_t raw_len) {
    if (!raw || raw_len < MP_IMAGE_RAW_BYTES || !safe_image_filename(file)) return -1;

    pthread_mutex_lock(&g_image_raw_cache_lock);
    unsigned long generation = g_image_raw_cache.generation;
    if (g_image_raw_cache.valid && g_image_raw_cache.bedtime == bedtime && strcmp(g_image_raw_cache.file, file) == 0) {
        memcpy(raw, g_image_raw_cache.raw, MP_IMAGE_RAW_BYTES);
        pthread_mutex_unlock(&g_image_raw_cache_lock);
        return 0;
    }
    pthread_mutex_unlock(&g_image_raw_cache_lock);

    uint8_t tmp[MP_IMAGE_RAW_BYTES];
    if (load_image_raw_uncached_by_file(file, bedtime, tmp, sizeof(tmp)) != 0) return -1;

    pthread_mutex_lock(&g_image_raw_cache_lock);
    if (g_image_raw_cache.generation == generation) {
        safe_str(g_image_raw_cache.file, sizeof(g_image_raw_cache.file), file);
        g_image_raw_cache.bedtime = bedtime ? 1 : 0;
        memcpy(g_image_raw_cache.raw, tmp, MP_IMAGE_RAW_BYTES);
        g_image_raw_cache.valid = 1;
    }
    pthread_mutex_unlock(&g_image_raw_cache_lock);

    memcpy(raw, tmp, MP_IMAGE_RAW_BYTES);
    return 0;
}

static int collect_image_files(int bedtime, char files[][ASSET_LIST_NAME_MAX], int max_files) {
    return scan_asset_files(image_dir(bedtime), ASSET_LIST_IMAGE_RAW, files, max_files);
}

static int random_image_file(int bedtime, char *out, size_t out_len) {
    char files[ASSET_LIST_MAX_FILES][ASSET_LIST_NAME_MAX];
    int count = collect_image_files(bedtime, files, ASSET_LIST_MAX_FILES);
    if (count <= 0) return -1;
    safe_str(out, out_len, files[rand() % count]);
    return 0;
}

static int sticky_image_file(int bedtime, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';

    time_t now = time(NULL);
    struct rotating_image_state *state = &g_rotating_images[bedtime ? 1 : 0];

    if (state->file[0] && now < state->next_change) {
        uint8_t test[MP_IMAGE_RAW_BYTES];
        if (load_image_raw_cached_by_file(state->file, bedtime, test, sizeof(test)) == 0) {
            safe_str(out, out_len, state->file);
            return 0;
        }
    }

    if (random_image_file(bedtime, state->file, sizeof(state->file)) == 0) {
        state->next_change = now + (bedtime
            ? BEDTIME_IMAGE_CHANGE_SECONDS
            : rand() % (CLOCK_IMAGE_CHANGE_MAX_SECONDS + 1));
        safe_str(out, out_len, state->file);
        return 0;
    }

    state->file[0] = '\0';
    state->next_change = now + 60;
    return -1;
}

static int clock_image_refresh_due(void) {
    int bedtime = is_bedtime_now();
    return time(NULL) >= g_rotating_images[bedtime ? 1 : 0].next_change;
}

static int draw_image_thumb_raw(const uint8_t *raw, int ox, int oy, int size) {
    if (!raw || size <= 0) return -1;

    for (int y = 0; y < size; y++) {
        int sy = (y * MP_IMAGE_HEIGHT) / size;
        for (int x = 0; x < size; x++) {
            int sx = (x * MP_IMAGE_WIDTH) / size;
            oled_set_px(ox + x, oy + y, (uint8_t)image_raw_pixel(raw, sx, sy));
        }
    }

    return 0;
}

static int draw_image_thumb_by_file(const char *file, int bedtime, int ox, int oy, int size) {
    uint8_t raw[MP_IMAGE_RAW_BYTES];
    if (load_image_raw_cached_by_file(file, bedtime, raw, sizeof(raw)) != 0) return -1;
    return draw_image_thumb_raw(raw, ox, oy, size);
}

static void refresh_story_collage(void) {
    char files[ASSET_LIST_MAX_FILES][ASSET_LIST_NAME_MAX];
    int count = collect_image_files(1, files, ASSET_LIST_MAX_FILES);
    memset(&g_story_collage, 0, sizeof(g_story_collage));
    if (count <= 0) return;

    int indices[ASSET_LIST_MAX_FILES];
    for (int i = 0; i < count; i++) indices[i] = i;
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }

    g_story_collage.count = count < STORY_COLLAGE_MAX_IMAGES ? count : STORY_COLLAGE_MAX_IMAGES;
    for (int i = 0; i < g_story_collage.count; i++)
        safe_str(g_story_collage.files[i], sizeof(g_story_collage.files[i]), files[indices[i]]);
}

static int story_display_phase(void) {
    uint64_t now_ms = monotonic_millis();
    int phase = 0;
    pthread_mutex_lock(&g_state.lock);
    /* Keep the Story Mode intro visible for its full window even if the
       audio thread exits very quickly during startup. Without this, a failed
       or fast audio startup can make the OLED/browser preview look like it
       only flashed instead of showing the configured intro message. */
    if (now_ms < g_state.story_intro_until_ms) phase = 1;
    else if (g_state.story_playing && now_ms < g_state.story_title_until_ms) phase = 2;
    pthread_mutex_unlock(&g_state.lock);
    return phase;
}

static int story_touch_input_locked(uint64_t now_ms) {
    int locked;
    pthread_mutex_lock(&g_state.lock);
    locked = g_state.story_playing && now_ms < g_state.story_title_until_ms;
    pthread_mutex_unlock(&g_state.lock);
    return locked;
}

static void draw_story_mode_screen(const char *message) {
    oled_clear_fb(0);

    int count = g_story_collage.count;
    if (count > 0) {
        int gap = 3;
        int total_w = count * STORY_COLLAGE_IMAGE_SIZE + (count - 1) * gap;
        int x = (OLED_W - total_w) / 2;
        int y = 2;
        for (int i = 0; i < count; i++) {
            (void)draw_image_thumb_by_file(g_story_collage.files[i], 1, x, y,
                                           STORY_COLLAGE_IMAGE_SIZE);
            x += STORY_COLLAGE_IMAGE_SIZE + gap;
        }
    } else {
        const char *missing = "NO BEDTIME IMAGES";
        int missing_w = text5x7_width(missing, 1);
        draw_text5x7((OLED_W - missing_w) / 2, 25, 1, missing, 8);
    }

    oled_fill_rect(0, OLED_H - CLOCK_STATUS_PILL_H, OLED_W, CLOCK_STATUS_PILL_H, 0);
    char text[STORY_MESSAGE_MAX];
    safe_str(text, sizeof(text), message && *message ? message : "STORY MODE!");
    for (size_t i = 0; text[i]; i++) text[i] = (char)toupper((unsigned char)text[i]);
    int width = text5x7_width(text, 1);
    int x = (OLED_W - width) / 2;
    if (x < 0) x = 0;
    draw_text5x7(x, OLED_H - 8, 1, text, 12);
    oled_flush_full();
}

static void draw_clock_screen(void) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    int raw_hour = tmv.tm_hour;
    int minute = tmv.tm_min;
    int clock_24h_mode = 0;
    int hour;

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
    int story_playing = g_state.story_playing;
    uint64_t story_intro_until_ms = g_state.story_intro_until_ms;
    uint64_t story_title_until_ms = g_state.story_title_until_ms;
    char story_message[STORY_MESSAGE_MAX];
    safe_str(story_message, sizeof(story_message), g_state.story_message);
    uint64_t audio_scroll_started_ms = g_state.audio_scroll_started_ms;
    char audio_display[SONG_METADATA_TEXT_MAX];
    safe_str(audio_display, sizeof(audio_display), g_state.audio_display);
    clock_24h_mode = g_state.clock_24h_mode;
    char oled_font_file[128];
    int oled_font_size = g_state.oled_font_size;
    int clock_font_size = clamp_int(oled_font_size, 18, 54);
    safe_str(oled_font_file, sizeof(oled_font_file), g_state.oled_font_file);
    pthread_mutex_unlock(&g_state.lock);

    uint64_t display_now_ms = monotonic_millis();
    if (display_now_ms < story_intro_until_ms) {
        draw_story_mode_screen(story_message);
        return;
    }
    if (story_playing)
        show_song_metadata = display_now_ms < story_title_until_ms;

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
    int image_x = CLOCK_SIDE_WIDGET_X;
    /* Keep the image visually centered in the usable area above the bottom status pills.
       The small negative nudge compensates for most image art having more visual weight
       in the lower half. */
    int image_area_h = OLED_H - CLOCK_STATUS_PILL_H;
    int image_y = ((image_area_h - CLOCK_SIDE_WIDGET_SIZE) / 2) + CLOCK_IMAGE_Y_NUDGE;
    int image_right = image_x + CLOCK_SIDE_WIDGET_SIZE;
    int clock_center_x = (image_right + OLED_W) / 2;
    char random_image_file[IMAGE_FILE_MAX];
    if (bedtime) {
        if (sticky_image_file(1, random_image_file, sizeof(random_image_file)) == 0) {
            draw_image_thumb_by_file(random_image_file, 1, image_x, image_y, CLOCK_SIDE_WIDGET_SIZE);
        }
    } else {
        if (sticky_image_file(0, random_image_file, sizeof(random_image_file)) == 0) {
            draw_image_thumb_by_file(random_image_file, 0, image_x, image_y, CLOCK_SIDE_WIDGET_SIZE);
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
                               story_playing, audio_display, audio_scroll_started_ms);

    /*
       The clock screen contains a 54x54 image beside the time, a blinking colon, bottom-left Wi-Fi/alarm status group. On SSD1322 modules,
       small partial updates around packed 4-bit graphics can occasionally leave
       edge noise, so a full flush is the cleaner and safer choice.
    */
oled_flush_full();
}


static void sanitize_message_text(char *s, size_t s_len) {
    if (!s || s_len == 0) return;

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
    safe_str(s, s_len, out);
}

struct ttf_line_metrics {
    int min_x;
    int width;
    int ascent;
    int descent;
};

static int measure_ttf_line_locked(FT_Face face, const char *text, struct ttf_line_metrics *metrics) {
    if (!face || !text || !*text || !metrics) return -1;

    int pen_x = 0;
    int min_x = 99999;
    int max_x = -99999;
    int ascent = 0;
    int descent = 0;

    for (const unsigned char *ch = (const unsigned char *)text; *ch; ch++) {
        if (FT_Load_Char(face, *ch, FT_LOAD_RENDER) != 0) continue;
        FT_GlyphSlot glyph = face->glyph;
        int gx0 = (pen_x >> 6) + glyph->bitmap_left;
        int gx1 = gx0 + (int)glyph->bitmap.width;
        int bottom = (int)glyph->bitmap.rows - glyph->bitmap_top;
        if (gx0 < min_x) min_x = gx0;
        if (gx1 > max_x) max_x = gx1;
        if (glyph->bitmap_top > ascent) ascent = glyph->bitmap_top;
        if (bottom > descent) descent = bottom;
        pen_x += (int)glyph->advance.x;
    }

    if (min_x == 99999) return -1;
    metrics->min_x = min_x;
    metrics->width = max_x > min_x ? max_x - min_x : (pen_x >> 6);
    metrics->ascent = ascent;
    metrics->descent = descent > 0 ? descent : 0;
    return 0;
}

static int with_ttf_line_metrics(const char *font_file, int px_size, const char *text,
                                 struct ttf_line_metrics *metrics) {
    if (!font_file || !*font_file || !text || !*text || !metrics) return -1;

    char font_path[512];
    make_font_path(font_file, font_path, sizeof(font_path));

    pthread_mutex_lock(&g_font.lock);
    int rc = font_cache_ensure_locked(font_file, font_path, px_size) == 0 && g_font.face
        ? measure_ttf_line_locked(g_font.face, text, metrics)
        : -1;
    pthread_mutex_unlock(&g_font.lock);
    return rc;
}

static int draw_truetype_line_cached_baseline(const char *font_file, int px_size,
                                              const char *text, int min_x,
                                              int x, int baseline_y) {
    if (!font_file || !*font_file || !text || !*text) return -1;

    char font_path[512];
    make_font_path(font_file, font_path, sizeof(font_path));

    pthread_mutex_lock(&g_font.lock);
    if (font_cache_ensure_locked(font_file, font_path, px_size) != 0 || !g_font.face) {
        pthread_mutex_unlock(&g_font.lock);
        return -1;
    }

    FT_Face face = g_font.face;
    int origin_x = x - min_x;
    int pen_x = 0;
    int drew = 0;
    for (const unsigned char *ch = (const unsigned char *)text; *ch; ch++) {
        if (FT_Load_Char(face, *ch, FT_LOAD_RENDER) != 0) continue;
        FT_GlyphSlot glyph = face->glyph;
        drew = 1;
        FT_Bitmap *bitmap = &glyph->bitmap;
        int gx = origin_x + (pen_x >> 6) + glyph->bitmap_left;
        int gy = baseline_y - glyph->bitmap_top;

        for (unsigned int row = 0; row < bitmap->rows; row++) {
            for (unsigned int col = 0; col < bitmap->width; col++) {
                uint8_t coverage = 0;
                if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
                    coverage = bitmap->buffer[row * bitmap->pitch + col];
                } else if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
                    uint8_t byte = bitmap->buffer[row * bitmap->pitch + (col >> 3)];
                    coverage = (byte & (0x80 >> (col & 7))) ? 255 : 0;
                }
                oled_blend_px(gx + (int)col, gy + (int)row, coverage, 15);
            }
        }
        pen_x += (int)glyph->advance.x;
    }

    pthread_mutex_unlock(&g_font.lock);
    return drew ? 0 : -1;
}

static int ttf_line_width_cached(const char *font_file, int px_size, const char *text) {
    struct ttf_line_metrics metrics;
    return with_ttf_line_metrics(font_file, px_size, text, &metrics) == 0 ? metrics.width : 0;
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

static int render_message_screen_file(const char *image_file, int image_bedtime, const char *message, int flush_to_oled) {
    if (!safe_image_filename(image_file)) image_file = "";
    if (!message) message = "";

    pthread_mutex_lock(&g_state.lock);
    char font_file[128];
    int font_size = g_state.oled_font_size;
    safe_str(font_file, sizeof(font_file), g_state.oled_font_file);
    pthread_mutex_unlock(&g_state.lock);

    oled_clear_fb(0);

    if (!message[0]) {
        if (!image_file[0] ||
            draw_image_thumb_by_file(image_file, image_bedtime ? 1 : 0, 0, 0, 64) != 0) {
            draw_text5x7(8, 27, 1, "NO IMAGE", 8);
        }
        if (flush_to_oled) oled_flush_full();
        return 0;
    }

    /* Full 64x64 image at left; text uses x=70..251, leaving a 6 px gap and 4 px right margin. */
    if (!image_file[0] || draw_image_thumb_by_file(image_file, image_bedtime ? 1 : 0, 0, 0, 64) != 0) {
        draw_text5x7(8, 27, 1, "NO IMAGE", 8);
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
    int n = wrap_message_lines(use_ttf ? font_file : "", ttf_px, message, text_w, lines, max_lines, scale);
    if (n <= 0) {
        safe_str(lines[0], sizeof(lines[0]), "Hello");
        n = 1;
    }

    struct ttf_line_metrics layout[MESSAGE_MAX_LINES];
    memset(layout, 0, sizeof(layout));
    const int line_gap = use_ttf ? 2 : 3;
    int total_h = line_gap * (n - 1);

    for (int i = 0; i < n; i++) {
        if (use_ttf && with_ttf_line_metrics(font_file, ttf_px, lines[i], &layout[i]) == 0) {
            if (layout[i].ascent <= 0) layout[i].ascent = ttf_px;
        } else {
            int fallback_scale = use_ttf ? 1 : scale;
            layout[i].width = text5x7_width(lines[i], fallback_scale);
            layout[i].ascent = 7 * fallback_scale;
            layout[i].descent = 0;
        }
        total_h += layout[i].ascent + layout[i].descent;
    }

    int line_y = (OLED_H - total_h) / 2;
    if (line_y < 0) line_y = 0;

    for (int i = 0; i < n; i++) {
        int line_x = text_x + (text_w - layout[i].width) / 2;
        if (line_x < text_x) line_x = text_x;

        if (use_ttf) {
            int baseline_y = line_y + layout[i].ascent;
            if (draw_truetype_line_cached_baseline(font_file, ttf_px, lines[i],
                                                   layout[i].min_x, line_x, baseline_y) != 0) {
                int fallback_w = text5x7_width(lines[i], 1);
                int fallback_x = text_x + (text_w - fallback_w) / 2;
                if (fallback_x < text_x) fallback_x = text_x;
                draw_text5x7(fallback_x, line_y, 1, lines[i], 15);
            }
        } else {
            draw_text5x7(line_x, line_y, scale, lines[i], 15);
        }

        line_y += layout[i].ascent + layout[i].descent + line_gap;
    }

    if (flush_to_oled) oled_flush_full();
    return 0;
}


static int draw_message_screen_file(const char *image_file, int image_bedtime, const char *message) {
    return render_message_screen_file(image_file, image_bedtime, message, 1);
}


/* ---------------- Audio ---------------- */

struct audio_play_request {
    char path[512];
    char file[MUSIC_FILE_MAX];
    int start_volume;
    int end_volume;
    int alarm_mode;
    int notification_mode;
};

static void oled_filter_metadata_text(const char *input, char *output, size_t output_len) {
    if (!output || output_len == 0) return;
    output[0] = '\0';
    if (!input || !*input) return;

    const unsigned char *cursor = (const unsigned char *)input;
    size_t used = 0;
    int previous_was_space = 1;

    while (*cursor) {
        const unsigned char *start = cursor;
        uint32_t cp = utf8_next_codepoint(&cursor);

        /* Combining marks have no standalone glyph and should not create gaps. */
        if (cp >= 0x0300 && cp <= 0x036f) continue;

        /* Unsupported code points would render as the fallback question mark. */
        if (font5x7_glyph(cp) == font5x7_unknown) continue;

        if (cp == ' ' || cp == 0x00a0) {
            if (previous_was_space) continue;
            if (used + 1 >= output_len) break;
            output[used++] = ' ';
            previous_was_space = 1;
            continue;
        }

        size_t bytes = (size_t)(cursor - start);
        if (bytes == 0 || used + bytes >= output_len) break;
        memcpy(output + used, start, bytes);
        used += bytes;
        previous_was_space = 0;
    }

    while (used > 0 && output[used - 1] == ' ') used--;
    output[used] = '\0';
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

    char oled_title[MP_ID3_TEXT_MAX];
    char oled_artist[MP_ID3_TEXT_MAX];
    oled_filter_metadata_text(metadata.title, oled_title, sizeof(oled_title));
    oled_filter_metadata_text(metadata.artist, oled_artist, sizeof(oled_artist));

    char combined[MP_ID3_TEXT_MAX * 2 + 4];
    if (oled_title[0] && oled_artist[0])
        snprintf(combined, sizeof(combined), "%s - %s", oled_title, oled_artist);
    else if (oled_title[0])
        safe_str(combined, sizeof(combined), oled_title);
    else if (oled_artist[0])
        safe_str(combined, sizeof(combined), oled_artist);
    else
        safe_str(combined, sizeof(combined), "NOW PLAYING");
    safe_str(display, display_len, combined);
}

static void clear_audio_metadata_locked(void) {
    g_state.audio_title[0] = '\0';
    g_state.audio_artist[0] = '\0';
    g_state.audio_display[0] = '\0';
    g_state.audio_scroll_started_ms = 0;
}

static int audio_is_alarm_session(void) {
    int alarm_mode;
    pthread_mutex_lock(&g_audio.lock);
    alarm_mode = g_audio.running && g_audio.alarm_mode;
    pthread_mutex_unlock(&g_audio.lock);
    return alarm_mode;
}

static int audio_should_stop(void) {
    int stop;
    uint64_t now_ms = monotonic_millis();
    pthread_mutex_lock(&g_audio.lock);
    if (!g_audio.stop_requested && g_audio.alarm_mode && g_audio.alarm_deadline_ms > 0 &&
        now_ms >= g_audio.alarm_deadline_ms) {
        g_audio.stop_requested = 1;
        g_audio.timed_out = 1;
    }
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

static int audio_file_playable(const char *path) {
    int err = MPG123_OK;
    mpg123_handle *mh = mpg123_new(NULL, &err);
    if (!mh) return 0;
    mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0.0);
    int ok = 0;
    if (mpg123_open(mh, path) == MPG123_OK) {
        long rate = 0;
        int channels = 0;
        int enc = 0;
        if (mpg123_getformat(mh, &rate, &channels, &enc) == MPG123_OK &&
            rate > 0 && channels >= 1 && channels <= 2) ok = 1;
        mpg123_close(mh);
    }
    mpg123_delete(mh);
    return ok;
}

static int choose_playable_library(const char *dir, const char *requested,
                                   char *file, size_t file_len,
                                   char *path, size_t path_len) {
    if (!dir || !*dir || !file || !path || file_len == 0 || path_len == 0) return -1;
    if (requested && *requested && safe_asset_filename(requested) && has_mp3_ext(requested)) {
        safe_str(file, file_len, requested);
        int n = snprintf(path, path_len, "%s/%s", dir, file);
        if (n > 0 && (size_t)n < path_len && access(path, R_OK) == 0 && audio_file_playable(path)) return 0;
    }

    char files[ASSET_LIST_MAX_FILES][ASSET_LIST_NAME_MAX];
    int count = scan_asset_files(dir, ASSET_LIST_MUSIC_MP3, files, ASSET_LIST_MAX_FILES);
    if (count <= 0) return -1;
    int first = rand() % count;
    for (int offset = 0; offset < count; offset++) {
        safe_str(file, file_len, files[(first + offset) % count]);
        int n = snprintf(path, path_len, "%s/%s", dir, file);
        if (n > 0 && (size_t)n < path_len && access(path, R_OK) == 0 && audio_file_playable(path)) return 0;
    }
    return -1;
}

static int choose_playable_uploaded_music(const char *requested, char *file, size_t file_len,
                                          char *path, size_t path_len) {
    return choose_playable_library(MUSIC_DIR, requested, file, file_len, path, path_len);
}

static int choose_playable_story(const char *requested, char *file, size_t file_len,
                                 char *path, size_t path_len) {
    return choose_playable_library(STORY_DIR, requested, file, file_len, path, path_len);
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

    if (req->alarm_mode) {
        if (mpg123_scan(mh) == MPG123_OK) {
            total_samples = mpg123_length(mh);
            (void)mpg123_seek(mh, 0, SEEK_SET);
        } else {
            total_samples = mpg123_length(mh);
        }
        if (total_samples > 0 && rate > 0) ramp_seconds = (double)total_samples / (double)rate;
        if (ramp_seconds < 1.0) ramp_seconds = 60.0;
        clock_gettime(CLOCK_MONOTONIC, &ramp_start_ts);
        alarm_volume_state_set(1, req->start_volume);
    }

    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) goto done;
    if (snd_pcm_set_params(pcm,
                           SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED,
                           (unsigned int)channels,
                           (unsigned int)rate,
                           1,
                           300000) < 0) goto done;

    const size_t buf_size = 8192;
    buf = malloc(buf_size);
    if (!buf) goto done;

    if (req->alarm_mode) {
        pthread_mutex_lock(&g_state.lock);
        g_state.last_successful_alarm = (long long)time(NULL);
        pthread_mutex_unlock(&g_state.lock);
        save_config();
        app_log("alarm", "Alarm audio started: %s; loops until touch or 30-minute limit", req->file);
    }

    while (!audio_should_stop()) {
        size_t done_bytes = 0;
        int r = mpg123_read(mh, buf, buf_size, &done_bytes);
        if (done_bytes > 0) {
            int volume = req->start_volume;
            if (req->alarm_mode && ramp_seconds > 0.0) {
                struct timespec now_ts;
                clock_gettime(CLOCK_MONOTONIC, &now_ts);
                double elapsed = (double)(now_ts.tv_sec - ramp_start_ts.tv_sec) +
                                 ((double)(now_ts.tv_nsec - ramp_start_ts.tv_nsec) / 1000000000.0);
                if (elapsed < 0.0) elapsed = 0.0;
                double t = elapsed / ramp_seconds;
                if (t > 1.0) t = 1.0;
                volume = req->start_volume +
                    (int)((double)(req->end_volume - req->start_volume) * t + 0.5);
                alarm_volume_state_set(1, volume);
            }
            audio_scale_s16(buf, done_bytes, volume);
            if (audio_write_pcm(pcm, buf, done_bytes, channels) != 0) break;
        }
        if (r == MPG123_DONE) {
            if (req->alarm_mode && !audio_should_stop() && mpg123_seek(mh, 0, SEEK_SET) >= 0) {
                continue;
            }
            break;
        }
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

    int timed_out = 0;
    pthread_mutex_lock(&g_audio.lock);
    timed_out = g_audio.timed_out;
    pthread_mutex_unlock(&g_audio.lock);

    if (req->alarm_mode) {
        alarm_volume_state_set(0, 0);
        if (timed_out) app_log("alarm", "Alarm stopped after the 30-minute safety limit");
    }

    if (!req->notification_mode) {
        uint64_t ended_ms = monotonic_millis();
        pthread_mutex_lock(&g_state.lock);
        int was_story = g_state.story_playing;
        uint64_t story_intro_until_ms = g_state.story_intro_until_ms;
        g_state.audio_playing = 0;
        g_state.audio_file[0] = '\0';
        g_state.story_playing = 0;
        if (was_story && ended_ms < story_intro_until_ms) {
            /* Preserve the startup splash until its scheduled end. This prevents
               a start-fail or immediate thread exit from wiping the Story Mode
               text before the OLED loop has held it onscreen. */
            g_state.story_title_until_ms = story_intro_until_ms;
        } else {
            g_state.story_intro_until_ms = 0;
            g_state.story_title_until_ms = 0;
        }
        clear_audio_metadata_locked();
        g_state.display_dirty = 1;
        pthread_mutex_unlock(&g_state.lock);
    }

    pthread_mutex_lock(&g_audio.lock);
    g_audio.running = 0;
    g_audio.stop_requested = 0;
    g_audio.alarm_mode = 0;
    g_audio.notification_mode = 0;
    g_audio.timed_out = 0;
    g_audio.alarm_deadline_ms = 0;
    g_audio.file[0] = '\0';
    pthread_cond_broadcast(&g_audio.stopped);
    pthread_mutex_unlock(&g_audio.lock);

    free(req);
    return NULL;
}

static void audio_clear_visible_state(void) {
    alarm_volume_state_set(0, 0);
    pthread_mutex_lock(&g_state.lock);
    g_state.audio_playing = 0;
    g_state.audio_file[0] = '\0';
    g_state.story_playing = 0;
    g_state.story_intro_until_ms = 0;
    g_state.story_title_until_ms = 0;
    clear_audio_metadata_locked();
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);
}

static int audio_request_stop_internal(int allow_alarm) {
    int requested = 0;
    int notification_mode = 0;
    pthread_mutex_lock(&g_audio.lock);
    if (g_audio.running && (!g_audio.alarm_mode || allow_alarm)) {
        g_audio.stop_requested = 1;
        notification_mode = g_audio.notification_mode;
        requested = 1;
    }
    pthread_mutex_unlock(&g_audio.lock);
    if (requested && !notification_mode) audio_clear_visible_state();
    return requested;
}

static int audio_request_stop(void) {
    return audio_request_stop_internal(0);
}

static int audio_request_stop_from_touch(void) {
    return audio_request_stop_internal(1);
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

static void audio_stop(void) {
    (void)audio_request_stop();
}

static void audio_force_stop(void) {
    (void)audio_request_stop_internal(1);
}

static int audio_stop_and_wait(unsigned int timeout_ms) {
    if (audio_is_alarm_session()) return -1;
    (void)audio_request_stop();
    return audio_wait_stopped(timeout_ms);
}

static int audio_force_stop_and_wait(unsigned int timeout_ms) {
    audio_force_stop();
    return audio_wait_stopped(timeout_ms);
}

static int audio_start_resolved_file(const char *path, const char *safe_file,
                                     int start_volume, int end_volume, int alarm_mode,
                                     int story_mode, int notification_mode) {
    char song_title[MP_ID3_TEXT_MAX];
    char song_artist[MP_ID3_TEXT_MAX];
    char song_display[SONG_METADATA_TEXT_MAX];

    song_metadata_for_file(path, safe_file,
                           song_title, sizeof(song_title),
                           song_artist, sizeof(song_artist),
                           song_display, sizeof(song_display));

    start_volume = clamp_int(start_volume, 0, 100);
    end_volume = clamp_int(end_volume, 0, 100);
    if (!notification_mode && audio_stop_and_wait(3000u) != 0) return -3;

    struct audio_play_request *req = calloc(1, sizeof(*req));
    if (!req) return -1;
    safe_str(req->path, sizeof(req->path), path);
    safe_str(req->file, sizeof(req->file), safe_file);
    req->start_volume = start_volume;
    req->end_volume = end_volume;
    req->alarm_mode = alarm_mode;
    req->notification_mode = notification_mode ? 1 : 0;

    pthread_mutex_lock(&g_audio.lock);
    if (notification_mode && g_audio.running) {
        pthread_mutex_unlock(&g_audio.lock);
        free(req);
        return -4;
    }
    g_audio.running = 1;
    g_audio.stop_requested = 0;
    g_audio.alarm_mode = alarm_mode;
    g_audio.notification_mode = notification_mode ? 1 : 0;
    g_audio.timed_out = 0;
    g_audio.alarm_deadline_ms = alarm_mode
        ? monotonic_millis() + (uint64_t)ALARM_MAX_DURATION_SECONDS * 1000u : 0;
    safe_str(g_audio.file, sizeof(g_audio.file), safe_file);
    pthread_mutex_unlock(&g_audio.lock);

    if (!notification_mode) {
        pthread_mutex_lock(&g_state.lock);
        uint64_t display_started_ms = monotonic_millis();
        g_state.audio_playing = 1;
        safe_str(g_state.audio_file, sizeof(g_state.audio_file), safe_file);
        safe_str(g_state.audio_title, sizeof(g_state.audio_title), song_title);
        safe_str(g_state.audio_artist, sizeof(g_state.audio_artist), song_artist);
        safe_str(g_state.audio_display, sizeof(g_state.audio_display), song_display);
        g_state.story_playing = story_mode ? 1 : 0;
        if (story_mode) {
            g_state.story_intro_until_ms = display_started_ms + STORY_INTRO_SECONDS * 1000u;
            g_state.story_title_until_ms = g_state.story_intro_until_ms + STORY_TITLE_SECONDS * 1000u;
            g_state.audio_scroll_started_ms = g_state.story_intro_until_ms;
        } else {
            g_state.story_intro_until_ms = 0;
            g_state.story_title_until_ms = 0;
            g_state.audio_scroll_started_ms = display_started_ms;
        }
        if (story_mode || g_state.show_song_metadata) g_state.display_mode = 0;
        g_state.display_dirty = 1;
        pthread_mutex_unlock(&g_state.lock);
    }
    if (alarm_mode) alarm_volume_state_set(1, start_volume);

    pthread_t tid;
    if (pthread_create(&tid, NULL, audio_thread_main, req) != 0) {
        pthread_mutex_lock(&g_audio.lock);
        g_audio.running = 0;
        g_audio.stop_requested = 0;
        g_audio.alarm_mode = 0;
        g_audio.notification_mode = 0;
        g_audio.timed_out = 0;
        g_audio.alarm_deadline_ms = 0;
        g_audio.file[0] = '\0';
        pthread_cond_broadcast(&g_audio.stopped);
        pthread_mutex_unlock(&g_audio.lock);
        if (!notification_mode) audio_clear_visible_state();
        free(req);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

static int audio_play_music_file(const char *music_file, int start_volume, int end_volume, int use_ramp) {
    char safe_file[MUSIC_FILE_MAX] = "";
    char path[512] = "";
    int alarm_mode = use_ramp ? 1 : 0;

    if (!alarm_mode) {
        int bedtime_music_enabled;
        pthread_mutex_lock(&g_state.lock);
        bedtime_music_enabled = g_state.bedtime_music_enabled;
        start_volume = end_volume = clamp_int(g_state.global_volume, 0, 100);
        pthread_mutex_unlock(&g_state.lock);
        if (is_bedtime_now() && !bedtime_music_enabled) {
            app_log("music", "Music request blocked during bedtime");
            return -2;
        }
    }

    if (choose_playable_uploaded_music(music_file, safe_file, sizeof(safe_file), path, sizeof(path)) != 0) {
        if (!alarm_mode || access(DEFAULT_ALARM_PATH, R_OK) != 0 || !audio_file_playable(DEFAULT_ALARM_PATH)) {
            app_log(alarm_mode ? "alarm" : "music", "No playable audio file is available");
            return -1;
        }
        safe_str(safe_file, sizeof(safe_file), DEFAULT_ALARM_LABEL);
        safe_str(path, sizeof(path), DEFAULT_ALARM_PATH);
        app_log("alarm", "Using protected built-in alarm sound");
    }

    return audio_start_resolved_file(path, safe_file, start_volume, end_volume, alarm_mode, 0, 0);
}

static int audio_play_story_file(const char *story_file) {
    char safe_file[MUSIC_FILE_MAX] = "";
    char path[512] = "";
    int enabled;
    int volume;

    pthread_mutex_lock(&g_state.lock);
    enabled = g_state.story_mode_enabled;
    volume = clamp_int(g_state.story_volume, 0, 100);
    pthread_mutex_unlock(&g_state.lock);
    if (!enabled) return -2;

    if (choose_playable_story(story_file, safe_file, sizeof(safe_file), path, sizeof(path)) != 0) {
        app_log("story", "No playable story is available");
        return -1;
    }
    refresh_story_collage();
    int result = audio_start_resolved_file(path, safe_file, volume, volume, 0, 1, 0);
    if (result == 0) app_log("story", "Story started: %s at %d%% volume", safe_file, volume);
    return result;
}

static int audio_play_message_chime(void) {
    if (access(MESSAGE_CHIME_PATH, R_OK) != 0 || !audio_file_playable(MESSAGE_CHIME_PATH))
        return -1;

    int volume;
    pthread_mutex_lock(&g_state.lock);
    volume = clamp_int(g_state.global_volume, 0, MESSAGE_CHIME_VOLUME_MAX);
    pthread_mutex_unlock(&g_state.lock);

    return audio_start_resolved_file(MESSAGE_CHIME_PATH, MESSAGE_CHIME_LABEL,
                                     volume, volume, 0, 0, 1);
}

struct oled_network_diagnostics {
    char interface_name[IFNAMSIZ];
    char ip_address[INET_ADDRSTRLEN];
    char ssid[IW_ESSID_MAX_SIZE + 1];
    char hostname[64];
    int signal_percent;
    int signal_dbm;
    int signal_available;
};

static void read_oled_network_diagnostics(struct oled_network_diagnostics *info) {
    if (!info) return;
    memset(info, 0, sizeof(*info));
    if (gethostname(info->hostname, sizeof(info->hostname) - 1) != 0)
        safe_str(info->hostname, sizeof(info->hostname), "Unavailable");

    FILE *wireless = fopen("/proc/net/wireless", "r");
    if (wireless) {
        char line[512];
        while (fgets(line, sizeof(line), wireless)) {
            char name[IFNAMSIZ] = "";
            double quality = 0.0;
            double level = 0.0;
            if (sscanf(line, " %15[^:]: %*d %lf %lf", name, &quality, &level) == 3) {
                safe_str(info->interface_name, sizeof(info->interface_name), name);
                int percent = (int)((quality / 70.0) * 100.0 + 0.5);
                info->signal_percent = clamp_int(percent, 0, 100);
                int dbm = (int)level;
                if (dbm > 100) dbm -= 256;
                info->signal_dbm = dbm;
                info->signal_available = 1;
                break;
            }
        }
        fclose(wireless);
    }

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;

    if (info->interface_name[0]) {
        struct iwreq request;
        char essid[IW_ESSID_MAX_SIZE + 1];
        memset(&request, 0, sizeof(request));
        memset(essid, 0, sizeof(essid));
        safe_str(request.ifr_name, sizeof(request.ifr_name), info->interface_name);
        request.u.essid.pointer = essid;
        request.u.essid.length = IW_ESSID_MAX_SIZE;
        if (ioctl(fd, SIOCGIWESSID, &request) == 0) {
            size_t length = request.u.essid.length;
            if (length > IW_ESSID_MAX_SIZE) length = IW_ESSID_MAX_SIZE;
            essid[length] = '\0';
            safe_str(info->ssid, sizeof(info->ssid), essid);
        }
    }

    struct ifreq interfaces[32];
    struct ifconf list;
    memset(&interfaces, 0, sizeof(interfaces));
    memset(&list, 0, sizeof(list));
    list.ifc_len = sizeof(interfaces);
    list.ifc_req = interfaces;
    if (ioctl(fd, SIOCGIFCONF, &list) == 0) {
        int best_score = -1;
        size_t count = (size_t)list.ifc_len / sizeof(struct ifreq);
        for (size_t i = 0; i < count; i++) {
            struct ifreq *candidate = &interfaces[i];
            if (candidate->ifr_addr.sa_family != AF_INET) continue;
            struct ifreq flags_request;
            memset(&flags_request, 0, sizeof(flags_request));
            safe_str(flags_request.ifr_name, sizeof(flags_request.ifr_name), candidate->ifr_name);
            if (ioctl(fd, SIOCGIFFLAGS, &flags_request) != 0) continue;
            if (!(flags_request.ifr_flags & IFF_UP) || (flags_request.ifr_flags & IFF_LOOPBACK)) continue;

            int score = 10;
            if (info->interface_name[0] && strcmp(candidate->ifr_name, info->interface_name) == 0) score = 100;
            else if (strncmp(candidate->ifr_name, "wl", 2) == 0) score = 70;
            else if (strncmp(candidate->ifr_name, "en", 2) == 0 ||
                     strncmp(candidate->ifr_name, "eth", 3) == 0) score = 50;
            if (score <= best_score) continue;

            char text[INET_ADDRSTRLEN] = "";
            struct sockaddr_in *address = (struct sockaddr_in *)&candidate->ifr_addr;
            if (!inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) continue;
            best_score = score;
            safe_str(info->ip_address, sizeof(info->ip_address), text);
            if (!info->interface_name[0])
                safe_str(info->interface_name, sizeof(info->interface_name), candidate->ifr_name);
        }
    }
    close(fd);
}

static void fit_oled_text(char *text, size_t text_len) {
    if (!text || text_len == 0) return;
    while (text[0] && text5x7_width(text, 1) > OLED_W - 4) {
        size_t length = strlen(text);
        if (length <= 3) break;
        text[length - 1] = '\0';
        length--;
        if (length >= 3) {
            text[length - 1] = '.';
            text[length - 2] = '.';
            text[length - 3] = '.';
        }
    }
}

static void draw_diagnostic_line(int y, const char *label, const char *value, uint8_t colour) {
    char line[96];
    snprintf(line, sizeof(line), "%s: %s", label, value && *value ? value : "Unavailable");
    fit_oled_text(line, sizeof(line));
    draw_text5x7(2, y, 1, line, colour);
}

static void draw_diagnostic_screen(void) {
    struct oled_network_diagnostics info;
    read_oled_network_diagnostics(&info);
    oled_clear_fb(0);
    draw_text5x7(2, 0, 1, "NETWORK DIAGNOSTICS", 15);
    draw_diagnostic_line(11, "WIFI", info.ssid, 12);
    char signal[48];
    if (info.signal_available)
        snprintf(signal, sizeof(signal), "%d%% %d dBm", info.signal_percent, info.signal_dbm);
    else
        safe_str(signal, sizeof(signal), "Unavailable");
    draw_diagnostic_line(22, "SIGNAL", signal, 12);
    draw_diagnostic_line(33, "IP", info.ip_address, 12);
    draw_diagnostic_line(44, "HOST", info.hostname, 12);
    draw_text5x7(2, 55, 1, "TAP TO CLOSE", 8);
    oled_flush_full();
}

static int diagnostic_screen_active(void) {
    int active;
    pthread_mutex_lock(&g_state.lock);
    active = g_state.display_mode == 3;
    pthread_mutex_unlock(&g_state.lock);
    return active;
}

static int open_diagnostic_screen(void) {
    int opened = 0;
    pthread_mutex_lock(&g_state.lock);
    if (!g_state.alarm_active && !g_state.audio_playing && !g_state.story_playing) {
        g_state.diagnostic_return_mode = g_state.display_mode;
        if (g_state.diagnostic_return_mode < 0 || g_state.diagnostic_return_mode > 2)
            g_state.diagnostic_return_mode = 0;
        g_state.display_mode = 3;
        g_state.diagnostic_until = time(NULL) + DIAGNOSTIC_SCREEN_SECONDS;
        g_state.display_dirty = 1;
        opened = 1;
    }
    pthread_mutex_unlock(&g_state.lock);
    if (opened) app_log("touch", "Eight-second hold opened OLED diagnostics");
    return opened;
}

static void close_diagnostic_screen(void) {
    int closed = 0;
    pthread_mutex_lock(&g_state.lock);
    if (g_state.display_mode == 3) {
        int return_mode = g_state.diagnostic_return_mode;
        if (return_mode == 2 && time(NULL) >= g_state.message_until) return_mode = 0;
        g_state.display_mode = return_mode;
        g_state.diagnostic_return_mode = 0;
        g_state.diagnostic_until = 0;
        g_state.display_dirty = 1;
        closed = 1;
    }
    pthread_mutex_unlock(&g_state.lock);
    if (closed) app_log("touch", "OLED diagnostics closed");
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
    int action_consumed = 0;
    int diagnostic_opened_this_press = 0;
    int diagnostic_exit_press = 0;
    int story_press_count = 0;
    uint64_t story_window_started_ms = 0;
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

        if (story_touch_input_locked(now_ms)) {
            if (current != stable && now_ms - raw_changed_ms >= TOUCH_DEBOUNCE_MS) {
                stable = current;
                touch_set_state(1, stable);
                update_led_scene();
            }

            /* Consume every press and release until the Story Mode intro and
               title have finished. A held sensor is also ignored on release. */
            action_consumed = 1;
            diagnostic_opened_this_press = 0;
            diagnostic_exit_press = 0;
            story_press_count = 0;
            story_window_started_ms = 0;
            pressed_ms = 0;
            continue;
        }

        if (current != stable && now_ms - raw_changed_ms >= TOUCH_DEBOUNCE_MS) {
            stable = current;
            touch_set_state(1, stable);
            update_led_scene();
            if (stable) {
                pressed_ms = now_ms;
                action_consumed = 0;
                diagnostic_opened_this_press = 0;
                diagnostic_exit_press = diagnostic_screen_active();
                if (audio_is_alarm_session()) {
                    action_consumed = 1;
                    if (audio_request_stop_from_touch())
                        app_log("touch", "Alarm dismissed with the touch sensor");
                }
            } else {
                uint64_t held_ms = pressed_ms > 0 && now_ms >= pressed_ms ? now_ms - pressed_ms : 0;
                if (diagnostic_exit_press) {
                    close_diagnostic_screen();
                    action_consumed = 1;
                    story_press_count = 0;
                    story_window_started_ms = 0;
                } else if (diagnostic_opened_this_press || action_consumed) {
                    story_press_count = 0;
                    story_window_started_ms = 0;
                } else if (held_ms >= TOUCH_LONG_PRESS_MS) {
                    story_press_count = 0;
                    story_window_started_ms = 0;
                    int play_result = audio_play_music_file("", 0, 0, 0);
                    if (play_result == 0)
                        app_log("touch", "Long press started random music");
                    else if (play_result == -2)
                        app_log("touch", "Long-press music is disabled during bedtime");
                } else if (audio_is_playing()) {
                    app_log("touch", "Short press requested audio stop");
                    (void)audio_request_stop_from_touch();
                    story_press_count = 0;
                    story_window_started_ms = 0;
                } else {
                    int story_enabled;
                    pthread_mutex_lock(&g_state.lock);
                    story_enabled = g_state.story_mode_enabled;
                    pthread_mutex_unlock(&g_state.lock);
                    if (!story_enabled) {
                        story_press_count = 0;
                        story_window_started_ms = 0;
                    } else {
                        if (story_press_count == 0 ||
                            now_ms - story_window_started_ms > STORY_PRESS_WINDOW_MS) {
                            story_press_count = 0;
                            story_window_started_ms = now_ms;
                        }
                        story_press_count++;
                        if (story_press_count >= STORY_PRESS_COUNT) {
                            story_press_count = 0;
                            story_window_started_ms = 0;
                            int play_result = audio_play_story_file("");
                            if (play_result == 0)
                                app_log("touch", "Ten short presses started a random story");
                            else if (play_result == -1)
                                app_log("touch", "Story gesture detected, but no playable story is available");
                        }
                    }
                }
                pressed_ms = 0;
                action_consumed = 0;
                diagnostic_opened_this_press = 0;
                diagnostic_exit_press = 0;
            }
        }

        if (stable && !action_consumed && !diagnostic_exit_press && pressed_ms > 0 &&
            now_ms - pressed_ms >= TOUCH_DIAGNOSTIC_PRESS_MS) {
            story_press_count = 0;
            story_window_started_ms = 0;
            if (open_diagnostic_screen()) {
                action_consumed = 1;
                diagnostic_opened_this_press = 1;
            }
        }
    }

    touch_set_state(0, 0);
    return NULL;
}

/* ---------------- Private binary IPC ---------------- */

static const char *oled_color_name_for_id(int id) {
    switch (id) {
        case MP_OLED_COLOR_YELLOW: return "yellow";
        case MP_OLED_COLOR_WHITE: return "white";
        case MP_OLED_COLOR_GREEN:
        default: return "green";
    }
}

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
        case 2: return "message";
        case 3: return "diagnostics";
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

static time_t next_alarm_time(const struct alarm_slot alarms[MAX_ALARMS], time_t now,
                              int *alarm_id) {
    time_t best = 0;
    int best_id = 0;
    struct tm base;
    localtime_r(&now, &base);
    for (int day = 0; day <= 7; day++) {
        struct tm day_tm = base;
        day_tm.tm_hour = 12;
        day_tm.tm_min = 0;
        day_tm.tm_sec = 0;
        day_tm.tm_mday += day;
        day_tm.tm_isdst = -1;
        time_t normalized = mktime(&day_tm);
        if (normalized == (time_t)-1) continue;
        localtime_r(&normalized, &day_tm);
        for (int i = 0; i < MAX_ALARMS; i++) {
            if (!alarms[i].enabled || !(alarms[i].weekdays & (1 << day_tm.tm_wday))) continue;
            struct tm candidate_tm = day_tm;
            candidate_tm.tm_hour = alarms[i].hour;
            candidate_tm.tm_min = alarms[i].min;
            candidate_tm.tm_sec = 0;
            candidate_tm.tm_isdst = -1;
            time_t candidate = mktime(&candidate_tm);
            if (candidate <= now) continue;
            if (!best || candidate < best) {
                best = candidate;
                best_id = i + 1;
            }
        }
    }
    if (alarm_id) *alarm_id = best_id;
    return best;
}

static void format_next_alarm(time_t value, int clock_24h_mode, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (value <= 0) {
        safe_str(out, out_len, "No alarm scheduled");
        return;
    }
    struct tm tmv;
    localtime_r(&value, &tmv);
    char day[32];
    char clock[32];
    strftime(day, sizeof(day), "%A", &tmv);
    strftime(clock, sizeof(clock), clock_24h_mode ? "%H:%M" : "%I:%M %p", &tmv);
    if (!clock_24h_mode && clock[0] == '0') memmove(clock, clock + 1, strlen(clock));
    snprintf(out, out_len, "%s at %s", day, clock);
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
    int global_volume = g_state.global_volume;
    int story_mode_enabled = g_state.story_mode_enabled;
    int story_volume = g_state.story_volume;
    int story_playing = g_state.story_playing;
    int story_intro_active = monotonic_millis() < g_state.story_intro_until_ms;
    char story_message[STORY_MESSAGE_MAX];
    mp_safe_str(story_message, sizeof(story_message), g_state.story_message);
    int show_song_metadata = g_state.show_song_metadata;
    time_t pending_message_at = g_state.pending_message_at;
    int bedtime_enabled = g_state.bedtime_enabled;
    int bsh = g_state.bedtime_start_hour;
    int bsm = g_state.bedtime_start_min;
    int beh = g_state.bedtime_end_hour;
    int bem = g_state.bedtime_end_min;
    int bedtime_dim = g_state.bedtime_dim_percent;
    int bedtime_music_enabled = g_state.bedtime_music_enabled;
    long long last_successful_alarm = g_state.last_successful_alarm;
    int clock_24h_mode = g_state.clock_24h_mode;
    int oled_color = g_state.oled_color;
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
    struct mp_led_profile led_profiles[MP_LED_SCENE_COUNT];
    struct mp_led_global_settings led_settings = g_state.led_settings;
    int led_bedtime_fade_active = g_state.led_bedtime_fade_active;
    time_t led_preview_until = g_state.led_preview_until;
    mp_safe_str(font_file, sizeof(font_file), g_state.oled_font_file);
    mp_safe_str(clock_name, sizeof(clock_name), g_state.clock_name);
    mp_safe_str(audio_file, sizeof(audio_file), g_state.audio_file);
    mp_safe_str(audio_title, sizeof(audio_title), g_state.audio_title);
    mp_safe_str(audio_artist, sizeof(audio_artist), g_state.audio_artist);
    mp_safe_str(audio_display, sizeof(audio_display), g_state.audio_display);
    memcpy(alarms, g_state.alarms, sizeof(alarms));
    memcpy(led_profiles, g_state.led_profiles, sizeof(led_profiles));
    pthread_mutex_unlock(&g_state.lock);
    touch_get_state(&touch_ok, &touch_pressed);

    struct mp_led_status led_status;
    memset(&led_status, 0, sizeof(led_status));
    led_status.scene = MP_LED_SCENE_DAYTIME;
    mp_led_snapshot(&led_status);
    int led_ok = led_status.ready;

    int next_alarm_id = 0;
    time_t next_alarm_at = next_alarm_time(alarms, now, &next_alarm_id);
    char next_alarm_text[96];
    format_next_alarm(next_alarm_at, clock_24h_mode, next_alarm_text, sizeof(next_alarm_text));

    char e_time[128], e_date[192], e_clock_name[160], e_audio_file[512];
    char e_audio_title[384], e_audio_artist[384], e_audio_display[768];
    char e_story_message[192];
    char e_font_file[256], e_font_name[256], e_next_alarm[192];
    mp_json_escape(e_time, sizeof(e_time), timestr);
    mp_json_escape(e_date, sizeof(e_date), datestr);
    mp_json_escape(e_clock_name, sizeof(e_clock_name), clock_name);
    mp_json_escape(e_audio_file, sizeof(e_audio_file), audio_file);
    mp_json_escape(e_audio_title, sizeof(e_audio_title), audio_title);
    mp_json_escape(e_audio_artist, sizeof(e_audio_artist), audio_artist);
    mp_json_escape(e_audio_display, sizeof(e_audio_display), audio_display);
    mp_json_escape(e_story_message, sizeof(e_story_message), story_message);
    mp_json_escape(e_font_file, sizeof(e_font_file), font_file);
    mp_json_escape(e_font_name, sizeof(e_font_name), font_file[0] ? font_file : oled_font_name_for_id(font));
    mp_json_escape(e_next_alarm, sizeof(e_next_alarm), next_alarm_text);

    long long message_send_in = pending_message_at > now ? (long long)(pending_message_at - now) : 0LL;
    struct mp_buffer body;
    if (mp_buffer_init(&body, 8192, MP_IPC_MAX_PAYLOAD) != 0)
        return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    mp_buffer_appendf(&body,
        "{\"time\":\"%s\",\"date\":\"%s\",\"clock_name\":\"%s\",\"app_version\":\"%s\","
        "\"uptime_seconds\":%ld,\"audio_file\":\"%s\",\"audio_title\":\"%s\",\"audio_artist\":\"%s\","
        "\"audio_display\":\"%s\",\"global_volume\":%d,\"story_mode_enabled\":%d,\"story_volume\":%d,"
        "\"story_playing\":%d,\"story_intro_active\":%d,\"story_message\":\"%s\",\"show_song_metadata\":%d,"
        "\"message_pending\":%d,\"message_send_in_seconds\":%lld,\"message_scheduled_for\":%lld,\"bedtime_enabled\":%d,"
        "\"bedtime_start_hour\":%d,\"bedtime_start_min\":%d,\"bedtime_end_hour\":%d,\"bedtime_end_min\":%d,"
        "\"bedtime_dim_percent\":%d,\"bedtime_music_enabled\":%d,\"clock_24h_mode\":%d,\"oled_color\":\"%s\",\"bedtime_active\":%d,\"oled_brightness_percent\":%d,"
        "\"next_alarm_id\":%d,\"next_alarm_at\":%lld,\"next_alarm_text\":\"%s\",\"last_successful_alarm\":%lld,"
        "\"audio_playing\":%d,\"alarm_active\":%d,\"alarm_volume_percent\":%d,\"display_mode\":\"%s\",\"oled_ok\":%d,"
        "\"touch_ok\":%d,\"touch_pressed\":%d,\"touch_gpio\":%d,"
        "\"oled_font\":%d,\"oled_font_size\":%d,\"oled_font_file\":\"%s\",\"oled_font_name\":\"%s\",\"alarms\":[",
        e_time, e_date, e_clock_name, APP_VERSION, uptime_seconds, e_audio_file,
        e_audio_title, e_audio_artist, e_audio_display, global_volume, story_mode_enabled, story_volume,
        story_playing, story_intro_active, e_story_message, show_song_metadata,
        message_send_in > 0 ? 1 : 0, message_send_in, (long long)pending_message_at, bedtime_enabled,
        bsh, bsm, beh, bem, bedtime_dim, bedtime_music_enabled, clock_24h_mode, oled_color_name_for_id(oled_color),
        is_bedtime_now(), oled_brightness, next_alarm_id, (long long)next_alarm_at, e_next_alarm,
        last_successful_alarm, audio, alarm_active, alarm_volume_percent,
        display_mode_name(mode), oled_ok, touch_ok, touch_pressed, GPIO_TOUCH,
        font, font_size, e_font_file, e_font_name);

    for (int i = 0; i < MAX_ALARMS && !body.failed; i++) {
        mp_buffer_appendf(&body,
            "%s{\"id\":%d,\"enabled\":%d,\"hour\":%d,\"min\":%d,\"weekdays\":%d,\"start_volume\":%d,\"end_volume\":%d,\"music_file\":\"",
            i ? "," : "", i + 1, alarms[i].enabled, alarms[i].hour, alarms[i].min,
            alarms[i].weekdays, alarms[i].start_volume, alarms[i].end_volume);
        mp_buffer_append_json_string(&body, alarms[i].music_file);
        mp_buffer_append(&body, "\"}");
    }
    mp_buffer_appendf(&body,
        "],\"led_ok\":%d,\"led_scene\":\"%s\",\"led_colour\":\"#%02X%02X%02X\","
        "\"led_output\":\"#%02X%02X%02X\",\"led_rgb\":\"#%02X%02X%02X\","
        "\"led_write_errors\":%u,\"led_pwm_hz\":%d,\"led_pwm_levels\":%d,"
        "\"led_common_cathode\":1,\"led_gpio_red\":%d,\"led_gpio_green\":%d,\"led_gpio_blue\":%d,"
        "\"led_preview_active\":%d,\"led_bedtime_fade_active\":%d,"
        "\"led_gpio\":{\"red\":%d,\"green\":%d,\"blue\":%d},"
        "\"led_settings\":{\"enabled\":%u,\"max_brightness\":%u,\"red_gain\":%u,"
        "\"green_gain\":%u,\"blue_gain\":%u,\"idle_off\":%u,\"bedtime_fade_minutes\":%u,"
        "\"touch_blink_enabled\":%u,\"touch_blink_brightness\":%u,"
        "\"touch_blink_color\":\"#%02X%02X%02X\"},"
        "\"led_enabled\":%u,\"led_max_brightness\":%u,\"led_red_gain\":%u,"
        "\"led_green_gain\":%u,\"led_blue_gain\":%u,\"led_idle_off\":%u,"
        "\"led_bedtime_fade_minutes\":%u,\"led_touch_blink_enabled\":%u,"
        "\"led_touch_blink_brightness\":%u,\"led_touch_blink_color\":\"#%02X%02X%02X\","
        "\"led_profiles\":[",
        led_ok, mp_led_scene_name(led_status.scene),
        led_status.colour_red, led_status.colour_green, led_status.colour_blue,
        led_status.output_red, led_status.output_green, led_status.output_blue,
        led_status.output_red, led_status.output_green, led_status.output_blue,
        led_status.write_errors, MP_LED_PWM_HZ, MP_LED_PWM_LEVELS,
        MP_LED_GPIO_RED, MP_LED_GPIO_GREEN, MP_LED_GPIO_BLUE,
        led_preview_until > now ? 1 : 0,
        led_bedtime_fade_active,
        MP_LED_GPIO_RED, MP_LED_GPIO_GREEN, MP_LED_GPIO_BLUE,
        led_settings.enabled, led_settings.max_brightness, led_settings.red_gain,
        led_settings.green_gain, led_settings.blue_gain, led_settings.idle_off,
        led_settings.bedtime_fade_minutes,
        led_settings.touch_blink_enabled, led_settings.touch_blink_brightness,
        led_settings.touch_blink_red, led_settings.touch_blink_green, led_settings.touch_blink_blue,
        led_settings.enabled, led_settings.max_brightness, led_settings.red_gain,
        led_settings.green_gain, led_settings.blue_gain, led_settings.idle_off,
        led_settings.bedtime_fade_minutes,
        led_settings.touch_blink_enabled, led_settings.touch_blink_brightness,
        led_settings.touch_blink_red, led_settings.touch_blink_green, led_settings.touch_blink_blue);
    for (int i = 0; i < MP_LED_SCENE_COUNT && !body.failed; i++) {
        const struct mp_led_profile *profile = &led_profiles[i];
        mp_buffer_appendf(&body,
            "%s{\"scene\":\"%s\",\"scene_id\":%d,\"effect\":\"%s\",\"effect_id\":%u,"
            "\"brightness\":%u,\"cycle_seconds\":%u,\"color1\":\"#%02X%02X%02X\",\"color2\":\"#%02X%02X%02X\"}",
            i ? "," : "", mp_led_scene_name((enum mp_led_scene)i), i,
            mp_led_effect_name((enum mp_led_effect)profile->effect), profile->effect,
            profile->brightness, profile->cycle_seconds,
            profile->red1, profile->green1, profile->blue1,
            profile->red2, profile->green2, profile->blue2);
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

static int ipc_message_preview(int client, const struct mp_ipc_display_message *request) {
    char image_file[IMAGE_FILE_MAX];
    char text[192];
    mp_safe_str(image_file, sizeof(image_file), request->image_file);
    mp_safe_str(text, sizeof(text), request->text);
    sanitize_message_text(text, sizeof(text));

    if (request->image_bedtime != 0 && request->image_bedtime != 1)
        return ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"invalid image library\"}");
    if (image_file[0] && !safe_image_filename(image_file))
        return ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"invalid image filename\"}");

    char reason[160] = "";
    if (text[0] && !message_fits_display(text, reason, sizeof(reason))) {
        struct mp_buffer body;
        if (mp_buffer_init(&body, 256, 1024) != 0)
            return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
        mp_buffer_append(&body, "{\"ok\":false,\"error\":\"");
        mp_buffer_append_json_string(&body, reason[0] ? reason : "Message does not fit on the OLED.");
        mp_buffer_append(&body, "\"}");
        return ipc_send_builder(client, 422, &body);
    }

    uint8_t snapshot[OLED_FB_BYTES];
    memset(snapshot, 0, sizeof(snapshot));
    uint8_t *previous_target = g_oled_render_fb;
    g_oled_render_fb = snapshot;
    int rc = render_message_screen_file(image_file, request->image_bedtime ? 1 : 0, text, 0);
    g_oled_render_fb = previous_target;
    if (rc != 0)
        return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"message preview failed\"}");

    int brightness_percent;
    pthread_mutex_lock(&g_state.lock);
    brightness_percent = g_state.oled_brightness_current;
    pthread_mutex_unlock(&g_state.lock);
    oled_apply_brightness_to_buffer(snapshot, sizeof(snapshot), brightness_percent);
    return ipc_send_response(client, 200, MP_IPC_CONTENT_BINARY, snapshot, sizeof(snapshot));
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
            if (audio_is_alarm_session())
                return ipc_send_json(client, 409,
                    "{\"ok\":false,\"error\":\"Press the touch sensor on the clock to stop the alarm\"}");
            (void)audio_request_stop();
            app_log("action", "Stop music requested");
            break;
        case MP_IPC_ACTION_PLAY_MUSIC: {
            int volume;
            pthread_mutex_lock(&g_state.lock);
            volume = g_state.global_volume;
            pthread_mutex_unlock(&g_state.lock);
            int result = audio_play_music_file(request->file, volume, volume, 0);
            if (result == -2)
                return ipc_send_json(client, 409,
                    "{\"ok\":false,\"error\":\"Music is disabled during bedtime hours\"}");
            if (result != 0)
                return ipc_send_json(client, 503,
                    "{\"ok\":false,\"error\":\"No playable music is available\"}");
            app_log("action", "Play music requested: %s", request->file);
            break;
        }
        case MP_IPC_ACTION_PLAY_STORY: {
            int result = audio_play_story_file(request->file);
            if (result == -2)
                return ipc_send_json(client, 409,
                    "{\"ok\":false,\"error\":\"Story mode is disabled\"}");
            if (result != 0)
                return ipc_send_json(client, 503,
                    "{\"ok\":false,\"error\":\"No playable story is available\"}");
            app_log("action", "Play story requested: %s", request->file);
            break;
        }
        default:
            return ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"unknown display action\"}");
    }
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int valid_message_delay(unsigned int seconds) {
    return seconds == 0 || seconds == 10 || seconds == 30 || seconds == MESSAGE_DELAY_MAX_SECONDS;
}

static void activate_pending_message_if_due(void) {
    time_t now = time(NULL);
    int activated = 0;
    int notification_sound = 0;
    pthread_mutex_lock(&g_state.lock);
    if (g_state.pending_message_at > 0 && now >= g_state.pending_message_at) {
        g_state.display_mode = 2;
        safe_str(g_state.message_image_file, sizeof(g_state.message_image_file),
                 g_state.pending_message_image_file);
        g_state.message_image_bedtime = g_state.pending_message_image_bedtime;
        safe_str(g_state.message_text, sizeof(g_state.message_text),
                 g_state.pending_message_text);
        g_state.message_until = now + MESSAGE_DEFAULT_DURATION_SECONDS;
        notification_sound = g_state.pending_message_notification_sound;
        g_state.pending_message_at = 0;
        g_state.pending_message_image_file[0] = '\0';
        g_state.pending_message_image_bedtime = 0;
        g_state.pending_message_notification_sound = 0;
        g_state.pending_message_text[0] = '\0';
        g_state.display_dirty = 1;
        activated = 1;
    }
    pthread_mutex_unlock(&g_state.lock);
    if (activated) {
        save_config();
        if (notification_sound) {
            int chime_result = audio_play_message_chime();
            if (chime_result == -4)
                app_log("message", "Message chime skipped because audio is already playing");
            else if (chime_result != 0)
                app_log("message", "Message chime could not be played");
        }
    }
}

static int ipc_display_message(int client, const struct mp_ipc_display_message *request) {
    char image_file[IMAGE_FILE_MAX] = "";
    char message[192];
    unsigned int delay_seconds = request->delay_seconds;
    int image_bedtime = request->image_bedtime ? 1 : 0;
    int notification_sound = request->notification_sound ? 1 : 0;
    uint64_t requested_at = request->scheduled_at;
    if (!valid_message_delay(delay_seconds))
        return ipc_send_json(client, 400,
            "{\"ok\":false,\"error\":\"delay_seconds must be 0, 10, 30, or 60\"}");
    if (requested_at && delay_seconds)
        return ipc_send_json(client, 400,
            "{\"ok\":false,\"error\":\"use either delay or scheduled time\"}");
    if (request->image_file[0]) {
        if (!safe_image_filename(request->image_file))
            return ipc_send_json(client, 400,
                "{\"ok\":false,\"error\":\"invalid image filename\"}");
        char image_path[512];
        if (make_image_path_by_file(request->image_file, image_bedtime,
                                    image_path, sizeof(image_path)) != 0 ||
            access(image_path, R_OK) != 0)
            return ipc_send_json(client, 404,
                "{\"ok\":false,\"error\":\"image not found\"}");
        mp_safe_str(image_file, sizeof(image_file), request->image_file);
    } else if (random_image_file(image_bedtime, image_file, sizeof(image_file)) != 0) {
        image_file[0] = '\0';
    }
    mp_safe_str(message, sizeof(message), request->text);
    sanitize_message_text(message, sizeof(message));
    if (!message[0] && !image_file[0])
        return ipc_send_json(client, 400,
            "{\"ok\":false,\"error\":\"choose an image or enter a message\"}");
    char reason[160] = "";
    if (message[0] && !message_fits_display(message, reason, sizeof(reason))) {
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
    time_t scheduled_at = requested_at ? (time_t)requested_at :
                          (delay_seconds ? now + (time_t)delay_seconds : 0);
    if (requested_at && ((uint64_t)scheduled_at != requested_at || scheduled_at <= now ||
        scheduled_at > now + (time_t)(30 * 24 * 60 * 60)))
        return ipc_send_json(client, 400,
            "{\"ok\":false,\"error\":\"specific time must be within the next 30 days\"}");
    pthread_mutex_lock(&g_state.lock);
    if (scheduled_at == 0) {
        g_state.display_mode = 2;
        mp_safe_str(g_state.message_image_file, sizeof(g_state.message_image_file), image_file);
        g_state.message_image_bedtime = image_bedtime;
        g_state.message_until = now + MESSAGE_DEFAULT_DURATION_SECONDS;
        mp_safe_str(g_state.message_text, sizeof(g_state.message_text), message);
        g_state.pending_message_at = 0;
        g_state.pending_message_image_file[0] = '\0';
        g_state.pending_message_image_bedtime = 0;
        g_state.pending_message_notification_sound = 0;
        g_state.pending_message_text[0] = '\0';
        g_state.display_dirty = 1;
    } else {
        mp_safe_str(g_state.pending_message_image_file, sizeof(g_state.pending_message_image_file), image_file);
        g_state.pending_message_image_bedtime = image_bedtime;
        g_state.pending_message_notification_sound = notification_sound;
        mp_safe_str(g_state.pending_message_text, sizeof(g_state.pending_message_text), message);
        g_state.pending_message_at = scheduled_at;
    }
    pthread_mutex_unlock(&g_state.lock);
    save_config();

    if (scheduled_at == 0) {
        if (notification_sound) {
            int chime_result = audio_play_message_chime();
            if (chime_result == -4)
                app_log("message", "Message chime skipped because audio is already playing");
            else if (chime_result != 0)
                app_log("message", "Message chime could not be played");
        }
        app_log("message", "Sent message with %s image %s: %.120s",
                image_bedtime ? "bedtime" : "day",
                image_file[0] ? image_file : "none", message);
        return ipc_send_json(client, 200, "{\"ok\":true,\"mode\":\"message\",\"delay_seconds\":0}");
    }

    app_log("message", "Scheduled message for %lld with %s image %s: %.120s",
            (long long)scheduled_at, image_bedtime ? "bedtime" : "day",
            image_file[0] ? image_file : "none", message);
    char response[224];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"mode\":\"scheduled-message\",\"delay_seconds\":%u,\"scheduled_for\":%lld}",
             delay_seconds, (long long)scheduled_at);
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
    pthread_mutex_unlock(&g_state.lock);
    save_config();
    app_log("alarm", "Saved alarm %d", id);
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int ipc_config_audio(int client, const struct mp_ipc_audio_config *request) {
    int changed = 0;
    int volume = -1;
    int show_metadata = -1;
    int story_enabled = -1;
    int story_volume = -1;
    char story_message[STORY_MESSAGE_MAX] = "";
    int story_message_changed = 0;
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
    if (request->present_mask & MP_IPC_AUDIO_STORY_ENABLED) {
        story_enabled = request->story_enabled ? 1 : 0;
        g_state.story_mode_enabled = story_enabled;
        g_state.display_mode = 0;
        g_state.display_dirty = 1;
        changed = 1;
    }
    if (request->present_mask & MP_IPC_AUDIO_STORY_VOLUME) {
        story_volume = clamp_int(request->story_volume, 0, 100);
        g_state.story_volume = story_volume;
        changed = 1;
    }
    if (request->present_mask & MP_IPC_AUDIO_STORY_MESSAGE) {
        safe_str(story_message, sizeof(story_message), request->story_message);
        sanitize_message_text(story_message, sizeof(story_message));
        if (!story_message[0]) safe_str(story_message, sizeof(story_message), "STORY MODE!");
        safe_str(g_state.story_message, sizeof(g_state.story_message), story_message);
        g_state.display_dirty = 1;
        story_message_changed = 1;
        changed = 1;
    }
    pthread_mutex_unlock(&g_state.lock);
    if (!changed)
        return ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"no audio settings supplied\"}");
    save_config();
    if (volume >= 0) app_log("music", "Saved global volume %d%%", volume);
    if (show_metadata >= 0) app_log("music", "Song metadata display %s", show_metadata ? "enabled" : "disabled");
    if (story_enabled >= 0) app_log("story", "Story mode %s", story_enabled ? "enabled" : "disabled");
    if (story_volume >= 0) app_log("story", "Saved story volume %d%%", story_volume);
    if (story_message_changed) app_log("story", "Saved story screen message: %s", story_message);
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

static int ipc_display_preview(int client) {
    pthread_mutex_lock(&g_state.lock);
    int oled_ok = g_state.oled_ok;
    pthread_mutex_unlock(&g_state.lock);
    if (!oled_ok)
        return ipc_send_json(client, 503, "{\"ok\":false,\"error\":\"OLED unavailable\"}");

    uint8_t snapshot[OLED_FB_BYTES];
    pthread_mutex_lock(&g_oled.preview_lock);
    memcpy(snapshot, g_oled.preview_fb, sizeof(snapshot));
    pthread_mutex_unlock(&g_oled.preview_lock);
    return ipc_send_response(client, 200, MP_IPC_CONTENT_BINARY, snapshot, sizeof(snapshot));
}

static int ipc_brightness_preview(int client, const struct mp_ipc_brightness_preview *request) {
    int percent = clamp_int(request->percent, 0, 100);
    int hold_seconds = clamp_int(request->hold_seconds, 1, 30);
    pthread_mutex_lock(&g_state.lock);
    if (!g_state.oled_ok) {
        pthread_mutex_unlock(&g_state.lock);
        return ipc_send_json(client, 503, "{\"ok\":false,\"error\":\"OLED unavailable\"}");
    }
    g_state.brightness_preview_percent = percent;
    g_state.brightness_preview_until = time(NULL) + hold_seconds;
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);

    /* The main OLED loop applies the preview. Keeping SPI writes on that thread
       prevents a slider request from interleaving commands with a framebuffer flush. */
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":true,\"brightness_percent\":%d}", percent);
    return ipc_send_json(client, 200, body);
}

static int ipc_config_display(int client, const struct mp_ipc_display_config *request) {
    pthread_mutex_lock(&g_state.lock);
    int font = g_state.oled_font;
    int font_size = g_state.oled_font_size;
    int bedtime_enabled = g_state.bedtime_enabled;
    int bedtime_dim = g_state.bedtime_dim_percent;
    int bedtime_music_enabled = g_state.bedtime_music_enabled;
    int clock_mode = g_state.clock_24h_mode;
    int oled_color = g_state.oled_color;
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
    if (request->present_mask & MP_IPC_DISPLAY_BEDTIME_MUSIC) bedtime_music_enabled = request->bedtime_music_enabled ? 1 : 0;
    if (request->present_mask & MP_IPC_DISPLAY_CLOCK_MODE) clock_mode = request->clock_24h_mode ? 1 : 0;
    if (request->present_mask & MP_IPC_DISPLAY_OLED_COLOR)
        oled_color = clamp_int(request->oled_color, MP_OLED_COLOR_YELLOW, MP_OLED_COLOR_WHITE);
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
    g_state.oled_color = oled_color;
    g_state.bedtime_enabled = bedtime_enabled;
    g_state.bedtime_dim_percent = bedtime_dim;
    g_state.bedtime_music_enabled = bedtime_music_enabled;
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
        if (event->kind == MP_IPC_ASSET_IMAGE) {
            invalidate_image_assets(0);
            pthread_mutex_lock(&g_state.lock);
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
        } else if (event->kind == MP_IPC_ASSET_BEDTIME_IMAGE) {
            invalidate_image_assets(1);
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
        if (event->kind == MP_IPC_ASSET_IMAGE || event->kind == MP_IPC_ASSET_BEDTIME_IMAGE) {
            int bedtime = event->kind == MP_IPC_ASSET_BEDTIME_IMAGE;
            invalidate_image_assets(bedtime);
            pthread_mutex_lock(&g_state.lock);
            int config_changed = 0;
            if (g_state.message_image_bedtime == bedtime &&
                strcmp(g_state.message_image_file, event->file) == 0) {
                g_state.message_image_file[0] = '\0';
            }
            if (g_state.pending_message_image_bedtime == bedtime &&
                strcmp(g_state.pending_message_image_file, event->file) == 0) {
                g_state.pending_message_image_file[0] = '\0';
                config_changed = 1;
            }
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
            if (config_changed) save_config();
        } else if (event->kind == MP_IPC_ASSET_MUSIC || event->kind == MP_IPC_ASSET_STORY) {
            int stop_audio = 0;
            int config_changed = 0;
            pthread_mutex_lock(&g_state.lock);
            stop_audio = strcmp(g_state.audio_file, event->file) == 0;
            pthread_mutex_unlock(&g_state.lock);
            if (stop_audio) audio_stop();
            pthread_mutex_lock(&g_state.lock);
            if (stop_audio) {
                g_state.audio_playing = 0;
                g_state.audio_file[0] = '\0';
            }
            if (event->kind == MP_IPC_ASSET_MUSIC) {
                for (int i = 0; i < MAX_ALARMS; i++) {
                    if (strcmp(g_state.alarms[i].music_file, event->file) == 0) {
                        g_state.alarms[i].music_file[0] = '\0';
                        config_changed = 1;
                    }
                }
            }
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
            if (config_changed) save_config();
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
        if (event->kind == MP_IPC_ASSET_IMAGE || event->kind == MP_IPC_ASSET_BEDTIME_IMAGE) {
            int bedtime = event->kind == MP_IPC_ASSET_BEDTIME_IMAGE;
            invalidate_image_assets(bedtime);
            pthread_mutex_lock(&g_state.lock);
            if (g_state.message_image_bedtime == bedtime)
                g_state.message_image_file[0] = '\0';
            if (g_state.pending_message_image_bedtime == bedtime)
                g_state.pending_message_image_file[0] = '\0';
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
            save_config();
        } else if (event->kind == MP_IPC_ASSET_MUSIC || event->kind == MP_IPC_ASSET_STORY) {
            audio_stop();
            pthread_mutex_lock(&g_state.lock);
            g_state.audio_playing = 0;
            g_state.audio_file[0] = '\0';
            if (event->kind == MP_IPC_ASSET_MUSIC)
                for (int i = 0; i < MAX_ALARMS; i++) g_state.alarms[i].music_file[0] = '\0';
            g_state.display_dirty = 1;
            pthread_mutex_unlock(&g_state.lock);
            if (event->kind == MP_IPC_ASSET_MUSIC) save_config();
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
    state.story_volume = g_state.story_volume;
    state.story_enabled = g_state.story_mode_enabled;
    safe_str(state.story_message, sizeof(state.story_message), g_state.story_message);
    state.builtin_font = g_state.oled_font;
    state.font_size = g_state.oled_font_size;
    mp_safe_str(state.current_music, sizeof(state.current_music), g_state.audio_file);
    mp_safe_str(state.selected_font, sizeof(state.selected_font), g_state.oled_font_file);
    pthread_mutex_unlock(&g_state.lock);
    return ipc_send_response(client, 200, MP_IPC_CONTENT_BINARY, &state, sizeof(state));
}

static int ipc_config_led(int client, const struct mp_ipc_led_config *request) {
    if (request->scene >= MP_LED_SCENE_COUNT)
        return ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"invalid LED scene\"}");

    struct mp_led_profile profile = request->profile;
    sanitize_led_profile(&profile);
    pthread_mutex_lock(&g_state.lock);
    g_state.led_profiles[request->scene] = profile;
    pthread_mutex_unlock(&g_state.lock);
    save_config();
    update_led_scene();
    app_log("lighting", "Saved %s LED profile: %s at %u%%",
            mp_led_scene_name((enum mp_led_scene)request->scene),
            mp_led_effect_name((enum mp_led_effect)profile.effect), profile.brightness);
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int ipc_config_led_global(int client, const struct mp_ipc_led_global_config *request) {
    struct mp_led_global_settings settings = request->settings;
    sanitize_led_settings(&settings);
    pthread_mutex_lock(&g_state.lock);
    g_state.led_settings = settings;
    pthread_mutex_unlock(&g_state.lock);
    save_config();
    update_led_scene();
    app_log("lighting", "Saved global LED settings: %s, max %u%%, gains %u/%u/%u",
            settings.enabled ? "enabled" : "disabled", settings.max_brightness,
            settings.red_gain, settings.green_gain, settings.blue_gain);
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int ipc_led_preview(int client, const struct mp_ipc_led_preview *request) {
    if (request->scene >= MP_LED_SCENE_COUNT)
        return ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"invalid LED scene\"}");
    if (!mp_led_ready())
        return ipc_send_json(client, 503, "{\"ok\":false,\"error\":\"RGB LED unavailable\"}");

    struct mp_led_profile profile = request->profile;
    sanitize_led_profile(&profile);
    int hold_seconds = clamp_int(request->hold_seconds, 1, 30);
    pthread_mutex_lock(&g_state.lock);
    g_state.led_preview_scene = request->scene;
    g_state.led_preview_profile = profile;
    g_state.led_preview_bypass_master = request->bypass_master ? 1 : 0;
    g_state.led_preview_raw_output = request->raw_output ? 1 : 0;
    g_state.led_preview_until = time(NULL) + hold_seconds;
    pthread_mutex_unlock(&g_state.lock);
    update_led_scene();
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int ipc_config_export(int client) {
    save_config();
    int fd = open(CONFIG_FILE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"configuration could not be read\"}");
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > MP_IPC_CONFIG_MAX_BYTES) {
        close(fd);
        return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"configuration is too large\"}");
    }
    char *data = malloc((size_t)st.st_size ? (size_t)st.st_size : 1u);
    if (!data) {
        close(fd);
        return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"allocation failed\"}");
    }
    ssize_t got = read(fd, data, (size_t)st.st_size);
    close(fd);
    if (got != st.st_size) {
        free(data);
        return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"configuration could not be read\"}");
    }
    int rc = ipc_send_response(client, 200, MP_IPC_CONTENT_TEXT, data, (size_t)got);
    free(data);
    return rc;
}

static int config_blob_valid(const struct mp_ipc_config_blob *blob) {
    if (!blob || blob->length == 0 || blob->length > MP_IPC_CONFIG_MAX_BYTES) return 0;
    for (uint32_t i = 0; i < blob->length; i++) {
        unsigned char ch = (unsigned char)blob->data[i];
        if (ch == 0) return 0;
        if (ch < 0x09 || (ch > 0x0d && ch < 0x20)) return 0;
    }
    return 1;
}

static int ipc_config_import(int client, const struct mp_ipc_config_blob *blob) {
    if (!config_blob_valid(blob))
        return ipc_send_json(client, 400, "{\"ok\":false,\"error\":\"backup configuration is invalid\"}");
    (void)audio_force_stop_and_wait(3000u);
    ensure_dir(CONFIG_DIR);
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), CONFIG_FILE ".restore");
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0640);
    int failed = fd < 0;
    if (!failed && (write_all(fd, blob->data, blob->length) != 0 || fsync(fd) != 0)) failed = 1;
    if (fd >= 0 && close(fd) != 0) failed = 1;
    if (failed) {
        unlink(tmp_path);
        return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"configuration could not be restored\"}");
    }
    if (rename(tmp_path, CONFIG_FILE) != 0) {
        unlink(tmp_path);
        return ipc_send_json(client, 500, "{\"ok\":false,\"error\":\"configuration could not be activated\"}");
    }
    pthread_mutex_lock(&g_state.lock);
    reset_persistent_state_locked();
    pthread_mutex_unlock(&g_state.lock);
    load_config();
    font_cache_reset();
    invalidate_image_assets(0);
    invalidate_image_assets(1);
    pthread_mutex_lock(&g_state.lock);
    g_state.display_mode = 0;
    g_state.display_dirty = 1;
    pthread_mutex_unlock(&g_state.lock);
    save_config();
    update_led_scene();
    app_log("backup", "Configuration restored from backup");
    return ipc_send_json(client, 200, "{\"ok\":true}");
}

static int ipc_factory_reset(int client) {
    (void)audio_force_stop_and_wait(3000u);
    pthread_mutex_lock(&g_state.lock);
    reset_persistent_state_locked();
    pthread_mutex_unlock(&g_state.lock);
    font_cache_reset();
    invalidate_image_assets(0);
    invalidate_image_assets(1);
    save_config();
    update_led_scene();
    app_log("system", "Factory reset completed");
    return ipc_send_json(client, 200, "{\"ok\":true}");
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
        case MP_IPC_OP_DISPLAY_MESSAGE:
            EXPECT(struct mp_ipc_display_message);
            return ipc_display_message(client, payload);
        case MP_IPC_OP_MESSAGE_PREVIEW:
            EXPECT(struct mp_ipc_display_message);
            return ipc_message_preview(client, payload);
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
        case MP_IPC_OP_DISPLAY_PREVIEW:
            if (payload_len != 0) return ipc_bad_payload(client);
            return ipc_display_preview(client);
        case MP_IPC_OP_BRIGHTNESS_PREVIEW:
            EXPECT(struct mp_ipc_brightness_preview);
            return ipc_brightness_preview(client, payload);
        case MP_IPC_OP_CONFIG_EXPORT:
            if (payload_len != 0) return ipc_bad_payload(client);
            return ipc_config_export(client);
        case MP_IPC_OP_CONFIG_IMPORT:
            EXPECT(struct mp_ipc_config_blob);
            return ipc_config_import(client, payload);
        case MP_IPC_OP_FACTORY_RESET:
            if (payload_len != 0) return ipc_bad_payload(client);
            return ipc_factory_reset(client);
        case MP_IPC_OP_CONFIG_LED:
            EXPECT(struct mp_ipc_led_config);
            return ipc_config_led(client, payload);
        case MP_IPC_OP_CONFIG_LED_GLOBAL:
            EXPECT(struct mp_ipc_led_global_config);
            return ipc_config_led_global(client, payload);
        case MP_IPC_OP_LED_PREVIEW:
            EXPECT(struct mp_ipc_led_preview);
            return ipc_led_preview(client, payload);
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
        case MP_IPC_OP_LOGS_GET:
        case MP_IPC_OP_LOGS_CLEAR:
        case MP_IPC_OP_ASSET_STATE:
        case MP_IPC_OP_PING:
        case MP_IPC_OP_DISPLAY_PREVIEW:
        case MP_IPC_OP_CONFIG_EXPORT:
        case MP_IPC_OP_FACTORY_RESET:
            return 0;
        case MP_IPC_OP_DISPLAY_ACTION: return sizeof(struct mp_ipc_display_action);
        case MP_IPC_OP_BRIGHTNESS_PREVIEW: return sizeof(struct mp_ipc_brightness_preview);
        case MP_IPC_OP_DISPLAY_MESSAGE:
        case MP_IPC_OP_MESSAGE_PREVIEW:
            return sizeof(struct mp_ipc_display_message);
        case MP_IPC_OP_CONFIG_ALARM: return sizeof(struct mp_ipc_alarm_config);
        case MP_IPC_OP_CONFIG_AUDIO: return sizeof(struct mp_ipc_audio_config);
        case MP_IPC_OP_CONFIG_PERSONALIZATION: return sizeof(struct mp_ipc_personalization_config);
        case MP_IPC_OP_CONFIG_DISPLAY: return sizeof(struct mp_ipc_display_config);
        case MP_IPC_OP_ASSET_EVENT: return sizeof(struct mp_ipc_asset_event);
        case MP_IPC_OP_CONFIG_IMPORT: return sizeof(struct mp_ipc_config_blob);
        case MP_IPC_OP_CONFIG_LED: return sizeof(struct mp_ipc_led_config);
        case MP_IPC_OP_LED_PREVIEW: return sizeof(struct mp_ipc_led_preview);
        case MP_IPC_OP_CONFIG_LED_GLOBAL: return sizeof(struct mp_ipc_led_global_config);
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

static void enforce_bedtime_music_policy(void) {
    int allow_music;
    int playing;
    pthread_mutex_lock(&g_state.lock);
    allow_music = g_state.bedtime_music_enabled;
    playing = g_state.audio_playing;
    pthread_mutex_unlock(&g_state.lock);
    if (!allow_music && playing && is_bedtime_now() && !audio_is_alarm_session()) {
        if (audio_request_stop()) app_log("music", "Music stopped because bedtime began");
    }
}

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
    if (fire) g_state.display_mode = 0;
    pthread_mutex_unlock(&g_state.lock);

    if (fire) {
        app_log("alarm", "Alarm fired at %02d:%02d", fire_alarm.hour, fire_alarm.min);
        if (audio_play_music_file(fire_alarm.music_file, fire_alarm.start_volume,
                                  fire_alarm.end_volume, 1) != 0)
            app_log("alarm", "Alarm could not start because no playable audio was available");
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
    ensure_dir(IMAGE_DIR);
    ensure_dir(BEDTIME_IMAGE_DIR);
    ensure_dir(MUSIC_DIR);
    ensure_dir(STORY_DIR);
    ensure_dir(FONT_DIR);
    ensure_dir(CONFIG_DIR);
    init_alarm_defaults();
    load_config();
    app_log("system", "mk-piclock %s starting", APP_VERSION);

    int touch_available = (touch_init() == 0);
    if (touch_available) app_log("system", "TTP223B touch input initialized on GPIO %d", GPIO_TOUCH);
    else app_log("system", "TTP223B touch input unavailable on GPIO %d", GPIO_TOUCH);

    int led_available = (mp_led_init() == 0);
    if (led_available)
        app_log("system", "Common-cathode RGB LED initialized on GPIO %d/%d/%d",
                MP_LED_GPIO_RED, MP_LED_GPIO_GREEN, MP_LED_GPIO_BLUE);
    else
        app_log("system", "RGB LED unavailable on GPIO %d/%d/%d",
                MP_LED_GPIO_RED, MP_LED_GPIO_GREEN, MP_LED_GPIO_BLUE);
    update_led_scene();

    if (oled_init() == 0) {
        pthread_mutex_lock(&g_state.lock);
        g_state.oled_ok = 1;
        pthread_mutex_unlock(&g_state.lock);
        app_log("system", "OLED initialized");
        apply_bedtime_brightness();
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
    int last_colon_phase = -1;
    int last_story_phase = -1;
    uint64_t last_diagnostic_refresh_ms = 0;
    while (g_running) {
        check_alarm();
        activate_pending_message_if_due();
        apply_bedtime_brightness();
        enforce_bedtime_music_policy();
        update_led_scene();

        pthread_mutex_lock(&g_state.lock);
        int mode = g_state.display_mode;
        int dirty = g_state.display_dirty;
        g_state.display_dirty = 0;
        int oled_ok = g_state.oled_ok;
        pthread_mutex_unlock(&g_state.lock);

        int marquee_active = 0;
        if (oled_ok) {
            if (mode == 2) {
                time_t until;
                char msg_image_file[IMAGE_FILE_MAX];
                int msg_image_bedtime;
                char msg_text[192];
                pthread_mutex_lock(&g_state.lock);
                until = g_state.message_until;
                safe_str(msg_image_file, sizeof(msg_image_file), g_state.message_image_file);
                msg_image_bedtime = g_state.message_image_bedtime;
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
                    draw_message_screen_file(msg_image_file, msg_image_bedtime, msg_text);
                }
            }

            if (mode == 3) {
                time_t diagnostic_until;
                pthread_mutex_lock(&g_state.lock);
                diagnostic_until = g_state.diagnostic_until;
                pthread_mutex_unlock(&g_state.lock);
                if (diagnostic_until <= 0 || time(NULL) >= diagnostic_until) {
                    close_diagnostic_screen();
                    pthread_mutex_lock(&g_state.lock);
                    mode = g_state.display_mode;
                    pthread_mutex_unlock(&g_state.lock);
                    last_mode = -1;
                } else {
                    uint64_t diagnostic_now_ms = monotonic_millis();
                    if (dirty || mode != last_mode ||
                        diagnostic_now_ms - last_diagnostic_refresh_ms >= DIAGNOSTIC_REFRESH_MS) {
                        draw_diagnostic_screen();
                        last_diagnostic_refresh_ms = diagnostic_now_ms;
                    }
                }
            }

            if (mode == 0) {
                time_t now = time(NULL);
                struct tm tmv;
                localtime_r(&now, &tmv);
                int colon_phase = clock_colon_blink_phase();
                int story_phase = story_display_phase();
                marquee_active = song_metadata_marquee_active();
                if (dirty || mode != last_mode || tmv.tm_min != last_min || colon_phase != last_colon_phase ||
                    story_phase != last_story_phase || clock_image_refresh_due()) {
                    last_min = tmv.tm_min;
                    last_colon_phase = colon_phase;
                    last_story_phase = story_phase;
                    draw_clock_screen();
                } else if (marquee_active) {
                    refresh_clock_footer();
                }
            } else if (mode == 1) {
                if (dirty || mode != last_mode) {
                    oled_clear_fb(0);
                    oled_flush_full();
                }
            }
        }

        last_mode = mode;
        usleep(marquee_active ? SONG_SCROLL_FRAME_US : 250000u);
    }

    if (touch_thread_started) pthread_join(touch_thread, NULL);
    touch_close();
    (void)audio_force_stop_and_wait(3000u);
    mp_led_shutdown();
    pthread_mutex_lock(&g_font.lock);
    font_cache_close_locked();
    pthread_mutex_unlock(&g_font.lock);
    oled_close();
    mpg123_exit();
    return 0;
}
