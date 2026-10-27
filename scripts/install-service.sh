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

UNIT_PATH=/etc/systemd/system/piscanner.service
DROPIN_DIR=/etc/systemd/system/piscanner.service.d
DROPIN_PATH="$DROPIN_DIR/groups.conf"
CONFIG_PATH=/etc/default/piscanner
BINARY_PATH=/usr/local/bin/piscanner

UDEV_RULE=/etc/udev/rules.d/99-piscanner-hidg.rules

LEGACY_UNIT=/etc/systemd/system/piscan.service
LEGACY_GADGET_UNIT=/etc/systemd/system/piscan-gadget.service
LEGACY_DROPIN_DIR=/etc/systemd/system/piscan.service.d
LEGACY_CONFIG=/etc/default/piscan
LEGACY_BINARY=/usr/local/bin/piscan
LEGACY_GADGET_SCRIPT=/usr/local/sbin/piscan-gadget.sh
LEGACY_MODULES_CONF=/etc/modules-load.d/piscan-gadget.conf
LEGACY_UDEV_RULE=/etc/udev/rules.d/99-piscan-hidg.rules

config_txt() {
    for path in /boot/firmware/config.txt /boot/config.txt; do
        [[ -f "$path" ]] && { printf '%s\n' "$path"; return; }
    done
    printf '/boot/firmware/config.txt\n'
}

if ((UNINSTALL)); then
    systemctl disable --now piscanner.service >/dev/null 2>&1 || true
    "$REPO_ROOT/scripts/setup-hid-gadget.sh" --uninstall || true
    rm -f -- "$UNIT_PATH" "$BINARY_PATH" "$UDEV_RULE"
    rm -rf -- "$DROPIN_DIR"
    udevadm control --reload-rules 2>/dev/null || true
    systemctl daemon-reload
    printf '%s\n' 'Removed piscanner.service and /usr/local/bin/piscanner.'
    printf 'Kept %s and the piscanner system user. Remove them manually if desired: sudo rm -f %s; sudo userdel piscanner\n' \
        "$CONFIG_PATH" "$CONFIG_PATH"
    exit 0
fi

if [[ -e "$LEGACY_UNIT" || -e "$LEGACY_GADGET_UNIT" ]]; then
    systemctl disable --now piscan.service piscan-gadget.service >/dev/null 2>&1 || true
fi
rm -f -- "$LEGACY_UNIT" "$LEGACY_GADGET_UNIT" "$LEGACY_BINARY" "$LEGACY_GADGET_SCRIPT" \
    "$LEGACY_MODULES_CONF" "$LEGACY_UDEV_RULE"
rm -rf -- "$LEGACY_DROPIN_DIR"
systemctl daemon-reload
udevadm control --reload-rules 2>/dev/null || true

if [[ -e "$LEGACY_CONFIG" && ! -e "$CONFIG_PATH" && ! -L "$CONFIG_PATH" ]]; then
    mv -- "$LEGACY_CONFIG" "$CONFIG_PATH"
    sed -i 's/^PISCAN_ARGS=/PISCANNER_ARGS=/' "$CONFIG_PATH"
    printf 'Migrated the existing configuration from %s to %s.\n' "$LEGACY_CONFIG" "$CONFIG_PATH"
fi

userdel piscan 2>/dev/null || true
groupdel piscan 2>/dev/null || true

if [[ -d /sys/kernel/config/usb_gadget/piscan ]]; then
    # Release the USB controller so the new gadget can bind to it.
    echo "" >/sys/kernel/config/usb_gadget/piscan/UDC 2>/dev/null || true
    printf '%s\n' 'The old piscan USB gadget is still present in configfs; reboot to switch to the piscanner gadget.'
fi

if ((!NO_BUILD)); then
    if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
        sudo -u "$SUDO_USER" -- env "SUDO_USER=$SUDO_USER" "USER=$SUDO_USER" \
            "$REPO_ROOT/scripts/build.sh" --build-dir "$BUILD_DIR"
    else
        "$REPO_ROOT/scripts/build.sh" --build-dir "$BUILD_DIR"
    fi
fi

if [[ ! -x "$BUILD_PATH/piscanner" ]]; then
    printf 'error: executable not found: %s/piscanner\n' "$BUILD_PATH" >&2
    exit 1
fi
install -m 0755 -- "$BUILD_PATH/piscanner" "$BINARY_PATH"

if ! getent passwd piscanner >/dev/null 2>&1; then
    if getent group piscanner >/dev/null 2>&1; then
        useradd --system --no-create-home --shell /usr/sbin/nologin --gid piscanner piscanner
    else
        useradd --system --no-create-home --shell /usr/sbin/nologin piscanner
    fi
fi
if ! getent group piscanner >/dev/null 2>&1; then
    groupadd --system piscanner
fi

supplementary_groups=()
for group in video gpio; do
    if getent group "$group" >/dev/null 2>&1; then
        usermod -aG "$group" piscanner
        supplementary_groups+=("$group")
    fi
done

if [[ -e "$CONFIG_PATH" || -L "$CONFIG_PATH" ]]; then
    printf '%s already exists; leaving it unchanged.\n' "$CONFIG_PATH"
else
    printf '%s\n' \
        "# Arguments passed to the piScanner service. See \`piscanner --help\`." \
        '# Tip: use a stable camera path from /dev/v4l/by-id/ instead of /dev/video0.' \
        'PISCANNER_ARGS="trigger --trigger gpio --device /dev/video0 --output hid --beeper-line 18"' >"$CONFIG_PATH"
    chmod 0644 "$CONFIG_PATH"
    printf 'Created %s.\n' "$CONFIG_PATH"
fi

if [[ -e "$CONFIG_PATH" || -L "$CONFIG_PATH" ]] &&
   grep -Eq -- '--beeper-line[[:space:]]+27([[:space:]]|$)' "$CONFIG_PATH"; then
    sed -i -E 's/--beeper-line[[:space:]]+27/--beeper-line 18/g' "$CONFIG_PATH"
    printf 'Updated %s from GPIO27 to GPIO18 for the PWM beeper.\n' "$CONFIG_PATH"
fi

# Hardware PWM overlay for the passive beeper on GPIO18 (header pin 12).
CONFIG_TXT=$(config_txt)
if ! grep -Eq '^[[:space:]]*dtoverlay=pwm(,|$)' "$CONFIG_TXT" 2>/dev/null; then
    printf 'dtoverlay=pwm,pin=18,func=2\n' >>"$CONFIG_TXT"
    printf 'Added PWM beeper overlay to %s (reboot required).\n' "$CONFIG_TXT"
fi

# USB HID gadget: lets the piscanner system user write /dev/hidg0 and wires
# the gadget unit in before the scanner starts.
"$REPO_ROOT/scripts/setup-hid-gadget.sh"
install -D -m 0644 /dev/stdin "$UDEV_RULE" <<'EOF'
KERNEL=="hidg*", MODE="0660", GROUP="piscanner", TAG+="systemd"
EOF
udevadm control --reload-rules 2>/dev/null || true

install -D -m 0644 -- "$REPO_ROOT/systemd/piscanner.service" "$UNIT_PATH"
mkdir -p -- "$DROPIN_DIR"
{
    printf '[Service]\nSupplementaryGroups=\n'
    if ((${#supplementary_groups[@]} > 0)); then
        printf 'SupplementaryGroups=%s\n' "${supplementary_groups[*]}"
    fi
} >"$DROPIN_PATH"
chmod 0644 "$DROPIN_PATH"

systemctl daemon-reload
systemctl enable piscanner.service
if ! systemctl restart piscanner.service; then
    printf '%s\n' 'warning: service restart failed; inspect the service status and journal' >&2
fi
if ! systemctl status piscanner --no-pager; then
    printf '%s\n' 'The service is not active; check logs with: journalctl -u piscanner -f' >&2
fi
