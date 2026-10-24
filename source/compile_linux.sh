#!/usr/bin/env bash
# Compiles the Linux ForlornHLE from this folder and puts it next to play_linux.sh (one folder up).
#
# Needs: a C++20 compiler (GCC 11+ or Clang 14+), CMake 3.16+, pkg-config, and the development files of SDL2, OpenGL
# and FFmpeg (libavformat, libavcodec, libavutil, libswresample). Missing ones are listed with the command that
# installs them. On Debian / Ubuntu without root (or with --local-deps), the packages are downloaded and unpacked
# into ~/.local/share/forlorn-hle/deps instead (apt-get download + dpkg -x; nothing is installed system-wide).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$(dirname "$here")"
data="${XDG_DATA_HOME:-$HOME/.local/share}/forlorn-hle"
deps="$data/deps"
local_deps=0
[ "${1:-}" = "--local-deps" ] && local_deps=1

say() { printf '%s\n' "$*"; }
missing_tools=()
for tool in cmake pkg-config; do command -v "$tool" >/dev/null || missing_tools+=("$tool"); done
command -v c++ >/dev/null || command -v g++ >/dev/null || command -v clang++ >/dev/null || missing_tools+=("c++ compiler")
modules="sdl2 libavformat libavcodec libavutil libswresample"

install_hint() {
    if command -v apt-get >/dev/null; then say "  sudo apt install build-essential cmake pkg-config libsdl2-dev libgl-dev libavformat-dev libavcodec-dev libavutil-dev libswresample-dev"
    elif command -v dnf >/dev/null; then say "  sudo dnf install gcc-c++ cmake pkgconf SDL2-devel mesa-libGL-devel ffmpeg-free-devel"
    elif command -v pacman >/dev/null; then say "  sudo pacman -S --needed base-devel cmake pkgconf sdl2 mesa ffmpeg"
    elif command -v zypper >/dev/null; then say "  sudo zypper install gcc-c++ cmake pkg-config libSDL2-devel Mesa-libGL-devel ffmpeg-devel"
    else say "  (install your distribution's development packages for SDL2, OpenGL and FFmpeg, plus CMake and a C++ compiler)"
    fi
}
if [ ${#missing_tools[@]} -ne 0 ]; then
    say "Missing tools: ${missing_tools[*]}. Install them, for example:"
    install_hint
    exit 1
fi

# Packages unpacked without root: their pkg-config files, headers and libraries are used from $deps/root.
use_local_deps() {
    local root="$deps/root" lib="$deps/root/usr/lib/x86_64-linux-gnu"
    export PKG_CONFIG_SYSROOT_DIR="$root"
    export PKG_CONFIG_PATH="$lib/pkgconfig:$root/usr/share/pkgconfig"
    cmake_extra=(-DCMAKE_PREFIX_PATH="$root/usr" -DOpenGL_GL_PREFERENCE=LEGACY
                 "-DCMAKE_EXE_LINKER_FLAGS=$(find "$lib" -type d -printf '-Wl,-rpath-link,%p ')")
}
fetch_local_deps() {
    command -v apt-get >/dev/null && command -v dpkg >/dev/null || { say "--local-deps needs apt-get and dpkg (Debian / Ubuntu)."; exit 1; }
    say "Downloading the development packages into $deps (no root needed)..."
    mkdir -p "$deps/debs" "$deps/root"
    local wanted="libsdl2-dev libgl-dev libavformat-dev libavcodec-dev libavutil-dev libswresample-dev"
    apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces --no-enhances $wanted 2>/dev/null |
        grep '^\w' | sort -u > "$deps/all.txt"
    dpkg-query -W -f='${Package}\n' | sort -u > "$deps/installed.txt"
    comm -23 "$deps/all.txt" "$deps/installed.txt" | grep -v ':i386' > "$deps/needed.txt" || true
    (cd "$deps/debs" && apt-get download $(cat "$deps/needed.txt") >/dev/null)
    for deb in "$deps/debs"/*.deb; do dpkg -x "$deb" "$deps/root"; done
    # Symlinks to libraries the system already has (installed packages were not downloaded) point at the system copy.
    local lib="$deps/root/usr/lib/x86_64-linux-gnu"
    find "$lib" -maxdepth 1 -xtype l | while read -r link; do
        target="/usr/lib/x86_64-linux-gnu/$(basename "$(readlink "$link")")"
        [ -e "$target" ] && ln -sf "$target" "$link"
    done
    touch "$deps/ready"
}

cmake_extra=()
if [ $local_deps -eq 1 ] || [ -f "$deps/ready" ]; then
    [ -f "$deps/ready" ] || fetch_local_deps
    use_local_deps
elif ! pkg-config --exists $modules 2>/dev/null; then
    say "Missing development packages: $(for m in $modules; do pkg-config --exists "$m" 2>/dev/null || printf '%s ' "$m"; done)"
    say "Install them, for example:"
    install_hint
    if command -v apt-get >/dev/null && command -v dpkg >/dev/null; then
        say "Without root they can be unpacked locally instead; doing that now (or run: $0 --local-deps)."
        fetch_local_deps
        use_local_deps
    else
        exit 1
    fi
fi

# Building on a Windows drive (WSL's /mnt/c) is slow: the build folder goes to the Linux home folder then.
case "$here" in
    /mnt/*) build="${XDG_CACHE_HOME:-$HOME/.cache}/forlorn-hle/build" ;;
    *) build="$here/build-linux" ;;
esac
say "Configuring..."
cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release "${cmake_extra[@]}" >/dev/null
say "Compiling (this takes a few minutes the first time)..."
cmake --build "$build" -j"$(nproc 2>/dev/null || echo 4)"
cp "$build/ForlornHLE" "$out/ForlornHLE"
chmod +x "$out/ForlornHLE"
say ""
say "Done: ForlornHLE was rebuilt. Start the game with ./play_linux.sh"
