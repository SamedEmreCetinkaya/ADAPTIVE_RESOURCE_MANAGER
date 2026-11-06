#!/bin/sh
# Builds the daemon and runs the unprivileged tests on other distributions, one container at a time. Also
# reports what each distribution packages: kernel, GNOME Shell, swapspace and zram-generator.
#
# Containers share this machine's kernel, so this proves nothing about zram, swap, SELinux or desktops on
# those systems. No container is privileged. The repository is mounted read-only, and only in a container
# with no network that drops to your user before it runs anything.
#
# Every container and image created here is deleted. Images that were already here stay. Results are saved
# after each image, so a crash loses only the image in progress, and the next run removes what it left.
#
# It refuses to run next to a virtual machine: on the one machine tested, a run on XanMod 7.2.15 with a
# virtual machine starting beside it ended in a frozen system, after the kernel found the CPU time counter
# running backwards. The same test on Debian 7.2.11 had no clock trouble.
# Usage: sudo sh TESTS/RUN_DISTRO_TESTS.sh [image...]
set -u
ROOT=$(dirname "$(dirname "$(readlink -f "$0")")")
docker info > /dev/null 2>&1 || { echo "Docker is not reachable. Run: sudo sh $0"; exit 1; }
virtual_machine_running() { find /proc/[0-9]*/fd -lname /dev/kvm 2>/dev/null | grep -q .; }
if virtual_machine_running; then
    echo "A virtual machine is using /dev/kvm. Close it, and anything that may start one, then run this again."
    exit 1
fi

IMAGES=${*:-debian:13 debian:12 ubuntu:24.04 ubuntu:26.04 fedora:latest archlinux:latest opensuse/tumbleweed:latest}
MEMORY_LIMIT=4g
DISK_LIMIT_KIB=$(( 20 * 1024 * 1024 ))
STEP_TIMEOUT_SECONDS=1200
CPU_LIMIT=$(( $(nproc) / 2 ))
[ "$CPU_LIMIT" -ge 1 ] || CPU_LIMIT=1
OWNER_UID=$(stat -c %u "$ROOT")
OWNER_GID=$(stat -c %g "$ROOT")
RESULT_DIRECTORY="$ROOT/TESTS/RESULTS/DISTRO_TEST_$(date +%Y%m%d_%H%M%S)"
PREFIX=adaptive-resource-manager-test
WORK=$(mktemp -d)
SUMMARY="$WORK/SUMMARY.txt"
DOCKER_ROOT=$(docker info --format '{{.DockerRootDir}}')
free_kib() { df -k --output=avail "$DOCKER_ROOT" | tail -n 1 | tr -d ' '; }
START_FREE_KIB=$(free_kib)
START_EPOCH=$(date +%s)
PEAK_USED_KIB=0
CONTAINER=""
TEST_IMAGE=""
PULLED_IMAGE=""

say() { echo "$1" | tee -a "$SUMMARY"; }

# Clock trouble seen by the journal since this run began. An ordinary user may not be able to read it.
report_clock() {
    if [ "$(id -u)" = 0 ]; then
        say "KERNEL=$(uname -r) CLOCKSOURCE=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource) TIME_JUMPS=$(journalctl --since "@$START_EPOCH" -u systemd-journald --no-pager -q -o cat --grep 'Time jumped' 2>/dev/null | wc -l) TSC_FAILURES=$(journalctl --since "@$START_EPOCH" -k --no-pager -q -o cat --grep 'Marking TSC unstable|Marking clocksource' 2>/dev/null | wc -l)"
    else
        say "KERNEL=$(uname -r) CLOCKSOURCE=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource) TIME_JUMPS=UNKNOWN TSC_FAILURES=UNKNOWN"
    fi
}

# Results belong to the owner of the repository, and root writes nothing there.
save_results() {
    if [ "$(id -u)" = 0 ]; then
        tar -C "$WORK" -cf - . | setpriv --reuid="$OWNER_UID" --regid="$OWNER_GID" --clear-groups sh -c 'mkdir -p "$1" && tar -C "$1" -xf -' sh "$RESULT_DIRECTORY"
    else
        mkdir -p "$RESULT_DIRECTORY" && tar -C "$WORK" -cf - . | tar -C "$RESULT_DIRECTORY" -xf -
    fi
    sync "$RESULT_DIRECTORY" "$RESULT_DIRECTORY/SUMMARY.txt" 2>/dev/null
}

remove_current() {
    [ -n "$CONTAINER" ] && docker rm -f "$CONTAINER" > /dev/null 2>&1
    [ -n "$TEST_IMAGE" ] && docker rmi -f "$TEST_IMAGE" > /dev/null 2>&1
    [ -n "$PULLED_IMAGE" ] && docker rmi "$PULLED_IMAGE" > /dev/null 2>&1
    CONTAINER=""
    TEST_IMAGE=""
    PULLED_IMAGE=""
}

finish() {
    remove_current
    say "LEFTOVER_CONTAINERS=$(docker ps -a -q --filter "name=$PREFIX" | wc -l)"
    say "LEFTOVER_IMAGES=$(docker images -q "$PREFIX/*" | wc -l)"
    say "PEAK_DISK_USED_MIB=$(( PEAK_USED_KIB / 1024 ))"
    say "DISK_NOT_RETURNED_MIB=$(( (START_FREE_KIB - $(free_kib)) / 1024 ))"
    report_clock
    save_results
    rm -rf "$WORK"
    echo "Results: $RESULT_DIRECTORY/SUMMARY.txt"
}
trap finish EXIT
trap 'exit 130' INT TERM

note_disk_use() {
    USED_KIB=$(( START_FREE_KIB - $(free_kib) ))
    [ "$USED_KIB" -gt "$PEAK_USED_KIB" ] && PEAK_USED_KIB=$USED_KIB
    [ "$USED_KIB" -le "$DISK_LIMIT_KIB" ]
}

# Runs as root in the container, with network and without the repository.
prepare_script() {
    cat <<'EOF'
report() { echo "$1=$2" >> /REPORT; }
: > /REPORT
OPTIONAL=""
if command -v apt-get > /dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    install_packages() { apt-get install -y -qq --no-install-recommends "$@"; }
    version_of() { apt-cache policy "$1" 2>/dev/null | sed -n 's/^ *Candidate: //p' | grep -v '(none)'; }
    BASE="gcc make libc6-dev linux-libc-dev python3 procps util-linux"
    ZRAM_PACKAGE=systemd-zram-generator
    if grep -q '^ID=ubuntu' /etc/os-release; then KERNEL_PACKAGE=linux-image-generic; else KERNEL_PACKAGE=linux-image-amd64; fi
elif command -v dnf > /dev/null 2>&1; then
    install_packages() { dnf -y -q install --setopt=install_weak_deps=False "$@"; }
    version_of() { dnf -q repoquery --latest-limit=1 --qf '%{version}\n' "$1" 2>/dev/null | head -n 1; }
    BASE="gcc make glibc-devel kernel-headers python3 procps-ng util-linux gawk"
    OPTIONAL="libasan libubsan"
    ZRAM_PACKAGE=zram-generator
    KERNEL_PACKAGE=kernel-core
elif command -v pacman > /dev/null 2>&1; then
    pacman -Syu --noconfirm
    install_packages() { pacman -S --noconfirm --needed "$@"; }
    version_of() { pacman -Si "$1" 2>/dev/null | sed -n 's/^Version *: //p'; }
    BASE="gcc make linux-api-headers python procps-ng util-linux gawk"
    ZRAM_PACKAGE=zram-generator
    KERNEL_PACKAGE=linux
elif command -v zypper > /dev/null 2>&1; then
    zypper -q -n refresh
    install_packages() { zypper -q -n install "$@"; }
    version_of() { zypper -q -n info "$1" 2>/dev/null | sed -n 's/^Version *: //p'; }
    BASE="gcc make glibc-devel linux-glibc-devel python3 procps util-linux gawk"
    OPTIONAL="libasan8 libubsan1"
    ZRAM_PACKAGE=zram-generator
    KERNEL_PACKAGE=kernel-default
else
    report PACKAGE_MANAGER UNKNOWN
    exit 1
fi
. /etc/os-release
report OS "$PRETTY_NAME"
if install_packages $BASE; then report BASE_PACKAGES PASSED; else report BASE_PACKAGES FAILED; exit 1; fi
[ -z "$OPTIONAL" ] || install_packages $OPTIONAL || report OPTIONAL_PACKAGES FAILED
report GCC "$(gcc -dumpfullversion)"
report C_LIBRARY "$(ldd --version | head -n 1)"
report LINUX_HEADERS "$(awk '/define LINUX_VERSION_CODE/{print int($3 / 65536) "." int($3 / 256) % 256}' /usr/include/linux/version.h)"
report PACKAGED_KERNEL "$(version_of "$KERNEL_PACKAGE")"
report PACKAGED_GNOME_SHELL "$(version_of gnome-shell)"
SWAPSPACE_VERSION=$(version_of swapspace)
report PACKAGED_SWAPSPACE "${SWAPSPACE_VERSION:-MISSING}"
ZRAM_VERSION=$(version_of "$ZRAM_PACKAGE")
report PACKAGED_ZRAM_GENERATOR "${ZRAM_VERSION:-MISSING} ($ZRAM_PACKAGE)"
[ -z "$SWAPSPACE_VERSION" ] || install_packages swapspace || report SWAPSPACE_INSTALL FAILED
[ -z "$ZRAM_VERSION" ] || install_packages "$ZRAM_PACKAGE" || report ZRAM_GENERATOR_INSTALL FAILED
# The paths ENABLE_ENFORCE.sh and the daemon rely on.
report SWAPSPACE_BINARY "$(command -v swapspace || echo MISSING)"
if [ -e /usr/lib/systemd/system/swapspace.service ]; then report SWAPSPACE_UNIT PRESENT; else report SWAPSPACE_UNIT MISSING; fi
if [ -x /usr/lib/systemd/system-generators/zram-generator ]; then report ZRAM_GENERATOR_PATH PRESENT; else report ZRAM_GENERATOR_PATH MISSING; fi
exit 0
EOF
}

# Runs as your user in the container, with no network and a read-only root filesystem.
run_script() {
    cat <<'EOF'
export HOME=/tmp TMPDIR=/tmp
cat /REPORT
grep -q '^BASE_PACKAGES=PASSED$' /REPORT || { echo BUILD=SKIPPED_NO_PACKAGES; exit 0; }
cp -r /source /tmp/source && cd /tmp/source || { echo COPY=FAILED; exit 1; }
rm -rf BUILD TESTS/RESULTS .git
: > /tmp/tests.log
if make -s > /tmp/build.log 2>&1; then
    echo "BUILD=PASSED warnings=$(grep -c 'warning:' /tmp/build.log)"
else
    echo BUILD=FAILED
    grep -E 'error|Error' /tmp/build.log | head -n 5
fi
if [ -x BUILD/adaptive-resource-managerd ]; then
    if make -s sanitize > /tmp/sanitize.log 2>&1; then
        echo SANITIZE_BUILD=PASSED
        sh TESTS/RUN_UNPRIVILEGED_TESTS.sh > /tmp/tests.log 2>&1
        echo "TESTS_EXIT=$? PASS=$(grep -c '^PASS ' /tmp/tests.log) FAIL=$(grep -c '^FAIL ' /tmp/tests.log)"
        grep '^FAIL ' /tmp/tests.log | head -n 30
        grep -E 'parameter not set|not found|Sanitizer|runtime error' /tmp/tests.log | head -n 10
    else
        echo SANITIZE_BUILD=FAILED
        grep -E 'error|Error' /tmp/sanitize.log | head -n 5
    fi
fi
for SCRIPT in SCRIPTS/*.sh TESTS/*.sh; do sh -n "$SCRIPT" || echo "SYNTAX_ERROR=$SCRIPT"; done
sh SCRIPTS/CHECK_SYSTEM.sh enforce | grep -E '^BLOCK|^CHECK_SYSTEM'
echo '--- FULL LOGS ---'
cat /tmp/build.log /tmp/tests.log
exit 0
EOF
}

# A run that was cut short, for example by a crash, could not clean up after itself.
docker ps -a -q --filter "name=$PREFIX" | xargs -r docker rm -f > /dev/null 2>&1
docker images -q "$PREFIX/*" | xargs -r docker rmi -f > /dev/null 2>&1

for IMAGE in $IMAGES; do
    if virtual_machine_running; then
        say "STOPPED_VIRTUAL_MACHINE_STARTED before=$IMAGE"
        exit 1
    fi
    case "$IMAGE" in
        *[!A-Za-z0-9._:/-]*) say "SKIPPED_INVALID_IMAGE_NAME=$IMAGE"; continue ;;
    esac
    NAME=$(echo "$IMAGE" | tr -c 'A-Za-z0-9._\n-' '_')
    LOGS="$WORK/$NAME"
    mkdir -p "$LOGS"
    say "== $IMAGE"
    if ! docker image inspect "$IMAGE" > /dev/null 2>&1; then
        if docker pull -q "$IMAGE" > "$LOGS/pull.log" 2>&1; then PULLED_IMAGE=$IMAGE; else say PULL=FAILED; continue; fi
    fi
    CONTAINER="$PREFIX-$NAME"
    TEST_IMAGE="$PREFIX/$NAME:prepared"
    echo "Installing packages..."
    timeout "$STEP_TIMEOUT_SECONDS" docker run --name "$CONTAINER" --security-opt no-new-privileges \
        --memory "$MEMORY_LIMIT" --memory-swap "$MEMORY_LIMIT" --cpus "$CPU_LIMIT" --pids-limit 4096 \
        "$IMAGE" sh -c "$(prepare_script)" > "$LOGS/prepare.log" 2>&1 < /dev/null
    say "PREPARE_EXIT=$?"
    docker commit "$CONTAINER" "$TEST_IMAGE" > /dev/null 2>&1 || { say COMMIT=FAILED; remove_current; continue; }
    docker rm -f "$CONTAINER" > /dev/null 2>&1
    if ! note_disk_use; then
        say "DISK_LIMIT_REACHED used_mib=$(( USED_KIB / 1024 ))"
        exit 1
    fi
    echo "Building and testing..."
    timeout "$STEP_TIMEOUT_SECONDS" docker run --name "$CONTAINER" --network none --read-only \
        --cap-drop ALL --cap-add SETUID --cap-add SETGID --security-opt no-new-privileges \
        --memory "$MEMORY_LIMIT" --memory-swap "$MEMORY_LIMIT" --cpus "$CPU_LIMIT" --pids-limit 4096 \
        --tmpfs /tmp:rw,exec,nosuid,nodev,size=2g,mode=1777 -v "$ROOT:/source:ro" "$TEST_IMAGE" \
        sh -c 'setpriv --reuid="$1" --regid="$2" --clear-groups sh -c "$3"; exit $?' sh "$OWNER_UID" "$OWNER_GID" "$(run_script)" \
        > "$LOGS/run.log" 2>&1 < /dev/null
    say "RUN_EXIT=$?"
    sed '/^--- FULL LOGS ---$/q' "$LOGS/run.log" | grep -v '^--- FULL LOGS ---$' | tee -a "$SUMMARY"
    note_disk_use
    remove_current
    report_clock
    save_results
done
say DISTRO_TESTS_FINISHED
