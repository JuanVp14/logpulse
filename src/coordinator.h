#ifndef LOGPULSE_COORDINATOR_H
#define LOGPULSE_COORDINATOR_H

#include "shm_ipc.h"
#include <pthread.h>
#include <time.h>

#define LP_EVT_LOG_CAP 8

typedef struct {
    pid_t pid;
    char filepath[LP_MAX_PATH];
    int status_pipe_rd;
    int ctrl_pipe_wr;
    int alive;
    worker_state_t state;
    int exit_code;
    int term_sig;
    char last_msg[LP_MAX_DETAIL + 16];
} coord_worker_t;

typedef struct {
    pid_t pid;
    char filepath[LP_MAX_PATH];
    worker_state_t state;
    uint64_t total_requests;
    double req_per_sec;
    uint64_t status_2xx;
    uint64_t status_3xx;
    uint64_t status_4xx;
    uint64_t status_5xx;
    uint64_t status_other;
    uint64_t total_bytes;
    uint64_t malformed_lines;
    char last_msg[LP_MAX_DETAIL + 16];
} worker_view_t;

typedef struct {
    int num_workers;
    int paused;
    lp_mode_t mode;
    int threads_per_worker;
    double elapsed_sec;

    uint64_t total_requests;
    double total_req_per_sec;
    uint64_t status_2xx;
    uint64_t status_3xx;
    uint64_t status_4xx;
    uint64_t status_5xx;
    uint64_t status_other;
    uint64_t total_bytes;
    uint64_t malformed_lines;

    worker_view_t workers[LP_MAX_WORKERS];

    uint32_t top_ip_count;
    top_count_entry_t top_ips[LP_TOP_N];

    uint32_t top_url_count;
    top_count_entry_t top_urls[LP_TOP_N];

    uint32_t top_slow_count;
    top_slow_entry_t top_slow_urls[LP_TOP_N];

    char evt_log[LP_EVT_LOG_CAP][192];
    int evt_log_count;
} consolidated_metrics_t;

typedef struct {
    shm_layout_t *shm;
    char shm_name[64];

    int num_workers;
    lp_mode_t mode;
    int threads_per_worker;
    struct timespec start_ts;

    coord_worker_t workers[LP_MAX_WORKERS];

    pthread_mutex_t state_mtx;
    int paused;
    int stop_requested;
    int ui_stop;

    int sig_pipe[2];
    int status_wake_pipe[2];
    int ui_wake_pipe[2];

    char evt_log[LP_EVT_LOG_CAP][192];
    int evt_log_count;
    int evt_log_head;
} coordinator_t;

int coordinator_run(int num_files, char **files, lp_mode_t mode, int threads_per_worker);

void coordinator_request_stop(coordinator_t *coord);
void coordinator_toggle_pause(coordinator_t *coord);
void coordinator_wake_ui(coordinator_t *coord);
void coordinator_push_event_log(coordinator_t *coord, const char *fmt, ...);
void coordinator_get_consolidated(coordinator_t *coord, consolidated_metrics_t *out);
int coordinator_export_csv(coordinator_t *coord, const char *csv_path);

const char *worker_state_str(worker_state_t st);

#endif /* LOGPULSE_COORDINATOR_H */
