#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [[ "$(uname -s)" != Darwin ]]; then
  echo "macOS build only." >&2
  exit 1
fi

name="$(python3 -c "import json; print(json.load(open('buildspec.json'))['name'])")"
obs_ver="$(python3 -c "import json; print(json.load(open('buildspec.json'))['dependencies']['obs-studio']['version'])")"

cmake --preset macos
cmake --build build_macos --config RelWithDebInfo

plugin="build_macos/rundir/RelWithDebInfo/${name}.plugin"
if [[ ! -d "$plugin" ]]; then
  plugin="build_macos/RelWithDebInfo/${name}.plugin"
fi
if [[ ! -d "$plugin" ]]; then
  echo "Missing ${name}.plugin under build_macos (rundir or RelWithDebInfo)." >&2
  exit 1
fi

plugin_dst="${HOME}/Library/Application Support/obs-studio/plugins"
mkdir -p "$plugin_dst"
rm -rf "${plugin_dst}/${name}.plugin"
cp -R "$plugin" "$plugin_dst/"

bin="${plugin_dst}/${name}.plugin/Contents/MacOS/${name}"
echo ""
echo "Installed for OBS ${obs_ver}:"
ls -la "$bin"
echo ""
echo "Fully quit and reopen OBS to load the new plugin."
