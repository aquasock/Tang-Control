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

g++ -std=c++17 -Wall -Wextra -Werror \
    -I"$repo_dir/core" \
    "$repo_dir/core/phosphor_ui_model.cpp" \
    "$repo_dir/tests/phosphor_ui_model_test.cpp" \
    -o "$build_dir/phosphor_ui_model_test"

"$build_dir/phosphor_ui_model_test"

g++ -std=c++17 -Wall -Wextra -Werror \
    -I"$repo_dir/core" \
    "$repo_dir/core/phosphor_metadata.cpp" \
    "$repo_dir/tests/phosphor_metadata_test.cpp" \
    -o "$build_dir/phosphor_metadata_test"

"$build_dir/phosphor_metadata_test"
g++ -std=c++17 -Wall -Wextra -Werror \
    -I"$repo_dir/utils" \
    "$repo_dir/tests/fpga_ext_frame_test.cpp" \
    -o "$build_dir/fpga_ext_frame_test"

"$build_dir/fpga_ext_frame_test"
python3 "$repo_dir/tests/tangctl_test.py"
