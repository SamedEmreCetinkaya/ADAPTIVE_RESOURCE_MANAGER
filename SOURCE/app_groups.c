// Application groups are the direct children of app.slice in each user manager. Grouping at that
// level treats a terminal and every shell it spawned, or a browser and all its helpers, as one task.
#include "resource_manager.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CPU_PERIOD_MICROSECONDS 100000LL
#define CPU_WEIGHT_MAXIMUM 10000LL
#define CPU_WEIGHT_MINIMUM 1LL
#define MAX_APP_GROUPS_PER_USER 512
#define MAX_APP_GROUPS_OF_OTHER_USERS 3584
// Only a change of the active seat can take the table past the two limits above.
#define MAX_APP_GROUPS_TOTAL 8192
#define MAX_ENTRIES_EXAMINED_PER_USER (MAX_APP_GROUPS_PER_USER * 4)
#define MAX_GROUP_PATH_LENGTH 600

struct app_group {
    char path[PATH_BUFFER_SIZE];
    uid_t owner;
    long long original_memory_high;
    long long original_memory_max;
    long long original_swap_max;
    long long original_cpu_weight;
    char original_cpu_max[64];
    bool is_foreground;
    bool seen;
    double demote_at;
};

static struct app_group *g_groups;
static int g_group_count;
static int g_group_capacity;
static struct app_group *g_foreground;
static uint64_t g_cpu_busy[CPU_SETSIZE];
static uint64_t g_cpu_total[CPU_SETSIZE];

static const char *group_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// A path that does not fit becomes empty, so it can never name a different file.
static void attribute_path(char *buffer, size_t size, const char *group_path, const char *attribute) {
    if (!join_child_path(buffer, size, group_path, attribute)) buffer[0] = '\0';
}

static bool has_suffix(const char *text, size_t length, const char *suffix) {
    size_t suffix_length = strlen(suffix);
    return length > suffix_length && !strcmp(text + length - suffix_length, suffix);
}

// Users can create directories under their own app.slice, so a name is only accepted if it could be a
// systemd unit. This keeps line breaks and separators out of the log and the saved limits.
static bool unit_name_is_valid(const char *name) {
    size_t length = strlen(name);
    for (const char *character = name; *character; character++) {
        if (!isalnum((unsigned char)*character) && !strchr(":_.-@\\", *character)) return false;
    }
    return has_suffix(name, length, ".slice") || has_suffix(name, length, ".scope") || has_suffix(name, length, ".service");
}

// The only accepted form is ROOT/user.slice/user-UID.slice/user@UID.service/app.slice/UNIT.
static bool group_path_is_valid(const char *path, uid_t *owner) {
    size_t root_length = strlen(g_paths.cgroup_root);
    if (strlen(path) > MAX_GROUP_PATH_LENGTH || strncmp(path, g_paths.cgroup_root, root_length)) return false;
    unsigned uid, same_uid;
    int consumed = -1;
    if (sscanf(path + root_length, "/user.slice/user-%u.slice/user@%u.service/app.slice/%n", &uid, &same_uid, &consumed) != 2) return false;
    if (consumed < 0 || uid != same_uid) return false;
    const char *name = path + root_length + consumed;
    if (strchr(name, '/') || !unit_name_is_valid(name)) return false;
    *owner = (uid_t)uid;
    return true;
}

static bool enforcing(void) { return g_policy.run_mode == RUN_MODE_ENFORCE; }

static void set_limit(const char *group_path, const char *attribute, long long value) {
    char path[PATH_BUFFER_SIZE];
    attribute_path(path, sizeof path, group_path, attribute);
    if (!enforcing() || access(path, F_OK) != 0) return;
    if (!write_limit_file(path, value)) log_event("event=WRITE_FAILED path=%s error=\"%s\"", path, strerror(errno));
}

static long long get_limit(const char *group_path, const char *attribute, long long fallback) {
    char path[PATH_BUFFER_SIZE];
    attribute_path(path, sizeof path, group_path, attribute);
    return read_limit_file(path, fallback);
}

// Upper bound arithmetic where LIMIT_UNLIMITED is larger than any number.
static long long limit_max(long long a, long long b) {
    if (a == LIMIT_UNLIMITED || b == LIMIT_UNLIMITED) return LIMIT_UNLIMITED;
    return a > b ? a : b;
}

static long long limit_min(long long a, long long b) {
    if (a == LIMIT_UNLIMITED) return b;
    if (b == LIMIT_UNLIMITED) return a;
    return a < b ? a : b;
}

static long long percent_limit(uint64_t total, double percent) {
    return percent >= 100.0 ? LIMIT_UNLIMITED : (long long)percent_of(total, percent);
}

static long long clamp_cpu_weight(long long base, double percent) {
    long long weight = (long long)((double)base * percent / 100.0);
    if (weight < CPU_WEIGHT_MINIMUM) return CPU_WEIGHT_MINIMUM;
    if (weight > CPU_WEIGHT_MAXIMUM) return CPU_WEIGHT_MAXIMUM;
    return weight;
}

static void sample_cpu_load(uint64_t *busy, uint64_t *total) {
    FILE *file = fopen("/proc/stat", "r");
    char line[256];
    while (file && fgets(line, sizeof line, file)) {
        int cpu;
        unsigned long long v[8];
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &cpu, &v[0], &v[1], &v[2], &v[3], &v[4],
                   &v[5], &v[6], &v[7]) != 9 || cpu < 0 || cpu >= CPU_SETSIZE) continue;
        uint64_t idle = v[3] + v[4], sum = 0;
        for (int i = 0; i < 8; i++) sum += v[i];
        total[cpu] = sum;
        busy[cpu] = sum - idle;
    }
    if (file) fclose(file);
}

static int share_of_cpus(int count, double percent) {
    int keep = (int)ceil((double)count * percent / 100.0);
    return keep < 1 ? 1 : keep;
}

static void take_percent_of_set(cpu_set_t *set, double percent) {
    int keep = share_of_cpus(CPU_COUNT(set), percent);
    int seen = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, set)) continue;
        if (++seen > keep) CPU_CLR(cpu, set);
    }
}

// Chooses the least busy CPUs since the previous group refresh.
static void least_busy_cpus(cpu_set_t *set, double percent) {
    uint64_t busy[CPU_SETSIZE] = {0}, total[CPU_SETSIZE] = {0};
    sample_cpu_load(busy, total);
    int keep = share_of_cpus(g_system.cpu_count, percent);
    CPU_ZERO(set);
    for (int chosen = 0; chosen < keep; chosen++) {
        int best = -1;
        double best_load = 2.0;
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (!CPU_ISSET(cpu, &g_system.cpus_all) || CPU_ISSET(cpu, set)) continue;
            uint64_t span = total[cpu] - g_cpu_total[cpu];
            double load = span ? (double)(busy[cpu] - g_cpu_busy[cpu]) / (double)span : 0.0;
            if (load < best_load) { best_load = load; best = cpu; }
        }
        if (best >= 0) CPU_SET(best, set);
    }
}

static void selector_to_cpu_set(const struct cpu_selector *selector, cpu_set_t *set) {
    switch (selector->kind) {
    case CPU_SELECTOR_ALL: *set = g_system.cpus_all; take_percent_of_set(set, selector->percent); break;
    case CPU_SELECTOR_PERFORMANCE: *set = g_system.cpus_performance; take_percent_of_set(set, selector->percent); break;
    case CPU_SELECTOR_EFFICIENCY: *set = g_system.cpus_efficiency; take_percent_of_set(set, selector->percent); break;
    case CPU_SELECTOR_LOW_POWER: *set = g_system.cpus_low_power; take_percent_of_set(set, selector->percent); break;
    case CPU_SELECTOR_LEAST_BUSY: least_busy_cpus(set, selector->percent); break;
    }
}

static void affinity_for_tree(const char *directory_path, const cpu_set_t *set) {
    char path[PATH_BUFFER_SIZE];
    attribute_path(path, sizeof path, directory_path, "cgroup.threads");
    FILE *file = open_for_reading(path);
    if (file) {
        int tid;
        while (fscanf(file, "%d", &tid) == 1) sched_setaffinity(tid, sizeof *set, set);
        fclose(file);
    }
    DIR *directory = opendir(directory_path);
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (entry->d_type != DT_DIR || entry->d_name[0] == '.') continue;
        char child[PATH_BUFFER_SIZE];
        if (join_child_path(child, sizeof child, directory_path, entry->d_name)) affinity_for_tree(child, set);
    }
    closedir(directory);
}

// The default selector keeps the original affinity of every task, including pinned ones.
static void apply_cpu_selector(const struct app_group *group, const struct cpu_selector *selector, bool was_restricted) {
    if (selector->kind == CPU_SELECTOR_ALL && selector->percent >= 100.0 && !was_restricted) return;
    if (!enforcing()) return;
    cpu_set_t set;
    selector_to_cpu_set(selector, &set);
    if (CPU_COUNT(&set) > 0) affinity_for_tree(group->path, &set);
}

static bool selector_restricts(const struct cpu_selector *selector) {
    return !(selector->kind == CPU_SELECTOR_ALL && selector->percent >= 100.0);
}

static void set_protection_chain(const char *group_path, long long low_bytes) {
    set_limit(group_path, "memory.low", low_bytes);
    // Recursive protection only reaches a child through protected ancestors.
    char ancestor[PATH_BUFFER_SIZE];
    snprintf(ancestor, sizeof ancestor, "%s", group_path);
    size_t root_length = strlen(g_paths.cgroup_root);
    for (;;) {
        char *slash = strrchr(ancestor, '/');
        if (!slash) break;
        *slash = '\0';
        if (strlen(ancestor) <= root_length) break;
        set_limit(ancestor, "memory.low", low_bytes);
    }
}

static void apply_foreground(struct app_group *group) {
    uint64_t memory = g_system.memory_total_bytes;
    long long high = percent_limit(memory, g_policy.foreground_memory_high_percent);
    set_limit(group->path, "memory.high", high);
    set_limit(group->path, "memory.max", limit_max(group->original_memory_max, high));
    set_limit(group->path, "memory.swap.max",
              limit_max(group->original_swap_max, percent_limit(swap_total_bytes(), g_policy.foreground_swap_max_percent)));
    set_limit(group->path, "cpu.weight", clamp_cpu_weight(group->original_cpu_weight, g_policy.foreground_cpu_weight_percent));
    if (enforcing()) {
        char path[PATH_BUFFER_SIZE];
        attribute_path(path, sizeof path, group->path, "cpu.max");
        if (access(path, F_OK) == 0) write_text_file(path, "max");
    }
    set_protection_chain(group->path, (long long)percent_of(memory, g_policy.foreground_memory_low_percent));
    apply_cpu_selector(group, &g_policy.foreground_cpu_set, selector_restricts(&g_policy.background_cpu_set));
}

// A background limit only makes sense against a foreground. Without a focus reporter, as on a desktop the
// extension does not support, the application in use would count as background too. Stays set until the
// daemon restarts: a reporter that went quiet does not prove that the next session has none.
static bool g_focus_reporter_seen;

static void apply_background(struct app_group *group) {
    if (!g_focus_reporter_seen) return;
    uint64_t memory = g_system.memory_total_bytes;
    set_limit(group->path, "memory.high",
              limit_min(group->original_memory_high, percent_limit(memory, g_policy.background_memory_high_percent)));
    set_limit(group->path, "memory.max", group->original_memory_max);
    set_limit(group->path, "memory.swap.max",
              limit_min(group->original_swap_max, percent_limit(swap_total_bytes(), g_policy.background_swap_max_percent)));
    set_limit(group->path, "memory.low", 0);
    set_limit(group->path, "cpu.weight", clamp_cpu_weight(group->original_cpu_weight, g_policy.background_cpu_weight_percent));
    if (enforcing()) {
        char path[PATH_BUFFER_SIZE], quota[64];
        attribute_path(path, sizeof path, group->path, "cpu.max");
        if (g_policy.background_cpu_quota_percent < 100.0) {
            long long microseconds = (long long)((double)(CPU_PERIOD_MICROSECONDS * g_system.cpu_count) * g_policy.background_cpu_quota_percent / 100.0);
            snprintf(quota, sizeof quota, "%lld %lld", microseconds, CPU_PERIOD_MICROSECONDS);
            if (access(path, F_OK) == 0) write_text_file(path, quota);
        } else if (access(path, F_OK) == 0 && group->original_cpu_max[0]) {
            write_text_file(path, group->original_cpu_max);
        }
    }
    apply_cpu_selector(group, &g_policy.background_cpu_set, selector_restricts(&g_policy.foreground_cpu_set));
}

static void restore_original(const struct app_group *group) {
    set_limit(group->path, "memory.high", group->original_memory_high);
    set_limit(group->path, "memory.max", group->original_memory_max);
    set_limit(group->path, "memory.swap.max", group->original_swap_max);
    set_limit(group->path, "memory.low", 0);
    set_limit(group->path, "cpu.weight", group->original_cpu_weight);
    if (enforcing() && group->original_cpu_max[0]) {
        char path[PATH_BUFFER_SIZE];
        attribute_path(path, sizeof path, group->path, "cpu.max");
        if (access(path, F_OK) == 0) write_text_file(path, group->original_cpu_max);
    }
}

// --- Originals survive a daemon crash so limits never ratchet onto themselves. ---

static void originals_path(char *buffer, size_t size) {
    snprintf(buffer, size, "%.600s/ORIGINAL_VALUES", g_paths.runtime_directory);
}

static void originals_save_all(void) {
    if (!enforcing()) return;
    char path[PATH_BUFFER_SIZE + 8], temporary[PATH_BUFFER_SIZE + 16];
    originals_path(path, sizeof path);
    snprintf(temporary, sizeof temporary, "%s.new", path);
    FILE *file = fopen(temporary, "w");
    if (!file) return;
    for (int i = 0; i < g_group_count; i++) {
        const struct app_group *group = &g_groups[i];
        fprintf(file, "%lld %lld %lld %lld %s|%s\n", group->original_memory_high, group->original_memory_max,
                group->original_swap_max, group->original_cpu_weight, group->original_cpu_max, group->path);
    }
    fclose(file);
    rename(temporary, path);
}

static struct app_group *group_add(const char *path) {
    if (g_group_count == g_group_capacity) {
        g_group_capacity = g_group_capacity ? g_group_capacity * 2 : 32;
        g_groups = realloc(g_groups, (size_t)g_group_capacity * sizeof *g_groups);
        if (!g_groups) {
            log_event("event=OUT_OF_MEMORY");
            exit(1);
        }
        g_foreground = NULL; // Reassigned by the caller through the path below when needed.
    }
    struct app_group *group = &g_groups[g_group_count++];
    memset(group, 0, sizeof *group);
    snprintf(group->path, sizeof group->path, "%s", path);
    return group;
}

void app_groups_load_saved_originals(void) {
    char path[PATH_BUFFER_SIZE + 8], line[PATH_BUFFER_SIZE + 128];
    originals_path(path, sizeof path);
    FILE *file = fopen(path, "r");
    if (!file) return;
    int restored = 0;
    while (fgets(line, sizeof line, file)) {
        line[strcspn(line, "\n")] = '\0';
        char *separator = strchr(line, '|');
        if (!separator) continue;
        *separator = '\0';
        long long high, max, swap, weight;
        char cpu_max[64] = "", cpu_second[32] = "";
        int fields = sscanf(line, "%lld %lld %lld %lld %63s %31s", &high, &max, &swap, &weight, cpu_max, cpu_second);
        if (fields < 4) continue;
        // Limits are written back to these paths, so only real application groups are accepted.
        struct stat info;
        uid_t owner;
        if (!group_path_is_valid(separator + 1, &owner) || g_group_count >= MAX_APP_GROUPS_TOTAL || stat(separator + 1, &info) != 0) continue;
        struct app_group *group = group_add(separator + 1);
        group->owner = owner;
        group->original_memory_high = high;
        group->original_memory_max = max;
        group->original_swap_max = swap;
        group->original_cpu_weight = weight;
        if (fields >= 5) snprintf(group->original_cpu_max, sizeof group->original_cpu_max, "%s%s%s", cpu_max, cpu_second[0] ? " " : "", cpu_second);
        // The crashed daemon left its limits here, and nothing applies new ones before a focus report.
        restore_original(group);
        restored++;
    }
    fclose(file);
    if (restored) log_event("event=ORIGINALS_RESTORED groups=%d", restored);
}

static struct app_group *group_find(const char *path) {
    for (int i = 0; i < g_group_count; i++) {
        if (!strcmp(g_groups[i].path, path)) return &g_groups[i];
    }
    return NULL;
}

static void group_record_originals(struct app_group *group) {
    group->original_memory_high = get_limit(group->path, "memory.high", LIMIT_UNLIMITED);
    group->original_memory_max = get_limit(group->path, "memory.max", LIMIT_UNLIMITED);
    group->original_swap_max = get_limit(group->path, "memory.swap.max", LIMIT_UNLIMITED);
    group->original_cpu_weight = get_limit(group->path, "cpu.weight", 100);
    char path[PATH_BUFFER_SIZE];
    attribute_path(path, sizeof path, group->path, "cpu.max");
    read_text_file(path, group->original_cpu_max, sizeof group->original_cpu_max);
}

static bool uid_in_list(uid_t uid, const uid_t *uids, int count) {
    for (int i = 0; i < count; i++) {
        if (uids[i] == uid) return true;
    }
    return false;
}

// A user can create directories without limit, so the work and memory for each user are capped. Every
// kept group counts, seen by this scan or not, because groups stay in the table while they exist. Other
// users also leave room for the user at an active seat, so they cannot crowd out that user's applications.
static bool refresh_groups_of_user(unsigned uid, const uid_t *seats, int seat_count) {
    char slice[PATH_BUFFER_SIZE];
    int length = snprintf(slice, sizeof slice, "%s/user.slice/user-%u.slice/user@%u.service/app.slice", g_paths.cgroup_root, uid, uid);
    if (length < 0 || (size_t)length >= sizeof slice) return false;
    DIR *directory = opendir(slice);
    if (!directory) return false;
    bool has_seat = uid_in_list((uid_t)uid, seats, seat_count);
    int owned = 0, other_users = 0;
    for (int i = 0; i < g_group_count; i++) {
        if (g_groups[i].owner == (uid_t)uid) owned++;
        if (!uid_in_list(g_groups[i].owner, seats, seat_count)) other_users++;
    }
    int groups = 0, examined = 0;
    bool changed = false;
    struct dirent *entry;
    while (groups < MAX_APP_GROUPS_PER_USER && examined < MAX_ENTRIES_EXAMINED_PER_USER && (entry = readdir(directory))) {
        if (entry->d_type != DT_DIR || entry->d_name[0] == '.') continue;
        examined++;
        char path[PATH_BUFFER_SIZE];
        uid_t owner;
        if (!join_child_path(path, sizeof path, slice, entry->d_name) || !group_path_is_valid(path, &owner)) continue;
        struct app_group *group = group_find(path);
        if (!group) {
            if (owned >= MAX_APP_GROUPS_PER_USER || g_group_count >= MAX_APP_GROUPS_TOTAL || (!has_seat && other_users >= MAX_APP_GROUPS_OF_OTHER_USERS)) {
                static bool limit_logged;
                if (!limit_logged) log_event("event=GROUP_LIMIT_REACHED uid=%u owned=%d total=%d", uid, owned, g_group_count);
                limit_logged = true;
                break;
            }
            group = group_add(path);
            group->owner = owner;
            owned++;
            if (!has_seat) other_users++;
            group_record_originals(group);
            apply_background(group);
            changed = true;
        } else if (group->original_cpu_weight == 0) {
            group_record_originals(group);
        }
        group->seen = true;
        groups++;
    }
    closedir(directory);
    return changed;
}

// The user at an active seat comes first. Directory names under user.slice are read, never trusted.
static bool refresh_groups_of_all_users(void) {
    uid_t seats[MAX_SEATS];
    int seat_count = active_seat_uids(seats, MAX_SEATS);
    bool changed = false;
    for (int i = 0; i < seat_count; i++) {
        if (refresh_groups_of_user((unsigned)seats[i], seats, seat_count)) changed = true;
    }
    char path[PATH_BUFFER_SIZE];
    if (!join_child_path(path, sizeof path, g_paths.cgroup_root, "user.slice")) return changed;
    DIR *directory = opendir(path);
    if (!directory) return changed;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        unsigned uid;
        int consumed = -1;
        if (sscanf(entry->d_name, "user-%u.slice%n", &uid, &consumed) != 1 || consumed < 0 || entry->d_name[consumed]) continue;
        if (!uid_in_list((uid_t)uid, seats, seat_count) && refresh_groups_of_user(uid, seats, seat_count)) changed = true;
    }
    closedir(directory);
    return changed;
}

void app_groups_refresh(void) {
    char foreground_path[PATH_BUFFER_SIZE] = "";
    if (g_foreground) snprintf(foreground_path, sizeof foreground_path, "%s", g_foreground->path);
    for (int i = 0; i < g_group_count; i++) g_groups[i].seen = false;
    bool changed = refresh_groups_of_all_users();
    // Remove only groups that are gone. A scan that stopped at a limit proves nothing about the rest, and
    // dropping a live group would lose its original limits.
    int kept = 0;
    for (int i = 0; i < g_group_count; i++) {
        struct stat info;
        if (g_groups[i].seen || stat(g_groups[i].path, &info) == 0) g_groups[kept++] = g_groups[i];
        else changed = true;
    }
    g_group_count = kept;
    g_foreground = foreground_path[0] ? group_find(foreground_path) : NULL;
    if (changed) originals_save_all();
    sample_cpu_load(g_cpu_busy, g_cpu_total);
}

void app_groups_restore_all(void) {
    for (int i = 0; i < g_group_count; i++) restore_original(&g_groups[i]);
    if (g_foreground) set_protection_chain(g_foreground->path, 0);
    if (enforcing()) {
        char path[PATH_BUFFER_SIZE + 8];
        originals_path(path, sizeof path);
        unlink(path);
    }
    free(g_groups);
    g_groups = NULL;
    g_group_count = g_group_capacity = 0;
    g_foreground = NULL;
}

void app_groups_process_timers(double now) {
    for (int i = 0; i < g_group_count; i++) {
        struct app_group *group = &g_groups[i];
        if (group->demote_at > 0 && now >= group->demote_at) {
            group->demote_at = 0;
            group->is_foreground = false;
            apply_background(group);
            log_event("event=GROUP_DEMOTED group=%s", group_name(group->path));
        }
    }
}

void app_groups_set_foreground(const char *group_path, double now) {
    struct app_group *next = group_path && group_path[0] ? group_find(group_path) : NULL;
    if (group_path && group_path[0] && !next) {
        app_groups_refresh();
        next = group_find(group_path);
    }
    if (next == g_foreground) return;
    if (g_foreground) {
        g_foreground->demote_at = now + g_policy.background_grace_seconds;
        if (!next) set_protection_chain(g_foreground->path, 0);
    }
    g_foreground = next;
    if (next) {
        next->demote_at = 0;
        next->is_foreground = true;
        apply_foreground(next);
    }
}

void app_groups_note_focus_reporter(void) {
    if (g_focus_reporter_seen) return;
    g_focus_reporter_seen = true;
    for (int i = 0; i < g_group_count; i++) apply_background(&g_groups[i]);
    log_event("event=FOCUS_REPORTER_SEEN groups=%d", g_group_count);
}

bool app_groups_focus_reporter_seen(void) { return g_focus_reporter_seen; }

const char *app_groups_foreground_path(void) { return g_foreground ? g_foreground->path : ""; }
const char *app_groups_foreground_name(void) { return g_foreground ? group_name(g_foreground->path) : "NONE"; }

// UNAVAILABLE means the CPU controller is off for this group, so its priority cannot be raised.
void app_groups_foreground_cpu_weight_text(char *buffer, size_t size) {
    long long weight = g_foreground ? get_limit(g_foreground->path, "cpu.weight", -1) : -1;
    if (!g_foreground) snprintf(buffer, size, "NONE");
    else if (weight < 0) snprintf(buffer, size, "UNAVAILABLE");
    else snprintf(buffer, size, "%lld", weight);
}

int app_groups_count(void) { return g_group_count; }

// Maps a process to the direct child of app.slice that contains it. The group must belong to the
// given user, so nobody can give priority to a group of another user.
bool app_group_path_of_pid(pid_t pid, uid_t owner, char *group_path, size_t size) {
    char path[PATH_BUFFER_SIZE + 32], text[PATH_BUFFER_SIZE];
    snprintf(path, sizeof path, "%s/%d/cgroup", g_paths.proc_cgroups, pid);
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool found = false;
    while (fgets(text, sizeof text, file)) {
        if (strncmp(text, "0::", 3)) continue;
        text[strcspn(text, "\n")] = '\0';
        char *marker = strstr(text, "/app.slice/");
        if (!marker) break;
        char *end = strchr(marker + strlen("/app.slice/"), '/');
        if (end) *end = '\0';
        int length = snprintf(group_path, size, "%s%s", g_paths.cgroup_root, text + 3);
        uid_t group_owner;
        found = length > 0 && (size_t)length < size && group_path_is_valid(group_path, &group_owner) && group_owner == owner;
        break;
    }
    fclose(file);
    return found;
}
