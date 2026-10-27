#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

usage() {
    cat <<'EOF'
Usage: scripts/build.sh [options]

Options:
  --skip-deps       Skip apt dependency installation
  --skip-tests      Build without configuring or running tests
  --clean           Remove the selected build directory before configuring
  --build-dir DIR   Select a build directory (default: build)
  -h, --help        Show this help
EOF
}

SKIP_DEPS=0
SKIP_TESTS=0
CLEAN=0
BUILD_DIR=build

while (($# > 0)); do
    case "$1" in
    --skip-deps)
        SKIP_DEPS=1
        shift
        ;;
    --skip-tests)
        SKIP_TESTS=1
        shift
        ;;
    --clean)
        CLEAN=1
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

if ((CLEAN)); then
    if [[ "$BUILD_PATH" == "/" || "$BUILD_PATH" == "$REPO_ROOT" || "$REPO_ROOT" == "$BUILD_PATH/"* ]]; then
        printf 'error: refusing to clean unsafe build directory: %s\n' "$BUILD_PATH" >&2
        exit 2
    fi
    rm -rf -- "$BUILD_PATH"
fi

SUDO=()
if ((EUID != 0)); then
    SUDO=(sudo)
fi

if ((!SKIP_DEPS)); then
    if ! command -v apt-get >/dev/null 2>&1; then
        printf '%s\n' 'error: apt-get is required to install build dependencies; use --skip-deps to skip this step' >&2
        exit 1
    fi
    "${SUDO[@]}" apt-get update
    "${SUDO[@]}" apt-get install -y build-essential cmake git pkg-config libturbojpeg0-dev v4l-utils
fi

cmake_args=(-S . -B "$BUILD_PATH" -DCMAKE_BUILD_TYPE=Release)
case "$(uname -m)" in
aarch64 | arm64)
    cmake_args+=(-DPISCANNER_NATIVE=ON)
    ;;
esac
if ((SKIP_TESTS)); then
    cmake_args+=(-DPISCANNER_BUILD_TESTS=OFF)
fi

cmake "${cmake_args[@]}"
cmake --build "$BUILD_PATH" -j"$(nproc)"
if ((!SKIP_TESTS)); then
    ctest --test-dir "$BUILD_PATH" --output-on-failure
fi

if ((EUID == 0)); then
    permission_user="${SUDO_USER:-${USER:-root}}"
else
    permission_user="${USER:-$(id -un)}"
fi
if [[ "$permission_user" == "root" ]]; then
    permission_user=
fi

group_changed=0
for group in video gpio; do
    if getent group "$group" >/dev/null 2>&1; then
        if [[ -n "$permission_user" ]]; then
            user_groups=" $(id -nG "$permission_user") "
            if [[ "$user_groups" != *" $group "* ]]; then
                "${SUDO[@]}" usermod -aG "$group" "$permission_user"
                printf 'Added %s to the %s group.\n' "$permission_user" "$group"
                group_changed=1
            fi
        fi
    elif [[ "$group" == "gpio" ]]; then
        printf '%s\n' 'warning: the gpio group does not exist; GPIO triggering may require root or a udev rule on non-Pi OS' >&2
    fi
done
if ((group_changed)); then
    printf '%s\n' 'A re-login is needed for the new group membership to take effect.'
fi

printf '\nBuild complete: %s/piscanner\n' "$BUILD_PATH"
printf 'Next steps:\n  %s/piscanner devices\n  %s/piscanner scan --stats\n  %s/scripts/install-service.sh\n' \
    "$BUILD_PATH" "$BUILD_PATH" "$REPO_ROOT"
