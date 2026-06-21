#define _GNU_SOURCE
#include "asset_store.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <png.h>
#include <mpg123.h>
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

int mp_asset_safe_face_filename(const char *name) {
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

void mp_asset_make_raw_face_name(const char *upload_name, char *output, size_t output_len) {
    char clean[MP_ASSET_NAME_MAX];
    mp_asset_sanitize_filename(upload_name, clean, sizeof(clean), "face.png");
    char *dot = strrchr(clean, '.');
    if (dot) *dot = '\0';
    if (!clean[0]) mp_safe_str(clean, sizeof(clean), "face");
    size_t max_base = output_len > 5 ? output_len - 5 : 0;
    if (strlen(clean) > max_base) clean[max_base] = '\0';
    snprintf(output, output_len, "%s.raw", clean);
}

void mp_asset_face_title(const char *file, char *output, size_t output_len) {
    if (!output || output_len == 0) return;
    output[0] = '\0';
    char tmp[MP_ASSET_NAME_MAX];
    mp_safe_str(tmp, sizeof(tmp), file && *file ? file : "Face");
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
    if (!output[0]) mp_safe_str(output, output_len, "Face");
}

int mp_asset_face_source_path(const char *raw_file, int bedtime, char *output, size_t output_len) {
    if (!output || output_len == 0 || !mp_asset_safe_face_filename(raw_file)) return -1;
    char png[MP_ASSET_NAME_MAX];
    mp_safe_str(png, sizeof(png), raw_file);
    char *dot = strrchr(png, '.');
    if (!dot) return -1;
    mp_safe_str(dot, (size_t)(png + sizeof(png) - dot), ".png");
    int n = snprintf(output, output_len, "%s/%s", bedtime ? MP_BEDTIME_FACE_DIR : MP_FACE_DIR, png);
    return n > 0 && (size_t)n < output_len ? 0 : -1;
}

static int asset_matches(const char *dir, const char *name, int kind) {
    if (!mp_asset_safe_filename(name)) return 0;
    char path[768];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if (kind == MP_ASSET_SCAN_FACE_RAW)
        return mp_asset_safe_face_filename(name) && st.st_size == MP_FACE_RAW_BYTES;
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
    unsigned char raw[MP_FACE_RAW_BYTES];
    memset(raw, 0, sizeof(raw));
    for (int y = 0; y < MP_FACE_HEIGHT; y++) {
        for (int x = 0; x < MP_FACE_WIDTH; x += 2) {
            unsigned char packed = 0;
            for (int p = 0; p < 2; p++) {
                int sx = crop_x + ((x + p) * crop) / MP_FACE_WIDTH;
                int sy = crop_y + (y * crop) / MP_FACE_HEIGHT;
                unsigned char *px = rgba + (size_t)sy * stride + (size_t)sx * 4;
                int a = px[3];
                int r = (px[0] * a + 128) >> 8;
                int g = (px[1] * a + 128) >> 8;
                int b = (px[2] * a + 128) >> 8;
                int gray4 = ((r * 77 + g * 150 + b * 29) >> 8) >> 4;
                if (p == 0) packed |= (unsigned char)((gray4 & 0x0f) << 4);
                else packed |= (unsigned char)(gray4 & 0x0f);
            }
            raw[(y * MP_FACE_WIDTH + x) / 2] = packed;
        }
    }
    free(rgba);
    png_image_free(&image);

    return atomic_write_file(raw_path, raw, sizeof(raw), 0640);
}

int mp_asset_save_face_png(const char *png_path, const char *upload_name, int bedtime, char *raw_name, size_t raw_name_len) {
    if (!png_path || !raw_name || raw_name_len == 0) return -1;
    mp_asset_make_raw_face_name(upload_name, raw_name, raw_name_len);
    if (!mp_asset_safe_face_filename(raw_name)) return -1;
    const char *dir = bedtime ? MP_BEDTIME_FACE_DIR : MP_FACE_DIR;
    if (mp_asset_ensure_dir(dir) != 0) return -1;
    char raw_path[768];
    snprintf(raw_path, sizeof(raw_path), "%s/%s", dir, raw_name);
    if (convert_png_to_raw(png_path, raw_path) != 0) return -1;
    char source_path[768];
    if (mp_asset_face_source_path(raw_name, bedtime, source_path, sizeof(source_path)) != 0 ||
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

static int delete_matching(const char *dir, int faces, int music) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!mp_asset_safe_filename(de->d_name)) continue;
        int match = faces ? (mp_asset_has_raw_ext(de->d_name) || mp_asset_has_png_ext(de->d_name))
                          : (music && mp_asset_has_mp3_ext(de->d_name));
        if (!match) continue;
        if (mp_asset_delete_file(dir, de->d_name) == 0) count++;
    }
    closedir(d);
    return count;
}

int mp_asset_delete_faces(int bedtime) {
    return delete_matching(bedtime ? MP_BEDTIME_FACE_DIR : MP_FACE_DIR, 1, 0);
}

int mp_asset_delete_music(void) {
    return delete_matching(MP_MUSIC_DIR, 0, 1);
}
