#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *SAMPLE_IPS[] = {
    "192.168.1.10", "192.168.1.25", "10.0.0.4", "10.0.0.17",
    "172.16.5.88", "203.0.113.19", "198.51.100.42", "192.0.2.77",
    "192.168.1.105", "10.0.1.33", "172.16.9.12", "203.0.113.99"
};

static const char *SAMPLE_METHODS[] = {
    "GET", "GET", "GET", "POST", "PUT", "DELETE"
};

static const char *SAMPLE_URLS[] = {
    "/", "/index.html", "/api/v1/users", "/api/v1/orders",
    "/api/v1/products", "/api/v1/reports/heavy", "/login",
    "/logout", "/static/app.js", "/static/styles.css",
    "/health", "/api/v1/search?q=logpulse"
};

static const int SAMPLE_STATUSES[] = {
    200, 200, 200, 200, 201, 204,
    301, 302, 304,
    400, 401, 403, 404,
    500, 502, 503
};

static void usage(const char *prog) {
    fprintf(stderr,
            "Uso: %s [-n LINEAS] [-o ARCHIVO] [-d DELAY_MS] [-m PCT_MALFORMADAS]\n"
            "  -n LINEAS           Numero de lineas a generar (por defecto 10000)\n"
            "  -o ARCHIVO          Archivo de salida (por defecto stdout)\n"
            "  -d DELAY_MS         Pausa en ms entre lotes de 10 lineas para modo vivo (defecto 0)\n"
            "  -m PCT_MALFORMADAS  Porcentaje aproximado de lineas malformadas 0..100 (defecto 2)\n",
            prog);
}

int main(int argc, char **argv) {
    long num_lines = 10000;
    const char *out_path = NULL;
    long delay_ms = 0;
    long bad_pct = 2;

    int opt;
    while ((opt = getopt(argc, argv, "n:o:d:m:h")) != -1) {
        switch (opt) {
            case 'n':
                num_lines = strtol(optarg, NULL, 10);
                if (num_lines <= 0) num_lines = 1;
                break;
            case 'o':
                out_path = optarg;
                break;
            case 'd':
                delay_ms = strtol(optarg, NULL, 10);
                if (delay_ms < 0) delay_ms = 0;
                break;
            case 'm':
                bad_pct = strtol(optarg, NULL, 10);
                if (bad_pct < 0) bad_pct = 0;
                if (bad_pct > 100) bad_pct = 100;
                break;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    FILE *fp = stdout;
    if (out_path) {
        fp = fopen(out_path, delay_ms > 0 ? "a" : "w");
        if (!fp) {
            perror("fopen");
            return 1;
        }
    }

    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    size_t n_ips = sizeof(SAMPLE_IPS) / sizeof(SAMPLE_IPS[0]);
    size_t n_methods = sizeof(SAMPLE_METHODS) / sizeof(SAMPLE_METHODS[0]);
    size_t n_urls = sizeof(SAMPLE_URLS) / sizeof(SAMPLE_URLS[0]);
    size_t n_statuses = sizeof(SAMPLE_STATUSES) / sizeof(SAMPLE_STATUSES[0]);

    for (long i = 0; i < num_lines; ++i) {
        if ((long)(rand_r(&seed) % 100) < bad_pct) {
            fprintf(fp, "MALFORMED_LOG_LINE #%ld missing fields\n", i);
        } else {
            const char *ip = SAMPLE_IPS[rand_r(&seed) % n_ips];
            const char *method = SAMPLE_METHODS[rand_r(&seed) % n_methods];
            size_t url_idx = (size_t)(rand_r(&seed) % n_urls);
            const char *url = SAMPLE_URLS[url_idx];
            int status = SAMPLE_STATUSES[rand_r(&seed) % n_statuses];
            unsigned long bytes = 128UL + (unsigned long)(rand_r(&seed) % 32768);
            double resp_ms = 1.0 + (double)(rand_r(&seed) % 150);
            if (url_idx == 5 || url_idx == 11) {
                resp_ms += 250.0 + (double)(rand_r(&seed) % 800);
            }

            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            char ts[64];
            strftime(ts, sizeof(ts), "%d/%b/%Y:%H:%M:%S %z", &tm_now);

            fprintf(fp,
                    "%s - - [%s] \"%s %s HTTP/1.1\" %d %lu \"-\" \"LogGen/1.0\" %.2f\n",
                    ip, ts, method, url, status, bytes, resp_ms);
        }

        if (delay_ms > 0 && (i % 10 == 9)) {
            fflush(fp);
            struct timespec ts;
            ts.tv_sec = delay_ms / 1000;
            ts.tv_nsec = (delay_ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
    }

    if (out_path) {
        fclose(fp);
    } else {
        fflush(fp);
    }
    return 0;
}
