// Shared declarations for adaptive-resource-managerd.
#ifndef RESOURCE_MANAGER_H
#define RESOURCE_MANAGER_H

#define _GNU_SOURCE
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define PATH_BUFFER_SIZE 640
#define MEBIBYTE (1024ULL * 1024ULL)
#define LIMIT_UNLIMITED (-1LL)
#define MAX_BACKING_DISKS 8
#define MAX_SWAP_FILES 32
#define MAX_SEATS 8

enum run_mode { RUN_MODE_OBSERVE, RUN_MODE_ENFORCE };

enum cpu_selector_kind {
    CPU_SELECTOR_ALL,
    CPU_SELECTOR_PERFORMANCE,
    CPU_SELECTOR_EFFICIENCY,
    CPU_SELECTOR_LOW_POWER,
    CPU_SELECTOR_LEAST_BUSY,
};

struct cpu_selector {
    enum cpu_selector_kind kind;
    double percent; // Share of the selected class; 100.00 means the whole class.
};

struct policy_config {
    enum run_mode run_mode;
    int focus_debounce_milliseconds;
    int background_grace_seconds;
    double foreground_memory_high_percent;
    double foreground_memory_low_percent;
    double background_memory_high_percent;
    double foreground_swap_max_percent;
    double background_swap_max_percent;
    double foreground_cpu_weight_percent;
    double background_cpu_weight_percent;
    double background_cpu_quota_percent;
    struct cpu_selector foreground_cpu_set;
    struct cpu_selector background_cpu_set;
    int foreground_readback_enabled;
    int foreground_readback_include_zram;
    double foreground_readback_reserve_percent;
    double foreground_disk_busy_target_percent;
    double foreground_pressure_stop_percent;
    int idle_readback_enabled;
    int idle_readback_calm_seconds;
    double idle_readback_reserve_percent;
    double idle_disk_busy_target_percent;
    double idle_pressure_stop_percent;
    int readback_step_milliseconds;
    double readback_initial_rate_percent_of_memory_per_second;
    double readback_max_rate_percent_of_memory_per_second;
    double readback_max_chunk_percent_of_memory;
    double swap_file_retire_unreachable_percent;
    double swap_file_retire_worst_case_read_percent;
    double swap_file_retire_min_free_swap_percent;
    double swap_file_min_free_disk_percent;
    double swap_file_growth_margin_percent_of_memory;
    char swap_file_directory[PATH_BUFFER_SIZE];
};

struct system_info {
    uint64_t memory_total_bytes;
    int cpu_count;
    int cpu_class_count;
    cpu_set_t cpus_all;
    cpu_set_t cpus_performance;
    cpu_set_t cpus_efficiency;
    cpu_set_t cpus_low_power;
    long page_size_bytes;
};

struct runtime_paths {
    char cgroup_root[PATH_BUFFER_SIZE];
    char runtime_directory[PATH_BUFFER_SIZE];
    char proc_swaps[PATH_BUFFER_SIZE];
    char seat_directory[PATH_BUFFER_SIZE];
};

struct pressure_sample {
    double some_average_10s;
    unsigned long long some_total_microseconds;
};

struct swap_file_entry {
    char path[PATH_BUFFER_SIZE];
    long long size_kib;
    long long used_kib;
};

struct swap_layout {
    struct swap_file_entry managed_files[MAX_SWAP_FILES];
    int managed_file_count;
    long long managed_used_kib;
    long long free_kib;
    bool zram_is_type_zero;
    bool zram_present;
};

struct group_swap_counts {
    uint64_t zram_pages;
    uint64_t disk_pages;
};

#define GPU_ENGINE_ENTRY_CAPACITY 512
#define GPU_ENGINE_NAME_SIZE 24
#define PROCESS_NAME_SIZE 32

struct gpu_engine_entry {
    long client;
    pid_t pid;
    char engine[GPU_ENGINE_NAME_SIZE];
    uint64_t busy_nanoseconds;
};

struct gpu_usage_sample {
    double taken_at;
    int entry_count;
    struct gpu_engine_entry entries[GPU_ENGINE_ENTRY_CAPACITY];
};

struct gpu_usage_summary {
    bool available;
    double busiest_engine_percent;
    char busiest_engine[GPU_ENGINE_NAME_SIZE];
    char top_process[PROCESS_NAME_SIZE];
};

struct app_group;

extern struct policy_config g_policy;
extern struct system_info g_system;
extern struct runtime_paths g_paths;

// util.c
double monotonic_seconds(void);
void log_event(const char *format, ...) __attribute__((format(printf, 1, 2)));
void sanitize_log_text(char *text);
bool join_child_path(char *buffer, size_t size, const char *parent, const char *name);
bool path_is_trusted(const char *path, bool directory);
int active_seat_uids(uid_t *uids, int capacity);
bool uid_has_active_seat(uid_t uid);
FILE *open_for_reading(const char *path);
bool read_text_file(const char *path, char *buffer, size_t buffer_size);
long long read_limit_file(const char *path, long long fallback);
long long read_keyed_value(const char *path, const char *key, long long fallback);
bool write_text_file(const char *path, const char *text);
bool write_limit_file(const char *path, long long value);
uint64_t memory_available_bytes(void);
uint64_t swap_total_bytes(void);
uint64_t percent_of(uint64_t total, double percent);
struct pressure_sample read_pressure(const char *path);
long read_vmstat_value(const char *name);

// config.c
void policy_set_defaults(void);
bool policy_load_file(const char *path);
bool policy_validate(void);
void policy_print(void);
const char *cpu_selector_name(const struct cpu_selector *selector, char *buffer, size_t buffer_size);

// system_info.c
bool system_info_detect(void);
void system_info_print_probe(void);
bool swap_layout_read(struct swap_layout *layout);
int backing_disks_of_swap_files(const struct swap_layout *layout, char disks[][PATH_BUFFER_SIZE], int capacity);
uint64_t gpu_memory_bytes_of_process(pid_t pid);
void gpu_usage_take_sample(struct gpu_usage_sample *sample);
void gpu_usage_summarize(const struct gpu_usage_sample *first, const struct gpu_usage_sample *second, struct gpu_usage_summary *summary);
int performance_cpu_average_mhz(void);

// app_groups.c
void app_groups_load_saved_originals(void);
void app_groups_refresh(void);
void app_groups_restore_all(void);
void app_groups_process_timers(double now);
void app_groups_set_foreground(const char *group_path, double now);
const char *app_groups_foreground_path(void);
const char *app_groups_foreground_name(void);
void app_groups_foreground_cpu_weight_text(char *buffer, size_t size);
bool app_group_path_of_pid(pid_t pid, uid_t owner, char *group_path, size_t size);
int app_groups_count(void);

// readback.c
enum readback_kind { READBACK_NONE, READBACK_FOREGROUND, READBACK_IDLE };
void readback_start_foreground(const char *group_path);
void readback_cancel(void);
void readback_tick(double now);
void readback_disk_guard_tick(double now);
double readback_next_due_seconds(double now);
enum readback_kind readback_active_kind(void);
void readback_measure_group(const char *group_path, struct group_swap_counts *counts, uint64_t *gpu_bytes);
void readback_status_text(char *buffer, size_t buffer_size);

#endif
