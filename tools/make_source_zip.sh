#!/usr/bin/env bash
# Rebuild RadekiOSNative.zip, the source snapshot that ships beside CHANGELOG.md.
#
# The working tree is canonical; the archive is a mirror of it (top-level RadekiOSNative/
# directory) and is refreshed after changing the tree:
#
#     tools/make_source_zip.sh
#
# CHANGELOG.md lives next to the archive instead of inside it, and the archive never
# contains itself. Keeping the archive current matters: extracting it over the repository
# is exactly how the CI workflow once lost its APK jobs (batch 6).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/RadekiOSNative.zip"
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT

cd "$root"
mkdir -p "$staging/RadekiOSNative"
mapfile -d '' files < <(git ls-files -z)
for f in "${files[@]}"; do
  case "$f" in
    CHANGELOG.md | RadekiOSNative.zip) continue ;;
  esac
  mkdir -p "$staging/RadekiOSNative/$(dirname "$f")"
  cp -p "$f" "$staging/RadekiOSNative/$f"
done

rm -f "$out"
(cd "$staging" && zip -q -r -X "$out" RadekiOSNative)
test -s "$out"
echo "wrote $out ($(stat -c%s "$out") bytes, $(git ls-files | wc -l) tracked files considered)"
