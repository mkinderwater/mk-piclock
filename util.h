#ifndef MK_PICLOCK_UTIL_H
#define MK_PICLOCK_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define MP_ID3_TEXT_MAX 160

struct mp_id3_metadata {
    char title[MP_ID3_TEXT_MAX];
    char artist[MP_ID3_TEXT_MAX];
};

struct mp_buffer {
    char *data;
    size_t len;
    size_t cap;
    size_t max;
    int failed;
};

void mp_safe_str(char *dst, size_t dst_len, const char *src);
int mp_write_full(int fd, const void *buf, size_t len);
int mp_read_full(int fd, void *buf, size_t len);
int mp_send_packet(int fd, const void *header, size_t header_len, const void *payload, size_t payload_len);
int mp_recv_packet(int fd, void *buffer, size_t capacity, size_t *received);
int mp_recv_packet_alloc(int fd, void **buffer, size_t max_size, size_t *received);

int mp_buffer_init(struct mp_buffer *buffer, size_t initial_capacity, size_t max_capacity);
void mp_buffer_free(struct mp_buffer *buffer);
int mp_buffer_append(struct mp_buffer *buffer, const char *text);
int mp_buffer_append_n(struct mp_buffer *buffer, const void *data, size_t length);
int mp_buffer_appendf(struct mp_buffer *buffer, const char *format, ...);
int mp_buffer_append_json_string(struct mp_buffer *buffer, const char *value);
char *mp_buffer_steal(struct mp_buffer *buffer, size_t *length);
void mp_json_escape(char *out, size_t cap, const char *value);
int mp_read_id3_metadata(const char *path, struct mp_id3_metadata *metadata);
void mp_title_from_filename(const char *filename, char *out, size_t out_len);

#endif
