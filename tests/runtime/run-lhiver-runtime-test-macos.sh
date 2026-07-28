#!/bin/bash

set -euo pipefail

usage() {
  echo "usage: $0 <enemy-heal-combat|orc-behind-detection|npc-sleep-placement> <gothic-root> [artifact-dir]" >&2
  exit 2
}

if [[ $# -lt 2 || $# -gt 3 ]]; then
  usage
fi

test_name=$1
game_root=$2
case "$test_name" in
  enemy-heal-combat|orc-behind-detection|npc-sleep-placement) ;;
  *) usage ;;
esac

script_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
artifact_dir=${3:-"$script_dir/artifacts/$(date +%F)-lhiver-uriziel"}
build_dir=${OPENGOTHIC_RUNTIME_BUILD_DIR:-"$repo_root/build-runtime-tests"}
binary=${OPENGOTHIC_BINARY:-"$build_dir/opengothic/Gothic2Notr"}
mod_file=${OPENGOTHIC_MOD:-Buddygoths_LhiverUriziel.ini}
default_save_slot=5
if [[ $test_name == npc-sleep-placement ]]; then
  default_save_slot=0
fi
save_slot=${OPENGOTHIC_SAVE:-$default_save_slot}
record_video=${OPENGOTHIC_RECORD:-1}
ready_timeout=${OPENGOTHIC_READY_TIMEOUT:-1200}
record_duration=${OPENGOTHIC_RECORD_DURATION:-}
if [[ -z $record_duration ]]; then
  if [[ $test_name == orc-behind-detection ]]; then
    record_duration=35
  elif [[ $test_name == npc-sleep-placement ]]; then
    record_duration=12
  else
    record_duration=50
  fi
fi

mkdir -p "$artifact_dir"
artifact_dir=$(cd "$artifact_dir" && pwd)
stem="$test_name-lhiver-uriziel"
result_file="$artifact_dir/$stem.json"
log_file="$artifact_dir/$stem.log"
video_file="$artifact_dir/$stem.mov"
preview_file="$artifact_dir/$stem.png"
preview_time=9
if [[ $test_name == enemy-heal-combat ]]; then
  preview_time=13
fi
tmp_dir=$(mktemp -d)
capture_pid=
game_pid=

cleanup() {
  if [[ -n ${capture_pid:-} ]]; then
    kill -INT "$capture_pid" >/dev/null 2>&1 || true
    wait "$capture_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n ${game_pid:-} ]]; then
    kill -TERM "$game_pid" >/dev/null 2>&1 || true
    wait "$game_pid" >/dev/null 2>&1 || true
  fi
  rm -rf "$tmp_dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

cmake_args=(
  -S "$repo_root"
  -B "$build_dir"
  -DOPENGOTHIC_RUNTIME_TESTS=ON
)
if [[ $(uname -s) == Darwin ]]; then
  macos_sdk=${OPENGOTHIC_OSX_SYSROOT:-$(xcrun --sdk macosx --show-sdk-path)}
  cmake_args+=("-DCMAKE_OSX_SYSROOT=$macos_sdk")
  if [[ -d "$macos_sdk/usr/include/c++/v1" ]]; then
    libcxx_include="-I$macos_sdk/usr/include/c++/v1"
    cmake_args+=(
      "-DCMAKE_CXX_FLAGS=$libcxx_include"
      "-DCMAKE_OBJCXX_FLAGS=$libcxx_include"
    )
  fi
fi
cmake "${cmake_args[@]}"
cmake --build "$build_dir" -j "${OPENGOTHIC_BUILD_JOBS:-8}"

window_id_helper="$tmp_dir/window-id"
clang -fobjc-arc -framework Foundation -framework CoreGraphics \
  "$script_dir/window-id.m" -o "$window_id_helper"

rm -f "$result_file"
(
  cd "$game_root"
  exec "$binary" \
    -g "$game_root" \
    "-game:$mod_file" \
    -save "$save_slot" \
    -window \
    -runtime-test "$test_name" \
    -runtime-test-output "$result_file"
) >"$log_file" 2>&1 &
game_pid=$!

window_id=
for _ in $(seq 1 300); do
  window_id=$("$window_id_helper" "Gothic II" "$game_pid" 2>/dev/null || true)
  if [[ -n $window_id ]]; then
    break
  fi
  sleep 0.1
done

if [[ -z $window_id ]]; then
  echo "OpenGothic window did not appear" >&2
  kill -TERM "$game_pid" >/dev/null 2>&1 || true
  wait "$game_pid" || true
  game_pid=
  exit 1
fi

runtime_ready=0
for ((attempt=0; attempt<ready_timeout*10; ++attempt)); do
  if grep -Fq "[RUNTIME_TEST] world-ready" "$log_file"; then
    runtime_ready=1
    break
  fi
  if ! kill -0 "$game_pid" >/dev/null 2>&1; then
    set +e
    wait "$game_pid"
    game_status=$?
    set -e
    game_pid=
    echo "OpenGothic exited before the runtime test became ready (status $game_status)" >&2
    exit "$game_status"
  fi
  sleep 0.1
done

if [[ $runtime_ready -ne 1 ]]; then
  echo "Runtime test did not become ready within $ready_timeout seconds" >&2
  kill -TERM "$game_pid" >/dev/null 2>&1 || true
  wait "$game_pid" || true
  game_pid=
  exit 1
fi

if [[ $record_video == 1 ]]; then
  # Bind recording to the exact PID-matched game window. Display capture can leak unrelated
  # applications if focus changes while an unattended test is running. A fixed duration lets
  # macOS finalize the movie itself; signal-stopping screencapture can leave no output file.
  screencapture -v "-V$record_duration" "-l$window_id" "$tmp_dir/raw.mov" &
  capture_pid=$!
fi

set +e
wait "$game_pid"
game_status=$?
set -e
game_pid=

if [[ -n ${capture_pid:-} ]]; then
  set +e
  wait "$capture_pid"
  capture_status=$?
  set -e
  capture_pid=
  if [[ $capture_status -ne 0 ]]; then
    echo "screencapture exited with status $capture_status" >&2
    exit "$capture_status"
  fi

  ffmpeg -hide_banner -loglevel error -y \
    -i "$tmp_dir/raw.mov" \
    -vf "scale='min(1920,iw)':-2" \
    -c:v libx264 -preset medium -crf 20 -movflags +faststart \
    "$video_file"
  ffmpeg -hide_banner -loglevel error -y \
    -ss "$preview_time" -i "$video_file" -frames:v 1 "$preview_file"
fi

if [[ $game_status -ne 0 ]]; then
  echo "OpenGothic exited with status $game_status" >&2
  exit "$game_status"
fi

python3 "$script_dir/verify-results.py" "$result_file"
echo "artifacts: $artifact_dir"
