// Small file, clock and kernel statistics helpers.
#include "resource_manager.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

void log_event(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    putchar('\n');
    fflush(stdout);
}

// Users choose process names, so anything that could forge a log line is replaced.
void sanitize_log_text(char *text) {
    for (; *text; text++) {
        unsigned char character = (unsigned char)*text;
        if (!isalnum(character) && !strchr(":_.-@\\", character)) *text = '_';
    }
}

bool join_child_path(char *buffer, size_t size, const char *parent, const char *name) {
    int length = snprintf(buffer, size, "%s/%s", parent, name);
    return length > 0 && (size_t)length < size;
}

// A path this daemon acts on must not be a link, and nobody but its own user may change it.
bool path_is_trusted(const char *path, bool directory) {
    struct stat info;
    if (lstat(path, &info) != 0) return false;
    if (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)) return false;
    return info.st_uid == geteuid() && !(info.st_mode & (S_IWGRP | S_IWOTH));
}

// logind keeps one file per seat. seat0 comes first, because it is the seat of an ordinary desktop.
int active_seat_uids(uid_t *uids, int capacity) {
    DIR *directory = opendir(g_paths.seat_directory);
    if (!directory) return 0;
    int count = 0;
    struct dirent *entry;
    while (count < capacity && (entry = readdir(directory))) {
        char path[PATH_BUFFER_SIZE], line[128];
        if (entry->d_name[0] == '.' || !join_child_path(path, sizeof path, g_paths.seat_directory, entry->d_name)) continue;
        FILE *file = open_for_reading(path);
        if (!file) continue;
        while (fgets(line, sizeof line, file)) {
            if (strncmp(line, "ACTIVE_UID=", 11)) continue;
            char *end;
            unsigned long uid = strtoul(line + 11, &end, 10);
            if (end == line + 11) break;
            if (!strcmp(entry->d_name, "seat0") && count) {
                uids[count++] = uids[0];
                uids[0] = (uid_t)uid;
            } else {
                uids[count++] = (uid_t)uid;
            }
            break;
        }
        fclose(file);
    }
    closedir(directory);
    return count;
}

bool uid_has_active_seat(uid_t uid) {
    uid_t uids[MAX_SEATS];
    int count = active_seat_uids(uids, MAX_SEATS);
    for (int i = 0; i < count; i++) {
        if (uids[i] == uid) return true;
    }
    return false;
}

// Cgroup names come from users, so nothing read from their tree may follow a link.
FILE *open_for_reading(const char *path) {
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return NULL;
    FILE *file = fdopen(descriptor, "r");
    if (!file) close(descriptor);
    return file;
}

bool read_text_file(const char *path, char *buffer, size_t buffer_size) {
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    ssize_t length = read(descriptor, buffer, buffer_size - 1);
    close(descriptor);
    if (length < 0) return false;
    buffer[length] = '\0';
    buffer[strcspn(buffer, "\n")] = '\0';
    return true;
}

long long read_limit_file(const char *path, long long fallback) {
    char text[64];
    if (!read_text_file(path, text, sizeof text)) return fallback;
    if (!strcmp(text, "max")) return LIMIT_UNLIMITED;
    return atoll(text);
}

long long read_keyed_value(const char *path, const char *key, long long fallback) {
    FILE *file = open_for_reading(path);
    if (!file) return fallback;
    char line[256];
    size_t key_length = strlen(key);
    long long value = fallback;
    while (fgets(line, sizeof line, file)) {
        if (!strncmp(line, key, key_length) && (line[key_length] == ' ' || line[key_length] == ':')) {
            value = atoll(line + key_length + 1);
            break;
        }
    }
    fclose(file);
    return value;
}

bool write_text_file(const char *path, const char *text) {
    int descriptor = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    ssize_t written = write(descriptor, text, strlen(text));
    close(descriptor);
    return written >= 0;
}

bool write_limit_file(const char *path, long long value) {
    char text[32];
    if (value == LIMIT_UNLIMITED) snprintf(text, sizeof text, "max");
    else snprintf(text, sizeof text, "%lld", value);
    return write_text_file(path, text);
}

uint64_t memory_available_bytes(void) {
    long long kib = read_keyed_value("/proc/meminfo", "MemAvailable", 0);
    return kib > 0 ? (uint64_t)kib * 1024 : 0;
}

uint64_t swap_total_bytes(void) {
    long long kib = read_keyed_value("/proc/meminfo", "SwapTotal", 0);
    return kib > 0 ? (uint64_t)kib * 1024 : 0;
}

// Integer arithmetic on hundredths of a percent keeps large totals exact.
uint64_t percent_of(uint64_t total, double percent) {
    uint64_t hundredths = (uint64_t)(percent * 100.0 + 0.5);
    return total / 10000 * hundredths + total % 10000 * hundredths / 10000;
}

struct pressure_sample read_pressure(const char *path) {
    struct pressure_sample sample = {100.0, 0};
    FILE *file = open_for_reading(path);
    if (!file) return sample;
    if (fscanf(file, "some avg10=%lf avg60=%*f avg300=%*f total=%llu", &sample.some_average_10s,
               &sample.some_total_microseconds) != 2) {
        sample.some_average_10s = 100.0;
    }
    fclose(file);
    return sample;
}

long read_vmstat_value(const char *name) {
    return (long)read_keyed_value("/proc/vmstat", name, 0);
}
