// Runtime hardware and kernel discovery. Nothing here assumes a specific machine or driver.
#include "resource_manager.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

struct system_info g_system;
struct runtime_paths g_paths;

// Prefer the scheduler capacity, then CPPC, then the maximum frequency.
static long cpu_rank(int cpu) {
    static const char *const SOURCES[] = {"cpu_capacity", "acpi_cppc/highest_perf", "cpufreq/cpuinfo_max_freq"};
    for (size_t i = 0; i < sizeof SOURCES / sizeof *SOURCES; i++) {
        char path[128], text[32];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/%s", cpu, SOURCES[i]);
        if (read_text_file(path, text, sizeof text)) return atol(text);
    }
    return 1;
}

static int compare_long_descending(const void *left, const void *right) {
    long a = *(const long *)left, b = *(const long *)right;
    return a < b ? 1 : a > b ? -1 : 0;
}

// The fastest class is PERFORMANCE and, with three or more classes, the slowest is LOW_POWER.
// Everything in between is EFFICIENCY. Uniform machines place every CPU in every class.
static void detect_cpu_classes(void) {
    CPU_ZERO(&g_system.cpus_all);
    CPU_ZERO(&g_system.cpus_performance);
    CPU_ZERO(&g_system.cpus_efficiency);
    CPU_ZERO(&g_system.cpus_low_power);
    if (sched_getaffinity(0, sizeof g_system.cpus_all, &g_system.cpus_all) != 0) {
        for (int cpu = 0; cpu < sysconf(_SC_NPROCESSORS_ONLN); cpu++) CPU_SET(cpu, &g_system.cpus_all);
    }
    g_system.cpu_count = CPU_COUNT(&g_system.cpus_all);

    long ranks[CPU_SETSIZE];
    int rank_count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &g_system.cpus_all)) continue;
        long rank = cpu_rank(cpu);
        bool seen = false;
        for (int i = 0; i < rank_count; i++) if (ranks[i] == rank) seen = true;
        if (!seen) ranks[rank_count++] = rank;
    }
    qsort(ranks, (size_t)rank_count, sizeof *ranks, compare_long_descending);
    g_system.cpu_class_count = rank_count;

    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &g_system.cpus_all)) continue;
        long rank = cpu_rank(cpu);
        if (rank_count <= 1) {
            CPU_SET(cpu, &g_system.cpus_performance);
            CPU_SET(cpu, &g_system.cpus_efficiency);
            CPU_SET(cpu, &g_system.cpus_low_power);
        } else if (rank == ranks[0]) {
            CPU_SET(cpu, &g_system.cpus_performance);
        } else if (rank_count >= 3 && rank == ranks[rank_count - 1]) {
            CPU_SET(cpu, &g_system.cpus_low_power);
        } else {
            CPU_SET(cpu, &g_system.cpus_efficiency);
        }
    }
    if (CPU_COUNT(&g_system.cpus_low_power) == 0) g_system.cpus_low_power = g_system.cpus_efficiency;
}

bool system_info_detect(void) {
    long long total_kib = read_keyed_value("/proc/meminfo", "MemTotal", 0);
    if (total_kib <= 0) return false;
    g_system.memory_total_bytes = (uint64_t)total_kib * 1024;
    g_system.page_size_bytes = sysconf(_SC_PAGESIZE);
    detect_cpu_classes();
    return true;
}

bool swap_layout_read(struct swap_layout *layout) {
    memset(layout, 0, sizeof *layout);
    FILE *file = fopen(g_paths.proc_swaps, "r");
    if (!file) return false;
    char line[PATH_BUFFER_SIZE + 128], name[PATH_BUFFER_SIZE], type[32];
    long long size, used;
    int priority, index = 0;
    size_t directory_length = strlen(g_policy.swap_file_directory);
    if (!fgets(line, sizeof line, file)) {
        fclose(file);
        return false;
    }
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "%639s %31s %lld %lld %d", name, type, &size, &used, &priority) != 5) continue;
        layout->free_kib += size - used;
        if (!strncmp(name, "/dev/zram", 9)) {
            layout->zram_present = true;
            if (index == 0) layout->zram_is_type_zero = true;
        }
        index++;
        if (!strcmp(type, "file") && !strncmp(name, g_policy.swap_file_directory, directory_length) &&
            name[directory_length] == '/' && layout->managed_file_count < MAX_SWAP_FILES) {
            struct swap_file_entry *entry = &layout->managed_files[layout->managed_file_count++];
            snprintf(entry->path, sizeof entry->path, "%s", name);
            entry->size_kib = size;
            entry->used_kib = used;
            layout->managed_used_kib += used;
        }
    }
    fclose(file);
    return true;
}

// Resolves a device number to its whole disk directory under /sys/block style paths.
static bool whole_disk_directory(dev_t device, char *directory, size_t size) {
    char link[128], probe[PATH_BUFFER_SIZE + 16];
    snprintf(link, sizeof link, "/sys/dev/block/%u:%u", major(device), minor(device));
    // realpath needs a PATH_MAX buffer unless it allocates the result itself.
    char *resolved = realpath(link, NULL);
    if (!resolved) return false;
    int length = snprintf(directory, size, "%s", resolved);
    free(resolved);
    if (length < 0 || (size_t)length >= size) return false;
    snprintf(probe, sizeof probe, "%.600s/partition", directory);
    if (access(probe, F_OK) == 0) {
        char *slash = strrchr(directory, '/');
        if (slash) *slash = '\0';
    }
    return true;
}

int backing_disks_of_swap_files(const struct swap_layout *layout, char disks[][PATH_BUFFER_SIZE], int capacity) {
    int count = 0;
    for (int i = 0; i < layout->managed_file_count && count < capacity; i++) {
        struct stat info;
        char directory[PATH_BUFFER_SIZE];
        if (stat(layout->managed_files[i].path, &info) != 0) continue;
        if (!whole_disk_directory(info.st_dev, directory, sizeof directory)) continue;
        bool seen = false;
        for (int k = 0; k < count; k++) if (!strcmp(disks[k], directory)) seen = true;
        if (!seen) snprintf(disks[count++], PATH_BUFFER_SIZE, "%s", directory);
    }
    return count;
}

// DRM fdinfo sizes are bytes unless followed by KiB or MiB.
static uint64_t parse_drm_size(const char *text) {
    char *end;
    uint64_t value = strtoull(text, &end, 10);
    while (isspace((unsigned char)*end)) end++;
    if (!strncmp(end, "KiB", 3)) return value * 1024;
    if (!strncmp(end, "MiB", 3)) return value * MEBIBYTE;
    return value;
}

// Uses the driver independent DRM fdinfo keys. A client is counted once per drm-client-id.
uint64_t gpu_memory_bytes_of_process(pid_t pid) {
    char directory_path[64];
    snprintf(directory_path, sizeof directory_path, "/proc/%d/fdinfo", pid);
    DIR *directory = opendir(directory_path);
    if (!directory) return 0;
    uint64_t total_bytes = 0;
    long seen_clients[64];
    int seen_count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!isdigit((unsigned char)entry->d_name[0])) continue;
        char path[PATH_BUFFER_SIZE], line[160];
        snprintf(path, sizeof path, "%s/%s", directory_path, entry->d_name);
        FILE *file = fopen(path, "r");
        if (!file) continue;
        long client = -1;
        uint64_t client_bytes = 0;
        bool is_drm = false;
        while (fgets(line, sizeof line, file)) {
            if (!strncmp(line, "drm-driver:", 11)) is_drm = true;
            else if (!strncmp(line, "drm-client-id:", 14)) client = atol(line + 14);
            else if (!strncmp(line, "drm-total-", 10)) {
                char *colon = strchr(line, ':');
                if (colon) client_bytes += parse_drm_size(colon + 1);
            }
        }
        fclose(file);
        if (!is_drm) continue;
        bool seen = false;
        for (int i = 0; i < seen_count; i++) if (seen_clients[i] == client) seen = true;
        if (seen) continue;
        if (seen_count < 64) seen_clients[seen_count++] = client;
        total_bytes += client_bytes;
    }
    closedir(directory);
    return total_bytes;
}

// DRM fdinfo reports busy time as drm-engine-<name>: <ns> ns on most drivers. Drivers that report
// cycles instead simply yield an unavailable summary.
static void gpu_usage_read_descriptor(struct gpu_usage_sample *sample, pid_t pid, const char *descriptor) {
    char path[96], line[160];
    snprintf(path, sizeof path, "/proc/%d/fdinfo/%.32s", pid, descriptor);
    FILE *file = fopen(path, "r");
    if (!file) return;
    long client = -1;
    struct gpu_engine_entry found[8];
    int found_count = 0;
    while (fgets(line, sizeof line, file)) {
        if (!strncmp(line, "drm-client-id:", 14)) {
            client = atol(line + 14);
        } else if (!strncmp(line, "drm-engine-", 11) && strncmp(line, "drm-engine-capacity-", 20) && found_count < 8) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = '\0';
            snprintf(found[found_count].engine, sizeof found[found_count].engine, "%.23s", line + 11);
            found[found_count].busy_nanoseconds = strtoull(colon + 1, NULL, 10);
            found_count++;
        }
    }
    fclose(file);
    for (int i = 0; i < found_count; i++) {
        bool seen = false;
        for (int k = 0; k < sample->entry_count; k++) {
            if (sample->entries[k].client == client && !strcmp(sample->entries[k].engine, found[i].engine)) seen = true;
        }
        if (seen || sample->entry_count >= GPU_ENGINE_ENTRY_CAPACITY) continue;
        struct gpu_engine_entry *entry = &sample->entries[sample->entry_count++];
        *entry = found[i];
        entry->client = client;
        entry->pid = pid;
    }
}

void gpu_usage_take_sample(struct gpu_usage_sample *sample) {
    sample->entry_count = 0;
    sample->taken_at = monotonic_seconds();
    DIR *processes = opendir("/proc");
    if (!processes) return;
    struct dirent *process;
    while ((process = readdir(processes))) {
        if (!isdigit((unsigned char)process->d_name[0])) continue;
        char directory_path[64];
        snprintf(directory_path, sizeof directory_path, "/proc/%.16s/fd", process->d_name);
        DIR *descriptors = opendir(directory_path);
        if (!descriptors) continue;
        struct dirent *descriptor;
        while ((descriptor = readdir(descriptors))) {
            if (!isdigit((unsigned char)descriptor->d_name[0])) continue;
            char link[128], target[32];
            snprintf(link, sizeof link, "%s/%.32s", directory_path, descriptor->d_name);
            ssize_t length = readlink(link, target, sizeof target - 1);
            if (length < 0) continue;
            target[length] = '\0';
            if (!strncmp(target, "/dev/dri/", 9)) gpu_usage_read_descriptor(sample, atoi(process->d_name), descriptor->d_name);
        }
        closedir(descriptors);
    }
    closedir(processes);
}

void gpu_usage_summarize(const struct gpu_usage_sample *first, const struct gpu_usage_sample *second, struct gpu_usage_summary *summary) {
    memset(summary, 0, sizeof *summary);
    double elapsed = second->taken_at - first->taken_at;
    if (elapsed <= 0 || !first->entry_count || !second->entry_count) return;
    uint64_t engine_totals[16] = {0};
    char engine_names[16][GPU_ENGINE_NAME_SIZE];
    int engine_count = 0;
    uint64_t top_delta = 0;
    pid_t top_pid = 0;
    for (int i = 0; i < second->entry_count; i++) {
        const struct gpu_engine_entry *now = &second->entries[i];
        for (int k = 0; k < first->entry_count; k++) {
            const struct gpu_engine_entry *before = &first->entries[k];
            if (before->client != now->client || strcmp(before->engine, now->engine)) continue;
            if (now->busy_nanoseconds < before->busy_nanoseconds) break;
            uint64_t delta = now->busy_nanoseconds - before->busy_nanoseconds;
            int slot = 0;
            while (slot < engine_count && strcmp(engine_names[slot], now->engine)) slot++;
            if (slot == engine_count && engine_count < 16) snprintf(engine_names[engine_count++], GPU_ENGINE_NAME_SIZE, "%.23s", now->engine);
            if (slot < 16) engine_totals[slot] += delta;
            if (delta > top_delta) { top_delta = delta; top_pid = now->pid; }
            break;
        }
    }
    if (!engine_count) return;
    summary->available = true;
    int busiest = 0;
    for (int i = 1; i < engine_count; i++) if (engine_totals[i] > engine_totals[busiest]) busiest = i;
    summary->busiest_engine_percent = (double)engine_totals[busiest] / (elapsed * 1e7);
    snprintf(summary->busiest_engine, sizeof summary->busiest_engine, "%.23s", engine_names[busiest]);
    snprintf(summary->top_process, sizeof summary->top_process, "NONE");
    if (top_pid) {
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/comm", top_pid);
        read_text_file(path, summary->top_process, sizeof summary->top_process);
        sanitize_log_text(summary->top_process);
    }
}

int performance_cpu_average_mhz(void) {
    long long total = 0;
    int count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &g_system.cpus_performance)) continue;
        char path[96], text[32];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        if (read_text_file(path, text, sizeof text)) {
            total += atoll(text);
            count++;
        }
    }
    return count ? (int)(total / count / 1000) : 0;
}

static void print_cpu_set(const char *name, const cpu_set_t *set) {
    printf("%s=", name);
    bool first = true;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, set)) continue;
        int last = cpu;
        while (last + 1 < CPU_SETSIZE && CPU_ISSET(last + 1, set)) last++;
        printf(first ? "%d" : ",%d", cpu);
        if (last != cpu) printf("-%d", last);
        first = false;
        cpu = last;
    }
    printf(" (%d)\n", CPU_COUNT(set));
}

static void print_gpu_devices(void) {
    DIR *directory = opendir("/sys/class/drm");
    if (!directory) {
        printf("GPU_DEVICES=NONE\n");
        return;
    }
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(directory))) {
        if (strncmp(entry->d_name, "card", 4) || !isdigit((unsigned char)entry->d_name[4]) || strchr(entry->d_name, '-')) continue;
        char link[PATH_BUFFER_SIZE], target[PATH_BUFFER_SIZE], vendor[32] = "?", probe[PATH_BUFFER_SIZE];
        snprintf(link, sizeof link, "/sys/class/drm/%.64s/device/driver", entry->d_name);
        ssize_t length = readlink(link, target, sizeof target - 1);
        if (length < 0) continue;
        target[length] = '\0';
        const char *driver = strrchr(target, '/');
        snprintf(probe, sizeof probe, "/sys/class/drm/%.64s/device/vendor", entry->d_name);
        read_text_file(probe, vendor, sizeof vendor);
        printf("GPU_DEVICE=%s DRIVER=%s VENDOR=%s\n", entry->d_name, driver ? driver + 1 : target, vendor);
        count++;
    }
    closedir(directory);
    if (!count) printf("GPU_DEVICES=NONE\n");
    char capacity[256] = "";
    bool has_dmem = read_text_file("/sys/fs/cgroup/dmem.capacity", capacity, sizeof capacity) && capacity[0];
    printf("GPU_MEMORY_CGROUP_CONTROL=%s\n", has_dmem ? "AVAILABLE_NOT_ENFORCED" : "UNAVAILABLE");
    uint64_t own = gpu_memory_bytes_of_process(getpid());
    printf("GPU_FDINFO_ACCOUNTING=DRM_STANDARD_KEYS (self=%llu MiB)\n", (unsigned long long)(own / MEBIBYTE));
}

static void print_swap_layout(void) {
    struct swap_layout layout;
    if (!swap_layout_read(&layout)) {
        printf("SWAP_LAYOUT=UNREADABLE\n");
        return;
    }
    printf("SWAP_TOTAL_MIB=%llu\n", (unsigned long long)(swap_total_bytes() / MEBIBYTE));
    printf("ZRAM_PRESENT=%d\nZRAM_IS_FIRST_SWAP_TYPE=%d\n", layout.zram_present, layout.zram_is_type_zero);
    printf("MANAGED_SWAP_FILES=%d\nMANAGED_SWAP_USED_MIB=%lld\n", layout.managed_file_count, layout.managed_used_kib / 1024);

    char disks[MAX_BACKING_DISKS][PATH_BUFFER_SIZE];
    int disk_count = backing_disks_of_swap_files(&layout, disks, MAX_BACKING_DISKS);
    struct stat info;
    char directory[PATH_BUFFER_SIZE];
    if (!disk_count && (stat(g_policy.swap_file_directory, &info) == 0 || stat("/var/lib", &info) == 0) &&
        whole_disk_directory(info.st_dev, directory, sizeof directory)) {
        snprintf(disks[0], PATH_BUFFER_SIZE, "%.600s", directory);
        disk_count = 1;
    }
    for (int i = 0; i < disk_count; i++) {
        char path[PATH_BUFFER_SIZE + 32], rotational[8] = "?", scheduler[128] = "?";
        snprintf(path, sizeof path, "%.600s/queue/rotational", disks[i]);
        read_text_file(path, rotational, sizeof rotational);
        snprintf(path, sizeof path, "%.600s/queue/scheduler", disks[i]);
        read_text_file(path, scheduler, sizeof scheduler);
        const char *name = strrchr(disks[i], '/');
        printf("SWAP_DISK=%s ROTATIONAL=%s SCHEDULER=\"%s\"\n", name ? name + 1 : disks[i], rotational, scheduler);
    }
}

void system_info_print_probe(void) {
    char controllers[256] = "", delegated[256] = "", mounts[1024];
    printf("MEMORY_TOTAL_MIB=%llu\nMEMORY_AVAILABLE_MIB=%llu\nPAGE_SIZE_BYTES=%ld\n",
           (unsigned long long)(g_system.memory_total_bytes / MEBIBYTE),
           (unsigned long long)(memory_available_bytes() / MEBIBYTE), g_system.page_size_bytes);
    printf("CPU_COUNT=%d\nCPU_CLASS_COUNT=%d\n", g_system.cpu_count, g_system.cpu_class_count);
    print_cpu_set("CPU_ALL", &g_system.cpus_all);
    print_cpu_set("CPU_PERFORMANCE", &g_system.cpus_performance);
    print_cpu_set("CPU_EFFICIENCY", &g_system.cpus_efficiency);
    print_cpu_set("CPU_LOW_POWER", &g_system.cpus_low_power);
    print_swap_layout();
    print_gpu_devices();
    char path[PATH_BUFFER_SIZE + 32];
    snprintf(path, sizeof path, "%.600s/cgroup.controllers", g_paths.cgroup_root);
    read_text_file(path, controllers, sizeof controllers);
    snprintf(path, sizeof path, "%.500s/user.slice/user-%d.slice/user@%d.service/cgroup.controllers", g_paths.cgroup_root, getuid(), getuid());
    read_text_file(path, delegated, sizeof delegated);
    printf("CGROUP_CONTROLLERS=\"%s\"\n", controllers);
    if (delegated[0]) printf("CGROUP_USER_DELEGATED=\"%s\"\n", delegated);
    bool recursive = false;
    FILE *file = fopen("/proc/mounts", "r");
    while (file && fgets(mounts, sizeof mounts, file)) {
        if (strstr(mounts, " cgroup2 ") && strstr(mounts, "memory_recursiveprot")) recursive = true;
    }
    if (file) fclose(file);
    printf("CGROUP_MEMORY_RECURSIVE_PROTECTION=%d\n", recursive);
}
