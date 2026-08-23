#!/usr/bin/env bash

# Copyright (C) 2026 imngkhang
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 3 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.fsf.org/licenses/>.

set -euo pipefail

ARCH="x86_64"
RAW_REPO="${GITHUB_REPOSITORY:-$(git config --get remote.origin.url | sed -E 's|.*github\.com[:/ ]||; s|\.git$||; s|/*$||')}"
REPO=$(echo "$RAW_REPO" | sed 's|\/|\||' | sed -E 's|^[^a-zA-Z0-9]+||')
MANIFEST="manifest.json"
DIST_DIR="dist"
APPDIR="${DIST_DIR}/${ARCH}.AppDir"
APPIMAGETOOL="./appimagetool"
COMPRESSLEVEL="20"

echo "=== Building AppImage for ${ARCH} ==="

# Check for required files and commands
if [[ ! -f "$MANIFEST" ]]; then
  echo "Error: $MANIFEST not found." >&2
  exit 1
fi

for cmd in jq wget ar tar sha256sum stat; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "Error: Required command '$cmd' is not installed." >&2
    exit 1
  fi
done

# Extract information from manifest
URL=$(jq -r '.architectures.x86_64.url' "$MANIFEST")
EXPECTED_SHA256=$(jq -r '.architectures.x86_64.sha256' "$MANIFEST")
EXPECTED_SIZE=$(jq -r '.architectures.x86_64.size' "$MANIFEST")
VERSION=$(jq -r '.architectures.x86_64.version' "$MANIFEST")

echo "Version: ${VERSION}"
echo "URL: ${URL}"

# Download appimagetool
if [[ ! -f "$APPIMAGETOOL" ]]; then
  echo "Downloading appimagetool..."
  wget -q https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage -O "$APPIMAGETOOL"
  chmod +x "$APPIMAGETOOL"
fi

# Create a temporary directory
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# Download the .deb package
DEB_FILE="${TMP_DIR}/classin.deb"
echo "Downloading .deb package..."
wget -q --show-progress -O "$DEB_FILE" "$URL"

# Verify the downloaded file
DOWNLOADED_SIZE=$(stat -c%s "$DEB_FILE")
if [[ "$DOWNLOADED_SIZE" != "$EXPECTED_SIZE" ]]; then
  echo "Error: File size mismatch." >&2
  echo "Expected: $EXPECTED_SIZE bytes" >&2
  echo "Got:      $DOWNLOADED_SIZE bytes" >&2
  exit 1
fi

DOWNLOADED_SHA256=$(sha256sum "$DEB_FILE" | awk '{print $1}')
if [[ "$DOWNLOADED_SHA256" != "$EXPECTED_SHA256" ]]; then
  echo "Error: SHA256 mismatch." >&2
  echo "Expected: $EXPECTED_SHA256" >&2
  echo "Got:      $DOWNLOADED_SHA256" >&2
  exit 1
fi

# Process the package
echo "Extracting package..."
cd "$TMP_DIR"
ar x classin.deb

if [ -f data.tar.xz ]; then
    tar -xf data.tar.xz 
elif [ -f data.tar.zst ]; then
    tar --zstd -xf data.tar.zst 
elif [ -f data.tar.gz ]; then
    tar -xf data.tar.gz 
else
    echo "Error: Unsupported or missing data.tar." >&2
    exit 1
fi

rm -f control.tar.* data.tar.* debian-binary classin.deb
cd - >/dev/null

# Setup and build the AppImage
echo "Setting up $APPDIR..."
mkdir -p "$DIST_DIR"
rm -rf "$APPDIR"
mkdir -p "$APPDIR"

cp -r "${TMP_DIR}/opt/apps/classin/"* "$APPDIR/"

mkdir -p "$APPDIR/usr/share/mime/packages"
if [[ -f "${TMP_DIR}/usr/share/mime/packages/eeo-edb-mime.xml" ]]; then
  cp "${TMP_DIR}/usr/share/mime/packages/eeo-edb-mime.xml" "$APPDIR/usr/share/mime/packages/"
fi

if [[ -f "${TMP_DIR}/usr/share/icons/hicolor/scalable/apps/classin.svg" ]]; then
  cp "${TMP_DIR}/usr/share/icons/hicolor/scalable/apps/classin.svg" "$APPDIR/classin.svg"
fi

if [[ -f "classin.desktop" ]]; then
  cp "classin.desktop" "$APPDIR/classin.desktop"
  if grep -q "^X-AppImage-Version=" "$APPDIR/classin.desktop"; then
    sed -i "s/^X-AppImage-Version=.*/X-AppImage-Version=${VERSION}/" "$APPDIR/classin.desktop"
  else
    echo "X-AppImage-Version=${VERSION}" >> "$APPDIR/classin.desktop"
  fi
else
  echo "Error: Desktop file not found in current directory." >&2
  exit 1
fi

UPDATE_INFO="gh-releases-zsync|${REPO}|continuous|*-${ARCH}.AppImage.zsync"

OUTPUT_NAME="${DIST_DIR}/ClassIn-${VERSION}-${ARCH}.AppImage"
echo "Packaging $OUTPUT_NAME..."
(
 ARCH="$ARCH" "$APPIMAGETOOL" -u "${UPDATE_INFO}" --comp zstd --mksquashfs-opt -Xcompression-level --mksquashfs-opt "$COMPRESSLEVEL" "$APPDIR" "$OUTPUT_NAME" && \
 mv *.AppImage.zsync dist/ 2>/dev/null || false
)

chmod -x "$OUTPUT_NAME"

echo "=== Build completed: $OUTPUT_NAME ==="
