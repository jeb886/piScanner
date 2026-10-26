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

GADGET_SCRIPT=/usr/local/sbin/piscan-gadget.sh
GADGET_UNIT=/etc/systemd/system/piscan-gadget.service
MODULES_CONF=/etc/modules-load.d/piscan-gadget.conf

# Pi OS mounts firmware config at /boot/firmware since Bookworm; fall back
# to /boot for older layouts.
config_txt() {
    for path in /boot/firmware/config.txt /boot/config.txt; do
        [[ -f "$path" ]] && { printf '%s\n' "$path"; return; }
    done
    printf '/boot/firmware/config.txt\n'
}

if ((UNINSTALL)); then
    systemctl disable --now piscan-gadget.service >/dev/null 2>&1 || true
    rm -f -- "$GADGET_UNIT" "$GADGET_SCRIPT" "$MODULES_CONF"
    systemctl daemon-reload
    printf '%s\n' 'Removed the HID gadget service. The dtoverlay line in config.txt was kept; remove it manually if desired.'
    exit 0
fi

# --- device-tree overlay: run the USB-C port in peripheral (gadget) mode ---
CONFIG=$(config_txt)
if ! grep -Eq '^[[:space:]]*dtoverlay=dwc3(,|$)' "$CONFIG" 2>/dev/null &&
   ! grep -Eq '^[[:space:]]*dtoverlay=dwc2' "$CONFIG" 2>/dev/null; then
    printf 'dtoverlay=dwc3,dr_mode=peripheral\n' >>"$CONFIG"
    printf 'Added dwc3 peripheral-mode overlay to %s (reboot required).\n' "$CONFIG"
fi

# --- module autoload ---
printf 'libcomposite\n' >"$MODULES_CONF"
modprobe libcomposite 2>/dev/null || true

# --- gadget creation script (runs every boot via the oneshot unit) ---
install -m 0755 -D /dev/stdin "$GADGET_SCRIPT" <<'GADGET'
#!/usr/bin/env bash
# Creates a USB HID keyboard gadget via configfs (libcomposite).
set -euo pipefail

GADGET=/sys/kernel/config/usb_gadget/piscan

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
echo "piscan" >strings/0x409/manufacturer
echo "piscan barcode scanner" >strings/0x409/product
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
Description=piscan USB HID keyboard gadget
After=systemd-modules-load.service
RequiresMountsFor=/sys/kernel/config
DefaultDependencies=no
Before=basic.target piscan.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/piscan-gadget.sh
RemainAfterExit=yes

[Install]
WantedBy=basic.target
UNIT

systemctl daemon-reload
systemctl enable piscan-gadget.service
systemctl start piscan-gadget.service 2>/dev/null \
    || printf '%s\n' 'note: gadget could not start now (overlay needs a reboot); it will start on boot.'

printf '%s\n' 'HID gadget setup complete.'
