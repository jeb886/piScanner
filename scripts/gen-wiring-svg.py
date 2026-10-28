#!/usr/bin/env python3
"""Generate docs/wiring.svg (Raspberry Pi 5 40-pin header wiring for piScanner)."""
import os

W, H = 1400, 1180
FONT = "DejaVu Sans, Arial, sans-serif"

PINS = {
    1: "3V3", 2: "5V", 3: "GPIO2", 4: "5V", 5: "GPIO3", 6: "GND", 7: "GPIO4", 8: "GPIO14",
    9: "GND", 10: "GPIO15", 11: "GPIO17", 12: "GPIO18", 13: "GPIO27", 14: "GND", 15: "GPIO22",
    16: "GPIO23", 17: "3V3", 18: "GPIO24", 19: "GPIO10", 20: "GND", 21: "GPIO9", 22: "GPIO25",
    23: "GPIO11", 24: "GPIO8", 25: "GND", 26: "GPIO7", 27: "GPIO0", 28: "GPIO1", 29: "GPIO5",
    30: "GND", 31: "GPIO6", 32: "GPIO12", 33: "GPIO13", 34: "GND", 35: "GPIO19", 36: "GPIO16",
    37: "GPIO26", 38: "GPIO20", 39: "GND", 40: "GPIO21",
}
COLORS = {"3V3": "#f39c12", "5V": "#e74c3c", "GND": "#444"}
GPIO_COLOR = "#5d8a5d"

DISPLAY_PINS = 26
DISPLAY = "#7b1fa2"
RED, BLUE, BLACK = "#d32f2f", "#1565c0", "#111"
# pin -> (wire colour, tag colour)
WIRES = {30: (BLACK, BLACK), 32: (RED, RED), 34: (BLACK, BLACK), 36: (BLUE, BLUE)}

X0, DX = 130, 60
EDGE_Y, INNER_Y = 422, 474
HDR_TOP, HDR_BOT = 392, 504


def pin_xy(n):
    x = X0 + DX * ((n - 1) // 2)
    return x, (INNER_Y if n % 2 else EDGE_Y)


out = []
e = out.append


def text(x, y, s, size=14, anchor="middle", fill="#222", bold=False, extra=""):
    w = ' font-weight="bold"' if bold else ""
    e(f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" fill="{fill}"{w}{extra}>{s}</text>')


def wire(d, color, width=8):
    e(f'<path d="{d}" fill="none" stroke="#fff" stroke-width="{width + 6}" stroke-linecap="round" stroke-linejoin="round"/>')
    e(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round"/>')


e(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="{FONT}">')
e(f'<rect width="{W}" height="{H}" fill="#fff"/>')
text(700, 44, "piScanner wiring — Raspberry Pi 5, board viewed from above", 28, bold=True)
text(700, 76, "40-pin header: pin 1 is the square pad at the end farthest from the USB/Ethernet ports. "
     "Odd pins = inner row, even pins = board-edge row.", 16, fill="#555")

# --- display HAT footprint: bands above and below pins 1-26, outline on the header
dx0 = X0 - 26
dx1 = pin_xy(DISPLAY_PINS)[0] + 26
for y0 in (318, 548):
    e(f'<rect x="{dx0}" y="{y0}" width="{dx1 - dx0}" height="36" rx="6" fill="{DISPLAY}"/>')
    text((dx0 + dx1) / 2, y0 + 25, "▼ 3.5\" SPI DISPLAY HAT — plugs onto pins 1–26 ▼" if y0 < 400
         else "▲ 3.5\" SPI DISPLAY HAT — plugs onto pins 1–26 ▲", 18, fill="#fff", bold=True)
# --- free-pin band on the right for contrast
fx0, fx1 = dx1 + 8, pin_xy(40)[0] + 30

# --- header
e(f'<rect x="{X0 - 34}" y="{HDR_TOP}" width="{pin_xy(40)[0] - X0 + 68}" height="{HDR_BOT - HDR_TOP}" rx="8" fill="#1b1b1b"/>')
e(f'<rect x="{dx0}" y="{HDR_TOP + 4}" width="{dx1 - dx0}" height="{HDR_BOT - HDR_TOP - 8}" rx="6" fill="{DISPLAY}" fill-opacity="0.35" stroke="{DISPLAY}" stroke-width="5"/>')
text(X0 - 40, EDGE_Y + 5, "edge", 12, "end", "#555")
text(X0 - 40, INNER_Y + 5, "inner", 12, "end", "#555")

# --- components and their wires (drawn before pin labels so tags sit on top of wires)
# Beeper: centred between pins 30 and 32
bx = (pin_xy(30)[0] + pin_xy(32)[0]) / 2
by = 210
# Trigger: centred between pins 34 and 36
tx = (pin_xy(34)[0] + pin_xy(36)[0]) / 2
ty = 210
for n, (c, _) in WIRES.items():
    x, y = pin_xy(n)
    top = by + 40 if n in (30, 32) else ty + 34
    wire(f"M{x},{top} V{y - 20}", c)

# beeper body
e(f'<circle cx="{bx}" cy="{by}" r="46" fill="#222" stroke="#000" stroke-width="2"/>')
e(f'<circle cx="{bx}" cy="{by}" r="9" fill="#666"/>')
text(pin_xy(32)[0] + 14, by + 64, "+", 22, "start", RED, True)
text(pin_xy(30)[0] - 14, by + 64, "−", 22, "end", BLACK, True)
text(bx - 70, by - 26, "BEEPER", 22, "end", RED, True)
text(bx - 70, by - 4, "2-wire PASSIVE buzzer", 14, "end")
text(bx - 70, by + 18, "+ (red) → pin 32 (GPIO12)", 14, "end", RED, True)
text(bx - 70, by + 40, "− (black) → pin 30 (GND)", 14, "end", BLACK, True)

# trigger switch body
e(f'<rect x="{tx - 50}" y="{ty - 36}" width="100" height="70" rx="8" fill="#ddd" stroke="#333" stroke-width="2"/>')
e(f'<circle cx="{tx}" cy="{ty - 1}" r="22" fill="#333"/>')
text(tx + 66, ty - 26, "TRIGGER", 22, "start", BLUE, True)
text(tx + 66, ty - 4, "momentary, N.O.", 14, "start")
text(tx + 66, ty + 18, "pin 36 (GPIO16)", 14, "start", BLUE, True)
text(tx + 66, ty + 40, "pin 34 (GND)", 14, "start", BLACK, True)

# --- pins and labels
for n, name in PINS.items():
    x, y = pin_xy(n)
    used = n in WIRES
    color = COLORS.get(name, GPIO_COLOR)
    if n == 1:
        e(f'<rect x="{x - 17}" y="{y - 17}" width="34" height="34" fill="{color}" stroke="#fff" stroke-width="2"/>')
    else:
        e(f'<circle cx="{x}" cy="{y}" r="17" fill="{color}"/>')
    if used:
        e(f'<circle cx="{x}" cy="{y}" r="22" fill="none" stroke="{WIRES[n][0] if WIRES[n][0] != BLACK else "#fff"}" stroke-width="4"/>')
    text(x, y + 5, n, 13, fill="#fff", bold=True)
    ly = y - 50 if n % 2 == 0 else y + 48
    lw = 7.5 * len(name) + 8
    if used:
        tc = WIRES[n][1]
        e(f'<rect x="{x - lw / 2 - 3}" y="{ly - 15}" width="{lw + 6}" height="21" rx="4" fill="{tc}"/>')
        text(x, ly, name, 12, fill="#fff", bold=True)
    else:
        text(x, ly, name, 12, fill="#333")

# --- legend (right of the lower display band)
lx, ly0 = 900, 600
items = [("3V3", "3.3 V power"), ("5V", "5 V power"), ("GND", "Ground (GND)"), (None, "GPIO (BCM number)")]
for i, (k, label) in enumerate(items):
    y = ly0 + 30 * i
    e(f'<circle cx="{lx}" cy="{y}" r="10" fill="{COLORS.get(k, GPIO_COLOR)}"/>')
    text(lx + 20, y + 5, label, 15, "start")
y = ly0 + 120
e(f'<rect x="{lx - 12}" y="{y - 9}" width="24" height="18" rx="3" fill="{DISPLAY}"/>')
text(lx + 20, y + 5, "Covered by the display HAT (pins 1–26)", 15, "start")
y += 30
e(f'<line x1="{lx - 12}" y1="{y}" x2="{lx + 10}" y2="{y}" stroke="{BLUE}" stroke-width="8" stroke-linecap="round"/>')
text(lx + 20, y + 5, "Wire you connect (pins 30–36)", 15, "start")

# --- USB section
oy = 165  # offset vs. the original layout
text(150, 615 + oy, "USB connections", 18, "start", bold=True)
e(f'<rect x="150" y="{635 + oy}" width="420" height="280" rx="16" fill="#2e7d32"/>')
e(f'<rect x="175" y="{645 + oy}" width="330" height="26" fill="#1b1b1b"/>')
e(f'<rect x="175" y="{645 + oy}" width="214" height="26" fill="{DISPLAY}" fill-opacity="0.8"/>')
text(282, 663 + oy, "display HAT", 13, fill="#fff", bold=True)
text(447, 663 + oy, "wires", 13, fill="#fff", bold=True)
e(f'<rect x="178" y="{648 + oy}" width="8" height="8" fill="#f39c12"/>')
text(175, 690 + oy, "40-pin header (pin 1 at this end)", 12, "start", "#fff")
text(360, 790 + oy, "Raspberry Pi 5", 22, fill="#fff", bold=True)
e(f'<rect x="540" y="{720 + oy}" width="70" height="70" rx="4" fill="#1565c0" stroke="#fff" stroke-width="2"/>')
text(575, 760 + oy, "USB 3", 14, fill="#fff")
text(625, 705 + oy, "blue USB 3 port", 13, "start", "#555")
e(f'<rect x="185" y="{895 + oy}" width="50" height="30" rx="8" fill="#9e9e9e" stroke="#fff" stroke-width="2"/>')
text(210, 945 + oy, "USB-C", 13)
e(f'<rect x="270" y="{900 + oy}" width="34" height="20" rx="3" fill="#9e9e9e"/>')
e(f'<rect x="330" y="{900 + oy}" width="34" height="20" rx="3" fill="#9e9e9e"/>')
text(317, 945 + oy, "micro-HDMI (unused)", 12, fill="#555")
e(f'<path d="M610,{755 + oy} C700,{755 + oy} 700,{700 + oy} 790,{700 + oy}" fill="none" stroke="#1565c0" stroke-width="5" stroke-linecap="round"/>')
e(f'<rect x="790" y="{665 + oy}" width="200" height="70" rx="35" fill="#111"/>')
e(f'<circle cx="890" cy="{700 + oy}" r="20" fill="#333" stroke="#777" stroke-width="3"/>')
text(890, 760 + oy, "Logitech Brio", 16, bold=True)
text(890, 780 + oy, "USB 3 cable", 13)
e(f'<path d="M210,{925 + oy} C210,{975 + oy} 600,{975 + oy} 780,{905 + oy}" fill="none" stroke="#555" stroke-width="5" stroke-linecap="round"/>')
e(f'<rect x="790" y="{820 + oy}" width="180" height="110" rx="6" fill="#333"/>')
e(f'<rect x="800" y="{830 + oy}" width="160" height="90" fill="#64b5f6"/>')
e(f'<rect x="770" y="{930 + oy}" width="220" height="14" rx="4" fill="#888"/>')
text(880, 970 + oy, "Windows PC", 16, bold=True)
text(880, 990 + oy, "USB-C cable: powers the Pi + Pi types barcodes as a keyboard", 13)

# --- notes
nx, ny = 1020, 760
notes = [
    ("GPIO pins are 3.3 V only.", RED, True),
    ("Never connect 5 V to a GPIO pin.", "#222", False),
    ("", "#222", False),
    ("The display HAT covers pins 1–26, so the", "#222", False),
    ("trigger and beeper use free pins 30–36.", "#222", False),
    ("", "#222", False),
    ("Trigger uses the Pi's internal pull-up;", "#222", False),
    ("pressing shorts GPIO16 to GND.", "#222", False),
    ("", "#222", False),
    ("Beeper: a small piezo connects directly;", "#222", False),
    ("a magnetic (coil) buzzer needs an NPN", "#222", False),
    ("transistor. Installer enables PWM on GPIO12.", "#222", False),
    ("", "#222", False),
    ("A PC port may not power Pi 5 + Brio.", "#222", False),
    ("If it browns out, use a USB-C power/", "#222", False),
    ("data splitter with a 5 V 5 A supply.", "#222", False),
]
e(f'<rect x="{nx}" y="{ny}" width="360" height="{50 + 20 * len(notes)}" rx="10" fill="#fff8e1" stroke="#f9a825" stroke-width="2"/>')
text(nx + 20, ny + 30, "Notes", 17, "start", bold=True)
for i, (s, c, b) in enumerate(notes):
    if s:
        text(nx + 20, ny + 60 + 20 * i, s, 14, "start", c, b)

e("</svg>")

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "wiring.svg")
with open(path, "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")
