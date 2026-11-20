#!/bin/zsh
# Builds the host and the development modules named in .env, then launches the host against them.
set -eu
root="${0:A:h:h:h}"
if [[ ! -f "$root/.env" ]]; then
  echo "Missing $root/.env: copy .env.example and set VESPERA_MODULES_DIR." >&2
  exit 1
fi
module_source="$(sed -n 's/^VESPERA_MODULES_DIR=//p' "$root/.env" | tail -1)"
if [[ -z "$module_source" ]]; then
  echo "$root/.env does not set VESPERA_MODULES_DIR." >&2
  exit 1
fi
[[ "$module_source" = /* ]] || module_source="$root/$module_source"
module_source="${module_source:A}"
if [[ ! -f "$module_source/CMakeLists.txt" ]]; then
  echo "VESPERA_MODULES_DIR ($module_source) is not a module checkout." >&2
  exit 1
fi
cmake --preset debug -S "$root"
cmake --build "$root/build/debug"
cmake -S "$module_source" -B "$module_source/build" -DVESPERA_SDK_ROOT="$root" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$module_source/build"
zsh "$root/apps/macos/build-app.sh"
export VESPERA_MODULE_PATH="$module_source/build/modules"
exec "$root/build/vespera.app/Contents/MacOS/vespera"
