#!/usr/bin/env bash
set -euo pipefail

prepare_only=false
case "${1:-}" in
    "") ;;
    --prepare-only) prepare_only=true ;;
    *) echo "Usage: $0 [--prepare-only]" >&2; exit 2 ;;
esac
if (( $# > 1 )); then
    echo "Usage: $0 [--prepare-only]" >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$script_dir/.." && pwd)
build_dir="$(dirname -- "$repo_dir")/build-baseline"
out_dir="$build_dir/os-lock-tests"
kernel="$build_dir/arch/x86/boot/bzImage"
packer="$build_dir/usr/gen_init_cpio"

# gen_init_cpio's text manifest cannot represent these paths with whitespace.
for path in "$repo_dir" "$build_dir"; do
    case "$path" in
        *[[:space:]]*)
            echo "Repository paths must not contain whitespace." >&2
            exit 1
            ;;
    esac
done

for tool in gcc readelf gzip cmp git sha256sum; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing tool: $tool" >&2
        exit 1
    fi
done
if [[ ! -s "$kernel" || ! -x "$packer" || ! -f "$build_dir/.config" ]]; then
    echo "Build the baseline kernel first; see OS_LOCK_BASELINE.md." >&2
    exit 1
fi
if ! cmp -s "$repo_dir/OS_LOCK_BASELINE.config" "$build_dir/.config"; then
    echo "build-baseline/.config differs from the recorded baseline; stopped." >&2
    exit 1
fi
if [[ ! -x /bin/busybox ]]; then
    echo "Install busybox-static first." >&2
    exit 1
fi
if readelf -l /bin/busybox | grep -q INTERP; then
    echo "/bin/busybox is dynamically linked; install busybox-static." >&2
    exit 1
fi

mkdir -p -- "$out_dir"
flags=(-O2 -Wall -Wextra -static -pthread)
for name in bench bench_sleep bench_checked pi_baseline pi_chain; do
    gcc "${flags[@]}" "$script_dir/$name.c" -o "$out_dir/$name"
done
gcc "${flags[@]}" -DITERATIONS=1000 "$script_dir/bench_checked.c" \
    -o "$out_dir/bench_checked_smoke"

manifest="$out_dir/initramfs.list"
cat > "$manifest" <<EOF
dir /bin 0755 0 0
dir /sbin 0755 0 0
dir /proc 0555 0 0
dir /sys 0555 0 0
dir /dev 0755 0 0
dir /tmp 01777 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
file /bin/busybox /bin/busybox 0755 0 0
file /init $script_dir/init 0755 0 0
file /bin/bench $out_dir/bench 0755 0 0
file /bin/bench_sleep $out_dir/bench_sleep 0755 0 0
file /bin/bench_checked $out_dir/bench_checked 0755 0 0
file /bin/bench_checked_smoke $out_dir/bench_checked_smoke 0755 0 0
file /bin/pi_baseline $out_dir/pi_baseline 0755 0 0
file /bin/pi_chain $out_dir/pi_chain 0755 0 0
EOF
"$packer" "$manifest" | gzip -9 > "$out_dir/initramfs.cpio.gz.tmp"
gzip -t "$out_dir/initramfs.cpio.gz.tmp"
mv -- "$out_dir/initramfs.cpio.gz.tmp" "$out_dir/initramfs.cpio.gz"

{
    printf 'source_commit='; git -C "$repo_dir" rev-parse HEAD
    printf 'compiler='; gcc -dumpfullversion
    printf 'cflags=%s\n' "${flags[*]}"
    printf 'guest_cpus=4\nguest_memory=2G\n'
    printf 'kernel=%s\n' "$kernel"
    printf 'artifact_sha256:\n'
    sha256sum "$script_dir/pi_baseline.c" "$out_dir/pi_baseline" \
        "$script_dir/pi_chain.c" "$out_dir/pi_chain" \
        "$build_dir/.config" "$kernel"
    printf 'worktree_status:\n'; git -C "$repo_dir" status --short
} > "$out_dir/environment.txt"
printf 'Prepared: %s\n' "$out_dir/initramfs.cpio.gz"

if "$prepare_only"; then
    exit 0
fi
if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "Missing tool: qemu-system-x86_64" >&2
    exit 1
fi
exec qemu-system-x86_64 \
    -accel kvm -cpu host -smp 4 -m 2G \
    -kernel "$kernel" -initrd "$out_dir/initramfs.cpio.gz" \
    -append "console=ttyS0 rdinit=/init nokaslr" \
    -nographic -no-reboot
