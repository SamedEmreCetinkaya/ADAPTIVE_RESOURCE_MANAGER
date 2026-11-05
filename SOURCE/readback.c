// Brings swapped pages back in small paced steps. process_madvise(MADV_WILLNEED) reads a page into
// the swap cache and keeps its swap slot, so the page stays clean and can be dropped without a write.
// Every value is derived from the machine: memory reserves are percentages of total memory, and the
// read rate adapts to how busy the backing disk is rather than to a fixed throughput.
#include "resource_manager.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#define PAGEMAP_SWAP_BIT (1ULL << 62)
#define PAGEMAP_TYPE_MASK 31ULL
#define PAGEMAP_OFFSET_SHIFT 5
#define PAGEMAP_OFFSET_MASK ((1ULL << 50) - 1)
#define SWAP_TYPE_COUNT 32
#define ZRAM_SWAP_TYPE 0 // zram starts first at boot, so it takes the first swap type. Verified at runtime.
#define NO_ZRAM_TYPE (-1)
#define ADDRESS_SPACE_END 0x7ffffffff000UL
#define IOVEC_BATCH 64
#define RATE_ADJUST_INTERVAL_SECONDS 1.0
#define RATE_INCREASE_FACTOR 1.25
#define RATE_DECREASE_FACTOR 0.5
#define RATE_MINIMUM_FRACTION_OF_INITIAL 0.1
#define PRESSURE_SAMPLE_INTERVAL_SECONDS 0.25
#define RESCAN_DELAY_SECONDS 60.0
#define RETIRE_RETRY_DELAY_SECONDS 60.0
#define CGROUP_HEADROOM_CHUNKS 16
#define SCAN_MEMORY_PERCENT_OF_MEMORY 0.25
#define DISK_GUARD_INTERVAL_SECONDS 1.0
#define SWAP_MANAGER_PROCESSES "/system.slice/swapspace.service/cgroup.procs"

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_process_madvise
#define SYS_process_madvise 440
#endif

struct page_range { unsigned long start, length; };

struct readback_target {
    pid_t pid;
    int oom_score_adjust;
    uint64_t pages;
    uint64_t issued_bytes;
    size_t range_count;
    size_t range_capacity;
    struct page_range *ranges;
    char cgroup[PATH_BUFFER_SIZE];
};

struct scan_result {
    struct readback_target *targets;
    size_t target_count;
    size_t target_capacity;
    uint64_t zram_pages;
    uint64_t disk_pages;
    uint64_t distinct_disk_pages;
    unsigned char *slot_bits[SWAP_TYPE_COUNT];
    size_t slot_bit_pages;
    int zram_type; // Swap type holding zram, or NO_ZRAM_TYPE when every type is a disk.
    uint64_t memory_used;
    bool memory_limit_reached;
};

static struct {
    enum readback_kind kind;
    struct scan_result scan;
    size_t target_index, range_index, range_offset;
    int pidfd;
    unsigned chunk_counter;
    double rate_bytes_per_second;
    double next_step_at;
    double next_rate_adjust_at;
    double next_pressure_at;
    double started_at;
    long last_swap_in_pages;
    long start_swap_in_pages;
    uint64_t issued_bytes;
    struct pressure_sample last_pressure;
    double last_pressure_at;
    char disks[MAX_BACKING_DISKS][PATH_BUFFER_SIZE];
    int disk_count;
    unsigned long long last_io_ticks[MAX_BACKING_DISKS];
    double last_io_at;
    bool retire_after;
} g_job = {.pidfd = -1};

static double g_idle_calm_since;
static double g_idle_not_before;
static double g_retire_not_before;

static struct {
    bool growth_held;   // Free disk is too close to the limit for swapspace to add another file.
    bool below_limit;   // Free disk is under the limit. Empty swap files go back at once.
    double next_check_at;
} g_disk_guard;

static bool enforcing(void) { return g_policy.run_mode == RUN_MODE_ENFORCE; }

// Only root can place a process in this systemd group, so its members cannot be forged. SIGUSR2 makes
// swapspace free unneeded files and add none for one cooldown period, the same as running out of disk.
static void hold_swap_growth(void) {
    char path[PATH_BUFFER_SIZE + 64];
    int length = snprintf(path, sizeof path, "%s%s", g_paths.cgroup_root, SWAP_MANAGER_PROCESSES);
    if (length < 0 || (size_t)length >= sizeof path || !path_is_trusted(path, false)) return;
    FILE *file = open_for_reading(path);
    if (!file) return;
    int pid;
    while (fscanf(file, "%d", &pid) == 1) {
        if (pid > 1) kill(pid, SIGUSR2);
    }
    fclose(file);
}

// Swap growth stops early enough that even the largest new file cannot take free disk below the limit.
// The signal repeats, because swapspace would otherwise start adding files again after its cooldown.
void readback_disk_guard_tick(double now) {
    if (!enforcing() || g_policy.swap_file_min_free_disk_percent <= 0 || now < g_disk_guard.next_check_at) return;
    g_disk_guard.next_check_at = now + DISK_GUARD_INTERVAL_SECONDS;
    struct statvfs disk;
    if (statvfs(g_policy.swap_file_directory, &disk) != 0) {
        g_disk_guard.growth_held = g_disk_guard.below_limit = false;
        return;
    }
    uint64_t free_bytes = (uint64_t)disk.f_bavail * disk.f_frsize;
    uint64_t limit = percent_of((uint64_t)disk.f_blocks * disk.f_frsize, g_policy.swap_file_min_free_disk_percent);
    bool held = free_bytes < limit + percent_of(g_system.memory_total_bytes, g_policy.swap_file_growth_margin_percent_of_memory);
    if (held != g_disk_guard.growth_held) {
        log_event("event=SWAP_GROWTH_%s free_disk_mib=%llu limit_mib=%llu", held ? "HELD" : "ALLOWED",
                  (unsigned long long)(free_bytes / MEBIBYTE), (unsigned long long)(limit / MEBIBYTE));
    }
    g_disk_guard.growth_held = held;
    g_disk_guard.below_limit = free_bytes < limit;
    if (held) hold_swap_growth();
}

static void scan_free(struct scan_result *scan) {
    for (size_t i = 0; i < scan->target_count; i++) free(scan->targets[i].ranges);
    free(scan->targets);
    for (int t = 0; t < SWAP_TYPE_COUNT; t++) free(scan->slot_bits[t]);
    memset(scan, 0, sizeof *scan);
}

// A process can swap every other page and make this root daemon track millions of ranges. The result
// stays within a share of memory, and whatever does not fit is simply not read back.
static bool scan_reserve(struct scan_result *scan, size_t bytes) {
    if (scan->memory_used + bytes > percent_of(g_system.memory_total_bytes, SCAN_MEMORY_PERCENT_OF_MEMORY)) {
        if (!scan->memory_limit_reached) log_event("event=SCAN_MEMORY_LIMIT_REACHED used_mib=%llu", (unsigned long long)(scan->memory_used / MEBIBYTE));
        scan->memory_limit_reached = true;
        return false;
    }
    scan->memory_used += bytes;
    return true;
}

static void target_add_page(struct scan_result *scan, struct readback_target *target, unsigned long address, size_t page_size) {
    if (target->range_count) {
        struct page_range *last = &target->ranges[target->range_count - 1];
        if (last->start + last->length == address) {
            last->length += page_size;
            target->pages++;
            return;
        }
    }
    if (!scan_reserve(scan, sizeof *target->ranges)) return;
    if (target->range_count == target->range_capacity) {
        target->range_capacity = target->range_capacity ? target->range_capacity * 2 : 64;
        target->ranges = realloc(target->ranges, target->range_capacity * sizeof *target->ranges);
        if (!target->ranges) {
            log_event("event=OUT_OF_MEMORY");
            exit(1);
        }
    }
    target->ranges[target->range_count].start = address;
    target->ranges[target->range_count].length = page_size;
    target->range_count++;
    target->pages++;
}

static void read_process_cgroup(pid_t pid, char *cgroup, size_t size) {
    char path[64], text[PATH_BUFFER_SIZE];
    cgroup[0] = '\0';
    snprintf(path, sizeof path, "/proc/%d/cgroup", pid);
    FILE *file = fopen(path, "r");
    if (!file) return;
    // A path that does not fit in one line, or that climbs upward, is not used at all.
    if (fgets(text, sizeof text, file) && !strncmp(text, "0::", 3) && strchr(text, '\n') && !strstr(text, "/..")) {
        text[strcspn(text, "\n")] = '\0';
        snprintf(cgroup, size, "%s", text + 3);
    }
    fclose(file);
}

// Finds swapped pages of one process. PAGEMAP_SCAN skips unmapped address space, which matters for
// browsers and runtimes that reserve terabytes of virtual memory.
static void scan_process(struct scan_result *scan, pid_t pid, bool include_zram) {
    static struct page_region regions[4096];
    static unsigned long long entries[8192];
    if (scan->memory_limit_reached) return;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/pagemap", pid);
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return;
    struct readback_target target;
    memset(&target, 0, sizeof target);
    target.pid = pid;
    size_t page_size = (size_t)g_system.page_size_bytes;
    unsigned long start = 0;
    while (start < ADDRESS_SPACE_END) {
        struct pm_scan_arg argument;
        memset(&argument, 0, sizeof argument);
        argument.size = sizeof argument;
        argument.start = start;
        argument.end = ADDRESS_SPACE_END;
        argument.vec = (unsigned long)regions;
        argument.vec_len = sizeof regions / sizeof *regions;
        argument.category_mask = PAGE_IS_SWAPPED;
        argument.return_mask = PAGE_IS_SWAPPED;
        int count = ioctl(descriptor, PAGEMAP_SCAN, &argument);
        if (count < 0) break;
        for (int i = 0; i < count; i++) {
            for (unsigned long address = regions[i].start; address < regions[i].end;) {
                size_t wanted = (regions[i].end - address) / page_size;
                if (wanted > sizeof entries / sizeof *entries) wanted = sizeof entries / sizeof *entries;
                ssize_t bytes = pread(descriptor, entries, wanted * 8, (off_t)(address / page_size) * 8);
                if (bytes <= 0) goto region_done;
                size_t got = (size_t)bytes / 8;
                for (size_t k = 0; k < got; k++) {
                    unsigned long long entry = entries[k];
                    if (!(entry & PAGEMAP_SWAP_BIT)) continue;
                    unsigned type = (unsigned)(entry & PAGEMAP_TYPE_MASK);
                    unsigned long long offset = (entry >> PAGEMAP_OFFSET_SHIFT) & PAGEMAP_OFFSET_MASK;
                    bool on_disk = (int)type != scan->zram_type;
                    if (on_disk) scan->disk_pages++;
                    else scan->zram_pages++;
                    if (on_disk && scan->slot_bit_pages && offset < scan->slot_bit_pages) {
                        if (!scan->slot_bits[type]) scan->slot_bits[type] = calloc(scan->slot_bit_pages / 8 + 1, 1);
                        unsigned char *bits = scan->slot_bits[type];
                        if (bits && !(bits[offset / 8] & (1U << (offset % 8)))) {
                            bits[offset / 8] |= (unsigned char)(1U << (offset % 8));
                            scan->distinct_disk_pages++;
                        }
                    }
                    if (on_disk || include_zram) target_add_page(scan, &target, address + k * page_size, page_size);
                }
                address += got * page_size;
            }
        region_done:;
        }
        if (argument.walk_end <= start || argument.walk_end >= ADDRESS_SPACE_END) break;
        start = argument.walk_end;
    }
    close(descriptor);
    if (!target.pages || !scan_reserve(scan, sizeof target)) {
        free(target.ranges);
        return;
    }
    char adjust_path[64], adjust_text[16];
    snprintf(adjust_path, sizeof adjust_path, "/proc/%d/oom_score_adj", pid);
    if (read_text_file(adjust_path, adjust_text, sizeof adjust_text)) target.oom_score_adjust = atoi(adjust_text);
    read_process_cgroup(pid, target.cgroup, sizeof target.cgroup);
    if (scan->target_count == scan->target_capacity) {
        scan->target_capacity = scan->target_capacity ? scan->target_capacity * 2 : 64;
        scan->targets = realloc(scan->targets, scan->target_capacity * sizeof *scan->targets);
        if (!scan->targets) {
            log_event("event=OUT_OF_MEMORY");
            exit(1);
        }
    }
    scan->targets[scan->target_count++] = target;
}

static void scan_group_tree(struct scan_result *scan, const char *directory_path, bool include_zram) {
    char path[PATH_BUFFER_SIZE];
    if (!join_child_path(path, sizeof path, directory_path, "cgroup.procs")) return;
    FILE *file = open_for_reading(path);
    if (file) {
        int pid;
        while (fscanf(file, "%d", &pid) == 1) scan_process(scan, pid, include_zram);
        fclose(file);
    }
    DIR *directory = opendir(directory_path);
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (entry->d_type != DT_DIR || entry->d_name[0] == '.') continue;
        char child[PATH_BUFFER_SIZE];
        if (join_child_path(child, sizeof child, directory_path, entry->d_name)) scan_group_tree(scan, child, include_zram);
    }
    closedir(directory);
}

static void scan_all_processes(struct scan_result *scan, bool include_zram) {
    DIR *directory = opendir("/proc");
    if (!directory) return;
    struct dirent *entry;
    pid_t self = getpid();
    while ((entry = readdir(directory))) {
        if (!isdigit((unsigned char)entry->d_name[0])) continue;
        pid_t pid = atoi(entry->d_name);
        if (pid != self) scan_process(scan, pid, include_zram);
    }
    closedir(directory);
}

static void scan_set_zram_type(struct scan_result *scan) {
    struct swap_layout layout;
    bool zram_first = swap_layout_read(&layout) && layout.zram_is_type_zero;
    scan->zram_type = zram_first ? ZRAM_SWAP_TYPE : NO_ZRAM_TYPE;
}

// Lower oom_score_adj usually means the process matters more to its application, for example the
// browser process and the visible tab. Among equals, smaller processes finish first.
static int compare_targets(const void *left, const void *right) {
    const struct readback_target *a = left, *b = right;
    if (a->oom_score_adjust != b->oom_score_adjust) return a->oom_score_adjust < b->oom_score_adjust ? -1 : 1;
    return a->pages < b->pages ? -1 : a->pages > b->pages;
}

// Every process counts here, not only those with swapped pages.
static uint64_t gpu_memory_of_group_tree(const char *directory_path) {
    uint64_t total = 0;
    char path[PATH_BUFFER_SIZE];
    if (!join_child_path(path, sizeof path, directory_path, "cgroup.procs")) return 0;
    FILE *file = open_for_reading(path);
    if (file) {
        int pid;
        while (fscanf(file, "%d", &pid) == 1) total += gpu_memory_bytes_of_process(pid);
        fclose(file);
    }
    DIR *directory = opendir(directory_path);
    if (!directory) return total;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (entry->d_type != DT_DIR || entry->d_name[0] == '.') continue;
        char child[PATH_BUFFER_SIZE];
        if (join_child_path(child, sizeof child, directory_path, entry->d_name)) total += gpu_memory_of_group_tree(child);
    }
    closedir(directory);
    return total;
}

void readback_measure_group(const char *group_path, struct group_swap_counts *counts, uint64_t *gpu_bytes) {
    struct scan_result scan;
    memset(&scan, 0, sizeof scan);
    scan_set_zram_type(&scan);
    scan_group_tree(&scan, group_path, true);
    counts->zram_pages = scan.zram_pages;
    counts->disk_pages = scan.disk_pages;
    *gpu_bytes = gpu_memory_of_group_tree(group_path);
    scan_free(&scan);
}

// --- pacing ---

static uint64_t chunk_bytes(void) {
    uint64_t page = (uint64_t)g_system.page_size_bytes;
    uint64_t by_rate = (uint64_t)(g_job.rate_bytes_per_second * g_policy.readback_step_milliseconds / 1000.0);
    uint64_t maximum = percent_of(g_system.memory_total_bytes, g_policy.readback_max_chunk_percent_of_memory);
    if (by_rate > maximum) by_rate = maximum;
    if (by_rate < page) by_rate = page;
    return by_rate / page * page;
}

static double disk_busy_percent(double now) {
    double busiest = 0.0;
    for (int i = 0; i < g_job.disk_count; i++) {
        char path[PATH_BUFFER_SIZE + 16];
        snprintf(path, sizeof path, "%s/stat", g_job.disks[i]);
        FILE *file = fopen(path, "r");
        if (!file) continue;
        unsigned long long field[10] = {0};
        int count = 0;
        while (count < 10 && fscanf(file, "%llu", &field[count]) == 1) count++;
        fclose(file);
        if (count < 10) continue;
        unsigned long long ticks = field[9];
        if (g_job.last_io_at > 0 && now > g_job.last_io_at) {
            double busy = (double)(ticks - g_job.last_io_ticks[i]) / ((now - g_job.last_io_at) * 10.0);
            if (busy > busiest) busiest = busy;
        }
        g_job.last_io_ticks[i] = ticks;
    }
    g_job.last_io_at = now;
    return busiest;
}

static void adjust_rate(double now) {
    if (now < g_job.next_rate_adjust_at) return;
    g_job.next_rate_adjust_at = now + RATE_ADJUST_INTERVAL_SECONDS;
    double target = g_job.kind == READBACK_FOREGROUND ? g_policy.foreground_disk_busy_target_percent : g_policy.idle_disk_busy_target_percent;
    double busy = disk_busy_percent(now);
    double initial = (double)percent_of(g_system.memory_total_bytes, g_policy.readback_initial_rate_percent_of_memory_per_second);
    double maximum = (double)percent_of(g_system.memory_total_bytes, g_policy.readback_max_rate_percent_of_memory_per_second);
    if (busy > target) g_job.rate_bytes_per_second *= RATE_DECREASE_FACTOR;
    else if (busy < target * 0.8) g_job.rate_bytes_per_second *= RATE_INCREASE_FACTOR;
    double minimum = initial * RATE_MINIMUM_FRACTION_OF_INITIAL;
    if (g_job.rate_bytes_per_second < minimum) g_job.rate_bytes_per_second = minimum;
    if (g_job.rate_bytes_per_second > maximum) g_job.rate_bytes_per_second = maximum;
}

// --- releasing pages when memory is needed elsewhere ---

static void release_issued_pages(void) {
    if (!enforcing()) return;
    double started = monotonic_seconds();
    long written_before = read_vmstat_value("pswpout");
    uint64_t released = 0;
    int groups = 0;
    struct scan_result *scan = &g_job.scan;
    for (size_t i = 0; i < scan->target_count; i++) {
        struct readback_target *target = &scan->targets[i];
        if (!target->issued_bytes || !target->cgroup[0]) continue;
        uint64_t amount = target->issued_bytes;
        target->issued_bytes = 0;
        for (size_t k = i + 1; k < scan->target_count; k++) {
            if (scan->targets[k].issued_bytes && !strcmp(scan->targets[k].cgroup, target->cgroup)) {
                amount += scan->targets[k].issued_bytes;
                scan->targets[k].issued_bytes = 0;
            }
        }
        char path[PATH_BUFFER_SIZE * 2];
        int path_length = snprintf(path, sizeof path, "%s%s/memory.stat", g_paths.cgroup_root, target->cgroup);
        if (path_length < 0 || (size_t)path_length >= sizeof path) continue;
        long long cached = read_keyed_value(path, "swapcached", 0);
        if (cached <= 0) continue;
        if (amount > (uint64_t)cached) amount = (uint64_t)cached;
        path_length = snprintf(path, sizeof path, "%s%s/memory.reclaim", g_paths.cgroup_root, target->cgroup);
        if (path_length < 0 || (size_t)path_length >= sizeof path) continue;
        int descriptor = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0) continue;
        char text[64];
        int length = snprintf(text, sizeof text, "%llu swappiness=max", (unsigned long long)amount);
        if (write(descriptor, text, (size_t)length) < 0 && errno == EINVAL) {
            length = snprintf(text, sizeof text, "%llu", (unsigned long long)amount);
            if (write(descriptor, text, (size_t)length) < 0 && errno != EAGAIN) log_event("event=RECLAIM_FAILED error=\"%s\"", strerror(errno));
        }
        close(descriptor);
        released += amount;
        groups++;
    }
    if (groups) {
        // Dropping clean pages writes nothing. Writes here come from other reclaim or from evicted application memory.
        long written = read_vmstat_value("pswpout") - written_before;
        log_event("event=PAGES_RELEASED mib=%llu groups=%d milliseconds=%.0f system_swap_out_mib=%llu", (unsigned long long)(released / MEBIBYTE), groups,
                  (monotonic_seconds() - started) * 1e3, (unsigned long long)((uint64_t)(written > 0 ? written : 0) * (uint64_t)g_system.page_size_bytes / MEBIBYTE));
    }
}

// --- job control ---

static void job_finish(void) {
    if (g_job.pidfd >= 0) close(g_job.pidfd);
    g_job.pidfd = -1;
    scan_free(&g_job.scan);
    g_job.kind = READBACK_NONE;
    g_job.retire_after = false;
}

void readback_cancel(void) {
    if (g_job.kind != READBACK_NONE) {
        log_event("event=READBACK_CANCELLED kind=%s issued_mib=%llu", g_job.kind == READBACK_FOREGROUND ? "FOREGROUND" : "IDLE",
                  (unsigned long long)(g_job.issued_bytes / MEBIBYTE));
    }
    job_finish();
}

static void job_prepare(enum readback_kind kind, double now) {
    g_job.kind = kind;
    g_job.target_index = g_job.range_index = g_job.range_offset = 0;
    g_job.chunk_counter = 0;
    g_job.issued_bytes = 0;
    g_job.started_at = now;
    g_job.next_step_at = now;
    g_job.next_rate_adjust_at = now + RATE_ADJUST_INTERVAL_SECONDS;
    g_job.rate_bytes_per_second = (double)percent_of(g_system.memory_total_bytes, g_policy.readback_initial_rate_percent_of_memory_per_second);
    g_job.last_swap_in_pages = g_job.start_swap_in_pages = read_vmstat_value("pswpin");
    g_job.last_pressure = read_pressure("/proc/pressure/memory");
    g_job.last_pressure_at = now;
    g_job.next_pressure_at = now + PRESSURE_SAMPLE_INTERVAL_SECONDS;
    g_job.last_io_at = 0;
    struct swap_layout layout;
    g_job.disk_count = swap_layout_read(&layout) ? backing_disks_of_swap_files(&layout, g_job.disks, MAX_BACKING_DISKS) : 0;
    disk_busy_percent(now);
    if (g_job.scan.target_count) qsort(g_job.scan.targets, g_job.scan.target_count, sizeof *g_job.scan.targets, compare_targets);
}

void readback_start_foreground(const char *group_path) {
    job_finish();
    if (!g_policy.foreground_readback_enabled || !enforcing() || !group_path[0]) return;
    double started = monotonic_seconds();
    scan_set_zram_type(&g_job.scan);
    scan_group_tree(&g_job.scan, group_path, g_policy.foreground_readback_include_zram != 0);
    job_prepare(READBACK_FOREGROUND, started);
    uint64_t total = 0;
    for (size_t i = 0; i < g_job.scan.target_count; i++) total += g_job.scan.targets[i].pages;
    if (!total) {
        job_finish();
        return;
    }
    log_event("event=READBACK_STARTED kind=FOREGROUND processes=%zu mib=%llu scan_milliseconds=%.0f", g_job.scan.target_count,
              (unsigned long long)(total * (uint64_t)g_system.page_size_bytes / MEBIBYTE), (monotonic_seconds() - started) * 1e3);
}

static long long group_headroom_bytes(const char *cgroup) {
    char path[PATH_BUFFER_SIZE * 2], attribute[PATH_BUFFER_SIZE * 2 + 32];
    long long smallest = -1;
    snprintf(path, sizeof path, "%s%s", g_paths.cgroup_root, cgroup);
    size_t root_length = strlen(g_paths.cgroup_root);
    while (strlen(path) > root_length) {
        snprintf(attribute, sizeof attribute, "%s/memory.current", path);
        long long current = read_limit_file(attribute, -2);
        if (current >= 0) {
            static const char *const LIMITS[] = {"memory.high", "memory.max"};
            for (int i = 0; i < 2; i++) {
                snprintf(attribute, sizeof attribute, "%s/%s", path, LIMITS[i]);
                long long limit = read_limit_file(attribute, LIMIT_UNLIMITED);
                if (limit == LIMIT_UNLIMITED) continue;
                long long room = limit - current;
                if (smallest < 0 || room < smallest) smallest = room < 0 ? 0 : room;
            }
        }
        char *slash = strrchr(path, '/');
        if (!slash) break;
        *slash = '\0';
    }
    return smallest;
}

static bool job_should_stop(double now) {
    bool foreground = g_job.kind == READBACK_FOREGROUND;
    double reserve_percent = foreground ? g_policy.foreground_readback_reserve_percent : g_policy.idle_readback_reserve_percent;
    uint64_t reserve = percent_of(g_system.memory_total_bytes, reserve_percent);
    uint64_t available = memory_available_bytes();
    if (available < reserve + chunk_bytes()) {
        log_event("event=READBACK_STOPPED reason=MEMORY_RESERVE available_mib=%llu reserve_mib=%llu",
                  (unsigned long long)(available / MEBIBYTE), (unsigned long long)(reserve / MEBIBYTE));
        return true;
    }
    if (now < g_job.next_pressure_at) return false;
    struct pressure_sample sample = read_pressure("/proc/pressure/memory");
    double elapsed = (now - g_job.last_pressure_at) * 1e6;
    double percent = elapsed > 0 ? (double)(sample.some_total_microseconds - g_job.last_pressure.some_total_microseconds) * 100.0 / elapsed : 0;
    g_job.last_pressure = sample;
    g_job.last_pressure_at = now;
    g_job.next_pressure_at = now + PRESSURE_SAMPLE_INTERVAL_SECONDS;
    double limit = foreground ? g_policy.foreground_pressure_stop_percent : g_policy.idle_pressure_stop_percent;
    if (percent > limit) {
        log_event("event=READBACK_STOPPED reason=MEMORY_PRESSURE percent=%.1f limit=%.2f", percent, limit);
        return true;
    }
    return false;
}

// swapoff and unlink run as root, so the file must be a direct child of a directory only this user
// can change, and not a link.
static bool swap_file_is_trusted(const char *path) {
    const char *name = path + strlen(g_policy.swap_file_directory) + 1;
    return !strchr(name, '/') && path_is_trusted(g_policy.swap_file_directory, true) && path_is_trusted(path, false);
}

static bool retire_swap_file(const struct swap_file_entry *entry) {
    if (!swap_file_is_trusted(entry->path)) {
        log_event("event=SWAP_FILE_UNTRUSTED path=%s", entry->path);
        return false;
    }
    double started = monotonic_seconds();
    if (swapoff(entry->path) != 0) {
        log_event("event=SWAPOFF_FAILED path=%s error=\"%s\"", entry->path, strerror(errno));
        return false;
    }
    double seconds = monotonic_seconds() - started;
    if (unlink(entry->path) != 0) log_event("event=UNLINK_FAILED path=%s error=\"%s\"", entry->path, strerror(errno));
    log_event("event=SWAP_FILE_RETIRED path=%s used_mib=%lld swapoff_seconds=%.2f", entry->path, entry->used_kib / 1024, seconds);
    return true;
}

// The swap left elsewhere must cover the time swapspace needs to add a file if demand returns.
static bool enough_swap_without(const struct swap_layout *layout, const struct swap_file_entry *entry) {
    long long remaining_kib = layout->free_kib - (entry->size_kib - entry->used_kib);
    return remaining_kib >= 0 &&
           (uint64_t)remaining_kib * 1024 >= percent_of(g_system.memory_total_bytes, g_policy.swap_file_retire_min_free_swap_percent);
}

// Pages still in the swap cache need no disk read at swapoff. The kernel counts this cache for all swap
// devices together, so what is left over is an estimate, and the worst case has its own limit.
static uint64_t swap_cached_bytes(void) {
    long long kib = read_keyed_value("/proc/meminfo", "SwapCached", 0);
    return kib > 0 ? (uint64_t)kib * 1024 : 0;
}

// Unused space goes back as soon as the system is calm. Only files that would need a longer read wait
// for the calm period and the paced read.
static void retire_small_file(double now) {
    if (!g_policy.idle_readback_enabled || !enforcing() || now < g_retire_not_before) return;
    struct swap_layout layout;
    if (!swap_layout_read(&layout) || !layout.managed_file_count) return;
    const struct swap_file_entry *smallest = &layout.managed_files[0];
    for (int i = 1; i < layout.managed_file_count; i++) {
        if (layout.managed_files[i].used_kib < smallest->used_kib) smallest = &layout.managed_files[i];
    }
    uint64_t used = (uint64_t)smallest->used_kib * 1024;
    uint64_t cached = swap_cached_bytes();
    uint64_t expected_read = used > cached ? used - cached : 0;
    uint64_t reserve = percent_of(g_system.memory_total_bytes, g_policy.idle_readback_reserve_percent);
    // Under the disk limit an empty file goes back whatever memory is doing: removing it reads nothing.
    bool empty_on_full_disk = g_disk_guard.below_limit && used == 0;
    if (!empty_on_full_disk) {
        if (used > percent_of(g_system.memory_total_bytes, g_policy.swap_file_retire_worst_case_read_percent) ||
            expected_read > percent_of(g_system.memory_total_bytes, g_policy.swap_file_retire_unreachable_percent) ||
            !enough_swap_without(&layout, smallest) || memory_available_bytes() < reserve + expected_read ||
            read_pressure("/proc/pressure/memory").some_average_10s >= g_policy.idle_pressure_stop_percent) {
            return;
        }
    }
    if (!retire_swap_file(smallest)) g_retire_not_before = now + RETIRE_RETRY_DELAY_SECONDS;
}

static void retire_files_if_possible(void) {
    struct swap_layout layout;
    if (!swap_layout_read(&layout) || !layout.managed_file_count) return;
    uint64_t used = (uint64_t)layout.managed_used_kib * 1024;
    uint64_t reached = g_job.scan.distinct_disk_pages * (uint64_t)g_system.page_size_bytes;
    uint64_t cached = swap_cached_bytes();
    uint64_t in_memory = reached > cached ? reached : cached;
    uint64_t unreachable_bytes = used > in_memory ? used - in_memory : 0;
    uint64_t unverified_bytes = used > reached ? used - reached : 0;
    uint64_t allowed = percent_of(g_system.memory_total_bytes, g_policy.swap_file_retire_unreachable_percent);
    if (unreachable_bytes > allowed || unverified_bytes > percent_of(g_system.memory_total_bytes, g_policy.swap_file_retire_worst_case_read_percent)) {
        log_event("event=SWAP_FILE_KEPT unreachable_mib=%llu allowed_mib=%llu", (unsigned long long)(unreachable_bytes / MEBIBYTE),
                  (unsigned long long)(allowed / MEBIBYTE));
        return;
    }
    for (int i = 0; i < layout.managed_file_count; i++) {
        const struct swap_file_entry *entry = &layout.managed_files[i];
        if (!enough_swap_without(&layout, entry)) {
            log_event("event=SWAP_FILE_KEPT path=%s reason=LOW_FREE_SWAP free_mib=%lld", entry->path, layout.free_kib / 1024);
            continue;
        }
        if (retire_swap_file(entry)) layout.free_kib -= entry->size_kib - entry->used_kib;
    }
}

static void job_complete(double now) {
    long read_pages = read_vmstat_value("pswpin") - g_job.start_swap_in_pages;
    log_event("event=READBACK_FINISHED kind=%s requested_mib=%llu swap_in_mib=%llu seconds=%.1f final_rate_mib_per_second=%.0f",
              g_job.kind == READBACK_FOREGROUND ? "FOREGROUND" : "IDLE", (unsigned long long)(g_job.issued_bytes / MEBIBYTE),
              (unsigned long long)((uint64_t)(read_pages > 0 ? read_pages : 0) * (uint64_t)g_system.page_size_bytes / MEBIBYTE),
              now - g_job.started_at, g_job.rate_bytes_per_second / (double)MEBIBYTE);
    bool was_idle = g_job.kind == READBACK_IDLE;
    if (was_idle && g_job.retire_after) retire_files_if_possible();
    job_finish();
    if (was_idle) g_idle_not_before = now + RESCAN_DELAY_SECONDS;
}

static void job_step(double now) {
    struct scan_result *scan = &g_job.scan;
    if (job_should_stop(now)) {
        bool idle = g_job.kind == READBACK_IDLE;
        if (idle) release_issued_pages();
        job_finish();
        g_idle_calm_since = 0;
        return;
    }
    adjust_rate(now);
    uint64_t wanted = chunk_bytes(), sent = 0;
    struct iovec vectors[IOVEC_BATCH];
    int vector_count = 0;
    while (g_job.target_index < scan->target_count) {
        struct readback_target *target = &scan->targets[g_job.target_index];
        if (g_job.pidfd < 0) {
            g_job.pidfd = (int)syscall(SYS_pidfd_open, target->pid, 0);
            g_job.range_index = g_job.range_offset = 0;
            if (g_job.pidfd < 0) {
                g_job.target_index++;
                continue;
            }
        }
        bool skip = false;
        if ((g_job.chunk_counter++ % CGROUP_HEADROOM_CHUNKS) == 0) {
            long long room = group_headroom_bytes(target->cgroup);
            if (room >= 0 && (uint64_t)room < wanted * CGROUP_HEADROOM_CHUNKS) skip = true;
        }
        if (!skip) {
            while (g_job.range_index < target->range_count && wanted > sent && vector_count < IOVEC_BATCH) {
                struct page_range *range = &target->ranges[g_job.range_index];
                uint64_t length = range->length - g_job.range_offset;
                if (length > wanted - sent) length = wanted - sent;
                vectors[vector_count].iov_base = (void *)(range->start + g_job.range_offset);
                vectors[vector_count].iov_len = length;
                vector_count++;
                sent += length;
                g_job.range_offset += length;
                if (g_job.range_offset == range->length) {
                    g_job.range_index++;
                    g_job.range_offset = 0;
                }
            }
            if (vector_count) {
                long result = syscall(SYS_process_madvise, g_job.pidfd, vectors, (size_t)vector_count, MADV_WILLNEED, 0);
                bool gone = result < 0 && (errno == ESRCH || errno == EPERM);
                if (!gone) {
                    target->issued_bytes += sent;
                    g_job.issued_bytes += sent;
                }
                if (gone) skip = true;
            }
        }
        if (skip || g_job.range_index >= target->range_count) {
            close(g_job.pidfd);
            g_job.pidfd = -1;
            g_job.target_index++;
        }
        if (vector_count) break;
    }
    if (g_job.target_index >= scan->target_count) {
        job_complete(now);
        return;
    }
    // Wait according to pages actually read: pages already cached cost no disk time.
    long swap_in = read_vmstat_value("pswpin");
    double read_bytes = (double)(swap_in - g_job.last_swap_in_pages) * (double)g_system.page_size_bytes;
    g_job.last_swap_in_pages = swap_in;
    double delay = read_bytes > 0 ? read_bytes / g_job.rate_bytes_per_second : 0;
    if (delay > 1.0) delay = 1.0;
    g_job.next_step_at = now + delay;
}

static void idle_consider(double now) {
    if (!g_policy.idle_readback_enabled || !enforcing()) return;
    if (now < g_idle_not_before) return;
    struct swap_layout layout;
    if (!swap_layout_read(&layout) || !layout.managed_used_kib || (layout.zram_present && !layout.zram_is_type_zero)) {
        g_idle_calm_since = 0;
        return;
    }
    struct pressure_sample memory = read_pressure("/proc/pressure/memory");
    uint64_t reserve = percent_of(g_system.memory_total_bytes, g_policy.idle_readback_reserve_percent);
    if (memory_available_bytes() < reserve || memory.some_average_10s >= g_policy.idle_pressure_stop_percent) {
        g_idle_calm_since = 0;
        return;
    }
    if (!g_idle_calm_since) {
        g_idle_calm_since = now;
        log_event("event=IDLE_WAIT disk_mib=%lld calm_seconds=%d", layout.managed_used_kib / 1024, g_policy.idle_readback_calm_seconds);
    }
    if (now - g_idle_calm_since < g_policy.idle_readback_calm_seconds) return;

    double started = monotonic_seconds();
    job_finish();
    long long largest = 0;
    for (int i = 0; i < layout.managed_file_count; i++) {
        if (layout.managed_files[i].size_kib > largest) largest = layout.managed_files[i].size_kib;
    }
    g_job.scan.slot_bit_pages = (size_t)largest / ((size_t)g_system.page_size_bytes / 1024) + 1;
    scan_set_zram_type(&g_job.scan);
    scan_all_processes(&g_job.scan, false);
    job_prepare(READBACK_IDLE, started);
    g_job.retire_after = true;
    g_idle_calm_since = 0;
    log_event("event=READBACK_STARTED kind=IDLE processes=%zu mib=%llu scan_milliseconds=%.0f", g_job.scan.target_count,
              (unsigned long long)(g_job.scan.disk_pages * (uint64_t)g_system.page_size_bytes / MEBIBYTE),
              (monotonic_seconds() - started) * 1e3);
}

void readback_tick(double now) {
    if (g_job.kind == READBACK_NONE) {
        retire_small_file(now);
        idle_consider(now);
        return;
    }
    if (now >= g_job.next_step_at) job_step(now);
}

double readback_next_due_seconds(double now) {
    if (g_job.kind == READBACK_NONE) return 1.0;
    double remaining = g_job.next_step_at - now;
    return remaining > 0 ? remaining : 0;
}

enum readback_kind readback_active_kind(void) { return g_job.kind; }

void readback_status_text(char *buffer, size_t buffer_size) {
    static const char *const NAMES[] = {"NONE", "FOREGROUND", "IDLE"};
    snprintf(buffer, buffer_size, "SWAP_GROWTH=%s READBACK=%s READBACK_ISSUED_MIB=%llu READBACK_RATE_MIB_PER_SECOND=%.0f",
             g_disk_guard.growth_held ? "HELD" : "ALLOWED", NAMES[g_job.kind],
             (unsigned long long)(g_job.issued_bytes / MEBIBYTE), g_job.rate_bytes_per_second / (double)MEBIBYTE);
}
