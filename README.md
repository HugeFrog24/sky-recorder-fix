# Sky Recorder Fix

A standalone arm64 Canvas mod that fixes repeating frames in Sky's in-game recordings.

Sky reuses Vulkan capture images before delayed recording callbacks read them. The bug also affects vanilla Sky.

The mod waits for the captured frame's GPU work, saves its pixels before image reuse, and supplies the saved frame to the original callback. Native functions are located at runtime; unsupported builds are rejected.

## Usage

1. Download `libsky_recorder_fix.so` from [Releases](https://github.com/HugeFrog24/sky-recorder-fix/releases).
2. In Canvas, open **Mod Manager > Add mod**, select the file, and enable it.
3. Press **Play** and record with the in-game recorder as usual.

The fix runs automatically, with no menu or configuration. On supported Vulkan builds, recordings should stop jumping backward or repeating frames. Unsupported builds are left unchanged. To remove the fix, uninstall the mod before launching Sky again.

## Build

Build: `make build` (requires CMake, Ninja and the Android NDK). Output: `build/release/libsky_recorder_fix.so`. Lint: `make lint`.
