#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT

g++ -std=c++17 -Wall -Wextra -Werror \
    -I"$repo_dir/core" \
    "$repo_dir/core/m3u_playlist.cpp" \
    "$repo_dir/tests/m3u_playlist_test.cpp" \
    -o "$build_dir/m3u_playlist_test"

"$build_dir/m3u_playlist_test" "$repo_dir/tests/fixtures/vlc-repeat.m3u8"
