# Changelog

All notable changes to **obs-soft-zoom** are listed here. Version numbers match [`buildspec.json`](buildspec.json).

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Crop-aware spotlight: zoom samples the filter input (after Crop/Pad). At N× zoom the sample is `1/N` of that image on each axis, centered on the cursor inside the crop. The yellow outline is the same window mapped onto the physical display.
- Yellow outline follows the selected scene source (or the only visible screen capture when nothing is selected). Other zoomed captures still zoom their own input but do not drive the overlay.
- [`scripts/build-and-install-macos.sh`](scripts/build-and-install-macos.sh) builds and copies the plugin into `~/Library/Application Support/obs-studio/plugins/`.
- OBS log lines (`spotlight …`, `soft zoom crop …`, `soft zoom overlay …`) when crop or overlay state changes.

### Fixed

- Crop/Pad above Soft Zoom no longer treated the full monitor as 0–1 for the cursor and outline (box and zoom now stay on the cropped region).
- Video zoom uses uniform UV crop so magnified pixels are not stretched.

### Changed

- Factory defaults: 4× zoom, 250 ms ease, 5 px outline, 50% dim, center anchor, follow mouse on.

## [0.1.0] - 2026-09-29

First public release (macOS universal, OBS Studio 31.1.x).

### Added

- **Soft Zoom** filter for macOS **Screen Capture** sources: cursor-anchored zoom (2x / 4x / 8x) with optional ease animation.
- Optional on-screen **outline** and **dim** on the physical display (not drawn by the GPU filter).
- **Global settings** shared by all Soft Zoom filters (`plugin_config/obs-soft-zoom/soft-zoom.json`).
- **Soft Zoom toggle** frontend hotkey (Settings → Hotkeys): one key zooms or restores every active filter. The binding is restored from the OBS hotkey profile on load.
- Anchor modes: center, upper/lower left/right; optional **follow mouse** while zoomed.
- **Reset all to defaults** on the filter panel. Restores factory global settings and applies them to every instance.
- Local release workflow: [`scripts/release-macos.sh`](scripts/release-macos.sh) (no GitHub Actions).

### Changed

- Presenter overlay is one global AppKit panel.
- Default **outline thickness** is 0.
- Outline-only overlay is a window sized to the frame, not a full-screen layer.

### Fixed

- Spotlight/crop anchor math and overlay sync when the parent source is hidden.
- Quit hang when closing overlay panels from a non-main thread.
- Filter UI flicker while editing globals (`OBS_PROPERTIES_DEFER_UPDATE`).
- Ghost spotlight box after the zoom eases in, and a second box when the mouse moves. The overlay stays hidden until the ease finishes, and each draw clears the previous frame.
- Ad-hoc code signature on the rundir bundle so macOS will `dlopen` the plugin after copy.

### Removed

- GitHub Actions / obs-plugintemplate CI (build and release on your Mac only).

[Unreleased]: https://github.com/benallfree/obs-soft-zoom/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/benallfree/obs-soft-zoom/releases/tag/v0.1.0
