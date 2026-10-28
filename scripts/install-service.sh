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
  --with-display    Configure the 3.5" SPI display (ILI9486) + touch
  -h, --help        Show this help
EOF
}

UNINSTALL=0
NO_BUILD=0
WITH_DISPLAY=0
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
    --with-display)
        WITH_DISPLAY=1
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

config_has_effective_line() {
    local config=$1 pattern=$2
    [[ -f "$config" ]] || return 1
    awk -v pattern="$pattern" '
        /^[[:space:]]*#/ { next }
        {
            line = $0
            sub(/^[[:space:]]*/, "", line)
            sub(/[[:space:]]*#.*/, "", line)
            sub(/[[:space:]]+$/, "", line)
            if (line ~ /^\[[^]]+\][[:space:]]*$/) {
                section = line
                sub(/^\[/, "", section)
                sub(/\][[:space:]]*$/, "", section)
                section = tolower(section)
                next
            }
            if ((section == "" || section == "all" || section == "pi5") && line ~ pattern)
                found = 1
        }
        END { exit !found }
    ' "$config"
}

append_config_line() {
    local config=$1 line=$2 last_section
    if [[ -f "$config" ]]; then
        last_section=$(awk '
            /^[[:space:]]*#/ { next }
            {
                line = $0
                sub(/^[[:space:]]*/, "", line)
                sub(/[[:space:]]*#.*/, "", line)
                sub(/[[:space:]]+$/, "", line)
                if (line ~ /^\[[^]]+\][[:space:]]*$/) {
                    section = line
                    sub(/^\[/, "", section)
                    sub(/\][[:space:]]*$/, "", section)
                    last_section = tolower(section)
                }
            }
            END { print last_section }
        ' "$config")
    else
        last_section=
    fi

    if [[ -s "$config" ]] && [[ $(tail -c 1 -- "$config" | wc -l) -eq 0 ]]; then
        printf '\n' >>"$config"
    fi
    if [[ -n "$last_section" && "$last_section" != "all" ]]; then
        printf '[all]\n%s\n' "$line" >>"$config"
    else
        printf '%s\n' "$line" >>"$config"
    fi
}

ensure_pwm_overlay() {
    local config=$1
    # Display layout moved the beeper: migrate an older pin=18 line.
    if config_has_effective_line "$config" 'dtoverlay=pwm,pin=18'; then
        sed -i -E 's/^([[:space:]]*)dtoverlay=pwm,pin=18,func=2/\1dtoverlay=pwm,pin=12,func=4/' "$config"
        printf 'Updated PWM beeper overlay to pin=12,func=4 in %s (reboot required).\n' "$config"
    fi
    if ! config_has_effective_line "$config" '^[[:space:]]*dtoverlay=pwm(,|$)'; then
        append_config_line "$config" 'dtoverlay=pwm,pin=12,func=4'
        printf 'Added PWM beeper overlay to %s (reboot required).\n' "$config"
    fi
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
        'PISCANNER_ARGS="trigger --trigger gpio --gpio-line 16 --device /dev/video0 --output hid --beeper-line 12 --log-file /run/piscanner/piscanner.log --display auto --display-rotate 270"' >"$CONFIG_PATH"
    chmod 0644 "$CONFIG_PATH"
    printf 'Created %s.\n' "$CONFIG_PATH"
fi

# Display-era pin layout: trigger GPIO16 (pin 36), beeper GPIO12 (pin 32).
if [[ -e "$CONFIG_PATH" || -L "$CONFIG_PATH" ]] &&
   grep -Eq -- '--beeper-line[[:space:]]+(18|27)([[:space:]]|$)' "$CONFIG_PATH"; then
    sed -i -E 's/--beeper-line[[:space:]]+(18|27)/--beeper-line 12/g' "$CONFIG_PATH"
    printf 'Updated %s to GPIO12 (pin 32) for the PWM beeper.\n' "$CONFIG_PATH"
fi
if [[ -e "$CONFIG_PATH" || -L "$CONFIG_PATH" ]] &&
   grep -q '^PISCANNER_ARGS=' "$CONFIG_PATH" && ! grep -q -- '--gpio-line' "$CONFIG_PATH"; then
    sed -i -E 's|^(PISCANNER_ARGS=".*)"$|\1 --gpio-line 16"|' "$CONFIG_PATH"
    printf 'Added --gpio-line 16 (pin 36) to %s.\n' "$CONFIG_PATH"
fi

if [[ -e "$CONFIG_PATH" || -L "$CONFIG_PATH" ]] &&
   grep -q '^PISCANNER_ARGS=' "$CONFIG_PATH" && ! grep -q -- '--log-file' "$CONFIG_PATH"; then
    sed -i -E 's|^(PISCANNER_ARGS=".*)"$|\1 --log-file /run/piscanner/piscanner.log"|' "$CONFIG_PATH"
    printf 'Added --log-file /run/piscanner/piscanner.log to %s.\n' "$CONFIG_PATH"
fi

# Hardware PWM overlay for the passive beeper on GPIO12 (header pin 32).
CONFIG_TXT=$(config_txt)
ensure_pwm_overlay "$CONFIG_TXT"

if ((WITH_DISPLAY)); then
    # 3.5" SPI HAT (ILI9486 panel + XPT2046 touch). The piscreen overlay
    # brings up both on spi0 (display cs0, touch cs1) via the fbtft
    # fb_ili9486 driver -> /dev/fbN. Reboot required.
    if ! config_has_effective_line "$CONFIG_TXT" 'dtparam=spi=on'; then
        append_config_line "$CONFIG_TXT" 'dtparam=spi=on'
        printf 'Enabled SPI in %s (reboot required).\n' "$CONFIG_TXT"
    fi
    # Remove stale overlays from the earlier mipi-dbi approach — they
    # double-claim spi0.0/0.1 and stop the panel's framebuffer entirely.
    if grep -Eq 'mipi-dbi-spi|compatible=ili9486|dc-gpio=24|dtoverlay=ads7846' "$CONFIG_TXT"; then
        sed -i '/mipi-dbi-spi,spi0-0/d;/compatible=ili9486/d;/width=480,height=320/d;/dc-gpio=24,reset-gpio=25/d;/dtoverlay=ads7846/d' \
            "$CONFIG_TXT"
        printf 'Removed stale mipi-dbi/ads7846 overlay lines from %s (reboot required).\n' "$CONFIG_TXT"
    fi
    if ! config_has_effective_line "$CONFIG_TXT" 'dtoverlay=piscreen'; then
        append_config_line "$CONFIG_TXT" 'dtoverlay=piscreen'
        printf 'Added piscreen display/touch overlay to %s (reboot required).\n' "$CONFIG_TXT"
    fi
    # Touchscreen evdev nodes need group "input" for the service account.
    if getent group input >/dev/null 2>&1; then
        usermod -aG input piscanner 2>/dev/null || true
    fi
    # Keep the kernel console off the panel: no console=tty1 on the kernel
    # command line, and the service unit unbinds vtcon0 before painting.
    local_cmdline=$(for path in /boot/firmware/cmdline.txt /boot/cmdline.txt; do
        [[ -f "$path" ]] && { printf '%s\n' "$path"; break; }
    done)
    if [[ -n "$local_cmdline" ]] && grep -q 'console=tty1' "$local_cmdline"; then
        [[ -f "${local_cmdline}.bak" ]] || cp "$local_cmdline" "${local_cmdline}.bak"
        sed -i 's/console=tty1 //' "$local_cmdline"
        printf 'Removed console=tty1 from %s (reboot required).\n' "$local_cmdline"
    fi
    # Show results on the panel when the service starts.
    if grep -q '^PISCANNER_ARGS=' "$CONFIG_PATH" 2>/dev/null && ! grep -q -- '--display' "$CONFIG_PATH"; then
        sed -i -E 's|^(PISCANNER_ARGS=".*)"$|\1 --display auto --display-rotate 270"|' "$CONFIG_PATH"
        printf 'Added --display auto --display-rotate 270 to %s.\n' "$CONFIG_PATH"
    elif grep -q '^PISCANNER_ARGS=' "$CONFIG_PATH" 2>/dev/null && grep -q -- '--display' "$CONFIG_PATH" &&
         ! grep -q -- '--display-rotate' "$CONFIG_PATH"; then
        sed -i -E 's|^(PISCANNER_ARGS=".*)"$|\1 --display-rotate 270"|' "$CONFIG_PATH"
        printf 'Added --display-rotate 270 to %s.\n' "$CONFIG_PATH"
    fi
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
