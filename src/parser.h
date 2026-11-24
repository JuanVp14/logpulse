#ifndef LOGPULSE_PARSER_H
#define LOGPULSE_PARSER_H

#include "shm_ipc.h"
#include <pthread.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    char ip[LP_MAX_IP];
    char url[LP_MAX_URL];
    int status_code;
    uint64_t bytes;
    double response_time_ms;
} parsed_log_t;

typedef struct hash_node {
    char *key;
    uint64_t count;
    double total_time_ms;
    double max_time_ms;
    struct hash_node *next;
} hash_node_t;

typedef struct {
    hash_node_t **buckets;
    size_t num_buckets;
    size_t size;
} str_map_t;

typedef struct {
    pthread_mutex_t mtx;
    struct timespec start_ts;

    uint64_t total_requests;
    uint64_t status_2xx;
    uint64_t status_3xx;
    uint64_t status_4xx;
    uint64_t status_5xx;
    uint64_t status_other;
    uint64_t total_bytes;
    uint64_t malformed_lines;

    str_map_t ip_map;
    str_map_t url_map;
} worker_stats_t;

int parse_combined_line(const char *line, parsed_log_t *out);

int worker_stats_init(worker_stats_t *st);
void worker_stats_destroy(worker_stats_t *st);
void worker_stats_record(worker_stats_t *st, const parsed_log_t *rec, int valid);
void worker_stats_snapshot(worker_stats_t *st, worker_shm_t *dst_local);

#endif /* LOGPULSE_PARSER_H */
