#!/usr/bin/env bash
# Downloads an H2GIS native library of a GitHub release of H2GIS, checks it
# against the SHA256SUMS of the release, and saves it under the name the
# driver loads.
#
#   download-h2gis.sh <asset> <destination file>
#   download-h2gis.sh h2gis-linux-x86_64.so build/h2gis.so
#
# H2GIS_NATIVE_URL: base URL of the release, e.g.
# https://github.com/orbisgis/H2GIS/releases/download/graalvm-snapshot
set -euo pipefail

asset="$1"
destination="$2"
url="${H2GIS_NATIVE_URL:?H2GIS_NATIVE_URL is not set}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL --retry 3 -o "$tmp/SHA256SUMS" "$url/SHA256SUMS"
curl -fsSL --retry 3 -o "$tmp/$asset" "$url/$asset"

expected=$(awk -v name="$asset" '$2 == name || $2 == "*" name { print $1 }' "$tmp/SHA256SUMS")
if [ -z "$expected" ]; then
  echo "$asset is not in $url/SHA256SUMS" >&2
  exit 1
fi
if command -v sha256sum > /dev/null; then
  actual=$(sha256sum "$tmp/$asset" | awk '{ print $1 }')
else
  actual=$(shasum -a 256 "$tmp/$asset" | awk '{ print $1 }')
fi
if [ "$expected" != "$actual" ]; then
  echo "Checksum of $asset: $actual, expected $expected" >&2
  exit 1
fi

mkdir -p "$(dirname "$destination")"
cp "$tmp/$asset" "$destination"
echo "$asset ($url): $(wc -c < "$destination" | tr -d ' ') bytes, sha256 $actual"
