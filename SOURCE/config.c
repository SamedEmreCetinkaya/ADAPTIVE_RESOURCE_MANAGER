// Policy configuration: KEY=VALUE file. Amounts are percentages written with two decimals.
#include "resource_manager.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct policy_config g_policy;

enum key_type { KEY_INTEGER, KEY_PERCENT, KEY_MODE, KEY_CPU_SELECTOR, KEY_STRING };

struct key_definition {
    const char *name;
    enum key_type type;
    size_t offset;
    double minimum;
    double maximum;
};

#define INTEGER_KEY(NAME, FIELD, MINIMUM, MAXIMUM) {NAME, KEY_INTEGER, offsetof(struct policy_config, FIELD), MINIMUM, MAXIMUM}
#define PERCENT_KEY(NAME, FIELD, MINIMUM, MAXIMUM) {NAME, KEY_PERCENT, offsetof(struct policy_config, FIELD), MINIMUM, MAXIMUM}
#define SELECTOR_KEY(NAME, FIELD) {NAME, KEY_CPU_SELECTOR, offsetof(struct policy_config, FIELD), 0, 0}

static const struct key_definition KEY_DEFINITIONS[] = {
    {"RUN_MODE", KEY_MODE, offsetof(struct policy_config, run_mode), 0, 0},
    INTEGER_KEY("FOCUS_DEBOUNCE_MILLISECONDS", focus_debounce_milliseconds, 0, 60000),
    INTEGER_KEY("BACKGROUND_GRACE_SECONDS", background_grace_seconds, 0, 3600),
    PERCENT_KEY("FOREGROUND_MEMORY_HIGH_PERCENT", foreground_memory_high_percent, 1, 100),
    PERCENT_KEY("FOREGROUND_MEMORY_LOW_PERCENT", foreground_memory_low_percent, 0, 100),
    PERCENT_KEY("BACKGROUND_MEMORY_HIGH_PERCENT", background_memory_high_percent, 1, 100),
    PERCENT_KEY("FOREGROUND_SWAP_MAX_PERCENT", foreground_swap_max_percent, 0, 100),
    PERCENT_KEY("BACKGROUND_SWAP_MAX_PERCENT", background_swap_max_percent, 0, 100),
    PERCENT_KEY("FOREGROUND_CPU_WEIGHT_PERCENT", foreground_cpu_weight_percent, 1, 10000),
    PERCENT_KEY("BACKGROUND_CPU_WEIGHT_PERCENT", background_cpu_weight_percent, 1, 10000),
    PERCENT_KEY("BACKGROUND_CPU_QUOTA_PERCENT", background_cpu_quota_percent, 1, 100),
    SELECTOR_KEY("FOREGROUND_CPU_SET", foreground_cpu_set),
    SELECTOR_KEY("BACKGROUND_CPU_SET", background_cpu_set),
    INTEGER_KEY("FOREGROUND_READBACK_ENABLED", foreground_readback_enabled, 0, 1),
    INTEGER_KEY("FOREGROUND_READBACK_INCLUDE_ZRAM", foreground_readback_include_zram, 0, 1),
    PERCENT_KEY("FOREGROUND_READBACK_RESERVE_PERCENT", foreground_readback_reserve_percent, 0, 100),
    PERCENT_KEY("FOREGROUND_DISK_BUSY_TARGET_PERCENT", foreground_disk_busy_target_percent, 1, 100),
    PERCENT_KEY("FOREGROUND_PRESSURE_STOP_PERCENT", foreground_pressure_stop_percent, 1, 100),
    INTEGER_KEY("IDLE_READBACK_ENABLED", idle_readback_enabled, 0, 1),
    INTEGER_KEY("IDLE_READBACK_CALM_SECONDS", idle_readback_calm_seconds, 0, 86400),
    PERCENT_KEY("IDLE_READBACK_RESERVE_PERCENT", idle_readback_reserve_percent, 0, 100),
    PERCENT_KEY("IDLE_DISK_BUSY_TARGET_PERCENT", idle_disk_busy_target_percent, 1, 100),
    PERCENT_KEY("IDLE_PRESSURE_STOP_PERCENT", idle_pressure_stop_percent, 1, 100),
    INTEGER_KEY("READBACK_STEP_MILLISECONDS", readback_step_milliseconds, 5, 1000),
    PERCENT_KEY("READBACK_INITIAL_RATE_PERCENT_OF_MEMORY_PER_SECOND", readback_initial_rate_percent_of_memory_per_second, 0.01, 100),
    PERCENT_KEY("READBACK_MAX_RATE_PERCENT_OF_MEMORY_PER_SECOND", readback_max_rate_percent_of_memory_per_second, 0.01, 100),
    PERCENT_KEY("READBACK_MAX_CHUNK_PERCENT_OF_MEMORY", readback_max_chunk_percent_of_memory, 0.01, 10),
    PERCENT_KEY("SWAP_FILE_RETIRE_UNREACHABLE_PERCENT", swap_file_retire_unreachable_percent, 0, 100),
    PERCENT_KEY("SWAP_FILE_RETIRE_WORST_CASE_READ_PERCENT", swap_file_retire_worst_case_read_percent, 0, 100),
    PERCENT_KEY("SWAP_FILE_RETIRE_MIN_FREE_SWAP_PERCENT", swap_file_retire_min_free_swap_percent, 0, 100),
    {"SWAP_FILE_DIRECTORY", KEY_STRING, offsetof(struct policy_config, swap_file_directory), 0, 0},
};

void policy_set_defaults(void) {
    memset(&g_policy, 0, sizeof g_policy);
    g_policy.run_mode = RUN_MODE_OBSERVE;
    g_policy.focus_debounce_milliseconds = 500;
    g_policy.background_grace_seconds = 30;
    g_policy.foreground_memory_high_percent = 65.00;
    g_policy.foreground_memory_low_percent = 30.00;
    g_policy.background_memory_high_percent = 55.00;
    g_policy.foreground_swap_max_percent = 100.00;
    g_policy.background_swap_max_percent = 100.00;
    g_policy.foreground_cpu_weight_percent = 500.00;
    g_policy.background_cpu_weight_percent = 100.00;
    g_policy.background_cpu_quota_percent = 100.00;
    g_policy.foreground_cpu_set = (struct cpu_selector){CPU_SELECTOR_ALL, 100.00};
    g_policy.background_cpu_set = (struct cpu_selector){CPU_SELECTOR_ALL, 100.00};
    g_policy.foreground_readback_enabled = 1;
    g_policy.foreground_readback_include_zram = 1;
    g_policy.foreground_readback_reserve_percent = 20.00;
    g_policy.foreground_disk_busy_target_percent = 60.00;
    g_policy.foreground_pressure_stop_percent = 25.00;
    g_policy.idle_readback_enabled = 1;
    g_policy.idle_readback_calm_seconds = 120;
    g_policy.idle_readback_reserve_percent = 30.00;
    g_policy.idle_disk_busy_target_percent = 25.00;
    g_policy.idle_pressure_stop_percent = 5.00;
    g_policy.readback_step_milliseconds = 60;
    g_policy.readback_initial_rate_percent_of_memory_per_second = 1.00;
    g_policy.readback_max_rate_percent_of_memory_per_second = 10.00;
    g_policy.readback_max_chunk_percent_of_memory = 0.05;
    g_policy.swap_file_retire_unreachable_percent = 1.00;
    g_policy.swap_file_retire_worst_case_read_percent = 5.00;
    g_policy.swap_file_retire_min_free_swap_percent = 30.00;
    snprintf(g_policy.swap_file_directory, sizeof g_policy.swap_file_directory, "/var/lib/swapspace");
}

static bool parse_number(const char *text, double *number) {
    char *end;
    *number = strtod(text, &end);
    return *text != '\0' && *end == '\0' && *number == *number;
}

// ALL, PERFORMANCE, EFFICIENCY, LOW_POWER or LEAST_BUSY, optionally followed by :PERCENT.
static bool parse_cpu_selector(const char *text, struct cpu_selector *selector) {
    static const struct { const char *name; enum cpu_selector_kind kind; } NAMES[] = {
        {"ALL", CPU_SELECTOR_ALL},
        {"PERFORMANCE", CPU_SELECTOR_PERFORMANCE},
        {"EFFICIENCY", CPU_SELECTOR_EFFICIENCY},
        {"LOW_POWER", CPU_SELECTOR_LOW_POWER},
        {"LEAST_BUSY", CPU_SELECTOR_LEAST_BUSY},
    };
    const char *colon = strchr(text, ':');
    size_t name_length = colon ? (size_t)(colon - text) : strlen(text);
    double percent = 100.00;
    if (colon && !parse_number(colon + 1, &percent)) return false;
    if (percent <= 0.0 || percent > 100.0) return false;
    for (size_t i = 0; i < sizeof NAMES / sizeof *NAMES; i++) {
        if (strlen(NAMES[i].name) == name_length && !strncmp(text, NAMES[i].name, name_length)) {
            selector->kind = NAMES[i].kind;
            selector->percent = percent;
            return true;
        }
    }
    return false;
}

const char *cpu_selector_name(const struct cpu_selector *selector, char *buffer, size_t buffer_size) {
    static const char *const NAMES[] = {"ALL", "PERFORMANCE", "EFFICIENCY", "LOW_POWER", "LEAST_BUSY"};
    if (selector->percent >= 100.0) snprintf(buffer, buffer_size, "%s", NAMES[selector->kind]);
    else snprintf(buffer, buffer_size, "%s:%.2f", NAMES[selector->kind], selector->percent);
    return buffer;
}

static bool apply_key(const char *key, const char *value, int line_number) {
    for (size_t i = 0; i < sizeof KEY_DEFINITIONS / sizeof *KEY_DEFINITIONS; i++) {
        const struct key_definition *definition = &KEY_DEFINITIONS[i];
        if (strcmp(definition->name, key)) continue;
        char *field = (char *)&g_policy + definition->offset;
        double number;
        switch (definition->type) {
        case KEY_MODE:
            if (!strcmp(value, "OBSERVE")) g_policy.run_mode = RUN_MODE_OBSERVE;
            else if (!strcmp(value, "ENFORCE")) g_policy.run_mode = RUN_MODE_ENFORCE;
            else goto invalid;
            return true;
        case KEY_INTEGER: {
            char *end;
            long integer = strtol(value, &end, 10);
            if (*value == '\0' || *end != '\0' || integer < definition->minimum || integer > definition->maximum) goto invalid;
            *(int *)field = (int)integer;
            return true;
        }
        case KEY_PERCENT:
            if (!parse_number(value, &number) || number < definition->minimum || number > definition->maximum) goto invalid;
            *(double *)field = number;
            return true;
        case KEY_CPU_SELECTOR:
            if (!parse_cpu_selector(value, (struct cpu_selector *)field)) goto invalid;
            return true;
        case KEY_STRING:
            // The daemon removes files from this directory, so its path must be plain and complete.
            if (value[0] != '/' || strstr(value, "/..") || strlen(value) >= PATH_BUFFER_SIZE || value[strlen(value) - 1] == '/') goto invalid;
            snprintf(field, PATH_BUFFER_SIZE, "%s", value);
            return true;
        }
    invalid:
        fprintf(stderr, "Invalid value at line %d: %s=%s\n", line_number, key, value);
        return false;
    }
    fprintf(stderr, "Unknown key at line %d: %s\n", line_number, key);
    return false;
}

bool policy_load_file(const char *path) {
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        perror(path);
        return false;
    }
    // Running as root, this file decides which limits are written and which files are removed.
    struct stat info;
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
        (geteuid() == 0 && (info.st_uid != 0 || (info.st_mode & (S_IWGRP | S_IWOTH))))) {
        fprintf(stderr, "Refusing %s: it must be a regular file that only root can change\n", path);
        close(descriptor);
        return false;
    }
    FILE *file = fdopen(descriptor, "r");
    if (!file) {
        perror(path);
        close(descriptor);
        return false;
    }
    char line[PATH_BUFFER_SIZE + 128];
    int line_number = 0;
    bool valid = true;
    while (fgets(line, sizeof line, file)) {
        line_number++;
        char *start = line;
        while (isspace((unsigned char)*start)) start++;
        if (*start == '#' || *start == '\0') continue;
        start[strcspn(start, "\r\n")] = '\0';
        char *separator = strchr(start, '=');
        if (!separator) {
            fprintf(stderr, "Missing '=' at line %d\n", line_number);
            valid = false;
            continue;
        }
        *separator = '\0';
        if (!apply_key(start, separator + 1, line_number)) valid = false;
    }
    fclose(file);
    return valid;
}

bool policy_validate(void) {
    bool valid = true;
    if (g_policy.foreground_memory_low_percent > g_policy.foreground_memory_high_percent) {
        fprintf(stderr, "FOREGROUND_MEMORY_LOW_PERCENT must not exceed FOREGROUND_MEMORY_HIGH_PERCENT\n");
        valid = false;
    }
    if (g_policy.readback_initial_rate_percent_of_memory_per_second > g_policy.readback_max_rate_percent_of_memory_per_second) {
        fprintf(stderr, "READBACK_INITIAL_RATE_PERCENT_OF_MEMORY_PER_SECOND must not exceed the maximum rate\n");
        valid = false;
    }
    return valid;
}

static void print_memory_share(const char *name, double percent) {
    printf("%s_PERCENT=%.2f %s_MIB=%llu\n", name, percent, name,
           (unsigned long long)(percent_of(g_system.memory_total_bytes, percent) / MEBIBYTE));
}

// Machine readable so tests can check the policy at any memory size.
void policy_print(void) {
    char foreground[32], background[32];
    printf("RUN_MODE=%s\nMEMORY_TOTAL_MIB=%llu\n", g_policy.run_mode == RUN_MODE_ENFORCE ? "ENFORCE" : "OBSERVE",
           (unsigned long long)(g_system.memory_total_bytes / MEBIBYTE));
    print_memory_share("FOREGROUND_MEMORY_HIGH", g_policy.foreground_memory_high_percent);
    print_memory_share("FOREGROUND_MEMORY_LOW", g_policy.foreground_memory_low_percent);
    print_memory_share("BACKGROUND_MEMORY_HIGH", g_policy.background_memory_high_percent);
    print_memory_share("FOREGROUND_READBACK_RESERVE", g_policy.foreground_readback_reserve_percent);
    print_memory_share("IDLE_READBACK_RESERVE", g_policy.idle_readback_reserve_percent);
    print_memory_share("READBACK_INITIAL_RATE_PER_SECOND", g_policy.readback_initial_rate_percent_of_memory_per_second);
    print_memory_share("READBACK_MAX_RATE_PER_SECOND", g_policy.readback_max_rate_percent_of_memory_per_second);
    print_memory_share("READBACK_MAX_CHUNK", g_policy.readback_max_chunk_percent_of_memory);
    print_memory_share("SWAP_FILE_RETIRE_UNREACHABLE", g_policy.swap_file_retire_unreachable_percent);
    print_memory_share("SWAP_FILE_RETIRE_WORST_CASE_READ", g_policy.swap_file_retire_worst_case_read_percent);
    print_memory_share("SWAP_FILE_RETIRE_MIN_FREE_SWAP", g_policy.swap_file_retire_min_free_swap_percent);
    printf("FOREGROUND_CPU_SET=%s\nBACKGROUND_CPU_SET=%s\n",
           cpu_selector_name(&g_policy.foreground_cpu_set, foreground, sizeof foreground),
           cpu_selector_name(&g_policy.background_cpu_set, background, sizeof background));
}
