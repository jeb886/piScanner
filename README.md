# piscan

`piscan` is a low-latency Raspberry Pi 5 barcode scanner for a generic UVC USB webcam and an external GPIO trigger. Results are sent to a CLI sink today; the sink interface leaves room for a display later.

## Hardware

- Raspberry Pi 5 and a UVC-compatible USB webcam.
- For the default trigger, connect a momentary switch between BCM GPIO17 (header pin 11) and GND (pin 9). The internal pull-up is enabled by default.
- GPIO is **3.3 V only**. Never connect a 5 V signal. Use an optocoupler for 12/24 V industrial triggers.
- Add the user to the `video` and `gpio` groups for device access.

## Build

On Raspberry Pi OS Bookworm or Trixie:

```sh
sudo apt install build-essential cmake git pkg-config libturbojpeg0-dev
git clone https://github.com/jeb886/pi-project.git
cd pi-project
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPISCAN_NATIVE=ON
cmake --build build -j4
```

`PISCAN_NATIVE=ON` optimizes for the Pi's CPU and is recommended when building on the target device. Cross-builds should leave it off.

## Usage

```sh
piscan devices
piscan scan --device /dev/video0
piscan scan --width 640 --height 480 --pixfmt yuyv --formats QRCode,Code128 --stats
piscan trigger --gpio-line 17
piscan trigger --trigger stdin --output json
piscan bench sample.pgm --iterations 100
```

`scan` runs continuously. `trigger` starts one scan session per press by default; use `--trigger-mode level` to scan while held. Press Enter to trigger with `--trigger stdin`. Output is plain decoded text by default or one JSON object per line with `--output json`. Diagnostics and optional statistics go to stderr.

## Performance

- Restrict `--formats` to the symbologies actually used; leave `--try-harder` off for speed.
- Try 640x480 or 1280x720 and measure with `--stats`.
- YUYV decoding uses the camera's luma bytes without copying. USB 2 bandwidth often limits uncompressed 720p to around 10 fps, so MJPEG can be faster overall; measure both.
- Use a fixed, short exposure to reduce motion blur, crop with `--roi x,y,w,h`, and set the CPU governor to `performance` for sustained scanning.

## Architecture

One capture thread keeps the camera streaming and hands buffers to independent decoder workers. The pipeline is latest-frame-wins: stale pending frames are returned to the camera rather than building latency. Each worker owns its decoder. `ResultSink` separates scanning from text/JSON output and is the seam for a future display.