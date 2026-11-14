#!/usr/bin/env bash
# Run independent benchmark repetitions and retain raw logs/CSV files.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
runs=5
size_mb=100
ops=0
dev=zram0
pattern=text
cold_mode=both
no_l2=0
output=""

usage() {
    cat <<'EOF'
usage: run-zram-tier-bench.sh [options]
  --runs N             independent repetitions (default 5; use 20+ for reports)
  --size-mb N          working set in MiB (default 100)
  --ops N              steady-state operations per read/write phase
  --dev NAME           zRAM device name (default zram0)
  --pattern P          text or random (default text; unique page salt)
  --cold-mode M        read, write or both (default both)
  --no-l2              run A/B only
  --output DIR         new result directory (default results/<timestamp>-<pid>)

Run setup-zram-l2.sh and make before this script. No host cleanup is done here.
EOF
}

while (($#)); do
    case "$1" in
        --runs) runs="${2:?missing --runs value}"; shift 2 ;;
        --size-mb) size_mb="${2:?missing --size-mb value}"; shift 2 ;;
        --ops) ops="${2:?missing --ops value}"; shift 2 ;;
        --dev) dev="${2:?missing --dev value}"; shift 2 ;;
        --pattern) pattern="${2:?missing --pattern value}"; shift 2 ;;
        --cold-mode) cold_mode="${2:?missing --cold-mode value}"; shift 2 ;;
        --no-l2) no_l2=1; shift ;;
        --output) output="${2:?missing --output value}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

for value in "$runs" "$size_mb" "$ops"; do
    [[ $value =~ ^[0-9]+$ ]] || { echo "error: numeric options require integers" >&2; exit 2; }
done
((runs > 0 && size_mb > 0)) || { echo "error: runs and size-mb must be positive" >&2; exit 2; }
[[ $dev =~ ^[A-Za-z0-9._-]+$ ]] || { echo "error: unsafe device name" >&2; exit 2; }
[[ $pattern == text || $pattern == random ]] || { echo "error: invalid pattern" >&2; exit 2; }
[[ $cold_mode == read || $cold_mode == write || $cold_mode == both ]] || {
    echo "error: invalid cold mode" >&2; exit 2;
}
if ((EUID != 0)); then
    echo "error: root/CAP_SYS_NICE is required; rerun with sudo" >&2
    exit 1
fi

backing="$(cat "/sys/block/${dev}/backing_dev" 2>/dev/null || true)"
if [[ -n $backing && $backing != none && -b $backing ]]; then
    "$script_dir/check-zram-env.sh" "$backing"
else
    "$script_dir/check-zram-env.sh"
fi
[[ -x $script_dir/zram-tier-bench ]] || {
    echo "error: binary missing; run make -C $script_dir first" >&2
    exit 1
}
[[ $(cat /proc/sys/vm/page-cluster) == 0 ]] || {
    echo "error: vm.page-cluster must be 0; rerun setup-zram-l2.sh" >&2
    exit 1
}
disksize="$(cat "/sys/block/${dev}/disksize" 2>/dev/null || echo 0)"
((disksize >= size_mb * 1024 * 1024)) || {
    echo "error: zRAM disksize is absent or smaller than the working set" >&2
    exit 1
}
if ! awk -v path="/dev/${dev}" 'NR > 1 && $1 == path { found = 1 } END { exit !found }' /proc/swaps; then
    echo "error: /dev/${dev} is not active swap" >&2
    exit 1
fi
if ((!no_l2)) && [[ -z $backing || $backing == none ]]; then
    echo "error: L2 requested but backing_dev is not configured" >&2
    exit 1
fi

if [[ -z $output ]]; then
    output="${script_dir}/results/$(date +%Y%m%d-%H%M%S)-$$"
fi
[[ ! -e $output ]] || { echo "error: output already exists: $output" >&2; exit 1; }
mkdir -p -- "$output"
printf 'RUNNING\n' > "$output/STATUS"

{
    printf 'timestamp='; date --iso-8601=seconds
    printf 'kernel='; uname -srmo
    printf 'device=%s\nsize_mb=%s\npattern=%s\ncold_mode=%s\nruns=%s\nops=%s\nno_l2=%s\n' \
        "$dev" "$size_mb" "$pattern" "$cold_mode" "$runs" "$ops" "$no_l2"
    printf 'page_cluster='; cat /proc/sys/vm/page-cluster
    printf 'backing_dev=%s\n' "$backing"
    swapon --show
    zramctl || true
    for stat in mm_stat bd_stat io_stat; do
        printf '%s=' "$stat"
        cat "/sys/block/${dev}/${stat}" 2>/dev/null || printf 'unavailable\n'
    done
} > "$output/environment.txt"

invalid() {
    local reason="$1"
    printf 'INVALID: %s\n' "$reason" | tee "$output/STATUS" >&2
    printf 'raw artifacts retained for diagnosis: %s\n' "$output" >&2
    exit 1
}

cmd=("$script_dir/zram-tier-bench" -s "$size_mb" -d "$dev" -p "$pattern" -m "$cold_mode")
((ops > 0)) && cmd+=(-o "$ops")
((no_l2)) && cmd+=(-L)

for ((run = 1; run <= runs; run++)); do
    log="$output/run-${run}.log"
    csv="$output/run-${run}.csv"
    printf '== run %d/%d ==\n' "$run" "$runs"
    set +e
    "${cmd[@]}" -c "$csv" 2>&1 | tee "$log"
    pipe_status=("${PIPESTATUS[@]}")
    program_rc=${pipe_status[0]}
    tee_rc=${pipe_status[1]}
    set -e
    ((program_rc == 0 && tee_rc == 0)) || invalid "run ${run} exited with program=${program_rc}, tee=${tee_rc}"
    if grep -Eq '数据校验.*FAIL|回退 madvise|pidfd_open.*失败|换出没达到|writeback 后 VmSwap 掉|bd_writes 没有增长' "$log"; then
        invalid "run ${run} failed a state-evidence or data-integrity check"
    fi
    if [[ $cold_mode == read || $cold_mode == both ]]; then
        grep -q '^A-read,fault_p50_us,' "$csv" || invalid "run ${run} lacks A-read latency"
        grep -q '^B-read,fault_p50_us,' "$csv" || invalid "run ${run} lacks B-read latency"
        if ((!no_l2)); then
            grep -q '^C-read,fault_p50_us,' "$csv" || invalid "run ${run} lacks C-read latency"
            grep -q 'bd_reads 累计=' "$log" || invalid "run ${run} lacks L2 readback evidence"
        fi
    fi
    if [[ $cold_mode == write || $cold_mode == both ]]; then
        grep -q '^A-write,fault_p50_us,' "$csv" || invalid "run ${run} lacks A-write latency"
        grep -q '^B-write,fault_p50_us,' "$csv" || invalid "run ${run} lacks B-write latency"
        ((!no_l2)) && { grep -q '^C-write,fault_p50_us,' "$csv" || invalid "run ${run} lacks C-write latency"; }
    fi
done

printf 'COMPLETE\n' > "$output/STATUS"
printf 'completed %d runs; raw results: %s\n' "$runs" "$output"
