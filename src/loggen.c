#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *IPS[] = {
    "192.168.10.24", "10.0.0.15", "172.16.0.8", "192.168.1.101",
    "192.168.10.50", "10.0.0.99", "181.50.12.4", "190.25.88.12",
    "192.168.10.77", "10.10.10.10", "172.16.5.20", "200.12.34.56"
};

static const char *URLS[] = {
    "/api/notas?id=42", "/api/usuarios?page=1", "/index.html",
    "/login", "/api/reportes?mes=11", "/static/app.js",
    "/api/cursos?sem=20262", "/dashboard", "/api/pagos",
    "/health", "/api/perfil?u=7", "/favicon.ico"
};

static const int CODES[] = {200, 200, 200, 201, 301, 304, 400, 404, 500, 503};

int main(int argc, char *argv[]) {
    long num_lines = 1000000;
    const char *out_path = "access.log";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            num_lines = atol(argv[++i]);
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        }
    }

    FILE *f = fopen(out_path, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }

    static char io_buf[1 << 20];
    setvbuf(f, io_buf, _IOFBF, sizeof(io_buf));

    unsigned int seed = (unsigned int)time(NULL);
    int n_ips = (int)(sizeof(IPS) / sizeof(IPS[0]));
    int n_urls = (int)(sizeof(URLS) / sizeof(URLS[0]));
    int n_codes = (int)(sizeof(CODES) / sizeof(CODES[0]));

    for (long i = 0; i < num_lines; i++) {
        if (i > 0 && i % 200000 == 0) {
            fprintf(f, "LINEA_CORRUPTA_SIN_FORMATO_VALIDO %ld\n", i);
            continue;
        }
        const char *ip = IPS[rand_r(&seed) % n_ips];
        const char *url = URLS[rand_r(&seed) % n_urls];
        int code = CODES[rand_r(&seed) % n_codes];
        int bytes = 200 + (rand_r(&seed) % 5000);
        double resp_time = (double)(1 + (rand_r(&seed) % 500)) / 1000.0;

        fprintf(f, "%s - - [09/Nov/2026:14:03:51 -0500] \"GET %s HTTP/1.1\" %d %d \"-\" \"Mozilla/5.0\" %.3f\n",
                ip, url, code, bytes, resp_time);
    }

    fclose(f);
    printf("Generadas %ld lineas en %s\n", num_lines, out_path);
    return 0;
}