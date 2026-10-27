#!/usr/bin/env bash
# Configures the Pi's USB-C port as a USB-HID keyboard ("keyboard wedge")
# so scanned barcodes are typed into whatever computer the Pi is plugged
# into. Idempotent; safe to re-run. Requires a reboot after first install
# (device-tree overlay).
#
#   sudo scripts/setup-hid-gadget.sh            install
#   sudo scripts/setup-hid-gadget.sh --uninstall remove (keeps config.txt line)

set -euo pipefail

if ((EUID != 0)); then
    exec sudo -- "$0" "$@"
fi

UNINSTALL=0
[[ "${1:-}" == "--uninstall" ]] && UNINSTALL=1

GADGET_SCRIPT=/usr/local/sbin/piscanner-gadget.sh
GADGET_UNIT=/etc/systemd/system/piscanner-gadget.service
MODULES_CONF=/etc/modules-load.d/piscanner-gadget.conf

# Pi OS mounts firmware config at /boot/firmware since Bookworm; fall back
# to /boot for older layouts.
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

ensure_gadget_overlay() {
    local config=$1
    if grep -Eq '^[[:space:]]*dtoverlay=dwc3,dr_mode=peripheral[[:space:]]*$' "$config"; then
        sed -i '/^[[:space:]]*dtoverlay=dwc3,dr_mode=peripheral[[:space:]]*$/d' "$config"
        printf 'Replaced stale dwc3 peripheral-mode overlay in %s.\n' "$config"
    fi

    if ! config_has_effective_line "$config" '^[[:space:]]*dtoverlay=dwc2,(.*,)?dr_mode=peripheral([,[:space:]]|$)'; then
        append_config_line "$config" 'dtoverlay=dwc2,dr_mode=peripheral'
        printf 'Added dwc2 peripheral-mode overlay to %s (reboot required).\n' "$config"
    fi

    if config_has_effective_line "$config" '^[[:space:]]*dtoverlay=dwc2,(.*,)?dr_mode=host([,[:space:]]|$)'; then
        printf 'warning: effective dwc2 host-mode overlay in %s conflicts with gadget peripheral mode; remove it to enable gadget mode.\n' \
            "$config" >&2
    fi
}

if ((UNINSTALL)); then
    systemctl disable --now piscanner-gadget.service >/dev/null 2>&1 || true
    rm -f -- "$GADGET_UNIT" "$GADGET_SCRIPT" "$MODULES_CONF"
    systemctl daemon-reload
    printf '%s\n' 'Removed the HID gadget service. The dtoverlay line in config.txt was kept; remove it manually if desired.'
    exit 0
fi

# --- device-tree overlay: use DWC2 peripheral mode for the USB-C port ---
CONFIG=$(config_txt)
ensure_gadget_overlay "$CONFIG"

# --- module autoload ---
printf 'libcomposite\n' >"$MODULES_CONF"
modprobe libcomposite 2>/dev/null || true

# --- gadget creation script (runs every boot via the oneshot unit) ---
install -m 0755 -D /dev/stdin "$GADGET_SCRIPT" <<'GADGET'
#!/usr/bin/env bash
# Creates a USB HID keyboard gadget via configfs (libcomposite).
set -euo pipefail

GADGET=/sys/kernel/config/usb_gadget/piscanner

mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
if [[ -d "$GADGET" ]]; then
    exit 0
fi

mkdir -p "$GADGET"
cd "$GADGET"

# Linux Foundation / Composite Gadget IDs
echo 0x1d6b >idVendor
echo 0x0104 >idProduct
echo 0x0100 >bcdDevice
echo 0x0200 >bcdUSB

mkdir -p strings/0x409
echo "piScanner" >strings/0x409/manufacturer
echo "piScanner barcode scanner" >strings/0x409/product
echo "0001" >strings/0x409/serialnumber

mkdir -p configs/c.1/strings/0x409
echo "keyboard" >configs/c.1/strings/0x409/configuration
echo 100 >configs/c.1/MaxPower

mkdir -p functions/hid.usb0
echo 1 >functions/hid.usb0/protocol   # keyboard
echo 1 >functions/hid.usb0/subclass   # boot interface
echo 8 >functions/hid.usb0/report_length
# Standard 8-byte boot keyboard report descriptor.
printf '\x05\x01\x09\x06\xa1\x01\x05\x07\x19\xe0\x29\xe7\x15\x00\x25\x01\x75\x01\x95\x08\x81\x02\x95\x01\x75\x08\x81\x01\x95\x06\x75\x08\x15\x00\x25\x65\x05\x07\x19\x00\x29\x65\x81\x00\xc0' >functions/hid.usb0/report_desc

ln -sf functions/hid.usb0 configs/c.1/

# Bind to the first available USB device controller.
udc=$(ls /sys/class/udc | head -n1)
echo "$udc" >UDC
GADGET

install -m 0644 -D /dev/stdin "$GADGET_UNIT" <<'UNIT'
[Unit]
Description=piScanner USB HID keyboard gadget
After=systemd-modules-load.service
RequiresMountsFor=/sys/kernel/config
DefaultDependencies=no
Before=basic.target piscanner.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/piscanner-gadget.sh
RemainAfterExit=yes

[Install]
WantedBy=basic.target
UNIT

systemctl daemon-reload
systemctl enable piscanner-gadget.service
systemctl start piscanner-gadget.service 2>/dev/null \
    || printf '%s\n' 'note: gadget could not start now (overlay needs a reboot); it will start on boot.'

printf '%s\n' 'HID gadget setup complete.'
