// Test helper. "hold MIB" keeps memory and reads commands from stdin: p=page out, v=verify, q=quit.
// "allocate MIB" fills memory, prints the time it took and exits, which creates memory pressure.
// "pressure MIB SECONDS" fills memory, then keeps touching random parts and reports the latency.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int compare_doubles(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return a < b ? -1 : a > b;
}

// Touches 256 random pages per round: the same batch size as the earlier pressure tests.
static void run_pressure(const char *memory, size_t size, double seconds) {
    enum { MAX_ROUNDS = 2000000, PAGES_PER_ROUND = 256, PAGE_BYTES = 4096 };
    double *latency = malloc(sizeof *latency * MAX_ROUNDS);
    if (!latency) return;
    size_t pages = size / PAGE_BYTES, rounds = 0;
    unsigned long state = 2463534242UL;
    volatile unsigned long sink = 0;
    double end = monotonic_seconds() + seconds;
    while (monotonic_seconds() < end && rounds < MAX_ROUNDS) {
        double started = monotonic_seconds();
        for (int i = 0; i < PAGES_PER_ROUND; i++) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            sink += *(const unsigned long *)(memory + (state % pages) * PAGE_BYTES);
        }
        latency[rounds++] = monotonic_seconds() - started;
    }
    (void)sink;
    if (rounds) {
        qsort(latency, rounds, sizeof *latency, compare_doubles);
        printf("PRESSURE_FINISHED rounds=%zu touch_median_ms=%.2f touch_p99_ms=%.2f touch_max_ms=%.2f\n", rounds, latency[rounds / 2] * 1e3,
               latency[rounds * 99 / 100] * 1e3, latency[rounds - 1] * 1e3);
    }
    free(latency);
}

static unsigned long checksum(const char *memory, size_t size) {
    unsigned long hash = 1469598103934665603UL;
    for (size_t i = 0; i < size; i += 4096) {
        hash ^= *(const unsigned long *)(memory + i);
        hash *= 1099511628211UL;
    }
    return hash;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    char *end;
    long size_mib = strtol(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || size_mib <= 0 || (unsigned long)size_mib > SIZE_MAX / (1024 * 1024)) return 2;
    size_t size = (size_t)size_mib * 1024 * 1024;
    char *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    madvise(memory, size, MADV_NOHUGEPAGE);
    double started = monotonic_seconds();
    unsigned long state = 88172645463325252UL;
    for (size_t i = 0; i < size; i += 8) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        *(unsigned long *)(memory + i) = (i / 8) % 2 ? state : 0;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!strcmp(argv[1], "pressure")) {
        printf("FILLED_MIB=%s SECONDS=%.2f\n", argv[2], monotonic_seconds() - started);
        fflush(stdout);
        run_pressure(memory, size, argc > 3 ? atof(argv[3]) : 60.0);
        return 0;
    }
    if (!strcmp(argv[1], "allocate")) {
        printf("ALLOCATED_MIB=%s SECONDS=%.2f\n", argv[2], monotonic_seconds() - started);
        return 0;
    }
    unsigned long expected = checksum(memory, size);
    printf("READY\n");
    int command;
    while ((command = getchar()) != EOF) {
        if (command == 'q') break;
        if (command == 'p') {
            madvise(memory, size, MADV_PAGEOUT);
            printf("PAGED_OUT\n");
        }
        if (command == 'v') {
            double verify_started = monotonic_seconds();
            int intact = checksum(memory, size) == expected;
            printf("VERIFY=%s SECONDS=%.2f\n", intact ? "OK" : "CORRUPT", monotonic_seconds() - verify_started);
        }
    }
    return 0;
}
