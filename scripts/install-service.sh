#!/usr/bin/env bash
set -euo pipefail

if ((EUID != 0)); then
    exec sudo -- "$0" "$@"
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

usage() {
    cat <<'EOF'
Usage: sudo scripts/install-service.sh [options]

Options:
  --uninstall       Remove the service unit and installed binary
  --no-build        Use the existing build binary
  --build-dir DIR   Select a build directory (default: build)
  -h, --help        Show this help
EOF
}

UNINSTALL=0
NO_BUILD=0
BUILD_DIR=build

while (($# > 0)); do
    case "$1" in
    --uninstall)
        UNINSTALL=1
        shift
        ;;
    --no-build)
        NO_BUILD=1
        shift
        ;;
    --build-dir)
        if (($# < 2)) || [[ -z "$2" ]]; then
            printf '%s\n' 'error: --build-dir requires a directory' >&2
            exit 2
        fi
        BUILD_DIR=$2
        shift 2
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        printf 'error: unknown option: %s\n' "$1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

if [[ "$BUILD_DIR" = /* ]]; then
    BUILD_PATH=$BUILD_DIR
else
    BUILD_PATH="$REPO_ROOT/$BUILD_DIR"
fi
BUILD_PATH="$(realpath -m -- "$BUILD_PATH")"

UNIT_PATH=/etc/systemd/system/piscan.service
DROPIN_DIR=/etc/systemd/system/piscan.service.d
DROPIN_PATH="$DROPIN_DIR/groups.conf"
CONFIG_PATH=/etc/default/piscan
BINARY_PATH=/usr/local/bin/piscan

UDEV_RULE=/etc/udev/rules.d/99-piscan-hidg.rules

if ((UNINSTALL)); then
    systemctl disable --now piscan.service >/dev/null 2>&1 || true
    "$REPO_ROOT/scripts/setup-hid-gadget.sh" --uninstall || true
    rm -f -- "$UNIT_PATH" "$BINARY_PATH" "$UDEV_RULE"
    rm -rf -- "$DROPIN_DIR"
    udevadm control --reload-rules 2>/dev/null || true
    systemctl daemon-reload
    printf '%s\n' 'Removed piscan.service and /usr/local/bin/piscan.'
    printf 'Kept %s and the piscan system user. Remove them manually if desired: sudo rm -f %s; sudo userdel piscan\n' \
        "$CONFIG_PATH" "$CONFIG_PATH"
    exit 0
fi

if ((!NO_BUILD)); then
    if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
        sudo -u "$SUDO_USER" -- env "SUDO_USER=$SUDO_USER" "USER=$SUDO_USER" \
            "$REPO_ROOT/scripts/build.sh" --build-dir "$BUILD_DIR"
    else
        "$REPO_ROOT/scripts/build.sh" --build-dir "$BUILD_DIR"
    fi
fi

if [[ ! -x "$BUILD_PATH/piscan" ]]; then
    printf 'error: executable not found: %s/piscan\n' "$BUILD_PATH" >&2
    exit 1
fi
install -m 0755 -- "$BUILD_PATH/piscan" "$BINARY_PATH"

if ! getent passwd piscan >/dev/null 2>&1; then
    if getent group piscan >/dev/null 2>&1; then
        useradd --system --no-create-home --shell /usr/sbin/nologin --gid piscan piscan
    else
        useradd --system --no-create-home --shell /usr/sbin/nologin piscan
    fi
fi
if ! getent group piscan >/dev/null 2>&1; then
    groupadd --system piscan
fi

supplementary_groups=()
for group in video gpio; do
    if getent group "$group" >/dev/null 2>&1; then
        usermod -aG "$group" piscan
        supplementary_groups+=("$group")
    fi
done

if [[ -e "$CONFIG_PATH" || -L "$CONFIG_PATH" ]]; then
    printf '%s already exists; leaving it unchanged.\n' "$CONFIG_PATH"
else
    printf '%s\n' \
        "# Arguments passed to piscan by piscan.service. See \`piscan --help\`." \
        '# Tip: use a stable camera path from /dev/v4l/by-id/ instead of /dev/video0.' \
        'PISCAN_ARGS="trigger --trigger gpio --device /dev/video0 --output hid"' >"$CONFIG_PATH"
    chmod 0644 "$CONFIG_PATH"
    printf 'Created %s.\n' "$CONFIG_PATH"
fi

# USB HID gadget: lets the piscan system user write /dev/hidg0 and wires
# the gadget unit in before the scanner starts.
"$REPO_ROOT/scripts/setup-hid-gadget.sh"
install -D -m 0644 /dev/stdin "$UDEV_RULE" <<'EOF'
KERNEL=="hidg*", MODE="0660", GROUP="piscan", TAG+="systemd"
EOF
udevadm control --reload-rules 2>/dev/null || true

install -D -m 0644 -- "$REPO_ROOT/systemd/piscan.service" "$UNIT_PATH"
mkdir -p -- "$DROPIN_DIR"
{
    printf '[Service]\nSupplementaryGroups=\n'
    if ((${#supplementary_groups[@]} > 0)); then
        printf 'SupplementaryGroups=%s\n' "${supplementary_groups[*]}"
    fi
} >"$DROPIN_PATH"
chmod 0644 "$DROPIN_PATH"

systemctl daemon-reload
systemctl enable piscan.service
if ! systemctl restart piscan.service; then
    printf '%s\n' 'warning: service restart failed; inspect the service status and journal' >&2
fi
if ! systemctl status piscan --no-pager; then
    printf '%s\n' 'The service is not active; check logs with: journalctl -u piscan -f' >&2
fi
