# OBS Soft Zoom

macOS OBS filter for display captures: hotkey zoom locked to the cursor, optional on-screen outline and dim (not in the stream).

## Install

Build with CMake (see [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) docs):

```bash
cmake --preset macos
cmake --build build_macos --config RelWithDebInfo
```

Copy `build_macos/rundir/RelWithDebInfo/obs-soft-zoom.plugin` to:

`~/Library/Application Support/obs-studio/plugins/`

Restart OBS.

## Use

1. Add **Soft Zoom** to a macOS **Screen Capture** source (Filters).
2. Set zoom (2x / 4x / 8x), ease, outline thickness, and dim opacity.
3. Bind **Soft Zoom toggle** under Settings → Hotkeys for that source.
4. Press the hotkey to zoom to the cursor; press again to restore.

Turn on **Hide OBS from capture** on the screen capture if the overlay appears in the preview.
