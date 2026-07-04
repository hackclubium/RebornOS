#!/usr/bin/env bash
# Boots build/disk-test.img headlessly and turns the kernel's
# isa-debug-exit write into a pass/fail decision -- no human watching
# QEMU required. See kernel/include/qemu_debug.h for the exit code
# convention.
set -uo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$DIR/build"

source "$DIR/tools/ovmf-paths.sh"
ensure_ovmf_vars "$BUILD/OVMF_VARS_test.fd"

rm -f "$BUILD/test-serial.log"

# -serial stdio (redirected to a file by the shell) rather than
# `-serial file:...`: QEMU's file: chardev backend buffers writes and
# isn't guaranteed to flush before the timeout below SIGTERMs it on a
# hang, which can make a real hang look like silent, contentless output.
#
# 90s rather than a tighter bound: CI runners have no KVM, so QEMU falls
# back to software emulation (TCG), which is noticeably slower through
# OVMF's PEI/DXE/BDS phases than a KVM-accelerated local run. Also gives
# real headroom to the kernel's own handful of iteration-bounded
# busy-waits (mouse.c's PS/2 polling, smp.c's per-AP INIT-SIPI-SIPI
# wait) -- their iteration ceilings bound *guest* work, not wall-clock
# time, so a host that's heavily loaded or has descheduled this VM's
# vCPU threads for a while can still stretch a normally-instant wait
# into several real seconds without anything actually being wrong.
timeout 90 qemu-system-x86_64 \
    -machine q35 -m 512M -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS_test.fd" \
    -drive file=fat:16:rw:"$BUILD/esp-test",format=raw \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -display none \
    -serial stdio \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -no-reboot \
    > "$BUILD/test-serial.log" 2>&1
STATUS=$?

echo "--- serial log ---"
cat "$BUILD/test-serial.log" 2>/dev/null || true
echo "------------------"

case "$STATUS" in
    1)
        echo "TEST PASS (qemu exit code 1)"
        exit 0
        ;;
    3)
        echo "TEST FAIL (qemu exit code 3 -- kernel signaled failure)"
        exit 1
        ;;
    124)
        echo "TEST FAIL (timed out -- kernel likely hung or triple-faulted)"
        exit 1
        ;;
    *)
        echo "TEST FAIL (unexpected qemu exit code $STATUS)"
        exit 1
        ;;
esac
