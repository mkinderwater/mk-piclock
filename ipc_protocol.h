#ifndef MK_PICLOCK_IPC_PROTOCOL_H
#define MK_PICLOCK_IPC_PROTOCOL_H

#include <stdint.h>

/* Private same-host protocol. Integer fields use native byte order because both
 * endpoints run on the same Linux device and are built from this header. */
#define MP_IPC_MAGIC 0x4D4B5043u /* MKPC */
#define MP_IPC_VERSION 4u
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
    MP_IPC_OP_DISPLAY_FACE = 3,
    MP_IPC_OP_DISPLAY_MESSAGE = 4,
    MP_IPC_OP_MESSAGE_LIMITS = 5,
    MP_IPC_OP_MESSAGE_FIT = 6,
    MP_IPC_OP_CONFIG_ALARM = 7,
    MP_IPC_OP_CONFIG_AUDIO = 8,
    MP_IPC_OP_CONFIG_PERSONALIZATION = 9,
    MP_IPC_OP_CONFIG_DISPLAY = 10,
    MP_IPC_OP_LOGS_GET = 11,
    MP_IPC_OP_LOGS_CLEAR = 12,
    MP_IPC_OP_ASSET_EVENT = 13,
    MP_IPC_OP_ASSET_STATE = 14,
    MP_IPC_OP_PING = 15
};

enum mp_ipc_display_action_code {
    MP_IPC_ACTION_CLOCK = 1,
    MP_IPC_ACTION_CLEAR = 2,
    MP_IPC_ACTION_STOP_AUDIO = 3,
    MP_IPC_ACTION_PLAY_MUSIC = 4
};

enum mp_ipc_asset_kind {
    MP_IPC_ASSET_FACE = 1,
    MP_IPC_ASSET_BEDTIME_FACE = 2,
    MP_IPC_ASSET_MUSIC = 3,
    MP_IPC_ASSET_FONT = 4
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

struct mp_ipc_display_face {
    int32_t id;
    char file[128];
};

struct mp_ipc_display_message {
    int32_t face_id;
    uint16_t delay_seconds;
    uint16_t reserved;
    char face_file[128];
    char text[192];
};

struct mp_ipc_message_fit {
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
    MP_IPC_AUDIO_SHOW_METADATA = 1u << 1
};

struct mp_ipc_audio_config {
    uint8_t present_mask;
    uint8_t global_volume;
    uint8_t show_song_metadata;
    uint8_t reserved;
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
    MP_IPC_DISPLAY_BEDTIME_END = 1u << 7
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
    uint8_t reserved;
    char oled_font_file[128];
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
    int32_t builtin_font;
    int32_t font_size;
    char current_music[256];
    char selected_font[128];
};

#endif
