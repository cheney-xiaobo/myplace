#!/usr/bin/env bash
# Configure a dedicated zRAM swap device with a brd backing device.
# This changes host kernel state; use only on an exclusive Linux test host.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dev=zram0
brd=/dev/ram0
zram_mb=512
l2_mb=256
algorithm=lz4
assume_yes=0
cleanup=0

usage() {
    cat <<'EOF'
usage:
  setup-zram-l2.sh [--dev zram0] [--brd /dev/ram0]
                   [--zram-mb 512] [--l2-mb 256] [--algorithm lz4] [--yes]
  setup-zram-l2.sh --cleanup [--dev zram0] [--yes]

The backing device must be unused and at least l2-mb in size. If /dev/ram0
is absent, the script loads brd with rd_nr=1 and the requested rd_size.
EOF
}

while (($#)); do
    case "$1" in
        --dev) dev="${2:?missing --dev value}"; shift 2 ;;
        --brd) brd="${2:?missing --brd value}"; shift 2 ;;
        --zram-mb) zram_mb="${2:?missing --zram-mb value}"; shift 2 ;;
        --l2-mb) l2_mb="${2:?missing --l2-mb value}"; shift 2 ;;
        --algorithm) algorithm="${2:?missing --algorithm value}"; shift 2 ;;
        --cleanup) cleanup=1; shift ;;
        --yes) assume_yes=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ $dev =~ ^[A-Za-z0-9._-]+$ ]] || { echo "error: unsafe zRAM name" >&2; exit 2; }
[[ $zram_mb =~ ^[0-9]+$ && $zram_mb -gt 0 ]] || { echo "error: invalid zram-mb" >&2; exit 2; }
[[ $l2_mb =~ ^[0-9]+$ && $l2_mb -gt 0 ]] || { echo "error: invalid l2-mb" >&2; exit 2; }
[[ $algorithm =~ ^[A-Za-z0-9._-]+$ ]] || { echo "error: unsafe algorithm name" >&2; exit 2; }
if ((EUID != 0)); then
    echo "error: root is required for swap, zRAM, brd and vm.page-cluster" >&2
    exit 1
fi

state_dir=/run/zram-tier-bench
state_file="${state_dir}/${dev}.conf"

confirm() {
    local answer
    ((assume_yes)) && return
    printf 'WARNING: this changes host swap, zRAM, brd and vm.page-cluster.\n'
    printf 'Use only on an exclusive test host. Type YES to continue: '
    IFS= read -r answer || { printf '\n' >&2; exit 1; }
    [[ $answer == YES ]] || { echo "aborted" >&2; exit 1; }
}

if ((cleanup)); then
    confirm
    if [[ ! -r $state_file ]]; then
        echo "error: no setup state for ${dev}; refusing a generic teardown" >&2
        exit 1
    fi
    saved_page_cluster= loaded_zram=0 loaded_brd=0
    # root-owned mode-0600 state written by this script; never use eval.
    # shellcheck disable=SC1090
    source "$state_file"
    [[ ${saved_dev:-} == "$dev" ]] || { echo "error: state device mismatch" >&2; exit 1; }
    if awk -v path="/dev/${dev}" 'NR > 1 && $1 == path { found = 1 } END { exit !found }' /proc/swaps; then
        swapoff "/dev/${dev}"
    fi
    if [[ -w /sys/block/${dev}/reset ]]; then
        echo 1 > "/sys/block/${dev}/reset"
    fi
    if [[ $saved_page_cluster =~ ^[0-9]+$ ]]; then
        echo "$saved_page_cluster" > /proc/sys/vm/page-cluster
    fi
    rm -f -- "$state_file"
    ((loaded_zram)) && rmmod zram || true
    ((loaded_brd)) && rmmod brd || true
    printf 'cleanup complete: %s; vm.page-cluster restored to %s\n' "$dev" "$saved_page_cluster"
    exit 0
fi

confirm
"$script_dir/check-zram-env.sh"
if [[ -e $state_file ]]; then
    echo "error: ${dev} was already configured by this script; clean up first" >&2
    exit 1
fi
if awk 'NR > 1 && $5 >= 100 { found = 1 } END { exit !found }' /proc/swaps; then
    echo "error: another swap area has priority >= 100; refusing ambiguous placement" >&2
    exit 1
fi

loaded_zram=0
loaded_brd=0
initialized=0
swap_active=0
page_changed=0
saved_page_cluster="$(cat /proc/sys/vm/page-cluster)"

rollback() {
    local rc=$?
    trap - EXIT
    if ((rc != 0)); then
        ((swap_active)) && swapoff "/dev/${dev}" || true
        ((initialized)) && echo 1 > "/sys/block/${dev}/reset" || true
        ((page_changed)) && echo "$saved_page_cluster" > /proc/sys/vm/page-cluster || true
        ((loaded_zram)) && rmmod zram || true
        ((loaded_brd)) && rmmod brd || true
        printf 'setup failed; attempted rollback (original status %d)\n' "$rc" >&2
    fi
    exit "$rc"
}
trap rollback EXIT

if [[ ! -d /sys/block/${dev} ]]; then
    modprobe zram num_devices=1
    loaded_zram=1
fi
[[ -d /sys/block/${dev} ]] || { echo "error: /sys/block/${dev} is absent" >&2; exit 1; }
[[ ! -s /sys/block/${dev}/disksize || $(cat "/sys/block/${dev}/disksize") == 0 ]] || {
    echo "error: ${dev} is already initialized; refusing to reset it" >&2
    exit 1
}
if awk -v path="/dev/${dev}" 'NR > 1 && $1 == path { found = 1 } END { exit !found }' /proc/swaps; then
    echo "error: ${dev} is already active swap" >&2
    exit 1
fi

if [[ ! -b $brd ]]; then
    [[ $brd == /dev/ram0 ]] || { echo "error: backing device does not exist: $brd" >&2; exit 1; }
    modprobe brd rd_nr=1 rd_size=$((l2_mb * 1024)) max_part=0
    loaded_brd=1
    command -v udevadm >/dev/null 2>&1 && udevadm settle || true
fi
[[ -b $brd ]] || { echo "error: backing is not a block device: $brd" >&2; exit 1; }
brd="$(realpath -e -- "$brd")"
findmnt -rn -S "$brd" >/dev/null && { echo "error: backing is mounted: $brd" >&2; exit 1; }
if awk -v path="$brd" 'NR > 1 && $1 == path { found = 1 } END { exit !found }' /proc/swaps; then
    echo "error: backing is already active swap: $brd" >&2
    exit 1
fi
backing_bytes="$(blockdev --getsize64 "$brd")"
((backing_bytes >= l2_mb * 1024 * 1024)) || {
    echo "error: $brd has $backing_bytes bytes; need at least $((l2_mb * 1024 * 1024))" >&2
    echo "hint: built-in brd size is fixed at boot; use a larger unused block device" >&2
    exit 1
}

grep -qw -- "$algorithm" "/sys/block/${dev}/comp_algorithm" || {
    echo "error: zRAM algorithm is unavailable: $algorithm" >&2
    exit 1
}
# These writes must precede disksize; changing them afterwards returns EBUSY.
echo "$brd" > "/sys/block/${dev}/backing_dev"
echo "$algorithm" > "/sys/block/${dev}/comp_algorithm"
echo "${zram_mb}M" > "/sys/block/${dev}/disksize"
initialized=1
mkswap "/dev/${dev}" >/dev/null
swapon -p 100 "/dev/${dev}"
swap_active=1
echo 0 > /proc/sys/vm/page-cluster
page_changed=1

install -d -m 0700 "$state_dir"
umask 077
{
    printf 'saved_dev=%q\n' "$dev"
    printf 'saved_page_cluster=%q\n' "$saved_page_cluster"
    printf 'loaded_zram=%q\n' "$loaded_zram"
    printf 'loaded_brd=%q\n' "$loaded_brd"
} > "$state_file"

trap - EXIT
printf 'ready: /dev/%s, algorithm=%s, disksize=%s MiB, backing=%s (%s bytes)\n' \
    "$dev" "$algorithm" "$zram_mb" "$brd" "$backing_bytes"
printf 'vm.page-cluster: %s -> 0; cleanup: sudo %s --dev %s --cleanup --yes\n' \
    "$saved_page_cluster" "$script_dir/setup-zram-l2.sh" "$dev"
