#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [[ "$(uname -s)" != Darwin ]]; then
  echo "macOS build only." >&2
  exit 1
fi

version="$(python3 -c "import json; print(json.load(open('buildspec.json'))['version'])")"
name="$(python3 -c "import json; print(json.load(open('buildspec.json'))['name'])")"
obs_ver="$(python3 -c "import json; print(json.load(open('buildspec.json'))['dependencies']['obs-studio']['version'])")"

cmake --preset macos
cmake --build build_macos --config RelWithDebInfo

plugin="build_macos/rundir/RelWithDebInfo/${name}.plugin"
if [[ ! -d "$plugin" ]]; then
  echo "Missing $plugin" >&2
  exit 1
fi

dist="$root/dist"
mkdir -p "$dist"
archive="$dist/${name}-${version}-macos-universal.zip"
rm -f "$archive"
(
  cd "$(dirname "$plugin")"
  ditto -c -k --sequesterRsrc --keepParent "$(basename "$plugin")" "$archive"
)

shasum -a 256 "$archive" | tee "$archive.sha256"
echo ""
echo "Built for OBS ${obs_ver}. Install:"
echo "  unzip ${archive} -d ~/Library/Application\\ Support/obs-studio/plugins/"
echo "Tag and release: git tag v${version} && gh release create v${version} --notes-file RELEASE.md ${archive}"
