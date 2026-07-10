#ifndef MK_PICLOCK_MUSIC_JOBS_H
#define MK_PICLOCK_MUSIC_JOBS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "asset_store.h"

#define MP_MUSIC_JOB_MAX 32
#define MP_MUSIC_JOB_ERROR_MAX 160

enum mp_music_job_state {
    MP_MUSIC_JOB_EMPTY = 0,
    MP_MUSIC_JOB_QUEUED = 1,
    MP_MUSIC_JOB_PROCESSING = 2,
    MP_MUSIC_JOB_COMPLETE = 3,
    MP_MUSIC_JOB_FAILED = 4
};

struct mp_music_job_snapshot {
    uint64_t id;
    int state;
    unsigned int progress;
    time_t created_at;
    time_t completed_at;
    char file[MP_ASSET_NAME_MAX];
    char error[MP_MUSIC_JOB_ERROR_MAX];
    struct mp_audio_optimize_settings settings;
};

typedef int (*mp_music_job_notify_callback)(const char *file, void *userdata);

struct mp_music_job_request {
    const char *source_path;
    const char *output_name;
};

int mp_music_jobs_start(uint64_t quota_bytes, uint64_t reserve_bytes,
                        mp_music_job_notify_callback notify, void *notify_userdata);
void mp_music_jobs_stop(void);
int mp_music_jobs_queue_batch(const struct mp_music_job_request *requests, size_t count,
                              const struct mp_audio_optimize_settings *settings,
                              uint64_t *job_ids);
int mp_music_jobs_clear_queued(void);
int mp_music_jobs_snapshot(struct mp_music_job_snapshot *jobs, int max_jobs);
int mp_music_jobs_active(void);
int mp_music_jobs_file_active(const char *file);
const char *mp_music_job_state_name(int state);

#endif
