# OBS Soft Zoom 0.1.0

macOS-only filter for **Screen Capture** sources: global zoom settings, one **Settings → Hotkeys** toggle for all instances, optional on-screen outline and dim (not in the stream).

## Requirements

- macOS 12+
- OBS Studio **31.1.x** (built against libobs from OBS 31.1.1)
- Apple Silicon or Intel (universal binary)

## Install

1. Download `obs-soft-zoom-0.1.0-macos-universal.zip` from this release.
2. Unzip and move `obs-soft-zoom.plugin` into:

   `~/Library/Application Support/obs-studio/plugins/`

3. Restart OBS.
4. Add **Soft Zoom** as a filter on each Screen Capture you want.
5. Bind **Soft Zoom toggle** under **Settings → Hotkeys**.

Outline and dim stay on your display. The yellow frame is hidden until the zoom ease finishes, then follows the cursor as a single box. **Reset all to defaults** restores factory settings for every Soft Zoom filter.

See [README.md](README.md) for usage.

## Build from source

```bash
./scripts/release-macos.sh
```

Releases are built and published locally (no CI).
