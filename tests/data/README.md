# Test image data

`upca/` and `qr/` contain real-camera photos of UPC-A and QR barcodes taken
from the zxing-cpp test suite (`test/samples/upca-*`, `test/samples/qrcode-*`,
tag v3.1.1, https://github.com/zxing-cpp/zxing-cpp). zxing-cpp is licensed
Apache-2.0; these images are redistributed here under the same license for
regression testing. Filenames were flattened (`<set>_<name>`) and WebP files
were converted to PNG; `.txt` sidecars hold the expected decoded text.

`hard/` holds photos that only decode with `--try-harder`; `negative/`
holds generated barcode-free images whose sidecar says `NONE`.

Test commands (from repo root, after `cmake --build build`):

    ./build/piscan decode tests/data/upca tests/data/qr --expect
    ./build/piscan decode tests/data/hard --expect --try-harder
    ./build/piscan decode tests/data/negative --expect
