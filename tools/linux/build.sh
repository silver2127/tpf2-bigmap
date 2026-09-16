#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
sdk=${TPF2MP_SDK_ROOT:-$HOME/.cache/tpf2mp/soldier-2.0.20260805.254767/root}
build=$repo/linux/out-soldier
[ -x "$sdk/usr/bin/cmake" ] || { echo "Install the soldier SDK using tpf2-multiplayer/tools/linux/build_native.sh or set TPF2MP_SDK_ROOT." >&2; exit 1; }
mkdir -p "$build"
bwrap --die-with-parent --unshare-pid --unshare-net --ro-bind "$sdk" / --proc /proc --dev /dev --tmpfs /tmp \
  --ro-bind "$repo" /work --bind "$build" /build --chdir /work \
  --setenv PATH /usr/bin:/bin --unsetenv LD_PRELOAD --unsetenv LD_LIBRARY_PATH \
  /bin/bash -ec 'cmake -S linux -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release; cmake --build /build --parallel 4; cd /build; ctest --output-on-failure'
