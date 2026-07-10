#define _GNU_SOURCE
#include "music_jobs.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define JOB_RETENTION_SECONDS 3600

struct music_job {
    struct mp_music_job_snapshot public;
    char source_path[768];
};

static struct music_job g_jobs[MP_MUSIC_JOB_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_worker;
static int g_worker_started = 0;
static int g_running = 0;
static uint64_t g_next_id = 1;
static uint64_t g_quota_bytes = 0;
static uint64_t g_reserve_bytes = 0;
static mp_music_job_notify_callback g_notify = NULL;
static void *g_notify_userdata = NULL;

const char *mp_music_job_state_name(int state) {
    switch (state) {
        case MP_MUSIC_JOB_QUEUED: return "queued";
        case MP_MUSIC_JOB_PROCESSING: return "processing";
        case MP_MUSIC_JOB_COMPLETE: return "complete";
        case MP_MUSIC_JOB_FAILED: return "failed";
        default: return "empty";
    }
}

static void clean_processing_directory(void) {
    DIR *dir = opendir(MP_MUSIC_PROCESS_DIR);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' &&
            (strncmp(entry->d_name, ".source-", 8) == 0 ||
             strncmp(entry->d_name, ".output-", 8) == 0)) {
            char path[768];
            int n = snprintf(path, sizeof(path), "%s/%s", MP_MUSIC_PROCESS_DIR, entry->d_name);
            if (n > 0 && (size_t)n < sizeof(path)) (void)unlink(path);
        }
    }
    closedir(dir);
}

static int slot_reusable_locked(int index, time_t now) {
    if (index < 0 || index >= MP_MUSIC_JOB_MAX) return 0;
    if (g_jobs[index].public.state == MP_MUSIC_JOB_EMPTY) return 1;
    return (g_jobs[index].public.state == MP_MUSIC_JOB_COMPLETE ||
            g_jobs[index].public.state == MP_MUSIC_JOB_FAILED) &&
           g_jobs[index].public.completed_at > 0 &&
           now - g_jobs[index].public.completed_at > JOB_RETENTION_SECONDS;
}

static int next_queued_locked(void) {
    int selected = -1;
    uint64_t lowest_id = UINT64_MAX;
    for (int i = 0; i < MP_MUSIC_JOB_MAX; i++) {
        if (g_jobs[i].public.state == MP_MUSIC_JOB_QUEUED && g_jobs[i].public.id < lowest_id) {
            selected = i;
            lowest_id = g_jobs[i].public.id;
        }
    }
    return selected;
}

static int find_job_locked(uint64_t id) {
    for (int i = 0; i < MP_MUSIC_JOB_MAX; i++) {
        if (g_jobs[i].public.state != MP_MUSIC_JOB_EMPTY && g_jobs[i].public.id == id) return i;
    }
    return -1;
}

static void progress_update(unsigned int percent, void *userdata) {
    uint64_t id = *(uint64_t *)userdata;
    if (percent > 99) percent = 99;
    pthread_mutex_lock(&g_lock);
    int index = find_job_locked(id);
    if (index >= 0 && g_jobs[index].public.state == MP_MUSIC_JOB_PROCESSING)
        g_jobs[index].public.progress = percent;
    pthread_mutex_unlock(&g_lock);
}

static void finish_job(uint64_t id, int success, const char *error) {
    pthread_mutex_lock(&g_lock);
    int index = find_job_locked(id);
    if (index >= 0) {
        g_jobs[index].public.state = success ? MP_MUSIC_JOB_COMPLETE : MP_MUSIC_JOB_FAILED;
        g_jobs[index].public.progress = success ? 100u : g_jobs[index].public.progress;
        g_jobs[index].public.completed_at = time(NULL);
        mp_safe_str(g_jobs[index].public.error, sizeof(g_jobs[index].public.error), error);
        g_jobs[index].source_path[0] = '\0';
    }
    pthread_mutex_unlock(&g_lock);
}

static int output_quota_allows(const char *target, uint64_t output_bytes) {
    uint64_t current = mp_asset_directory_bytes(MP_MUSIC_DIR, MP_ASSET_SCAN_MUSIC_MP3);
    uint64_t replaced = 0;
    struct stat existing;
    if (stat(target, &existing) == 0 && S_ISREG(existing.st_mode) && existing.st_size > 0)
        replaced = (uint64_t)existing.st_size;
    uint64_t retained = current >= replaced ? current - replaced : 0;
    if (output_bytes > g_quota_bytes || retained > g_quota_bytes - output_bytes) return 0;
    return mp_asset_has_free_space(MP_MUSIC_DIR, output_bytes, g_reserve_bytes);
}

static void process_job(struct music_job job) {
    char output_template[768];
    snprintf(output_template, sizeof(output_template), "%s/.output-XXXXXX", MP_MUSIC_PROCESS_DIR);
    int output_fd = mkstemp(output_template);
    if (output_fd < 0) {
        (void)unlink(job.source_path);
        finish_job(job.public.id, 0, "processed file could not be created");
        return;
    }
    close(output_fd);

    char error[MP_MUSIC_JOB_ERROR_MAX] = "";
    int optimized = mp_asset_optimize_mp3(job.source_path, output_template, &job.public.settings,
                                           progress_update, &job.public.id, error, sizeof(error));
    if (optimized != 0 || mp_asset_validate_mp3(output_template) != 0) {
        (void)unlink(output_template);
        (void)unlink(job.source_path);
        finish_job(job.public.id, 0, error[0] ? error : "processed MP3 failed validation");
        return;
    }

    struct stat output_info;
    if (stat(output_template, &output_info) != 0 || output_info.st_size <= 0) {
        (void)unlink(output_template);
        (void)unlink(job.source_path);
        finish_job(job.public.id, 0, "processed MP3 is empty");
        return;
    }

    char target[768];
    int target_len = snprintf(target, sizeof(target), "%s/%s", MP_MUSIC_DIR, job.public.file);
    if (target_len <= 0 || (size_t)target_len >= sizeof(target) ||
        !output_quota_allows(target, (uint64_t)output_info.st_size)) {
        (void)unlink(output_template);
        (void)unlink(job.source_path);
        finish_job(job.public.id, 0, "music quota or free-space reserve would be exceeded");
        return;
    }

    if (rename(output_template, target) != 0) {
        (void)unlink(output_template);
        (void)unlink(job.source_path);
        finish_job(job.public.id, 0, "processed MP3 could not replace the destination file");
        return;
    }
    (void)chmod(target, 0640);
    (void)unlink(job.source_path);

    const char *warning = NULL;
    if (g_notify && g_notify(job.public.file, g_notify_userdata) != 0)
        warning = "processed MP3 is ready, but the clock core could not reload the library";
    finish_job(job.public.id, 1, warning);
}

static void *worker_main(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        int index = next_queued_locked();
        while (g_running && index < 0) {
            pthread_cond_wait(&g_cond, &g_lock);
            index = next_queued_locked();
        }
        if (!g_running) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        struct music_job job = g_jobs[index];
        g_jobs[index].public.state = MP_MUSIC_JOB_PROCESSING;
        g_jobs[index].public.progress = 1;
        pthread_mutex_unlock(&g_lock);
        process_job(job);
    }
    return NULL;
}

int mp_music_jobs_start(uint64_t quota_bytes, uint64_t reserve_bytes,
                        mp_music_job_notify_callback notify, void *notify_userdata) {
    if (g_worker_started) return 0;
    if (mp_asset_ensure_dir(MP_MUSIC_DIR) != 0 || mp_asset_ensure_dir(MP_MUSIC_PROCESS_DIR) != 0)
        return -1;
    clean_processing_directory();
    memset(g_jobs, 0, sizeof(g_jobs));
    g_quota_bytes = quota_bytes;
    g_reserve_bytes = reserve_bytes;
    g_notify = notify;
    g_notify_userdata = notify_userdata;
    g_running = 1;
    if (pthread_create(&g_worker, NULL, worker_main, NULL) != 0) {
        g_running = 0;
        return -1;
    }
    g_worker_started = 1;
    return 0;
}

void mp_music_jobs_stop(void) {
    if (!g_worker_started) return;
    pthread_mutex_lock(&g_lock);
    g_running = 0;
    time_t now = time(NULL);
    for (int i = 0; i < MP_MUSIC_JOB_MAX; i++) {
        if (g_jobs[i].public.state != MP_MUSIC_JOB_QUEUED) continue;
        if (g_jobs[i].source_path[0]) (void)unlink(g_jobs[i].source_path);
        g_jobs[i].source_path[0] = '\0';
        g_jobs[i].public.state = MP_MUSIC_JOB_FAILED;
        g_jobs[i].public.completed_at = now;
        mp_safe_str(g_jobs[i].public.error, sizeof(g_jobs[i].public.error),
                    "processing cancelled because the API stopped");
    }
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_worker, NULL);
    g_worker_started = 0;
}

int mp_music_jobs_queue_batch(const struct mp_music_job_request *requests, size_t count,
                              const struct mp_audio_optimize_settings *settings,
                              uint64_t *job_ids) {
    if (!requests || count == 0 || count > MP_MUSIC_JOB_MAX || !settings) return -1;
    for (size_t i = 0; i < count; i++) {
        if (!requests[i].source_path || !*requests[i].source_path ||
            !mp_asset_safe_filename(requests[i].output_name) ||
            !mp_asset_has_mp3_ext(requests[i].output_name)) return -1;
    }

    pthread_mutex_lock(&g_lock);
    if (!g_running) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    for (int i = 0; i < MP_MUSIC_JOB_MAX; i++) {
        if (g_jobs[i].public.state == MP_MUSIC_JOB_QUEUED ||
            g_jobs[i].public.state == MP_MUSIC_JOB_PROCESSING) {
            pthread_mutex_unlock(&g_lock);
            return 1;
        }
    }

    int slots[MP_MUSIC_JOB_MAX];
    unsigned char selected[MP_MUSIC_JOB_MAX] = {0};
    size_t slot_count = 0;
    time_t now = time(NULL);
    for (int i = 0; i < MP_MUSIC_JOB_MAX && slot_count < count; i++) {
        if (slot_reusable_locked(i, now)) {
            slots[slot_count++] = i;
            selected[i] = 1;
        }
    }
    /* Preserve recent history when capacity exists, but never let completed
       history block a new batch after processing has gone idle. */
    for (int i = 0; i < MP_MUSIC_JOB_MAX && slot_count < count; i++) {
        if (!selected[i] &&
            (g_jobs[i].public.state == MP_MUSIC_JOB_COMPLETE ||
             g_jobs[i].public.state == MP_MUSIC_JOB_FAILED)) {
            slots[slot_count++] = i;
            selected[i] = 1;
        }
    }
    if (slot_count < count) {
        pthread_mutex_unlock(&g_lock);
        return 2;
    }

    for (size_t i = 0; i < count; i++) {
        struct music_job *job = &g_jobs[slots[i]];
        memset(job, 0, sizeof(*job));
        job->public.id = g_next_id++;
        if (g_next_id == 0) g_next_id = 1;
        job->public.state = MP_MUSIC_JOB_QUEUED;
        job->public.created_at = now;
        job->public.settings = *settings;
        mp_safe_str(job->public.file, sizeof(job->public.file), requests[i].output_name);
        mp_safe_str(job->source_path, sizeof(job->source_path), requests[i].source_path);
        if (job_ids) job_ids[i] = job->public.id;
    }
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int mp_music_jobs_clear_queued(void) {
    pthread_mutex_lock(&g_lock);
    int removed = 0;
    for (int i = 0; i < MP_MUSIC_JOB_MAX; i++) {
        if (g_jobs[i].public.state != MP_MUSIC_JOB_QUEUED) continue;
        if (g_jobs[i].source_path[0]) (void)unlink(g_jobs[i].source_path);
        memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
        removed++;
    }
    pthread_mutex_unlock(&g_lock);
    return removed;
}

int mp_music_jobs_snapshot(struct mp_music_job_snapshot *jobs, int max_jobs) {
    if (!jobs || max_jobs <= 0) return 0;
    pthread_mutex_lock(&g_lock);
    time_t now = time(NULL);
    int count = 0;
    for (int i = 0; i < MP_MUSIC_JOB_MAX; i++) {
        if ((g_jobs[i].public.state == MP_MUSIC_JOB_COMPLETE ||
             g_jobs[i].public.state == MP_MUSIC_JOB_FAILED) &&
            g_jobs[i].public.completed_at > 0 &&
            now - g_jobs[i].public.completed_at > JOB_RETENTION_SECONDS) {
            memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
            continue;
        }
        if (count < max_jobs && g_jobs[i].public.state != MP_MUSIC_JOB_EMPTY)
            jobs[count++] = g_jobs[i].public;
    }
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (jobs[j].id > jobs[i].id) {
                struct mp_music_job_snapshot tmp = jobs[i];
                jobs[i] = jobs[j];
                jobs[j] = tmp;
            }
        }
    }
    return count;
}

int mp_music_jobs_active(void) {
    pthread_mutex_lock(&g_lock);
    int count = 0;
    for (int i = 0; i < MP_MUSIC_JOB_MAX; i++) {
        if (g_jobs[i].public.state == MP_MUSIC_JOB_QUEUED ||
            g_jobs[i].public.state == MP_MUSIC_JOB_PROCESSING)
            count++;
    }
    pthread_mutex_unlock(&g_lock);
    return count;
}

int mp_music_jobs_file_active(const char *file) {
    if (!file || !*file) return 0;
    pthread_mutex_lock(&g_lock);
    int active = 0;
    for (int i = 0; i < MP_MUSIC_JOB_MAX; i++) {
        if ((g_jobs[i].public.state == MP_MUSIC_JOB_QUEUED ||
             g_jobs[i].public.state == MP_MUSIC_JOB_PROCESSING) &&
            strcmp(g_jobs[i].public.file, file) == 0) {
            active = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return active;
}
