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

# This is the script to fetch the latest ClassIn .deb package information from EEO and generate a manifest file.
set -euo pipefail

CONF_URL="https://www.eeo.cn/sysshare/custom/download_conf.json"
MANIFEST_TEMPLATE="manifest.json.in"
MANIFEST_OUTPUT="manifest.json"

for cmd in jq wget sha256sum sed curl; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "Error: Required command '$cmd' is not installed." >&2
    exit 1
  fi
done

if [[ ! -f "$MANIFEST_TEMPLATE" ]]; then
  echo "Error: Template file '$MANIFEST_TEMPLATE' not found." >&2
  exit 1
fi

echo "Fetching EEO configuration..."
CONFIG_DATA=$(curl -sSL "$CONF_URL")

URL_AMD64=$(echo "$CONFIG_DATA" | jq -r '.[] | select(.confName=="eeocn_linux_amd64") | .confValue')
VER_AMD64=$(echo "$CONFIG_DATA" | jq -r '.[] | select(.confName=="eeocn_linux_amd64_version") | .confValue')

URL_ARM64=$(echo "$CONFIG_DATA" | jq -r '.[] | select(.confName=="eeocn_linux_arm") | .confValue')
VER_ARM64=$(echo "$CONFIG_DATA" | jq -r '.[] | select(.confName=="eeocn_linux_arm_version") | .confValue')

echo "AMD64 Version: $VER_AMD64"
echo "ARM64 Version: $VER_ARM64"

if [[ -z "$VER_AMD64" || "$VER_AMD64" == "null" ]] || [[ -z "$VER_ARM64" || "$VER_ARM64" == "null" ]]; then
    echo "Error: null version. Exiting..."
    exit 1
fi

# Check if the manifest file needs to be regenerated
NEED_MANIFEST_GEN=true
if [[ -f "$MANIFEST_OUTPUT" ]]; then
  EXISTING_URL_AMD64=$(jq -r '.architectures.x86_64.url' "$MANIFEST_OUTPUT" 2>/dev/null || true)
  EXISTING_URL_ARM64=$(jq -r '.architectures.aarch64.url' "$MANIFEST_OUTPUT" 2>/dev/null || true)

  if [[ "$EXISTING_URL_AMD64" == "$URL_AMD64" && "$EXISTING_URL_ARM64" == "$URL_ARM64" ]]; then
    echo "Manifest $MANIFEST_OUTPUT is already up to date."
    NEED_MANIFEST_GEN=false
  fi
fi

# Generate the manifest file if needed
if [[ "$NEED_MANIFEST_GEN" == true ]]; then
  echo "Generating new $MANIFEST_OUTPUT..."
  rm -f "$MANIFEST_OUTPUT"

  # Process AMD64 package
  DEB_AMD64="tmp_amd64.deb"
  wget -q -O "$DEB_AMD64" "$URL_AMD64"
  SHA256_AMD64=$(sha256sum "$DEB_AMD64" | awk '{print $1}')
  SIZE_AMD64=$(stat -c%s "$DEB_AMD64")
  rm -f "$DEB_AMD64"

  # Process ARM64 package
  DEB_ARM64="tmp_arm64.deb"
  wget -q -O "$DEB_ARM64" "$URL_ARM64"
  SHA256_ARM64=$(sha256sum "$DEB_ARM64" | awk '{print $1}')
  SIZE_ARM64=$(stat -c%s "$DEB_ARM64")
  rm -f "$DEB_ARM64"

  # Generate the manifest file by replacing placeholders in the template
  sed \
    -e "s|@CLASSIN_VERSION_AMD64@|${VER_AMD64}|g" \
    -e "s|@CLASSIN_URL_AMD64@|${URL_AMD64}|g" \
    -e "s|@CLASSIN_SHA256_AMD64@|${SHA256_AMD64}|g" \
    -e "s|\"@CLASSIN_SIZE_AMD64@\"|${SIZE_AMD64}|g" \
    -e "s|@CLASSIN_VERSION_ARM64@|${VER_ARM64}|g" \
    -e "s|@CLASSIN_URL_ARM64@|${URL_ARM64}|g" \
    -e "s|@CLASSIN_SHA256_ARM64@|${SHA256_ARM64}|g" \
    -e "s|\"@CLASSIN_SIZE_ARM64@\"|${SIZE_ARM64}|g" \
    "$MANIFEST_TEMPLATE" > "$MANIFEST_OUTPUT"

  echo "Successfully generated $MANIFEST_OUTPUT!"
fi
