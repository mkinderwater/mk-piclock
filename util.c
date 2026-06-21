#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

void mp_safe_str(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    size_t i = 0;
    while (i + 1 < dst_len && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

int mp_write_full(int fd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

int mp_read_full(int fd, void *buf, size_t len) {
    unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

int mp_send_packet(int fd, const void *header, size_t header_len, const void *payload, size_t payload_len) {
    struct iovec iov[2] = {
        {.iov_base = (void *)header, .iov_len = header_len},
        {.iov_base = (void *)payload, .iov_len = payload_len}
    };
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = iov;
    message.msg_iovlen = payload_len ? 2 : 1;
    size_t total = header_len + payload_len;
    for (;;) {
        ssize_t sent = sendmsg(fd, &message, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        return sent == (ssize_t)total ? 0 : -1;
    }
}

int mp_recv_packet(int fd, void *buffer, size_t capacity, size_t *received) {
    if (!buffer || capacity == 0 || !received) return -1;
    for (;;) {
        ssize_t size = recv(fd, buffer, capacity, MSG_TRUNC);
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0 || (size_t)size > capacity) return -1;
        *received = (size_t)size;
        return 0;
    }
}

int mp_recv_packet_alloc(int fd, void **buffer, size_t max_size, size_t *received) {
    if (!buffer || !received || max_size == 0) return -1;
    *buffer = NULL;
    *received = 0;
    ssize_t packet_size;
    for (;;) {
        packet_size = recv(fd, NULL, 0, MSG_PEEK | MSG_TRUNC);
        if (packet_size < 0 && errno == EINTR) continue;
        break;
    }
    if (packet_size <= 0 || (size_t)packet_size > max_size) return -1;
    void *data = malloc((size_t)packet_size);
    if (!data) return -1;
    size_t actual = 0;
    if (mp_recv_packet(fd, data, (size_t)packet_size, &actual) != 0 || actual != (size_t)packet_size) {
        free(data);
        return -1;
    }
    *buffer = data;
    *received = actual;
    return 0;
}

static int mp_buffer_reserve(struct mp_buffer *buffer, size_t extra) {
    if (!buffer || buffer->failed || buffer->len >= buffer->max ||
        extra > buffer->max - buffer->len - 1) {
        if (buffer) buffer->failed = 1;
        return -1;
    }
    size_t needed = buffer->len + extra + 1;
    if (needed <= buffer->cap) return 0;
    size_t next = buffer->cap ? buffer->cap : 64;
    while (next < needed && next < buffer->max) {
        size_t grown = next > buffer->max / 2 ? buffer->max : next * 2;
        if (grown <= next) break;
        next = grown;
    }
    if (next < needed) {
        buffer->failed = 1;
        return -1;
    }
    char *data = realloc(buffer->data, next);
    if (!data) {
        buffer->failed = 1;
        return -1;
    }
    buffer->data = data;
    buffer->cap = next;
    return 0;
}

int mp_buffer_init(struct mp_buffer *buffer, size_t initial_capacity, size_t max_capacity) {
    if (!buffer || max_capacity < 2) return -1;
    memset(buffer, 0, sizeof(*buffer));
    buffer->max = max_capacity;
    if (initial_capacity < 64) initial_capacity = 64;
    if (initial_capacity > max_capacity) initial_capacity = max_capacity;
    buffer->data = calloc(1, initial_capacity);
    if (!buffer->data) {
        buffer->failed = 1;
        return -1;
    }
    buffer->cap = initial_capacity;
    return 0;
}

void mp_buffer_free(struct mp_buffer *buffer) {
    if (!buffer) return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

int mp_buffer_append_n(struct mp_buffer *buffer, const void *data, size_t length) {
    if (!data && length) return -1;
    if (mp_buffer_reserve(buffer, length) != 0) return -1;
    if (length) memcpy(buffer->data + buffer->len, data, length);
    buffer->len += length;
    buffer->data[buffer->len] = '\0';
    return 0;
}

int mp_buffer_append(struct mp_buffer *buffer, const char *text) {
    if (!text) text = "";
    return mp_buffer_append_n(buffer, text, strlen(text));
}

int mp_buffer_appendf(struct mp_buffer *buffer, const char *format, ...) {
    if (!buffer || !format || buffer->failed) return -1;
    va_list ap;
    va_start(ap, format);
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 || mp_buffer_reserve(buffer, (size_t)needed) != 0) {
        va_end(ap);
        return -1;
    }
    int written = vsnprintf(buffer->data + buffer->len, buffer->cap - buffer->len, format, ap);
    va_end(ap);
    if (written != needed) {
        buffer->failed = 1;
        return -1;
    }
    buffer->len += (size_t)written;
    return 0;
}

int mp_buffer_append_json_string(struct mp_buffer *buffer, const char *value) {
    if (!value) value = "";
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        unsigned char ch = *p;
        if (ch == '"' || ch == '\\') {
            char pair[2] = {'\\', (char)ch};
            if (mp_buffer_append_n(buffer, pair, sizeof(pair)) != 0) return -1;
        } else if (ch == '\n') {
            if (mp_buffer_append(buffer, "\\n") != 0) return -1;
        } else if (ch == '\r') {
            if (mp_buffer_append(buffer, "\\r") != 0) return -1;
        } else if (ch == '\t') {
            if (mp_buffer_append(buffer, "\\t") != 0) return -1;
        } else if (ch < 32) {
            if (mp_buffer_appendf(buffer, "\\u%04x", ch) != 0) return -1;
        } else if (mp_buffer_append_n(buffer, &ch, 1) != 0) {
            return -1;
        }
    }
    return 0;
}

char *mp_buffer_steal(struct mp_buffer *buffer, size_t *length) {
    if (!buffer || buffer->failed || !buffer->data) return NULL;
    char *data = buffer->data;
    if (length) *length = buffer->len;
    buffer->data = NULL;
    buffer->len = buffer->cap = buffer->max = 0;
    return data;
}

void mp_json_escape(char *out, size_t cap, const char *value) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    struct mp_buffer buffer;
    if (mp_buffer_init(&buffer, cap, cap) != 0) return;
    if (mp_buffer_append_json_string(&buffer, value) == 0) mp_safe_str(out, cap, buffer.data);
    mp_buffer_free(&buffer);
}


static uint32_t mp_read_be24(const unsigned char *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t mp_read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t mp_read_synchsafe32(const unsigned char *p) {
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7) | (uint32_t)(p[3] & 0x7f);
}

static int mp_utf8_append(char *out, size_t cap, size_t *used, uint32_t cp) {
    unsigned char bytes[4];
    size_t count = 0;
    if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    if (cp < 0x80) {
        bytes[count++] = (unsigned char)cp;
    } else if (cp < 0x800) {
        bytes[count++] = (unsigned char)(0xc0 | (cp >> 6));
        bytes[count++] = (unsigned char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        bytes[count++] = (unsigned char)(0xe0 | (cp >> 12));
        bytes[count++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        bytes[count++] = (unsigned char)(0x80 | (cp & 0x3f));
    } else {
        bytes[count++] = (unsigned char)(0xf0 | (cp >> 18));
        bytes[count++] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        bytes[count++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        bytes[count++] = (unsigned char)(0x80 | (cp & 0x3f));
    }
    if (*used + count + 1 > cap) return -1;
    memcpy(out + *used, bytes, count);
    *used += count;
    out[*used] = '\0';
    return 0;
}

static void mp_trim_metadata_text(char *text) {
    if (!text) return;
    char *src = text;
    while (*src && isspace((unsigned char)*src)) src++;
    if (src != text) memmove(text, src, strlen(src) + 1);
    size_t len = strlen(text);
    while (len && isspace((unsigned char)text[len - 1])) text[--len] = '\0';
}

static void mp_decode_id3_text(const unsigned char *data, size_t length, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!data || length < 2) return;

    unsigned char encoding = data[0];
    data++;
    length--;
    size_t used = 0;

    if (encoding == 0) {
        for (size_t i = 0; i < length && data[i]; i++) {
            uint32_t cp = data[i];
            if (cp < 32 && cp != '\t') cp = ' ';
            if (mp_utf8_append(out, out_len, &used, cp) != 0) break;
        }
    } else if (encoding == 3) {
        for (size_t i = 0; i < length && data[i] && used + 1 < out_len; i++) {
            unsigned char ch = data[i];
            if (ch < 32 && ch != '\t') ch = ' ';
            out[used++] = (char)ch;
        }
        out[used] = '\0';
    } else if (encoding == 1 || encoding == 2) {
        int big_endian = encoding == 2;
        size_t pos = 0;
        if (encoding == 1 && length >= 2) {
            if (data[0] == 0xfe && data[1] == 0xff) {
                big_endian = 1;
                pos = 2;
            } else if (data[0] == 0xff && data[1] == 0xfe) {
                big_endian = 0;
                pos = 2;
            }
        }
        while (pos + 1 < length) {
            uint16_t first = big_endian
                ? (uint16_t)(((uint16_t)data[pos] << 8) | data[pos + 1])
                : (uint16_t)(((uint16_t)data[pos + 1] << 8) | data[pos]);
            pos += 2;
            if (first == 0) break;
            uint32_t cp = first;
            if (first >= 0xd800 && first <= 0xdbff && pos + 1 < length) {
                uint16_t second = big_endian
                    ? (uint16_t)(((uint16_t)data[pos] << 8) | data[pos + 1])
                    : (uint16_t)(((uint16_t)data[pos + 1] << 8) | data[pos]);
                if (second >= 0xdc00 && second <= 0xdfff) {
                    cp = 0x10000u + (((uint32_t)first - 0xd800u) << 10) + ((uint32_t)second - 0xdc00u);
                    pos += 2;
                }
            }
            if (cp < 32 && cp != '\t') cp = ' ';
            if (mp_utf8_append(out, out_len, &used, cp) != 0) break;
        }
    }
    mp_trim_metadata_text(out);
}

static void mp_decode_id3v1_field(const unsigned char *data, size_t length, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    size_t used = 0;
    while (length && (data[length - 1] == 0 || data[length - 1] == ' ')) length--;
    for (size_t i = 0; i < length; i++) {
        uint32_t cp = data[i];
        if (cp < 32 && cp != '\t') cp = ' ';
        if (mp_utf8_append(out, out_len, &used, cp) != 0) break;
    }
    mp_trim_metadata_text(out);
}

static void mp_parse_id3v2_frames(const unsigned char *tag, size_t length, int version,
                                  unsigned char flags, struct mp_id3_metadata *metadata) {
    size_t pos = 0;
    if ((flags & 0x40) && length >= 4) {
        uint32_t ext_size = version == 4 ? mp_read_synchsafe32(tag) : mp_read_be32(tag);
        size_t skip = version == 3 ? (size_t)ext_size + 4u : (size_t)ext_size;
        if (skip > length) return;
        pos = skip;
    }

    while (pos < length && (!metadata->title[0] || !metadata->artist[0])) {
        char id[5] = "";
        uint32_t frame_size = 0;
        size_t header_size = version == 2 ? 6u : 10u;
        if (length - pos < header_size) break;
        unsigned char format_flags = 0;
        if (version == 2) {
            if (tag[pos] == 0) break;
            memcpy(id, tag + pos, 3);
            frame_size = mp_read_be24(tag + pos + 3);
        } else {
            if (tag[pos] == 0) break;
            memcpy(id, tag + pos, 4);
            frame_size = version == 4 ? mp_read_synchsafe32(tag + pos + 4) : mp_read_be32(tag + pos + 4);
            format_flags = tag[pos + 9];
        }
        if (frame_size == 0 || frame_size > length - pos - header_size) break;
        if ((version == 3 && (format_flags & 0xe0)) ||
            (version == 4 && (format_flags & 0x4f))) {
            pos += header_size + frame_size;
            continue;
        }
        pos += header_size;
        const unsigned char *payload = tag + pos;
        int is_title = (version == 2 && strcmp(id, "TT2") == 0) ||
                       (version >= 3 && strcmp(id, "TIT2") == 0);
        int is_artist = (version == 2 && strcmp(id, "TP1") == 0) ||
                        (version >= 3 && strcmp(id, "TPE1") == 0);
        if (is_title && !metadata->title[0])
            mp_decode_id3_text(payload, frame_size, metadata->title, sizeof(metadata->title));
        else if (is_artist && !metadata->artist[0])
            mp_decode_id3_text(payload, frame_size, metadata->artist, sizeof(metadata->artist));
        pos += frame_size;
    }
}

int mp_read_id3_metadata(const char *path, struct mp_id3_metadata *metadata) {
    if (!path || !metadata) return -1;
    memset(metadata, 0, sizeof(*metadata));
    FILE *file = fopen(path, "rb");
    if (!file) return -1;

    unsigned char header[10];
    size_t got = fread(header, 1, sizeof(header), file);
    if (got == sizeof(header) && memcmp(header, "ID3", 3) == 0 &&
        header[3] >= 2 && header[3] <= 4) {
        uint32_t tag_size = mp_read_synchsafe32(header + 6);
        if (tag_size > 0 && tag_size <= 4u * 1024u * 1024u) {
            unsigned char *tag = malloc(tag_size);
            if (tag && fread(tag, 1, tag_size, file) == tag_size)
                mp_parse_id3v2_frames(tag, tag_size, header[3], header[5], metadata);
            free(tag);
        }
    }

    if (!metadata->title[0] || !metadata->artist[0]) {
        if (fseek(file, -128, SEEK_END) == 0) {
            unsigned char tail[128];
            if (fread(tail, 1, sizeof(tail), file) == sizeof(tail) && memcmp(tail, "TAG", 3) == 0) {
                if (!metadata->title[0])
                    mp_decode_id3v1_field(tail + 3, 30, metadata->title, sizeof(metadata->title));
                if (!metadata->artist[0])
                    mp_decode_id3v1_field(tail + 33, 30, metadata->artist, sizeof(metadata->artist));
            }
        }
    }
    fclose(file);
    return metadata->title[0] || metadata->artist[0] ? 0 : -1;
}

void mp_title_from_filename(const char *filename, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!filename) return;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    mp_safe_str(out, out_len, base);
    char *dot = strrchr(out, '.');
    if (dot && strcasecmp(dot, ".mp3") == 0) *dot = '\0';
    for (char *p = out; *p; p++) {
        if (*p == '_' || *p == '-') *p = ' ';
    }
    mp_trim_metadata_text(out);
}
