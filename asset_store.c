#define _GNU_SOURCE
#include "asset_store.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <png.h>
#include <mpg123.h>
#include <lame/lame.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

int mp_asset_ensure_dir(const char *path) {
    if (!path || !*path) return -1;
    char tmp[512];
    mp_safe_str(tmp, sizeof(tmp), path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int has_ext(const char *name, const char *ext) {
    const char *dot = strrchr(name ? name : "", '.');
    return dot && strcasecmp(dot, ext) == 0;
}

int mp_asset_has_png_ext(const char *name) { return has_ext(name, ".png"); }
int mp_asset_has_raw_ext(const char *name) { return has_ext(name, ".raw"); }
int mp_asset_has_mp3_ext(const char *name) { return has_ext(name, ".mp3"); }
int mp_asset_has_font_ext(const char *name) {
    return has_ext(name, ".ttf") || has_ext(name, ".otf");
}

int mp_asset_safe_filename(const char *name) {
    if (!name || !*name || strlen(name) >= 240) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!(isalnum(*p) || *p == '.' || *p == '_' || *p == '-')) return 0;
    }
    return 1;
}

int mp_asset_safe_image_filename(const char *name) {
    return mp_asset_safe_filename(name) && mp_asset_has_raw_ext(name);
}

void mp_asset_sanitize_filename(const char *input, char *output, size_t output_len, const char *fallback) {
    if (!output || output_len == 0) return;
    output[0] = '\0';
    if (!fallback || !*fallback) fallback = "upload.bin";
    const char *base = input && *input ? input : fallback;
    for (const char *p = base; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)base; *p && j + 1 < output_len; p++) {
        output[j++] = (isalnum(*p) || *p == '.' || *p == '_' || *p == '-') ? (char)*p : '_';
    }
    output[j] = '\0';
    if (!output[0] || strcmp(output, ".") == 0 || strcmp(output, "..") == 0)
        mp_safe_str(output, output_len, fallback);
}

void mp_asset_make_raw_image_name(const char *upload_name, char *output, size_t output_len) {
    char clean[MP_ASSET_NAME_MAX];
    mp_asset_sanitize_filename(upload_name, clean, sizeof(clean), "image.png");
    char *dot = strrchr(clean, '.');
    if (dot) *dot = '\0';
    if (!clean[0]) mp_safe_str(clean, sizeof(clean), "image");
    size_t max_base = output_len > 5 ? output_len - 5 : 0;
    if (strlen(clean) > max_base) clean[max_base] = '\0';
    snprintf(output, output_len, "%s.raw", clean);
}

void mp_asset_image_title(const char *file, char *output, size_t output_len) {
    if (!output || output_len == 0) return;
    output[0] = '\0';
    char tmp[MP_ASSET_NAME_MAX];
    mp_safe_str(tmp, sizeof(tmp), file && *file ? file : "Image");
    char *dot = strrchr(tmp, '.');
    if (dot) *dot = '\0';
    size_t j = 0;
    int cap_next = 1;
    for (char *p = tmp; *p && j + 1 < output_len; p++) {
        char ch = *p;
        if (ch == '_' || ch == '-') {
            if (j > 0 && output[j - 1] != ' ') output[j++] = ' ';
            cap_next = 1;
            continue;
        }
        if (cap_next && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        output[j++] = ch;
        cap_next = 0;
    }
    while (j > 0 && output[j - 1] == ' ') j--;
    output[j] = '\0';
    if (!output[0]) mp_safe_str(output, output_len, "Image");
}

int mp_asset_image_source_path(const char *raw_file, int bedtime, char *output, size_t output_len) {
    if (!output || output_len == 0 || !mp_asset_safe_image_filename(raw_file)) return -1;
    char png[MP_ASSET_NAME_MAX];
    mp_safe_str(png, sizeof(png), raw_file);
    char *dot = strrchr(png, '.');
    if (!dot) return -1;
    mp_safe_str(dot, (size_t)(png + sizeof(png) - dot), ".png");
    int n = snprintf(output, output_len, "%s/%s", bedtime ? MP_BEDTIME_IMAGE_DIR : MP_IMAGE_DIR, png);
    return n > 0 && (size_t)n < output_len ? 0 : -1;
}

static int asset_matches(const char *dir, const char *name, int kind) {
    if (!mp_asset_safe_filename(name)) return 0;
    char path[768];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if (kind == MP_ASSET_SCAN_IMAGE_RAW)
        return mp_asset_safe_image_filename(name) && st.st_size == MP_IMAGE_RAW_BYTES;
    if (kind == MP_ASSET_SCAN_MUSIC_MP3) return mp_asset_has_mp3_ext(name);
    if (kind == MP_ASSET_SCAN_FONT) return mp_asset_has_font_ext(name);
    return 0;
}

static int compare_names(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}

int mp_asset_scan(const char *dir, int kind, char files[][MP_ASSET_NAME_MAX], int max_files) {
    if (!dir || !files || max_files <= 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < max_files) {
        if (!asset_matches(dir, de->d_name, kind)) continue;
        mp_safe_str(files[count++], MP_ASSET_NAME_MAX, de->d_name);
    }
    closedir(d);
    qsort(files, (size_t)count, sizeof(files[0]), compare_names);
    return count;
}

static int fsync_parent_dir(const char *path) {
    char parent[768];
    mp_safe_str(parent, sizeof(parent), path);
    char *slash = strrchr(parent, '/');
    if (!slash) return -1;
    if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

static int atomic_copy_file(const char *source, const char *target, mode_t mode) {
    int in = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0) return -1;
    char temp[800];
    int n = snprintf(temp, sizeof(temp), "%s.tmp.XXXXXX", target);
    if (n <= 0 || (size_t)n >= sizeof(temp)) { close(in); return -1; }
    int out = mkstemp(temp);
    if (out < 0) { close(in); return -1; }
    (void)fchmod(out, mode);
    unsigned char buf[32768];
    int rc = 0;
    for (;;) {
        ssize_t got = read(in, buf, sizeof(buf));
        if (got < 0) { if (errno == EINTR) continue; rc = -1; break; }
        if (got == 0) break;
        if (mp_write_full(out, buf, (size_t)got) != 0) { rc = -1; break; }
    }
    if (rc == 0 && fsync(out) != 0) rc = -1;
    if (close(out) != 0) rc = -1;
    close(in);
    if (rc == 0 && rename(temp, target) != 0) rc = -1;
    if (rc == 0) (void)fsync_parent_dir(target);
    if (rc != 0) unlink(temp);
    return rc;
}

static int atomic_write_file(const char *target, const void *data, size_t length, mode_t mode) {
    char temp[800];
    int n = snprintf(temp, sizeof(temp), "%s.tmp.XXXXXX", target);
    if (n <= 0 || (size_t)n >= sizeof(temp)) return -1;
    int fd = mkstemp(temp);
    if (fd < 0) return -1;
    (void)fchmod(fd, mode);
    int rc = mp_write_full(fd, data, length);
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    if (rc == 0 && rename(temp, target) != 0) rc = -1;
    if (rc == 0) (void)fsync_parent_dir(target);
    if (rc != 0) unlink(temp);
    return rc;
}

int mp_asset_move_file(const char *source, const char *target) {
    if (atomic_copy_file(source, target, 0640) != 0) return -1;
    return unlink(source) == 0 || errno == ENOENT ? 0 : -1;
}

static int convert_png_to_raw(const char *png_path, const char *raw_path) {
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, png_path)) return -1;
    if (image.width == 0 || image.height == 0 || image.width > MP_PNG_MAX_WIDTH ||
        image.height > MP_PNG_MAX_HEIGHT ||
        (uint64_t)image.width * (uint64_t)image.height > MP_PNG_MAX_PIXELS) {
        png_image_free(&image);
        return -1;
    }
    image.format = PNG_FORMAT_RGBA;
    size_t stride = PNG_IMAGE_ROW_STRIDE(image);
    size_t size = PNG_IMAGE_SIZE(image);
    unsigned char *rgba = malloc(size);
    if (!rgba) { png_image_free(&image); return -1; }
    if (!png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        free(rgba);
        png_image_free(&image);
        return -1;
    }

    int src_w = (int)image.width;
    int src_h = (int)image.height;
    int crop = src_w < src_h ? src_w : src_h;
    int crop_x = (src_w - crop) / 2;
    int crop_y = (src_h - crop) / 2;
    unsigned char raw[MP_IMAGE_RAW_BYTES];
    memset(raw, 0, sizeof(raw));
    for (int y = 0; y < MP_IMAGE_HEIGHT; y++) {
        for (int x = 0; x < MP_IMAGE_WIDTH; x += 2) {
            unsigned char packed = 0;
            for (int p = 0; p < 2; p++) {
                int sx = crop_x + ((x + p) * crop) / MP_IMAGE_WIDTH;
                int sy = crop_y + (y * crop) / MP_IMAGE_HEIGHT;
                unsigned char *px = rgba + (size_t)sy * stride + (size_t)sx * 4;
                int a = px[3];
                int r = (px[0] * a + 128) >> 8;
                int g = (px[1] * a + 128) >> 8;
                int b = (px[2] * a + 128) >> 8;
                int gray4 = ((r * 77 + g * 150 + b * 29) >> 8) >> 4;
                if (p == 0) packed |= (unsigned char)((gray4 & 0x0f) << 4);
                else packed |= (unsigned char)(gray4 & 0x0f);
            }
            raw[(y * MP_IMAGE_WIDTH + x) / 2] = packed;
        }
    }
    free(rgba);
    png_image_free(&image);

    return atomic_write_file(raw_path, raw, sizeof(raw), 0640);
}

int mp_asset_save_image_png(const char *png_path, const char *upload_name, int bedtime, char *raw_name, size_t raw_name_len) {
    if (!png_path || !raw_name || raw_name_len == 0) return -1;
    mp_asset_make_raw_image_name(upload_name, raw_name, raw_name_len);
    if (!mp_asset_safe_image_filename(raw_name)) return -1;
    const char *dir = bedtime ? MP_BEDTIME_IMAGE_DIR : MP_IMAGE_DIR;
    if (mp_asset_ensure_dir(dir) != 0) return -1;
    char raw_path[768];
    snprintf(raw_path, sizeof(raw_path), "%s/%s", dir, raw_name);
    if (convert_png_to_raw(png_path, raw_path) != 0) return -1;
    char source_path[768];
    if (mp_asset_image_source_path(raw_name, bedtime, source_path, sizeof(source_path)) != 0 ||
        atomic_copy_file(png_path, source_path, 0640) != 0) {
        unlink(raw_path);
        return -1;
    }
    return 0;
}

int mp_asset_validate_font(const char *path) {
    FT_Library library = NULL;
    FT_Face face = NULL;
    if (FT_Init_FreeType(&library) != 0) return -1;
    int rc = FT_New_Face(library, path, 0, &face) == 0 ? 0 : -1;
    if (face) FT_Done_Face(face);
    FT_Done_FreeType(library);
    return rc;
}


static pthread_once_t g_mpg123_once = PTHREAD_ONCE_INIT;
static int g_mpg123_ready = 0;

static void init_mpg123_once(void) {
    g_mpg123_ready = mpg123_init() == MPG123_OK;
}

int mp_asset_read_mp3_info(const char *path, struct mp_audio_info *info) {
    if (!path || !info) return -1;
    memset(info, 0, sizeof(*info));
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
        info->file_size_bytes = (uint64_t)st.st_size;

    pthread_once(&g_mpg123_once, init_mpg123_once);
    if (!g_mpg123_ready) return -1;
    int error = MPG123_OK;
    mpg123_handle *handle = mpg123_new(NULL, &error);
    if (!handle) return -1;
    int rc = -1;
    if (mpg123_open(handle, path) == MPG123_OK) {
        long rate = 0;
        int channels = 0, encoding = 0;
        if (mpg123_getformat(handle, &rate, &channels, &encoding) == MPG123_OK && rate > 0 && channels > 0) {
            info->sample_rate_hz = rate;
            info->channels = channels;
            struct mpg123_frameinfo frame;
            memset(&frame, 0, sizeof(frame));
            if (mpg123_info(handle, &frame) == MPG123_OK) {
                info->bitrate_kbps = frame.bitrate;
                info->vbr_mode = frame.vbr == MPG123_VBR ? 1 : frame.vbr == MPG123_ABR ? 2 : 0;
                info->layer = frame.layer;
            }
            off_t samples = mpg123_length(handle);
            if (samples > 0) {
                info->duration_seconds = (double)samples / (double)rate;
            } else if (info->bitrate_kbps > 0 && info->file_size_bytes > 0) {
                info->duration_seconds = ((double)info->file_size_bytes * 8.0) /
                                         ((double)info->bitrate_kbps * 1000.0);
                info->duration_estimated = 1;
            }
            if (info->bitrate_kbps <= 0 && info->duration_seconds > 0.0 && info->file_size_bytes > 0)
                info->bitrate_kbps = (int)((double)info->file_size_bytes * 8.0 /
                                           info->duration_seconds / 1000.0 + 0.5);
            rc = 0;
        }
    }
    mpg123_close(handle);
    mpg123_delete(handle);
    return rc;
}

int mp_asset_validate_mp3(const char *path) {
    pthread_once(&g_mpg123_once, init_mpg123_once);
    if (!g_mpg123_ready || !path) return -1;
    int error = MPG123_OK;
    mpg123_handle *handle = mpg123_new(NULL, &error);
    if (!handle) return -1;
    int rc = -1;
    if (mpg123_open(handle, path) == MPG123_OK) {
        long rate = 0;
        int channels = 0, encoding = 0;
        if (mpg123_getformat(handle, &rate, &channels, &encoding) == MPG123_OK &&
            rate > 0 && channels > 0) {
            unsigned char sample[4096];
            size_t done = 0;
            int read_rc = mpg123_read(handle, sample, sizeof(sample), &done);
            if (done > 0 || read_rc == MPG123_OK || read_rc == MPG123_DONE) rc = 0;
        }
    }
    mpg123_close(handle);
    mpg123_delete(handle);
    return rc;
}


static void audio_error(char *error, size_t error_len, const char *message) {
    if (!error || error_len == 0) return;
    mp_safe_str(error, error_len, message ? message : "audio processing failed");
}

static uint32_t id3_synchsafe32(const unsigned char value[4]) {
    return ((uint32_t)(value[0] & 0x7f) << 21) |
           ((uint32_t)(value[1] & 0x7f) << 14) |
           ((uint32_t)(value[2] & 0x7f) << 7) |
           (uint32_t)(value[3] & 0x7f);
}

static int copy_id3v2_prefix(FILE *source, FILE *output) {
    unsigned char header[10];
    if (fseek(source, 0, SEEK_SET) != 0 || fread(header, 1, sizeof(header), source) != sizeof(header)) {
        clearerr(source);
        return 0;
    }
    if (memcmp(header, "ID3", 3) != 0 || header[3] < 2 || header[3] > 4) return 0;
    uint32_t body_size = id3_synchsafe32(header + 6);
    uint64_t total = 10u + (uint64_t)body_size + ((header[5] & 0x10u) ? 10u : 0u);
    if (total > 4u * 1024u * 1024u + 20u) return 0;
    if (fseek(source, 0, SEEK_SET) != 0) return -1;
    unsigned char buffer[8192];
    uint64_t left = total;
    while (left > 0) {
        size_t wanted = left > sizeof(buffer) ? sizeof(buffer) : (size_t)left;
        size_t got = fread(buffer, 1, wanted, source);
        if (got != wanted || fwrite(buffer, 1, got, output) != got) return -1;
        left -= got;
    }
    return 1;
}

static int read_id3v1_suffix(FILE *source, unsigned char tag[128]) {
    if (fseek(source, -128, SEEK_END) != 0) {
        clearerr(source);
        return 0;
    }
    if (fread(tag, 1, 128, source) != 128) {
        clearerr(source);
        return 0;
    }
    return memcmp(tag, "TAG", 3) == 0;
}

static int write_bytes(FILE *file, const unsigned char *data, size_t length) {
    return length == 0 || fwrite(data, 1, length, file) == length ? 0 : -1;
}

int mp_asset_optimize_mp3(const char *source_path, const char *output_path,
                          const struct mp_audio_optimize_settings *settings,
                          mp_audio_progress_callback progress, void *progress_userdata,
                          char *error, size_t error_len) {
    if (!source_path || !output_path || !settings) {
        audio_error(error, error_len, "invalid audio processing request");
        return -1;
    }
    if (!(settings->bitrate_kbps == 64 || settings->bitrate_kbps == 96 ||
          settings->bitrate_kbps == 128 || settings->bitrate_kbps == 160) ||
        !(settings->sample_rate_hz == 32000 || settings->sample_rate_hz == 44100) ||
        settings->lowpass_hz < 10000 || settings->lowpass_hz > 19000 ||
        settings->quality < 0 || settings->quality > 9) {
        audio_error(error, error_len, "unsupported audio processing settings");
        return -1;
    }

    pthread_once(&g_mpg123_once, init_mpg123_once);
    if (!g_mpg123_ready) {
        audio_error(error, error_len, "MP3 decoder unavailable");
        return -1;
    }

    int mpg_error = MPG123_OK;
    mpg123_handle *decoder = mpg123_new(NULL, &mpg_error);
    lame_t encoder = NULL;
    FILE *source = NULL;
    FILE *output = NULL;
    unsigned char *pcm = NULL;
    unsigned char *encoded = NULL;
    int result = -1;

    source = fopen(source_path, "rb");
    if (!source) {
        audio_error(error, error_len, "source MP3 could not be opened");
        goto cleanup;
    }
    output = fopen(output_path, "w+b");
    if (!output) {
        audio_error(error, error_len, "processed MP3 could not be created");
        goto cleanup;
    }

    unsigned char id3v1[128];
    int has_id3v1 = read_id3v1_suffix(source, id3v1);
    if (copy_id3v2_prefix(source, output) < 0) {
        audio_error(error, error_len, "MP3 metadata could not be copied");
        goto cleanup;
    }

    if (!decoder || mpg123_open(decoder, source_path) != MPG123_OK) {
        audio_error(error, error_len, "source MP3 could not be decoded");
        goto cleanup;
    }
    long input_rate = 0;
    int channels = 0, encoding = 0;
    if (mpg123_getformat(decoder, &input_rate, &channels, &encoding) != MPG123_OK ||
        input_rate <= 0 || (channels != 1 && channels != 2)) {
        audio_error(error, error_len, "source MP3 format is unsupported");
        goto cleanup;
    }
    if (!(encoding & MPG123_ENC_SIGNED_16)) {
        mpg123_format_none(decoder);
        if (mpg123_format(decoder, input_rate, channels, MPG123_ENC_SIGNED_16) != MPG123_OK) {
            audio_error(error, error_len, "source MP3 could not be converted to PCM");
            goto cleanup;
        }
    }

    encoder = lame_init();
    if (!encoder) {
        audio_error(error, error_len, "MP3 encoder could not be initialized");
        goto cleanup;
    }
    off_t total_samples = mpg123_length(decoder);
    if (total_samples > 0) (void)lame_set_num_samples(encoder, (unsigned long)total_samples);
    if (lame_set_num_channels(encoder, channels) < 0 ||
        lame_set_in_samplerate(encoder, (int)input_rate) < 0 ||
        lame_set_out_samplerate(encoder, (int)settings->sample_rate_hz) < 0 ||
        lame_set_mode(encoder, MONO) < 0 ||
        lame_set_brate(encoder, settings->bitrate_kbps) < 0 ||
        lame_set_quality(encoder, settings->quality) < 0 ||
        lame_set_lowpassfreq(encoder, settings->lowpass_hz) < 0) {
        audio_error(error, error_len, "MP3 encoder settings were rejected");
        goto cleanup;
    }
    (void)lame_set_bWriteVbrTag(encoder, 1);
    lame_set_write_id3tag_automatic(encoder, 0);
    if (lame_init_params(encoder) < 0) {
        audio_error(error, error_len, "MP3 encoder settings could not be applied");
        goto cleanup;
    }

    const size_t frames_per_chunk = 8192;
    size_t pcm_bytes = frames_per_chunk * (size_t)channels * sizeof(short);
    size_t encoded_bytes = (size_t)(1.25 * (double)frames_per_chunk) + 7200u;
    pcm = malloc(pcm_bytes);
    encoded = malloc(encoded_bytes);
    if (!pcm || !encoded) {
        audio_error(error, error_len, "not enough memory to process MP3");
        goto cleanup;
    }

    if (progress) progress(1, progress_userdata);
    uint64_t processed_samples = 0;
    for (;;) {
        size_t done = 0;
        int read_result = mpg123_read(decoder, pcm, pcm_bytes, &done);
        if (done > 0) {
            int frames = (int)(done / ((size_t)channels * sizeof(short)));
            int produced = channels == 2
                ? lame_encode_buffer_interleaved(encoder, (short *)pcm, frames, encoded, (int)encoded_bytes)
                : lame_encode_buffer(encoder, (short *)pcm, (short *)pcm, frames, encoded, (int)encoded_bytes);
            if (produced < 0 || write_bytes(output, encoded, (size_t)(produced > 0 ? produced : 0)) != 0) {
                audio_error(error, error_len, "processed MP3 could not be written");
                goto cleanup;
            }
            processed_samples += (uint64_t)frames;
            if (progress && total_samples > 0) {
                unsigned int percent = (unsigned int)((processed_samples * 94u) / (uint64_t)total_samples);
                if (percent > 94u) percent = 94u;
                progress(percent < 1u ? 1u : percent, progress_userdata);
            }
        }
        if (read_result == MPG123_DONE) break;
        if (read_result == MPG123_NEW_FORMAT) continue;
        if (read_result != MPG123_OK) {
            audio_error(error, error_len, "source MP3 ended unexpectedly");
            goto cleanup;
        }
    }

    int final_bytes = lame_encode_flush(encoder, encoded, (int)encoded_bytes);
    if (final_bytes < 0 || write_bytes(output, encoded, (size_t)(final_bytes > 0 ? final_bytes : 0)) != 0) {
        audio_error(error, error_len, "MP3 encoder could not finish the file");
        goto cleanup;
    }
    if (fflush(output) != 0) {
        audio_error(error, error_len, "processed MP3 could not be flushed");
        goto cleanup;
    }
    lame_mp3_tags_fid(encoder, output);
    if (fseek(output, 0, SEEK_END) != 0 ||
        (has_id3v1 && fwrite(id3v1, 1, sizeof(id3v1), output) != sizeof(id3v1)) ||
        fflush(output) != 0 || fsync(fileno(output)) != 0) {
        audio_error(error, error_len, "processed MP3 could not be finalized");
        goto cleanup;
    }
    if (progress) progress(98, progress_userdata);
    result = 0;

cleanup:
    free(encoded);
    free(pcm);
    if (encoder) lame_close(encoder);
    if (decoder) {
        mpg123_close(decoder);
        mpg123_delete(decoder);
    }
    if (output && fclose(output) != 0 && result == 0) result = -1;
    if (source) fclose(source);
    if (result != 0) unlink(output_path);
    return result;
}

int mp_asset_has_free_space(const char *path, uint64_t needed_bytes, uint64_t reserve_bytes) {
    struct statvfs info;
    if (!path || statvfs(path, &info) != 0) return 0;
    uint64_t available = (uint64_t)info.f_bavail * (uint64_t)info.f_frsize;
    return available >= needed_bytes && available - needed_bytes >= reserve_bytes;
}

uint64_t mp_asset_directory_bytes(const char *dir, int kind) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    uint64_t total = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!asset_matches(dir, de->d_name, kind)) continue;
        char path[768];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
            total += (uint64_t)st.st_size;
    }
    closedir(d);
    return total;
}

int mp_asset_delete_file(const char *dir, const char *file) {
    if (!dir || !mp_asset_safe_filename(file)) return -1;
    char path[768];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (n <= 0 || (size_t)n >= sizeof(path)) return -1;
    return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
}

static int delete_matching(const char *dir, int images, int music) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!mp_asset_safe_filename(de->d_name)) continue;
        int match = images ? (mp_asset_has_raw_ext(de->d_name) || mp_asset_has_png_ext(de->d_name))
                          : (music && mp_asset_has_mp3_ext(de->d_name));
        if (!match) continue;
        if (mp_asset_delete_file(dir, de->d_name) == 0) count++;
    }
    closedir(d);
    return count;
}

int mp_asset_delete_images(int bedtime) {
    return delete_matching(bedtime ? MP_BEDTIME_IMAGE_DIR : MP_IMAGE_DIR, 1, 0);
}

int mp_asset_delete_music(void) {
    return delete_matching(MP_MUSIC_DIR, 0, 1);
}

int mp_asset_delete_stories(void) {
    return delete_matching(MP_STORY_DIR, 0, 1);
}
