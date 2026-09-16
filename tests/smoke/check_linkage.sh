#!/usr/bin/env bash
set -euo pipefail

distro=${1:?Usage: check_linkage.sh harmonic|jetty path/to/libGstPlaneCameraSystem.so}
library=${2:?Missing plugin library}
case "$distro" in
  harmonic) sim_major=8; plugin_major=2 ;;
  jetty) sim_major=10; plugin_major=4 ;;
  *) echo "Unsupported distribution: $distro" >&2; exit 1 ;;
esac
test -f "$library"
linkage=$(ldd "$library")
printf '%s\n' "$linkage"
if grep -q 'not found' <<< "$linkage"; then
  echo 'Unresolved plugin dependency' >&2
  exit 1
fi
for component in sim rendering plugin; do
  major=$sim_major
  if [[ "$component" == plugin ]]; then major=$plugin_major; fi
  # Harmonic uses libgz-sim8.so.8; Jetty uses libgz-sim.so.10.
  expected="libgz-${component}(${major})?\\.so\\.${major}([[:space:].]|$)"
  if ! grep -Eq "$expected" <<< "$linkage"; then
    echo "Missing expected gz-$component major $major dependency" >&2
    exit 1
  fi
  if grep -E "libgz-${component}([0-9]+)?\\.so" <<< "$linkage" |
      grep -Ev "$expected"; then
    echo "Plugin links an incompatible gz-$component major" >&2
    exit 1
  fi
done
if grep -Eq 'libgazebo[^[:space:]]*\.so' <<< "$linkage"; then
  echo 'Modern adapter unexpectedly links Gazebo Classic' >&2
  exit 1
fi
echo "PASS: $distro plugin dependencies resolve to the expected Gazebo majors"
