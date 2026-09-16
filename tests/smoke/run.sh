#!/usr/bin/env bash
# Exit 77 only for explicit rendering unavailability, after proving plugin load.
set -euo pipefail
distro=${1:?Usage: run.sh harmonic|jetty build-directory}
build_dir=$(cd "${2:?Missing build directory}" && pwd)
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
case "$distro" in
  harmonic) major=8 ;;
  jetty) major=10 ;;
  *) echo "Unsupported distribution: $distro" >&2; exit 1 ;;
esac
bash "$source_dir/tests/smoke/check_linkage.sh" "$distro" \
  "$build_dir/libGstPlaneCameraSystem.so"
command -v gz >/dev/null
command -v timeout >/dev/null
log_file="$build_dir/runtime-smoke.log"
export GZ_SIM_SYSTEM_PLUGIN_PATH="$build_dir${GZ_SIM_SYSTEM_PLUGIN_PATH:+:$GZ_SIM_SYSTEM_PLUGIN_PATH}"
export GZ_PARTITION="gst-plane-smoke-${distro}-$$"
export GZ_IP=127.0.0.1
export GZ_HOMEDIR="$build_dir/runtime-home"
mkdir -p "$GZ_HOMEDIR"
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-offscreen}
render_args=()
if [[ -z "${DISPLAY:-}" ]]; then
  render_args+=(--headless-rendering)
fi

# The minimal examples contain two independent streams and no downloaded assets.
# Force the simulator version as an additional defense on mixed installations.
status=0
timeout --signal=INT --kill-after=10s 45s \
  gz sim --force-version "$major" -s -r -v 4 "${render_args[@]}" \
  "$source_dir/examples/$distro/world.sdf" >"$log_file" 2>&1 || status=$?
cat "$log_file"

# Loader/ABI failures always fail, even if rendering also failed later.
if grep -Eq 'SystemLoader[.]cc|Failed to load system plugin|Failed to load plugin|Failed to find plugin|Error while loading the library|Found no render engine plugins|could not (find|load).*GstPlaneCamera|undefined symbol|symbol lookup error|error while loading shared libraries' "$log_file"; then
  echo 'FAIL: plugin/system loading error' >&2
  exit 1
fi
if ! grep -q 'active WORLD direct-rendering-camera streamer loaded' "$log_file"; then
  echo 'FAIL: no evidence that GstPlaneCameraSystem loaded' >&2
  exit 1
fi

complete=true
for camera in camera_front camera_down; do
  for event in 'STREAMING' 'APP_SRC_FIRST_FRAME'; do
    if ! grep -Fq "$event [$camera]" "$log_file"; then complete=false; fi
  done
done
if "$complete" && grep -q 'discovered camera entity' "$log_file"; then
  if [[ "$status" != 0 && "$status" != 124 ]]; then
    echo "FAIL: simulator exited abnormally ($status) after receiving frames" >&2
    exit 1
  fi
  echo "PASS: $distro loaded, discovered both cameras, started pipelines, and pushed appsrc frames"
  exit 0
fi

# Deliberately narrow: missing plugins, missing resources, a silent timeout,
# GStreamer failures, and crashes without a renderer diagnostic cannot skip.
if grep -Eiq 'Unable to open display|Could not open display|Unable to create (the )?rendering window|Unable to create a (suitable )?GL(X)? context|EGL_NOT_INITIALIZED|No (suitable )?(EGL|GLX|OpenGL) (display|context)|OGRE EXCEPTION.*(RenderingAPIException|GLX)' "$log_file"; then
  echo "SKIP: $distro plugin loaded, but the renderer is unavailable; see $log_file"
  exit 77
fi
echo "FAIL: missing camera/pipeline/appsrc evidence (simulator exit $status); see $log_file" >&2
exit 1
