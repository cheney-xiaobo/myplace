#!/usr/bin/env bash
# Read-only preflight for the zRAM -> backing-device benchmark.
set -euo pipefail

min_backing_bytes=$((128 * 1024 * 1024))
backing="${1:-}"
failures=0

pass() { printf 'PASS: %s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*"; }
fail() {
    printf 'FAIL: %s\n' "$*" >&2
    failures=$((failures + 1))
}

if [[ $# -gt 1 ]]; then
    echo "usage: $0 [backing-block-device]" >&2
    exit 2
fi

if [[ $(uname -s) == Linux ]]; then
    pass "Linux host ($(uname -r), $(uname -m))"
else
    echo "FAIL: this benchmark requires Linux, not $(uname -s)" >&2
    exit 1
fi

for command_name in awk blockdev findmnt gcc grep make mkswap realpath swapon zramctl; do
    if command -v "$command_name" >/dev/null 2>&1; then
        pass "tool ${command_name} is available"
    else
        fail "missing tool: ${command_name}"
    fi
done

config_command=()
if [[ -r /proc/config.gz ]] && command -v zcat >/dev/null 2>&1; then
    config_command=(zcat /proc/config.gz)
elif [[ -r /boot/config-$(uname -r) ]]; then
    config_command=(cat "/boot/config-$(uname -r)")
fi

config_value() {
    local key="$1"
    [[ ${#config_command[@]} -gt 0 ]] || return 2
    "${config_command[@]}" | awk -v key="$key" '
        $0 == key "=y" || $0 == key "=m" { print $0; found = 1 }
        $0 == "# " key " is not set" { print $0; found = 1 }
        END { if (!found) exit 1 }
    '
}

check_config() {
    local key="$1" required="$2" value
    if [[ ${#config_command[@]} -eq 0 ]]; then
        if [[ $required == required ]]; then
            fail "${key}: cannot prove the running kernel config"
        else
            warn "${key}: kernel config unavailable"
        fi
        return
    fi
    if ! value="$(config_value "$key")"; then
        if [[ $required == required ]]; then
            fail "${key}: not present in the running kernel config"
        else
            warn "${key}: not present in the running kernel config"
        fi
        return
    fi
    case "$value" in
        "${key}=y"|"${key}=m") pass "$value" ;;
        *)
            if [[ $required == required ]]; then
                fail "${key} must be enabled (found: ${value})"
            else
                warn "${value}"
            fi
            ;;
    esac
}

check_config CONFIG_SWAP required
check_config CONFIG_ZSMALLOC required
check_config CONFIG_ZRAM required
check_config CONFIG_ZRAM_WRITEBACK required
check_config CONFIG_ZRAM_MEMORY_TRACKING optional
check_config CONFIG_BLK_DEV_RAM optional

if compgen -G '/sys/block/zram*' >/dev/null; then
    pass "zRAM sysfs device exists"
else
    warn "no initialized zRAM device; setup must load/create one"
fi

if [[ -r /proc/swaps ]]; then
    swap_devices="$(awk 'NR > 1 { print $1 }' /proc/swaps)"
    if [[ -z $swap_devices ]]; then
        warn "no active swap"
    else
        while IFS= read -r device; do
            case "$device" in
                /dev/zram*) pass "active zRAM swap: ${device}" ;;
                *) warn "non-zRAM swap is active: ${device}; its traffic can contaminate results" ;;
            esac
        done <<< "$swap_devices"
    fi
fi

if [[ -n $backing ]]; then
    if [[ ! -b $backing ]]; then
        fail "backing path is not a block device: ${backing}"
    else
        backing="$(realpath -e -- "$backing")"
        pass "backing block device: ${backing}"
        backing_bytes="$(blockdev --getsize64 "$backing")"
        if ((backing_bytes >= min_backing_bytes)); then
            pass "backing capacity is ${backing_bytes} bytes"
        else
            fail "backing capacity ${backing_bytes} is below ${min_backing_bytes} bytes"
        fi
        if findmnt -rn -S "$backing" >/dev/null 2>&1; then
            fail "backing device is mounted: ${backing}"
        fi
        if awk 'NR > 1 { print $1 }' /proc/swaps | grep -Fxq -- "$backing"; then
            fail "backing device is already active swap: ${backing}"
        fi
    fi
fi

if ((failures > 0)); then
    printf 'PREFLIGHT: NOT READY (%d failure(s)); do not record benchmark results\n' "$failures" >&2
    exit 1
fi
printf 'PREFLIGHT: READY; benchmark results still require per-state evidence\n'
