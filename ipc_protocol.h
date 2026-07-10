#ifndef MK_PICLOCK_IPC_PROTOCOL_H
#define MK_PICLOCK_IPC_PROTOCOL_H

#include <stdint.h>

/* Private same-host protocol. Integer fields use native byte order because both
 * endpoints run on the same Linux device and are built from this header. */
#define MP_IPC_MAGIC 0x4D4B5043u /* MKPC */
#define MP_IPC_VERSION 16u
#define MP_IPC_MAX_PAYLOAD (128u * 1024u)
#define MP_IPC_MAX_PACKET (sizeof(struct mp_ipc_request_header) + MP_IPC_MAX_PAYLOAD)

struct mp_ipc_request_header {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t payload_len;
};

struct mp_ipc_response_header {
    uint32_t magic;
    uint16_t version;
    uint16_t status;
    uint32_t body_len;
    uint16_t content_type;
    uint16_t reserved;
};

_Static_assert(sizeof(struct mp_ipc_request_header) == 12, "IPC request header must be 12 bytes");
_Static_assert(sizeof(struct mp_ipc_response_header) == 16, "IPC response header must be 16 bytes");

enum mp_ipc_content_type {
    MP_IPC_CONTENT_JSON = 1,
    MP_IPC_CONTENT_TEXT = 2,
    MP_IPC_CONTENT_BINARY = 3
};

enum mp_ipc_opcode {
    MP_IPC_OP_STATUS = 1,
    MP_IPC_OP_DISPLAY_ACTION = 2,
    MP_IPC_OP_DISPLAY_MESSAGE = 4,
    MP_IPC_OP_CONFIG_ALARM = 7,
    MP_IPC_OP_CONFIG_AUDIO = 8,
    MP_IPC_OP_CONFIG_PERSONALIZATION = 9,
    MP_IPC_OP_CONFIG_DISPLAY = 10,
    MP_IPC_OP_LOGS_GET = 11,
    MP_IPC_OP_LOGS_CLEAR = 12,
    MP_IPC_OP_ASSET_EVENT = 13,
    MP_IPC_OP_ASSET_STATE = 14,
    MP_IPC_OP_PING = 15,
    MP_IPC_OP_DISPLAY_PREVIEW = 16,
    MP_IPC_OP_BRIGHTNESS_PREVIEW = 17,
    MP_IPC_OP_MESSAGE_PREVIEW = 18,
    MP_IPC_OP_CONFIG_EXPORT = 19,
    MP_IPC_OP_CONFIG_IMPORT = 20,
    MP_IPC_OP_FACTORY_RESET = 21,
    MP_IPC_OP_CONFIG_LED = 22,
    MP_IPC_OP_LED_PREVIEW = 23,
    MP_IPC_OP_CONFIG_LED_GLOBAL = 24
};

enum mp_ipc_display_action_code {
    MP_IPC_ACTION_CLOCK = 1,
    MP_IPC_ACTION_CLEAR = 2,
    MP_IPC_ACTION_STOP_AUDIO = 3,
    MP_IPC_ACTION_PLAY_MUSIC = 4,
    MP_IPC_ACTION_PLAY_STORY = 5
};

enum mp_ipc_asset_kind {
    MP_IPC_ASSET_IMAGE = 1,
    MP_IPC_ASSET_BEDTIME_IMAGE = 2,
    MP_IPC_ASSET_MUSIC = 3,
    MP_IPC_ASSET_FONT = 4,
    MP_IPC_ASSET_STORY = 5
};

enum mp_ipc_asset_action {
    MP_IPC_ASSET_UPLOADED = 1,
    MP_IPC_ASSET_DELETED = 2,
    MP_IPC_ASSET_DELETED_ALL = 3
};

struct mp_ipc_display_action {
    uint8_t action;
    uint8_t reserved[3];
    char file[256];
};


struct mp_ipc_brightness_preview {
    uint8_t percent;
    uint8_t hold_seconds;
    uint8_t reserved[2];
};

struct mp_ipc_display_message {
    uint64_t scheduled_at;
    uint16_t delay_seconds;
    uint8_t image_bedtime;
    uint8_t notification_sound;
    char image_file[128];
    char text[192];
};


struct mp_ipc_alarm_config {
    uint8_t id;
    uint8_t enabled;
    uint8_t hour;
    uint8_t minute;
    uint8_t weekdays;
    uint8_t start_volume;
    uint8_t end_volume;
    uint8_t reserved;
    char music_file[256];
};

enum mp_ipc_audio_field {
    MP_IPC_AUDIO_GLOBAL_VOLUME = 1u << 0,
    MP_IPC_AUDIO_SHOW_METADATA = 1u << 1,
    MP_IPC_AUDIO_STORY_ENABLED = 1u << 2,
    MP_IPC_AUDIO_STORY_VOLUME = 1u << 3,
    MP_IPC_AUDIO_STORY_MESSAGE = 1u << 4
};

struct mp_ipc_audio_config {
    uint8_t present_mask;
    uint8_t global_volume;
    uint8_t show_song_metadata;
    uint8_t story_enabled;
    uint8_t story_volume;
    uint8_t reserved[3];
    char story_message[64];
};

struct mp_ipc_personalization_config {
    char clock_name[64];
};

enum mp_ipc_display_field {
    MP_IPC_DISPLAY_FONT = 1u << 0,
    MP_IPC_DISPLAY_FONT_SIZE = 1u << 1,
    MP_IPC_DISPLAY_FONT_FILE = 1u << 2,
    MP_IPC_DISPLAY_BEDTIME_ENABLED = 1u << 3,
    MP_IPC_DISPLAY_BEDTIME_DIM = 1u << 4,
    MP_IPC_DISPLAY_CLOCK_MODE = 1u << 5,
    MP_IPC_DISPLAY_BEDTIME_START = 1u << 6,
    MP_IPC_DISPLAY_BEDTIME_END = 1u << 7,
    MP_IPC_DISPLAY_OLED_COLOR = 1u << 8,
    MP_IPC_DISPLAY_BEDTIME_MUSIC = 1u << 9
};

enum mp_oled_color {
    MP_OLED_COLOR_YELLOW = 0,
    MP_OLED_COLOR_GREEN = 1,
    MP_OLED_COLOR_WHITE = 2
};

struct mp_ipc_display_config {
    uint16_t present_mask;
    uint8_t oled_font;
    uint8_t oled_font_size;
    uint8_t bedtime_enabled;
    uint8_t bedtime_dim_percent;
    uint8_t clock_24h_mode;
    uint8_t bedtime_start_hour;
    uint8_t bedtime_start_minute;
    uint8_t bedtime_end_hour;
    uint8_t bedtime_end_minute;
    uint8_t oled_color;
    uint8_t bedtime_music_enabled;
    uint8_t reserved2[2];
    char oled_font_file[128];
};

#define MP_IPC_CONFIG_MAX_BYTES 32768u

struct mp_ipc_config_blob {
    uint32_t length;
    char data[MP_IPC_CONFIG_MAX_BYTES];
};

enum mp_led_scene {
    MP_LED_SCENE_ALARM = 0,
    MP_LED_SCENE_BEDTIME = 1,
    MP_LED_SCENE_MESSAGE = 2,
    MP_LED_SCENE_MUSIC = 3,
    MP_LED_SCENE_DAYTIME = 4,
    MP_LED_SCENE_STORIES = 5,
    MP_LED_SCENE_TOUCH = 6,
    MP_LED_SCENE_COUNT = 7
};

enum mp_led_effect {
    MP_LED_EFFECT_SOLID = 0,
    MP_LED_EFFECT_FADE = 1,
    MP_LED_EFFECT_RAINBOW = 2
};

struct mp_led_profile {
    uint8_t effect;
    uint8_t brightness;
    uint8_t cycle_seconds;
    uint8_t reserved;
    uint8_t red1;
    uint8_t green1;
    uint8_t blue1;
    uint8_t red2;
    uint8_t green2;
    uint8_t blue2;
};

struct mp_led_global_settings {
    uint8_t enabled;
    uint8_t max_brightness;
    uint8_t red_gain;
    uint8_t green_gain;
    uint8_t blue_gain;
    uint8_t idle_off;
    uint8_t bedtime_fade_minutes;
    uint8_t touch_blink_enabled;
    uint8_t touch_blink_brightness;
    uint8_t touch_blink_red;
    uint8_t touch_blink_green;
    uint8_t touch_blink_blue;
};

struct mp_ipc_led_config {
    uint8_t scene;
    uint8_t reserved[3];
    struct mp_led_profile profile;
};

struct mp_ipc_led_preview {
    uint8_t scene;
    uint8_t hold_seconds;
    uint8_t bypass_master;
    uint8_t raw_output;
    struct mp_led_profile profile;
};

struct mp_ipc_led_global_config {
    struct mp_led_global_settings settings;
};

struct mp_ipc_asset_event {
    uint8_t kind;
    uint8_t action;
    uint8_t reserved[2];
    uint32_t count;
    char file[256];
};

struct mp_ipc_asset_state {
    int32_t global_volume;
    int32_t story_volume;
    int32_t story_enabled;
    char story_message[64];
    int32_t builtin_font;
    int32_t font_size;
    char current_music[256];
    char selected_font[128];
};

#endif
