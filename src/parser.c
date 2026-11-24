#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "parser.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_BUCKETS 4096

static char *lp_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

static uint64_t fnv1a_hash(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static int str_map_init(str_map_t *m, size_t buckets) {
    m->buckets = calloc(buckets, sizeof(hash_node_t *));
    if (!m->buckets) {
        return -1;
    }
    m->num_buckets = buckets;
    m->size = 0;
    return 0;
}

static void str_map_destroy(str_map_t *m) {
    if (!m->buckets) {
        return;
    }
    for (size_t i = 0; i < m->num_buckets; ++i) {
        hash_node_t *cur = m->buckets[i];
        while (cur) {
            hash_node_t *next = cur->next;
            free(cur->key);
            free(cur);
            cur = next;
        }
    }
    free(m->buckets);
    m->buckets = NULL;
    m->num_buckets = 0;
    m->size = 0;
}

static void str_map_rehash(str_map_t *m) {
    size_t new_cap = m->num_buckets * 2;
    hash_node_t **new_buckets = calloc(new_cap, sizeof(hash_node_t *));
    if (!new_buckets) {
        return;
    }
    for (size_t i = 0; i < m->num_buckets; ++i) {
        hash_node_t *cur = m->buckets[i];
        while (cur) {
            hash_node_t *next = cur->next;
            size_t idx = (size_t)(fnv1a_hash(cur->key) & (new_cap - 1));
            cur->next = new_buckets[idx];
            new_buckets[idx] = cur;
            cur = next;
        }
    }
    free(m->buckets);
    m->buckets = new_buckets;
    m->num_buckets = new_cap;
}

static void str_map_add(str_map_t *m, const char *key, double time_ms) {
    if ((m->size + 1) * 4 > m->num_buckets * 3) {
        str_map_rehash(m);
    }
    size_t idx = (size_t)(fnv1a_hash(key) & (m->num_buckets - 1));
    hash_node_t *cur = m->buckets[idx];
    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            cur->count++;
            cur->total_time_ms += time_ms;
            if (time_ms > cur->max_time_ms) {
                cur->max_time_ms = time_ms;
            }
            return;
        }
        cur = cur->next;
    }

    hash_node_t *node = malloc(sizeof(hash_node_t));
    if (!node) {
        return;
    }
    node->key = lp_strdup(key);
    if (!node->key) {
        free(node);
        return;
    }
    node->count = 1;
    node->total_time_ms = time_ms;
    node->max_time_ms = time_ms;
    node->next = m->buckets[idx];
    m->buckets[idx] = node;
    m->size++;
}

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static const char *skip_token(const char *p) {
    while (*p && !isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static const char *read_quoted(const char *p, const char **start, size_t *len) {
    if (*p != '"') {
        return NULL;
    }
    p++;
    *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1) != '\0') {
            p += 2;
        } else {
            p++;
        }
    }
    if (*p != '"') {
        return NULL;
    }
    *len = (size_t)(p - *start);
    return p + 1;
}

int parse_combined_line(const char *line, parsed_log_t *out) {
    if (!line || !out) {
        return -1;
    }

    const char *p = skip_ws(line);
    if (*p == '\0') {
        return -1;
    }

    /* 1. Client IP */
    const char *ip_start = p;
    p = skip_token(p);
    size_t ip_len = (size_t)(p - ip_start);
    if (ip_len == 0 || ip_len >= LP_MAX_IP) {
        return -1;
    }
    memcpy(out->ip, ip_start, ip_len);
    out->ip[ip_len] = '\0';

    /* 2. Ident & authuser */
    p = skip_ws(p);
    if (*p == '\0') return -1;
    p = skip_token(p);
    p = skip_ws(p);
    if (*p == '\0') return -1;
    p = skip_token(p);

    /* 3. Timestamp [dd/Mon/yyyy:hh:mm:ss +zzzz] */
    p = skip_ws(p);
    if (*p != '[') {
        return -1;
    }
    p++;
    const char *close_bracket = strchr(p, ']');
    if (!close_bracket) {
        return -1;
    }
    p = close_bracket + 1;

    /* 4. Request line "METHOD URL PROTO" */
    p = skip_ws(p);
    const char *req_start = NULL;
    size_t req_len = 0;
    p = read_quoted(p, &req_start, &req_len);
    if (!p || req_len == 0) {
        return -1;
    }

    const char *rp = req_start;
    const char *req_end = req_start + req_len;
    while (rp < req_end && isspace((unsigned char)*rp)) rp++;
    const char *method_start = rp;
    while (rp < req_end && !isspace((unsigned char)*rp)) rp++;
    if (rp == method_start) {
        return -1;
    }
    while (rp < req_end && isspace((unsigned char)*rp)) rp++;
    const char *url_start = rp;
    while (rp < req_end && !isspace((unsigned char)*rp)) rp++;
    size_t url_len = (size_t)(rp - url_start);
    if (url_len == 0) {
        return -1;
    }
    if (url_len >= LP_MAX_URL) {
        url_len = LP_MAX_URL - 1;
    }
    memcpy(out->url, url_start, url_len);
    out->url[url_len] = '\0';

    /* 5. Status code */
    p = skip_ws(p);
    if (!isdigit((unsigned char)*p)) {
        return -1;
    }
    char *endptr = NULL;
    errno = 0;
    long status = strtol(p, &endptr, 10);
    if (errno != 0 || endptr == p || (!isspace((unsigned char)*endptr) && *endptr != '\0')) {
        return -1;
    }
    if (status < 100 || status > 599) {
        return -1;
    }
    out->status_code = (int)status;
    p = endptr;

    /* 6. Bytes transferred (integer or '-') */
    p = skip_ws(p);
    if (*p == '-') {
        p++;
        if (*p != '\0' && !isspace((unsigned char)*p)) {
            return -1;
        }
        out->bytes = 0;
    } else if (isdigit((unsigned char)*p)) {
        errno = 0;
        unsigned long long bytes = strtoull(p, &endptr, 10);
        if (errno != 0 || endptr == p || (!isspace((unsigned char)*endptr) && *endptr != '\0')) {
            return -1;
        }
        out->bytes = (uint64_t)bytes;
        p = endptr;
    } else {
        return -1;
    }

    /* 7. Referer & User-Agent */
    p = skip_ws(p);
    const char *dummy_s = NULL;
    size_t dummy_l = 0;
    p = read_quoted(p, &dummy_s, &dummy_l);
    if (!p) {
        return -1;
    }
    p = skip_ws(p);
    p = read_quoted(p, &dummy_s, &dummy_l);
    if (!p) {
        return -1;
    }

    /* 8. Response time (ms) */
    p = skip_ws(p);
    if (*p == '\0') {
        out->response_time_ms = 0.0;
    } else {
        errno = 0;
        double rt = strtod(p, &endptr);
        if (errno != 0 || endptr == p || rt < 0.0) {
            return -1;
        }
        p = skip_ws(endptr);
        if (*p != '\0') {
            return -1;
        }
        out->response_time_ms = rt;
    }

    return 0;
}

int worker_stats_init(worker_stats_t *st) {
    memset(st, 0, sizeof(*st));
    if (pthread_mutex_init(&st->mtx, NULL) != 0) {
        return -1;
    }
    if (str_map_init(&st->ip_map, INITIAL_BUCKETS) != 0) {
        pthread_mutex_destroy(&st->mtx);
        return -1;
    }
    if (str_map_init(&st->url_map, INITIAL_BUCKETS) != 0) {
        str_map_destroy(&st->ip_map);
        pthread_mutex_destroy(&st->mtx);
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &st->start_ts);
    return 0;
}

void worker_stats_destroy(worker_stats_t *st) {
    str_map_destroy(&st->ip_map);
    str_map_destroy(&st->url_map);
    pthread_mutex_destroy(&st->mtx);
}

void worker_stats_record(worker_stats_t *st, const parsed_log_t *rec, int valid) {
    pthread_mutex_lock(&st->mtx);
    if (!valid || !rec) {
        st->malformed_lines++;
        pthread_mutex_unlock(&st->mtx);
        return;
    }

    st->total_requests++;
    st->total_bytes += rec->bytes;

    if (rec->status_code >= 200 && rec->status_code < 300) {
        st->status_2xx++;
    } else if (rec->status_code >= 300 && rec->status_code < 400) {
        st->status_3xx++;
    } else if (rec->status_code >= 400 && rec->status_code < 500) {
        st->status_4xx++;
    } else if (rec->status_code >= 500 && rec->status_code < 600) {
        st->status_5xx++;
    } else {
        st->status_other++;
    }

    str_map_add(&st->ip_map, rec->ip, 0.0);
    str_map_add(&st->url_map, rec->url, rec->response_time_ms);
    pthread_mutex_unlock(&st->mtx);
}

static void insert_top_count(top_count_entry_t *arr, uint32_t *n, const char *key, uint64_t count) {
    uint32_t len = *n;
    uint32_t pos = len;
    for (uint32_t i = 0; i < len; ++i) {
        if (count > arr[i].count) {
            pos = i;
            break;
        }
    }
    if (pos >= LP_TOP_N) {
        return;
    }
    uint32_t new_len = (len < LP_TOP_N) ? (len + 1) : LP_TOP_N;
    for (uint32_t j = new_len - 1; j > pos; --j) {
        arr[j] = arr[j - 1];
    }
    snprintf(arr[pos].key, sizeof(arr[pos].key), "%s", key);
    arr[pos].count = count;
    *n = new_len;
}

static void insert_top_slow(top_slow_entry_t *arr, uint32_t *n, const hash_node_t *node) {
    double avg = node->count > 0 ? (node->total_time_ms / (double)node->count) : 0.0;
    uint32_t len = *n;
    uint32_t pos = len;
    for (uint32_t i = 0; i < len; ++i) {
        if (avg > arr[i].avg_time_ms) {
            pos = i;
            break;
        }
    }
    if (pos >= LP_TOP_N) {
        return;
    }
    uint32_t new_len = (len < LP_TOP_N) ? (len + 1) : LP_TOP_N;
    for (uint32_t j = new_len - 1; j > pos; --j) {
        arr[j] = arr[j - 1];
    }
    snprintf(arr[pos].url, sizeof(arr[pos].url), "%s", node->key);
    arr[pos].count = node->count;
    arr[pos].total_time_ms = node->total_time_ms;
    arr[pos].avg_time_ms = avg;
    arr[pos].max_time_ms = node->max_time_ms;
    *n = new_len;
}

void worker_stats_snapshot(worker_stats_t *st, worker_shm_t *dst_local) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    pthread_mutex_lock(&st->mtx);

    double elapsed = (double)(now.tv_sec - st->start_ts.tv_sec) +
                     (double)(now.tv_nsec - st->start_ts.tv_nsec) / 1e9;
    if (elapsed < 0.001) {
        elapsed = 0.001;
    }

    dst_local->total_requests = st->total_requests;
    dst_local->req_per_sec = (double)st->total_requests / elapsed;
    dst_local->status_2xx = st->status_2xx;
    dst_local->status_3xx = st->status_3xx;
    dst_local->status_4xx = st->status_4xx;
    dst_local->status_5xx = st->status_5xx;
    dst_local->status_other = st->status_other;
    dst_local->total_bytes = st->total_bytes;
    dst_local->malformed_lines = st->malformed_lines;

    dst_local->top_ip_count = 0;
    for (size_t i = 0; i < st->ip_map.num_buckets; ++i) {
        for (hash_node_t *cur = st->ip_map.buckets[i]; cur != NULL; cur = cur->next) {
            insert_top_count(dst_local->top_ips, &dst_local->top_ip_count, cur->key, cur->count);
        }
    }

    dst_local->top_url_count = 0;
    dst_local->top_slow_count = 0;
    for (size_t i = 0; i < st->url_map.num_buckets; ++i) {
        for (hash_node_t *cur = st->url_map.buckets[i]; cur != NULL; cur = cur->next) {
            insert_top_count(dst_local->top_urls, &dst_local->top_url_count, cur->key, cur->count);
            insert_top_slow(dst_local->top_slow_urls, &dst_local->top_slow_count, cur);
        }
    }

    pthread_mutex_unlock(&st->mtx);
}
