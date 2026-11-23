#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BUF_CAP 4096
#define MAX_LINE 512
#define TABLE_CAP 1024

typedef struct {
    char text[MAX_LINE];
    int is_poison;
} BufferItem;

typedef struct {
    BufferItem items[BUF_CAP];
    int head, tail, count;
    pthread_mutex_t mtx;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} BoundedBuffer;

typedef struct {
    char key[128];
    long count;
    double total_time;
    long time_count;
} Entry;

typedef struct {
    long total_reqs;
    long malformed;
    long c2xx, c3xx, c4xx, c5xx;
    long long total_bytes;
    Entry ips[TABLE_CAP];
    int n_ips;
    Entry urls[TABLE_CAP];
    int n_urls;
} Metrics;

typedef struct {
    BoundedBuffer *buf;
    const char *filepath;
    int num_parsers;
} ReaderArgs;

typedef struct {
    BoundedBuffer *buf;
    Metrics local;
} ParserArgs;

static void buf_init(BoundedBuffer *b) {
    b->head = b->tail = b->count = 0;
    pthread_mutex_init(&b->mtx, NULL);
    pthread_cond_init(&b->not_full, NULL);
    pthread_cond_init(&b->not_empty, NULL);
}

static void buf_destroy(BoundedBuffer *b) {
    pthread_mutex_destroy(&b->mtx);
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
}

static void buf_push(BoundedBuffer *b, const char *line, int poison) {
    pthread_mutex_lock(&b->mtx);
    while (b->count == BUF_CAP) {
        pthread_cond_wait(&b->not_full, &b->mtx);
    }
    b->items[b->tail].is_poison = poison;
    if (!poison && line) {
        strncpy(b->items[b->tail].text, line, MAX_LINE - 1);
        b->items[b->tail].text[MAX_LINE - 1] = '\0';
    }
    b->tail = (b->tail + 1) % BUF_CAP;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mtx);
}

static void buf_pop(BoundedBuffer *b, BufferItem *out) {
    pthread_mutex_lock(&b->mtx);
    while (b->count == 0) {
        pthread_cond_wait(&b->not_empty, &b->mtx);
    }
    *out = b->items[b->head];
    b->head = (b->head + 1) % BUF_CAP;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mtx);
}

static void add_entry(Entry *arr, int *n, const char *key, double resp_time, int has_time) {
    for (int i = 0; i < *n; i++) {
        if (strcmp(arr[i].key, key) == 0) {
            arr[i].count++;
            if (has_time) {
                arr[i].total_time += resp_time;
                arr[i].time_count++;
            }
            return;
        }
    }
    if (*n < TABLE_CAP) {
        strncpy(arr[*n].key, key, sizeof(arr[*n].key) - 1);
        arr[*n].key[sizeof(arr[*n].key) - 1] = '\0';
        arr[*n].count = 1;
        arr[*n].total_time = has_time ? resp_time : 0.0;
        arr[*n].time_count = has_time ? 1 : 0;
        (*n)++;
    }
}

static void parse_line(const char *line, Metrics *m) {
    char ip[64], method[16], url[128], proto[16];
    int status = 0;
    long bytes = 0;
    double rtime = 0.0;

    int matched = sscanf(line, "%63s %*s %*s [%*[^]]] \"%15s %127s %15[^\"]\" %d %ld \"%*[^\"]\" \"%*[^\"]\" %lf",
                         ip, method, url, proto, &status, &bytes, &rtime);

    if (matched < 6) {
        m->malformed++;
        return;
    }

    int has_time = (matched == 7);
    char *q = strchr(url, '?');
    if (q) *q = '\0';

    m->total_reqs++;
    m->total_bytes += bytes;
    if (status >= 200 && status < 300) m->c2xx++;
    else if (status >= 300 && status < 400) m->c3xx++;
    else if (status >= 400 && status < 500) m->c4xx++;
    else if (status >= 500 && status < 600) m->c5xx++;

    add_entry(m->ips, &m->n_ips, ip, 0.0, 0);
    add_entry(m->urls, &m->n_urls, url, rtime, has_time);
}

static void *reader_thread(void *arg) {
    ReaderArgs *ra = (ReaderArgs *)arg;
    FILE *f = fopen(ra->filepath, "r");
    if (!f) {
        perror("fopen");
    } else {
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
            buf_push(ra->buf, line, 0);
        }
        fclose(f);
    }
    for (int i = 0; i < ra->num_parsers; i++) {
        buf_push(ra->buf, NULL, 1);
    }
    return NULL;
}

static void *parser_thread(void *arg) {
    ParserArgs *pa = (ParserArgs *)arg;
    BufferItem item;
    while (1) {
        buf_pop(pa->buf, &item);
        if (item.is_poison) break;
        parse_line(item.text, &pa->local);
    }
    return NULL;
}

static void merge_metrics(Metrics *dst, const Metrics *src) {
    dst->total_reqs += src->total_reqs;
    dst->malformed += src->malformed;
    dst->c2xx += src->c2xx;
    dst->c3xx += src->c3xx;
    dst->c4xx += src->c4xx;
    dst->c5xx += src->c5xx;
    dst->total_bytes += src->total_bytes;

    for (int i = 0; i < src->n_ips; i++) {
        int found = 0;
        for (int k = 0; k < dst->n_ips; k++) {
            if (strcmp(dst->ips[k].key, src->ips[i].key) == 0) {
                dst->ips[k].count += src->ips[i].count;
                found = 1;
                break;
            }
        }
        if (!found && dst->n_ips < TABLE_CAP) {
            dst->ips[dst->n_ips++] = src->ips[i];
        }
    }

    for (int i = 0; i < src->n_urls; i++) {
        int found = 0;
        for (int k = 0; k < dst->n_urls; k++) {
            if (strcmp(dst->urls[k].key, src->urls[i].key) == 0) {
                dst->urls[k].count += src->urls[i].count;
                dst->urls[k].total_time += src->urls[i].total_time;
                dst->urls[k].time_count += src->urls[i].time_count;
                found = 1;
                break;
            }
        }
        if (!found && dst->n_urls < TABLE_CAP) {
            dst->urls[dst->n_urls++] = src->urls[i];
        }
    }
}

static int cmp_count_desc(const void *a, const void *b) {
    const Entry *ea = (const Entry *)a;
    const Entry *eb = (const Entry *)b;
    return (eb->count > ea->count) - (eb->count < ea->count);
}

static int cmp_avg_time_desc(const void *a, const void *b) {
    const Entry *ea = (const Entry *)a;
    const Entry *eb = (const Entry *)b;
    double ta = ea->time_count ? (ea->total_time / ea->time_count) : 0.0;
    double tb = eb->time_count ? (eb->total_time / eb->time_count) : 0.0;
    return (tb > ta) - (tb < ta);
}

int main(int argc, char *argv[]) {
    int num_threads = 4;
    const char *filepath = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
            if (num_threads < 1 || num_threads > 16) num_threads = 4;
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            i++;
        } else {
            filepath = argv[i];
        }
    }

    if (!filepath) {
        fprintf(stderr, "Uso: %s [-m lote] [-t N] archivo\n", argv[0]);
        return 1;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    BoundedBuffer buf;
    buf_init(&buf);

    pthread_t r_tid;
    ReaderArgs r_args = { .buf = &buf, .filepath = filepath, .num_parsers = num_threads };
    pthread_create(&r_tid, NULL, reader_thread, &r_args);

    pthread_t p_tids[16];
    ParserArgs *p_args = calloc((size_t)num_threads, sizeof(ParserArgs));
    for (int i = 0; i < num_threads; i++) {
        p_args[i].buf = &buf;
        pthread_create(&p_tids[i], NULL, parser_thread, &p_args[i]);
    }

    pthread_join(r_tid, NULL);
    Metrics total = {0};
    for (int i = 0; i < num_threads; i++) {
        pthread_join(p_tids[i], NULL);
        merge_metrics(&total, &p_args[i].local);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("=== LogPulse (Avance - Modo Lote | %d hilos) ===\n", num_threads);
    printf("Tiempo total: %.3f s | Peticiones/seg: %.0f\n", elapsed, total.total_reqs / elapsed);
    printf("Total peticiones: %ld | Mal formadas: %ld\n", total.total_reqs, total.malformed);
    printf("HTTP: 2xx=%ld | 3xx=%ld | 4xx=%ld | 5xx=%ld\n", total.c2xx, total.c3xx, total.c4xx, total.c5xx);
    printf("Bytes transferidos: %lld\n\n", total.total_bytes);

    qsort(total.ips, (size_t)total.n_ips, sizeof(Entry), cmp_count_desc);
    printf("--- Top 10 IPs ---\n");
    for (int i = 0; i < total.n_ips && i < 10; i++) {
        printf("%2d. %-16s (%ld)\n", i + 1, total.ips[i].key, total.ips[i].count);
    }

    qsort(total.urls, (size_t)total.n_urls, sizeof(Entry), cmp_count_desc);
    printf("\n--- Top 10 URLs mas solicitadas ---\n");
    for (int i = 0; i < total.n_urls && i < 10; i++) {
        printf("%2d. %-25s (%ld)\n", i + 1, total.urls[i].key, total.urls[i].count);
    }

    qsort(total.urls, (size_t)total.n_urls, sizeof(Entry), cmp_avg_time_desc);
    printf("\n--- Top 10 URLs mas lentas (promedio) ---\n");
    for (int i = 0; i < total.n_urls && i < 10; i++) {
        double avg = total.urls[i].time_count ? (total.urls[i].total_time / total.urls[i].time_count) : 0.0;
        printf("%2d. %-25s (%.4f s)\n", i + 1, total.urls[i].key, avg);
    }

    free(p_args);
    buf_destroy(&buf);
    return 0;
}