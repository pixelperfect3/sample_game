#!/bin/bash
# ---------------------------------------------------------------------------
# Build an Android APK for Sample Game (Gradle-free).
#
# Prerequisites:
#   - Android NDK (set ANDROID_NDK or uses default path)
#   - Android SDK (set ANDROID_SDK_ROOT or ANDROID_HOME)
#   - Build tools: aapt2, zipalign, apksigner (sdkmanager 'build-tools;34.0.0')
#   - Platform: android.jar (sdkmanager 'platforms;android-34')
#   - Java 17+
#   - Desktop build must exist (for pre-compiled shader headers)
#
# Usage: ./android/build_apk.sh [--debug] [--install]
# ---------------------------------------------------------------------------

set -euo pipefail

BUILD_TYPE="Release"
INSTALL=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)   BUILD_TYPE="Debug"; shift ;;
        --install) INSTALL=true; shift ;;
        -h|--help) head -12 "$0" | tail -10; exit 0 ;;
        *) echo "ERROR: Unknown option '$1'"; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ABI="arm64-v8a"
ANDROID_NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/26.1.10909125}"
ANDROID_SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
OUTPUT="${PROJECT_ROOT}/build/android/SampleGame.apk"

# ── Validate ────────────────────────────────────────────────────────────────

ERRORS=0

if [ ! -d "$ANDROID_NDK" ]; then
    echo "ERROR: Android NDK not found at $ANDROID_NDK"
    echo "  Set ANDROID_NDK to your NDK installation."
    ERRORS=1
fi

if [ ! -d "$ANDROID_SDK" ]; then
    echo "ERROR: Android SDK not found at $ANDROID_SDK"
    echo "  Set ANDROID_SDK_ROOT to your SDK installation."
    ERRORS=1
fi

# Locate build-tools
BUILD_TOOLS_DIR=""
if [ -d "$ANDROID_SDK/build-tools" ]; then
    BUILD_TOOLS_DIR=$(ls -d "$ANDROID_SDK/build-tools"/*/ 2>/dev/null \
        | sort -V | tail -1 | sed 's:/$::')
fi

AAPT2="${BUILD_TOOLS_DIR:+$BUILD_TOOLS_DIR/aapt2}"
ZIPALIGN="${BUILD_TOOLS_DIR:+$BUILD_TOOLS_DIR/zipalign}"
APKSIGNER="${BUILD_TOOLS_DIR:+$BUILD_TOOLS_DIR/apksigner}"

[ ! -x "$AAPT2" ] && AAPT2=$(command -v aapt2 2>/dev/null || true)
[ ! -x "$ZIPALIGN" ] && ZIPALIGN=$(command -v zipalign 2>/dev/null || true)
[ ! -x "$APKSIGNER" ] && APKSIGNER=$(command -v apksigner 2>/dev/null || true)

for tool in AAPT2 ZIPALIGN APKSIGNER; do
    if [ -z "${!tool}" ]; then
        echo "ERROR: ${tool,,} not found. Install: sdkmanager 'build-tools;34.0.0'"
        ERRORS=1
    fi
done

# Locate android.jar
ANDROID_JAR=""
if [ -f "$ANDROID_SDK/platforms/android-34/android.jar" ]; then
    ANDROID_JAR="$ANDROID_SDK/platforms/android-34/android.jar"
else
    ANDROID_JAR=$(ls "$ANDROID_SDK/platforms"/android-*/android.jar 2>/dev/null \
        | sort -V | tail -1 || true)
fi
[ -z "$ANDROID_JAR" ] && echo "ERROR: android.jar not found" && ERRORS=1

[ "$ERRORS" -ne 0 ] && exit 1

echo "=== Sample Game — APK Build ==="
echo "  ABI:        ${ABI}"
echo "  Build type: ${BUILD_TYPE}"
echo "  NDK:        ${ANDROID_NDK}"
echo ""

# ── Step 1: Build native library ────────────────────────────────────────────

echo "[1/6] Building native library..."
BUILD_DIR="${PROJECT_ROOT}/build/android/${ABI}"

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="${ABI}" \
    -DANDROID_PLATFORM=android-24 \
    -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DSAMA_ANDROID=ON \
    2>&1 | tail -3

# Copy pre-compiled shader headers from the desktop build (shaderc is a
# host tool and cannot run when cross-compiled for ARM).
DESKTOP_SHADERS="${PROJECT_ROOT}/build/_deps/sama-build/include/generated/shaders"
ANDROID_SHADERS="${BUILD_DIR}/_deps/sama-build/include/generated/shaders"
if [ -d "$DESKTOP_SHADERS" ]; then
    mkdir -p "$ANDROID_SHADERS"
    cp "$DESKTOP_SHADERS"/*.bin.h "$ANDROID_SHADERS/"
    echo "  Copied pre-compiled shader headers from desktop build."
else
    echo "WARNING: No desktop shader headers found at $DESKTOP_SHADERS"
    echo "  Build the desktop target first: cmake --build build --target sample_game"
fi

cmake --build "${BUILD_DIR}" --target sample_game \
    -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

SO_PATH="${BUILD_DIR}/libsample_game.so"
if [ ! -f "$SO_PATH" ]; then
    echo "ERROR: Native library not found at ${SO_PATH}"
    exit 1
fi

# ── Step 2: Stage APK contents ──────────────────────────────────────────────

echo "[2/6] Staging APK contents..."
STAGING_DIR="${PROJECT_ROOT}/build/android/apk_staging"
rm -rf "$STAGING_DIR"
mkdir -p "${STAGING_DIR}/lib/${ABI}"

cp "$SO_PATH" "${STAGING_DIR}/lib/${ABI}/libsample_game.so"

# Copy libc++_shared.so from the NDK (required by ANDROID_STL=c++_shared).
LIBCXX="${ANDROID_NDK}/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
if [ -f "$LIBCXX" ]; then
    cp "$LIBCXX" "${STAGING_DIR}/lib/${ABI}/"
fi

# Copy game assets.
if [ -d "${PROJECT_ROOT}/assets" ]; then
    mkdir -p "${STAGING_DIR}/assets"
    cp -r "${PROJECT_ROOT}/assets"/* "${STAGING_DIR}/assets/"
fi
if [ -d "${PROJECT_ROOT}/levels" ]; then
    mkdir -p "${STAGING_DIR}/assets/levels"
    cp -r "${PROJECT_ROOT}/levels"/* "${STAGING_DIR}/assets/levels/"
fi

cp "${PROJECT_ROOT}/android/AndroidManifest.xml" "${STAGING_DIR}/AndroidManifest.xml"

# ── Step 3: Create base APK ────────────────────────────────────────────────

echo "[3/6] Creating base APK..."
BASE_APK="${PROJECT_ROOT}/build/android/base.apk"
"$AAPT2" link -o "$BASE_APK" \
    --manifest "${STAGING_DIR}/AndroidManifest.xml" \
    -I "$ANDROID_JAR"

# ── Step 4: Add native lib + assets ────────────────────────────────────────

echo "[4/6] Adding native library and assets..."
UNSIGNED_APK="${PROJECT_ROOT}/build/android/unsigned.apk"
cp "$BASE_APK" "$UNSIGNED_APK"
(cd "$STAGING_DIR" && zip -r "$UNSIGNED_APK" lib/ assets/ 2>/dev/null || \
 cd "$STAGING_DIR" && zip -r "$UNSIGNED_APK" lib/)

# ── Step 5: Align ──────────────────────────────────────────────────────────

echo "[5/6] Aligning APK..."
ALIGNED_APK="${PROJECT_ROOT}/build/android/aligned.apk"
"$ZIPALIGN" -f 4 "$UNSIGNED_APK" "$ALIGNED_APK"

# ── Step 6: Sign ───────────────────────────────────────────────────────────

echo "[6/6] Signing APK..."
mkdir -p "$(dirname "$OUTPUT")"

DEBUG_KS="$HOME/.android/debug.keystore"
if [ ! -f "$DEBUG_KS" ]; then
    mkdir -p "$HOME/.android"
    keytool -genkeypair -v \
        -keystore "$DEBUG_KS" \
        -storepass android \
        -alias androiddebugkey \
        -keypass android \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US"
fi

"$APKSIGNER" sign \
    --ks "$DEBUG_KS" \
    --ks-pass pass:android \
    --ks-key-alias androiddebugkey \
    --key-pass pass:android \
    --out "$OUTPUT" \
    "$ALIGNED_APK"

# Clean up
rm -f "$BASE_APK" "$UNSIGNED_APK" "$ALIGNED_APK"

echo ""
echo "=== APK built successfully ==="
echo "  Output: ${OUTPUT}"
if [ -f "$OUTPUT" ]; then
    SIZE=$(du -h "$OUTPUT" | cut -f1)
    echo "  Size:   ${SIZE}"
fi

if [ "$INSTALL" = true ]; then
    echo ""
    echo "Installing via adb..."
    adb install -r "$OUTPUT"
fi
