#!/usr/bin/env bash
# Shared helper, meant to be `source`d. Locates the distro's OVMF
# firmware (the file names moved around across Ubuntu releases) and
# stages a writable copy of the NVRAM vars file, since the system one is
# typically read-only/shared.
#
# Deliberately does NOT `set -euo pipefail` here: this file is sourced
# into the caller's shell, so setting shell options here would silently
# override whatever the caller chose (e.g. test-qemu.sh needs errexit
# OFF so it can inspect QEMU's exit code itself).

find_ovmf_code() {
    for candidate in \
        /usr/share/OVMF/OVMF_CODE_4M.fd \
        /usr/share/OVMF/OVMF_CODE.fd \
        /usr/share/ovmf/OVMF.fd
    do
        if [ -f "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done
    echo "ERROR: could not find OVMF firmware. Is the 'ovmf' package installed?" >&2
    exit 1
}

find_ovmf_vars_template() {
    for candidate in \
        /usr/share/OVMF/OVMF_VARS_4M.fd \
        /usr/share/OVMF/OVMF_VARS.fd
    do
        if [ -f "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done
    echo "ERROR: could not find an OVMF_VARS template. Is the 'ovmf' package installed?" >&2
    exit 1
}

ensure_ovmf_vars() {
    local dest="$1"
    if [ ! -f "$dest" ]; then
        mkdir -p "$(dirname "$dest")"
        cp "$(find_ovmf_vars_template)" "$dest"
    fi
}

OVMF_CODE="$(find_ovmf_code)"
