#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
version=0.4.0-linux-dev.2
runtime=${1:?Usage: package.sh PATH_TO_NATIVE_PLUGINHOST}
[ -f "$runtime" ]
"$repo/tools/linux/build.sh"
stage=$repo/dist/tpf2-bigmap-$version
mkdir -p "$stage/plugins" "$stage/runtime"
cp "$repo/linux/out-soldier/tpf2_bigmap.so" "$repo/linux/tpf2_bigmap.cfg" "$stage/plugins/"
cp "$runtime" "$stage/runtime/tpf2_pluginhost.so"
cp "$repo/tools/linux/install.sh" "$repo/tools/linux/uninstall.sh" "$repo/tools/linux/tpf2-bigmap-launch" "$stage/"
cp "$repo/linux/out-soldier/bigmap-density-restore" "$stage/"
cp "$repo/LICENSE" "$stage/"
cp "$repo/docs/linux/INSTALL.md" "$repo/docs/linux/PORT.md" "$stage/"
{
  echo "tpf2-bigmap $version; Steam Linux 35924"
  echo "Source: $(git -C "$repo" rev-parse HEAD)"
  [ -z "$(git -C "$repo" status --porcelain)" ] || echo 'Source has uncommitted changes'
  echo 'Compiler/runtime baseline: Valve soldier SDK 2.0.20260805.254767, GCC 8.3, glibc <= 2.31'
  echo 'Shared host: tpf2-multiplayer native plugin ABI 1; see PORT.md for source provenance.'
  sha256sum "$runtime"
} > "$stage/BUILDINFO"
(cd "$stage" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar -C "$repo/dist" -czf "$repo/dist/tpf2-bigmap-$version.tar.gz" "tpf2-bigmap-$version"
echo "$repo/dist/tpf2-bigmap-$version.tar.gz"
