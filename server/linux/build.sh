#!/usr/bin/env bash
# Builds the Linux server: ./build.sh  ->  ./mgmp_server (next to this script's parent folder's build output).
# Needs g++ 8+ (or clang++) with C++17; nothing else -- the server uses only the C library, POSIX sockets and the
# bundled third_party/json.hpp.
#     sudo apt install -y g++        # Ubuntu / Debian
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/.."
out="${1:-$here/mgmp_server}"
CXX="${CXX:-g++}"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -I"$src/../third_party" "$src/mgmp_server.cpp" -o "$out"
echo "built $out"
