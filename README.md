# LogPulse

Analizador concurrente de logs de servidor web en **C11** bajo Linux Ubuntu con arquitectura **Coordinador / Trabajadores**, IPC mediante **memoria compartida POSIX + semáforos** y **tuberías (`pipe`)**, manejo robusto de señales (`sigaction`) e interfaz interactiva en terminal con **`ncurses`**.

## Integrantes

* **Juan Pablo Vergara Pinillos**
* **Cristian Andrés Olivero Garzón**
* **Andres **

## Arquitectura

* **Proceso Coordinador (`src/coordinator.c`):**
  * Crea el segmento de memoria compartida en `/dev/shm` (`shm_open`, `ftruncate`, `mmap`) protegido con semáforos POSIX (`sem_init` con `pshared = 1`).
  * Lanza un proceso trabajador por archivo mediante `fork()` y nunca lee los archivos de log directamente.
  * Mantiene un **hilo de estado** dedicado a leer las tuberías (`pipe`) de los trabajadores usando `poll()`.
  * Captura `SIGINT`, `SIGTERM` y `SIGCHLD` con `sigaction()` (patrón *self-pipe*) para detectar caídas inesperadas (como `kill -9`) en tiempo real y ejecutar un cierre limpio sin procesos zombis ni fugas en `/dev/shm`.
* **Procesos Trabajadores (`src/worker.c`, `src/parser.c`):**
  * Cada trabajador ejecuta un **hilo lector** (modo `lote` o modo `vivo` mediante `inotify` + `poll()` sin espera activa), un **búfer circular acotado** (`pthread_mutex_t` y `pthread_cond_t`), un pool de `N` **hilos parseadores** con terminación ordenada por píldora venenosa y un **hilo publicador** que actualiza periódicamente la memoria compartida.
* **Interfaz `ncurses` (`src/ui.c`):**
  * Hilo dedicado con lectura de teclado no bloqueante (`nodelay` + `poll()`), soporte de redimensionado (`SIGWINCH` / `KEY_RESIZE`), refresco de panel al menos una vez por segundo y exportación consolidada a CSV.

## Requisitos e instalación (Ubuntu)

```bash
sudo apt update
sudo apt install -y build-essential libncurses-dev valgrind
```

## Compilación

```bash
make clean && make
```

Compila sin advertencias con `-std=c11 -O2 -Wall -Wextra` generando los binarios `./logpulse` y `./loggen`.

Para compilar con **ThreadSanitizer**:

```bash
make tsan
```

Para compilar con símbolos de depuración para **Valgrind**:

```bash
make debug
```

## Uso

### 1. Generar logs sintéticos (`loggen`)

```bash
./loggen -n 50000 -o acceso1.log
./loggen -n 50000 -o acceso2.log
```

Para simular tráfico en tiempo real (modo `vivo`):

```bash
./loggen -n 2000 -d 200 -o vivo.log
```

### 2. Ejecutar `logpulse`

```bash
./logpulse [-m lote|vivo] [-t N] archivo1 [archivo2 ...]
```

* `-m lote|vivo`: modo de lectura (`lote` por defecto, `vivo` monitorea nuevas líneas sin espera activa).
* `-t N`: número de hilos parseadores por trabajador (`1` a `16`, por defecto `4`).

Ejemplo con múltiples archivos:

```bash
./logpulse -m lote -t 4 acceso1.log acceso2.log
```

### Controles en la interfaz `ncurses`

* `q`: iniciar cierre limpio del coordinador y todos los trabajadores.
* `p`: pausar o reanudar el procesamiento y refresco de métricas.
* `e`: exportar las métricas consolidadas actuales a `metricas_logpulse.csv` sin detener la ejecución.

## Verificación de memoria y concurrencia

```bash
valgrind --leak-check=full --show-leak-kinds=all ./logpulse -m lote -t 4 acceso1.log
```

```bash
make tsan
./logpulse -m lote -t 4 acceso1.log acceso2.log
```
