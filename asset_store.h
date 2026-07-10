#ifndef MK_PICLOCK_ASSET_STORE_H
#define MK_PICLOCK_ASSET_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "asset_format.h"

#ifndef MP_APP_ROOT
#define MP_APP_ROOT "/opt/mk-piclock"
#endif
#define MP_IMAGE_DIR MP_APP_ROOT "/assets/images"
#define MP_BEDTIME_IMAGE_DIR MP_APP_ROOT "/assets/bedtime-images"
#define MP_MUSIC_DIR MP_APP_ROOT "/assets/music"
#define MP_MUSIC_PROCESS_DIR MP_MUSIC_DIR "/.processing"
#define MP_STORY_DIR MP_APP_ROOT "/assets/stories"
#define MP_FONT_DIR MP_APP_ROOT "/assets/fonts"
#define MP_ASSET_NAME_MAX 256
#define MP_ASSET_LIST_MAX 256
#define MP_IMAGE_UPLOAD_MAX_BYTES (8u * 1024u * 1024u)
#define MP_FONT_UPLOAD_MAX_BYTES (16u * 1024u * 1024u)
#define MP_MUSIC_UPLOAD_MAX_BYTES (64u * 1024u * 1024u)
#define MP_STORY_UPLOAD_MAX_BYTES (256u * 1024u * 1024u)
#define MP_PNG_MAX_WIDTH 4096u
#define MP_PNG_MAX_HEIGHT 4096u
#define MP_PNG_MAX_PIXELS (16u * 1024u * 1024u)

enum mp_asset_scan_kind {
    MP_ASSET_SCAN_IMAGE_RAW = 1,
    MP_ASSET_SCAN_MUSIC_MP3 = 2,
    MP_ASSET_SCAN_FONT = 3
};

int mp_asset_ensure_dir(const char *path);
int mp_asset_safe_filename(const char *name);
int mp_asset_safe_image_filename(const char *name);
int mp_asset_has_png_ext(const char *name);
int mp_asset_has_raw_ext(const char *name);
int mp_asset_has_mp3_ext(const char *name);
int mp_asset_has_font_ext(const char *name);
void mp_asset_sanitize_filename(const char *input, char *output, size_t output_len, const char *fallback);
void mp_asset_make_raw_image_name(const char *upload_name, char *output, size_t output_len);
void mp_asset_image_title(const char *file, char *output, size_t output_len);
int mp_asset_image_source_path(const char *raw_file, int bedtime, char *output, size_t output_len);
int mp_asset_scan(const char *dir, int kind, char files[][MP_ASSET_NAME_MAX], int max_files);
int mp_asset_save_image_png(const char *png_path, const char *upload_name, int bedtime, char *raw_name, size_t raw_name_len);
int mp_asset_validate_font(const char *path);


struct mp_audio_optimize_settings {
    int bitrate_kbps;
    long sample_rate_hz;
    int lowpass_hz;
    int quality;
};

typedef void (*mp_audio_progress_callback)(unsigned int percent, void *userdata);

struct mp_audio_info {
    uint64_t file_size_bytes;
    long sample_rate_hz;
    int channels;
    int bitrate_kbps;
    int vbr_mode;
    int layer;
    double duration_seconds;
    int duration_estimated;
};

int mp_asset_validate_mp3(const char *path);
int mp_asset_read_mp3_info(const char *path, struct mp_audio_info *info);
int mp_asset_optimize_mp3(const char *source_path, const char *output_path,
                          const struct mp_audio_optimize_settings *settings,
                          mp_audio_progress_callback progress, void *progress_userdata,
                          char *error, size_t error_len);
int mp_asset_has_free_space(const char *path, uint64_t needed_bytes, uint64_t reserve_bytes);
uint64_t mp_asset_directory_bytes(const char *dir, int kind);
int mp_asset_move_file(const char *source, const char *target);
int mp_asset_delete_file(const char *dir, const char *file);
int mp_asset_delete_images(int bedtime);
int mp_asset_delete_music(void);
int mp_asset_delete_stories(void);

#endif
