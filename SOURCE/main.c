// adaptive-resource-managerd: gives the focused application priority over memory and CPU and brings
// its swapped pages back. All amounts are relative to the machine it runs on.
#include "resource_manager.h"

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_CONFIG_PATH "/etc/adaptive-resource-manager/RESOURCE_POLICY.conf"
#define DEFAULT_RUNTIME_DIRECTORY "/run/adaptive-resource-manager"
#define DEFAULT_CGROUP_ROOT "/sys/fs/cgroup"
#define DEFAULT_SEAT_DIRECTORY "/run/systemd/seats"
#define MAX_FOCUS_REPORTS_PER_POLL 32
#define REJECTED_REPORT_LOG_INTERVAL_SECONDS 1.0
#define FOCUS_SOCKET_CHECK_SECONDS 1.0
#define FOCUS_SOCKET_NAME "focus.sock"
#define STATUS_FILE_NAME "STATUS"
#define GROUP_REFRESH_INTERVAL_SECONDS 5.0
#define STATUS_INTERVAL_SECONDS 5.0
#define STALL_MEASURE_WINDOW_SECONDS 5.0
#define MAX_POLL_MILLISECONDS 1000

static volatile sig_atomic_t g_stop;

static struct {
    bool active;
    double measure_at;
    char group_path[PATH_BUFFER_SIZE];
    unsigned long long memory_some, io_some, cpu_some;
    struct gpu_usage_sample gpu_first;
} g_stall;
static struct gpu_usage_sample g_gpu_second;

static void handle_stop_signal(int signal_number) {
    (void)signal_number;
    g_stop = 1;
}

// Built under a temporary name and moved into place. Closing the socket it replaces drops every sender
// connected to the old one, because permissions are only checked when a sender connects.
static int open_focus_socket(uid_t owner) {
    int descriptor = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (descriptor < 0) return -1;
    struct sockaddr_un address;
    char final_path[sizeof address.sun_path];
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    int final_length = snprintf(final_path, sizeof final_path, "%s/%s", g_paths.runtime_directory, FOCUS_SOCKET_NAME);
    int length = snprintf(address.sun_path, sizeof address.sun_path, "%s/%s.new", g_paths.runtime_directory, FOCUS_SOCKET_NAME);
    if (final_length < 0 || (size_t)final_length >= sizeof final_path || length < 0 || (size_t)length >= sizeof address.sun_path) {
        close(descriptor);
        errno = ENAMETOOLONG;
        return -1;
    }
    int enabled = 1;
    unlink(address.sun_path);
    // Credentials are on before anyone can reach the socket, so no datagram arrives without them.
    if (setsockopt(descriptor, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof enabled) != 0 ||
        bind(descriptor, (struct sockaddr *)&address, sizeof address) != 0 || chmod(address.sun_path, 0600) != 0 ||
        (owner != geteuid() && chown(address.sun_path, owner, (gid_t)-1) != 0) || rename(address.sun_path, final_path) != 0) {
        int saved_error = errno;
        unlink(address.sun_path);
        close(descriptor);
        errno = saved_error;
        return -1;
    }
    return descriptor;
}

// A socket every local user could reach would let any of them fill its queue and block the reports of
// the active user, so only that user owns it, and without an active seat this daemon does. The owner can
// change the mode, and this daemon cannot change it back without another capability, so a socket with
// the wrong owner or mode is replaced. A failed attempt runs again at the next check.
static void update_focus_socket(int *descriptor, double now) {
    static double next_check_at;
    static uid_t failed_owner = (uid_t)-1;
    if (now < next_check_at) return;
    next_check_at = now + FOCUS_SOCKET_CHECK_SECONDS;
    uid_t seats[MAX_SEATS];
    uid_t owner = active_seat_uids(seats, MAX_SEATS) ? seats[0] : geteuid();
    char path[PATH_BUFFER_SIZE + 32];
    struct stat info;
    snprintf(path, sizeof path, "%s/%s", g_paths.runtime_directory, FOCUS_SOCKET_NAME);
    if (lstat(path, &info) == 0 && S_ISSOCK(info.st_mode) && info.st_uid == owner && (info.st_mode & 07777) == 0600) return;
    int replacement = open_focus_socket(owner);
    if (replacement < 0) {
        if (owner != failed_owner) log_event("event=FOCUS_SOCKET_REPLACE_FAILED uid=%u error=\"%s\"", (unsigned)owner, strerror(errno));
        failed_owner = owner;
        return;
    }
    close(*descriptor);
    *descriptor = replacement;
    failed_owner = (uid_t)-1;
    log_event("event=FOCUS_SOCKET_REPLACED uid=%u", (unsigned)owner);
}

// A flood of bad reports must not fill the journal.
static void log_rejected_report(long pid, uid_t sender, const char *reason) {
    static double next_log_at;
    double now = monotonic_seconds();
    if (now < next_log_at) return;
    next_log_at = now + REJECTED_REPORT_LOG_INTERVAL_SECONDS;
    log_event("event=FOCUS_REPORT_REJECTED pid=%ld sender_uid=%u reason=%s", pid, (unsigned)sender, reason);
}

static bool read_focus_report(int descriptor, pid_t *reported, uid_t *reporter) {
    char text[32];
    char control[CMSG_SPACE(sizeof(struct ucred))];
    struct iovec vector = {text, sizeof text - 1};
    struct msghdr message;
    memset(&message, 0, sizeof message);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    ssize_t length = recvmsg(descriptor, &message, MSG_CMSG_CLOEXEC);
    if (length <= 0 || (message.msg_flags & MSG_CTRUNC)) return false;
    text[length] = '\0';

    uid_t sender = (uid_t)-1;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_CREDENTIALS && header->cmsg_len >= CMSG_LEN(sizeof(struct ucred))) {
            struct ucred credentials;
            memcpy(&credentials, CMSG_DATA(header), sizeof credentials);
            sender = credentials.uid;
        }
    }
    char *end;
    long pid = strtol(text, &end, 10);
    if (end == text || pid <= 0 || pid > 4194304 || sender == (uid_t)-1) return false;
    // Checked by user ID, so a remote login of the active seat user counts as that user. Another local
    // user gets nothing here, and without logind's seat data nobody does.
    if (!uid_has_active_seat(sender)) {
        log_rejected_report(pid, sender, "NOT_ACTIVE_SEAT_USER");
        return false;
    }
    char path[64];
    struct stat info;
    snprintf(path, sizeof path, "/proc/%ld", pid);
    if (stat(path, &info) != 0 || info.st_uid != sender) {
        log_rejected_report(pid, sender, "NOT_OWNER");
        return false;
    }
    *reported = (pid_t)pid;
    *reporter = sender;
    return true;
}

static void read_group_stall_totals(const char *group_path, unsigned long long *memory_some, unsigned long long *io_some, unsigned long long *cpu_some) {
    char path[PATH_BUFFER_SIZE + 32];
    snprintf(path, sizeof path, "%s/memory.pressure", group_path);
    *memory_some = read_pressure(path).some_total_microseconds;
    snprintf(path, sizeof path, "%s/io.pressure", group_path);
    *io_some = read_pressure(path).some_total_microseconds;
    snprintf(path, sizeof path, "%s/cpu.pressure", group_path);
    *cpu_some = read_pressure(path).some_total_microseconds;
}

static void log_focus_change(const char *group_path) {
    char path[PATH_BUFFER_SIZE + 32];
    struct group_swap_counts counts = {0, 0};
    uint64_t gpu_bytes = 0;
    readback_measure_group(group_path, &counts, &gpu_bytes);
    snprintf(path, sizeof path, "%s/memory.current", group_path);
    long long current = read_limit_file(path, 0);
    snprintf(path, sizeof path, "%s/memory.swap.current", group_path);
    long long swap = read_limit_file(path, 0);
    uint64_t page = (uint64_t)g_system.page_size_bytes;
    log_event("event=FOCUS_CHANGED group=%s mode=%s memory_mib=%lld swap_mib=%lld swap_zram_mib=%llu swap_disk_mib=%llu gpu_memory_mib=%llu available_mib=%llu",
              app_groups_foreground_name(), g_policy.run_mode == RUN_MODE_ENFORCE ? "ENFORCE" : "OBSERVE",
              (long long)(current / (long long)MEBIBYTE), (long long)(swap / (long long)MEBIBYTE),
              (unsigned long long)(counts.zram_pages * page / MEBIBYTE), (unsigned long long)(counts.disk_pages * page / MEBIBYTE),
              (unsigned long long)(gpu_bytes / MEBIBYTE), (unsigned long long)(memory_available_bytes() / MEBIBYTE));
}

static void switch_focus(const char *group_path, pid_t reported_pid, double now) {
    if (!strcmp(group_path, app_groups_foreground_path())) return;
    app_groups_set_foreground(group_path, now);
    // A path that is not a known group counts as no foreground. Nothing below may run on it.
    if (!group_path[0] || strcmp(group_path, app_groups_foreground_path())) {
        char comm_path[64], comm[32] = "?";
        snprintf(comm_path, sizeof comm_path, "/proc/%d/comm", reported_pid);
        read_text_file(comm_path, comm, sizeof comm);
        sanitize_log_text(comm);
        log_event("event=FOCUS_CHANGED group=NONE pid=%d process=%s", reported_pid, comm);
        g_stall.active = false;
        return;
    }
    log_focus_change(group_path);
    // Measures how long the application stalled right after the click: the number that matters.
    snprintf(g_stall.group_path, sizeof g_stall.group_path, "%s", group_path);
    read_group_stall_totals(group_path, &g_stall.memory_some, &g_stall.io_some, &g_stall.cpu_some);
    gpu_usage_take_sample(&g_stall.gpu_first);
    g_stall.measure_at = now + STALL_MEASURE_WINDOW_SECONDS;
    g_stall.active = true;
    if (readback_active_kind() != READBACK_NONE) readback_cancel();
    readback_start_foreground(group_path);
}

static void report_stall_if_due(double now) {
    if (!g_stall.active || now < g_stall.measure_at) return;
    unsigned long long memory_some, io_some, cpu_some;
    read_group_stall_totals(g_stall.group_path, &memory_some, &io_some, &cpu_some);
    gpu_usage_take_sample(&g_gpu_second);
    struct gpu_usage_summary gpu;
    gpu_usage_summarize(&g_stall.gpu_first, &g_gpu_second, &gpu);
    const char *name = strrchr(g_stall.group_path, '/');
    // GPU and CPU figures attribute a stutter that memory and disk stall times cannot explain.
    log_event("event=FOCUS_STALL group=%s window_seconds=%.0f memory_stall_milliseconds=%llu io_stall_milliseconds=%llu cpu_stall_milliseconds=%llu "
              "gpu_busy_percent=%.0f gpu_engine=%s gpu_top_process=%s performance_cpu_mhz=%d",
              name ? name + 1 : g_stall.group_path, STALL_MEASURE_WINDOW_SECONDS, (memory_some - g_stall.memory_some) / 1000,
              (io_some - g_stall.io_some) / 1000, (cpu_some - g_stall.cpu_some) / 1000, gpu.available ? gpu.busiest_engine_percent : -1.0,
              gpu.available ? gpu.busiest_engine : "UNAVAILABLE", gpu.available ? gpu.top_process : "UNAVAILABLE", performance_cpu_average_mhz());
    g_stall.active = false;
}

static void write_status(void) {
    char path[PATH_BUFFER_SIZE + 32], temporary[PATH_BUFFER_SIZE + 48], readback[160], cpu_weight[32];
    readback_status_text(readback, sizeof readback);
    app_groups_foreground_cpu_weight_text(cpu_weight, sizeof cpu_weight);
    snprintf(path, sizeof path, "%s/%s", g_paths.runtime_directory, STATUS_FILE_NAME);
    snprintf(temporary, sizeof temporary, "%s.new", path);
    FILE *file = fopen(temporary, "w");
    if (!file) return;
    // Every local user can read this file, so it holds only figures they can already see. Which
    // application has focus stays in the journal.
    fchmod(fileno(file), 0644);
    struct swap_layout layout;
    long long disk_mib = swap_layout_read(&layout) ? layout.managed_used_kib / 1024 : 0;
    fprintf(file, "RUN_MODE=%s FOREGROUND=%s FOCUS_REPORTER=%s FOREGROUND_CPU_WEIGHT=%s GROUPS=%d DISK_SWAP_MIB=%lld MEMORY_AVAILABLE_MIB=%llu %s\n",
            g_policy.run_mode == RUN_MODE_ENFORCE ? "ENFORCE" : "OBSERVE", app_groups_foreground_path()[0] ? "ACTIVE" : "NONE",
            app_groups_focus_reporter_seen() ? "SEEN" : "NONE", cpu_weight, app_groups_count(), disk_mib,
            (unsigned long long)(memory_available_bytes() / MEBIBYTE), readback);
    fclose(file);
    rename(temporary, path);
}

static int run_daemon(void) {
    umask(077);
    if (g_policy.run_mode == RUN_MODE_ENFORCE && geteuid() != 0 && !strcmp(g_paths.cgroup_root, DEFAULT_CGROUP_ROOT)) {
        fprintf(stderr, "ENFORCE needs root\n");
        return 1;
    }
    // The socket and the saved limits live here, so nobody else may be able to replace them.
    if (!path_is_trusted(g_paths.runtime_directory, true)) {
        fprintf(stderr, "Refusing runtime directory %s: it must be a directory that only this user can change\n", g_paths.runtime_directory);
        return 1;
    }
    int focus_socket = open_focus_socket(geteuid());
    if (focus_socket < 0) {
        perror("focus socket");
        return 1;
    }
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = handle_stop_signal;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);

    log_event("event=STARTED mode=%s memory_total_mib=%llu cpus=%d cpu_classes=%d", g_policy.run_mode == RUN_MODE_ENFORCE ? "ENFORCE" : "OBSERVE",
              (unsigned long long)(g_system.memory_total_bytes / MEBIBYTE), g_system.cpu_count, g_system.cpu_class_count);
    app_groups_load_saved_originals();
    app_groups_refresh();
    write_status();

    bool pending = false;
    double pending_since = 0, next_refresh = 0, next_status = 0;
    char pending_path[PATH_BUFFER_SIZE] = "";
    pid_t pending_pid = 0;
    struct pollfd poll_entry = {.fd = focus_socket, .events = POLLIN};
    while (!g_stop) {
        double now = monotonic_seconds();
        update_focus_socket(&focus_socket, now);
        poll_entry.fd = focus_socket;
        double wait = readback_next_due_seconds(now);
        if (pending) {
            double remaining = pending_since + g_policy.focus_debounce_milliseconds / 1000.0 - now;
            if (remaining < wait) wait = remaining;
        }
        int timeout = wait <= 0 ? 0 : wait * 1000 > MAX_POLL_MILLISECONDS ? MAX_POLL_MILLISECONDS : (int)(wait * 1000) + 1;
        int ready = poll(&poll_entry, 1, timeout);
        if (ready < 0 && errno != EINTR) {
            perror("poll");
            break;
        }
        now = monotonic_seconds();
        if (ready > 0 && (poll_entry.revents & POLLIN)) {
            pid_t pid;
            uid_t sender;
            for (int report = 0; report < MAX_FOCUS_REPORTS_PER_POLL && read_focus_report(focus_socket, &pid, &sender); report++) {
                app_groups_note_focus_reporter();
                if (!app_group_path_of_pid(pid, sender, pending_path, sizeof pending_path)) pending_path[0] = '\0';
                pending = true;
                pending_pid = pid;
                pending_since = now;
            }
        }
        if (pending && now - pending_since >= g_policy.focus_debounce_milliseconds / 1000.0) {
            pending = false;
            switch_focus(pending_path, pending_pid, now);
        }
        if (now >= next_refresh) {
            app_groups_refresh();
            next_refresh = now + GROUP_REFRESH_INTERVAL_SECONDS;
        }
        app_groups_process_timers(now);
        report_stall_if_due(now);
        readback_disk_guard_tick(monotonic_seconds());
        readback_tick(monotonic_seconds());
        if (now >= next_status) {
            write_status();
            next_status = now + STATUS_INTERVAL_SECONDS;
        }
    }
    log_event("event=STOPPING");
    readback_cancel();
    app_groups_restore_all();
    write_status();
    close(focus_socket);
    char path[PATH_BUFFER_SIZE + 32];
    snprintf(path, sizeof path, "%s/%s", g_paths.runtime_directory, FOCUS_SOCKET_NAME);
    unlink(path);
    return 0;
}

static void print_usage(const char *program) {
    printf("Usage: %s [--config PATH] [--runtime-dir DIR] [--cgroup-root DIR] [--proc-swaps PATH] [--seat-directory DIR] [--proc-cgroups DIR] [--probe] [--print-policy [--assume-memory-gib N]]\n", program);
}

int main(int argc, char **argv) {
    static const struct option OPTIONS[] = {
        {"config", required_argument, NULL, 'c'},
        {"runtime-dir", required_argument, NULL, 'r'},
        {"cgroup-root", required_argument, NULL, 'g'},
        {"proc-swaps", required_argument, NULL, 's'},
        {"seat-directory", required_argument, NULL, 'S'},
        {"proc-cgroups", required_argument, NULL, 'C'},
        {"probe", no_argument, NULL, 'p'},
        {"print-policy", no_argument, NULL, 'P'},
        {"assume-memory-gib", required_argument, NULL, 'm'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    const char *config_path = NULL;
    bool probe = false, print_only = false;
    double assumed_memory_gib = 0.0;
    snprintf(g_paths.cgroup_root, sizeof g_paths.cgroup_root, "%s", DEFAULT_CGROUP_ROOT);
    snprintf(g_paths.runtime_directory, sizeof g_paths.runtime_directory, "%s", DEFAULT_RUNTIME_DIRECTORY);
    snprintf(g_paths.proc_swaps, sizeof g_paths.proc_swaps, "/proc/swaps");
    snprintf(g_paths.seat_directory, sizeof g_paths.seat_directory, "%s", DEFAULT_SEAT_DIRECTORY);
    snprintf(g_paths.proc_cgroups, sizeof g_paths.proc_cgroups, "/proc");
    int option;
    while ((option = getopt_long(argc, argv, "", OPTIONS, NULL)) != -1) {
        switch (option) {
        case 'c': config_path = optarg; break;
        case 'r': snprintf(g_paths.runtime_directory, sizeof g_paths.runtime_directory, "%s", optarg); break;
        case 'g': snprintf(g_paths.cgroup_root, sizeof g_paths.cgroup_root, "%s", optarg); break;
        case 's': snprintf(g_paths.proc_swaps, sizeof g_paths.proc_swaps, "%s", optarg); break;
        case 'S': snprintf(g_paths.seat_directory, sizeof g_paths.seat_directory, "%s", optarg); break;
        case 'C': snprintf(g_paths.proc_cgroups, sizeof g_paths.proc_cgroups, "%s", optarg); break;
        case 'p': probe = true; break;
        case 'P': print_only = true; break;
        case 'm': assumed_memory_gib = atof(optarg); break;
        case 'h': print_usage(argv[0]); return 0;
        default: print_usage(argv[0]); return 2;
        }
    }
    policy_set_defaults();
    if (!system_info_detect()) {
        fprintf(stderr, "Could not read system information\n");
        return 1;
    }
    if (config_path) {
        if (!policy_load_file(config_path)) return 2;
    } else if (access(DEFAULT_CONFIG_PATH, R_OK) == 0 && !policy_load_file(DEFAULT_CONFIG_PATH)) {
        return 2;
    }
    if (!policy_validate()) return 2;
    if (probe) {
        system_info_print_probe();
        return 0;
    }
    if (print_only) {
        if (assumed_memory_gib > 0.0) g_system.memory_total_bytes = (uint64_t)(assumed_memory_gib * 1024.0 * (double)MEBIBYTE);
        policy_print();
        return 0;
    }
    return run_daemon();
}
