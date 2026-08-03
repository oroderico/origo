#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cmake -S "$repo_root/host" -B "$repo_root/build-host" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$repo_root/build-host" --parallel
exec "$repo_root/build-host/origo-simulator" "$@"
