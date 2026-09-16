# Testing

The test suite separates configuration and GStreamer behavior, Gazebo compilation and linkage, and camera delivery at runtime. A successful configure or build does not prove that a rendering camera delivered a frame. A runtime skip does not count as a runtime pass.

Run the commands below from the repository root after installing the [dependencies](../README.md#dependencies). Use a separate build directory and environment for each Gazebo distribution.

## Core tests without Gazebo

```bash
cmake -S . -B build-core \
  -DGST_PLANE_CAMERA_BUILD_PLUGIN=OFF \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Werror"
cmake --build build-core -j"$(nproc)"
ctest --test-dir build-core --output-on-failure
```

This builds and runs the real GStreamer core without Gazebo headers or libraries. The software tests require the GStreamer runtime elements, including `x264enc`; they do not require an NVIDIA GPU or CUDA SDK.

| Test | Coverage |
| --- | --- |
| `core_config` | Defaults and bounds; sensor-rate rounding and invalid values; camera-name fallback; `PX4_VIDEO_HOST_IP` precedence, including unset versus empty values |
| `core_pipeline` | Real GStreamer element properties; frame validation and ownership; timestamps and duration; independent streams; error/EOS retries; resolution changes; packed RGB row strides; RTP output; missing-NVENC fallback; concurrent submission and shutdown |
| `core_nvenc` | Optional `nvh264enc` initialization and frame submission; skips if the factory or usable hardware is unavailable |
| `cmake.backend_selection` | Supported distribution selection, rejection of unknown distributions, and rejection of changing a distribution in an existing build cache |

The software pipeline assertions include live RGB appsrc with nonblocking submission, the one-buffer downstream-leaky queue, I420 conversion, x264 bitrate/preset/zerolatency settings, and RTP/H.264 payload type 96 with MTU 1200. They inspect UDP destination settings and disabled sink synchronization. Buffer probes check that modifying the source pixels after submission does not alter the copied frame, that timestamps advance, and that explicit timestamps are retained.

Error and EOS messages are injected into a real pipeline bus. The tests check that failure is consumed even when no new frame arrives, that retry waits one second, and that a second stream continues independently. A resolution change to 66-pixel-wide RGB exercises a packed 198-byte row, which differs from GStreamer's default aligned stride. A payloader probe observes encoded RTP packets and checks their version, payload type, and size. **There is no UDP receiver in this test**, so it does not verify network reception or playback.

The missing-NVENC fallback test temporarily removes the encoder factory from the process's GStreamer registry, ensuring that the x264 fallback is exercised on machines with or without a GPU. Successful fallback does not establish successful hardware encoding. The [configuration baseline](../tests/core/BASELINE.md) records the behavior these tests preserve.

## Build and linkage tests for each distribution

In a Harmonic environment:

```bash
cmake -S . -B build-harmonic \
  -DGZ_DISTRO=harmonic \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-harmonic -j"$(nproc)"
ctest --test-dir build-harmonic --output-on-failure -E '^plugin\.runtime$'
```

In a Jetty environment:

```bash
cmake -S . -B build-jetty \
  -DGZ_DISTRO=jetty \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-jetty -j"$(nproc)"
ctest --test-dir build-jetty --output-on-failure -E '^plugin\.runtime$'
```

These builds add `plugin.linkage` to the core tests. It checks the complete `ldd` output for unresolved dependencies and requires the selected `gz-sim`, `gz-rendering`, and `gz-plugin` major versions: 8/8/2 for Harmonic and 10/10/4 for Jetty. Dependencies on incompatible Gazebo versions fail the test. This proves compilation and linkage, not rendering or simulator integration.

## Rendering smoke tests

Enable the optional runtime test in the matching build. For Jetty:

```bash
cmake -S . -B build-jetty \
  -DGZ_DISTRO=jetty \
  -DBUILD_TESTING=ON \
  -DGST_PLANE_CAMERA_RUNTIME_TESTS=ON
cmake --build build-jetty -j"$(nproc)"
ctest --test-dir build-jetty --output-on-failure -R '^plugin\.runtime$'
```

For Harmonic, substitute `build-harmonic` and `-DGZ_DISTRO=harmonic`. The underlying scripts can also be run directly, without enabling the CMake option:

```bash
# Run the command matching the active Gazebo environment.
bash tests/smoke/run.sh jetty build-jetty
bash tests/smoke/run.sh harmonic build-harmonic
```

The smoke test runs the distribution's standalone world for 45 seconds. It requires the correct plugin to load, camera discovery, and both `STREAMING` and `APP_SRC_FIRST_FRAME` markers for `camera_front` and `camera_down`. An appsrc marker proves that direct camera acquisition reached GStreamer's appsrc; it does not prove receiver playback. The test writes `runtime-smoke.log` in the build directory.

A usable Ogre2 renderer and graphics drivers are required. The script uses the available `DISPLAY`, or requests headless rendering when no display is set. Plugin loading and ABI errors always fail. Only a recognized renderer-unavailable diagnostic after successful plugin loading can produce exit code 77, which CTest reports as skipped. Missing camera evidence without that diagnostic fails.

## Camera removal, respawn, and format rejection

Run the matching command with Python 3, the selected distribution's `gz` CLI, and a working renderer:

```bash
python3 tests/integration/dynamic_cameras.py jetty build-jetty
# Or, in a Harmonic environment:
python3 tests/integration/dynamic_cameras.py harmonic build-harmonic
```

Add `--headless-rendering` to explicitly request EGL instead of an available X11 display. This integration script is separate from CTest and is not currently run by the GitHub Actions workflow. It checks:

1. Both cameras reach appsrc with independent UDP destinations.
2. Removing the model stops both camera streams.
3. Respawning the model creates fresh camera entities and both streams reach appsrc again.
4. A further respawn changes `camera_front` to `L8`; that unsupported stream is rejected while the RGB `camera_down` stream reaches appsrc.
5. SIGINT shuts down the simulator without forced termination or a nonzero exit.

The output is saved as `dynamic-cameras.log` in the build directory. Unlike the rendering smoke test, this script fails when rendering is unavailable; it has no skip path.

## Interpreting results and CI

CTest records output under `build-<target>/Testing/Temporary/LastTest.log`. Inspect skipped tests as well as the final exit status:

- `core_pipeline` can skip if a required GStreamer runtime factory is missing. Install the missing runtime package and rerun before claiming software pipeline coverage.
- `core_nvenc` can skip if a required factory or usable NVIDIA encoder is unavailable. A skip provides no hardware-encoding evidence.
- `plugin.runtime` can skip for explicit renderer unavailability after the plugin has loaded. Compilation and linkage may still pass, but camera delivery remains unverified for that run.

The [Build and smoke test workflow](../.github/workflows/build.yml) builds the core without Gazebo and uses separate Ubuntu 24.04 containers for Harmonic and Jetty. Each distribution job compiles the plugin, runs the core/linkage tests, attempts a rendering smoke test, and installs to a staging prefix. Its uploaded artifacts include runtime and CTest logs. A green workflow can include skipped tests; inspect the logs for the exact commit when assessing runtime coverage. Current run results are available in [GitHub Actions](https://github.com/Xovium-Tech/px4-gazebo-gstreamer-camera-plugin/actions/workflows/build.yml).

## Recorded local results — 2026-09-16

These results describe the local verification performed on this date. They are separate from the live CI status and do not imply that every feature has been exercised on both distributions.

| Check | Harmonic | Jetty |
| --- | --- | --- |
| Plugin compilation | Passed | Passed |
| Dependency linkage | Passed: 8/8/2, no unresolved libraries | Passed: 10/10/4, no unresolved libraries |
| Default CTest suite | Four passed; optional NVENC test skipped | Four passed; optional NVENC test skipped |
| Direct RGB camera acquisition and appsrc delivery | Passed for both cameras through a native `gz::sim::Server` harness | Passed for both cameras through `gz sim` |
| Repository CLI rendering smoke script | Not run locally on Harmonic | Passed |
| Removal and respawn | Not run on Harmonic | Passed |
| Unsupported `L8` format isolated from the remaining RGB stream | Not run on Harmonic | Passed |
| Installation | Passed; installed library linkage checked | Passed |
| Hardware NVIDIA encoding | Not validated; optional test skipped | Not validated; optional test skipped |
| Manual PX4 flight and receiver playback | Not performed | Not performed |

### Harmonic runtime evidence

The plugin compiled against `gz-sim8` 8.15.0, `gz-rendering8` 8.2.3, and `gz-plugin2` 2.0.4. Its library resolved `libgz-sim8.so.8`, `libgz-rendering8.so.8`, and `libgz-plugin2.so.2`, with no missing dependencies or Jetty simulator dependency.

The native server run loaded the real plugin and copied 320×240 `R8G8B8` images from both rendering cameras. Both independent x264 streams emitted `APP_SRC_FIRST_FRAME`: `camera_front` at 30 FPS to `127.0.0.1:5606`, and `camera_down` at 15 FPS to `127.0.0.1:5604`. The server ran for 20 seconds, stopped cleanly, and returned zero. The local evidence is `build-harmonic/runtime-server.log`, alongside configure/build/linkage logs and CTest output; generated build logs are not committed.

The local Harmonic packages were an isolated extracted installation on a Jetty workstation and did not include the Harmonic CLI. A temporary, uncommitted C++ harness linked to `gz-sim8::gz-sim8` therefore launched `gz::sim::Server` directly. It selected the versioned `gz-rendering8-ogre2` engine, an available X11 display, and the Bullet Featherstone physics plugin because that partial installation lacked the default DART dependency. This is camera runtime evidence from the native server, not a local execution of the repository's Harmonic `gz sim` smoke command. The portable commands above target a complete installation; the temporary harness is not part of the reproducible repository test suite.

### Jetty runtime evidence

The plugin compiled against installed `gz-sim` 10.5.0, `gz-rendering` 10.0.2, and `gz-plugin` 4.0.0. Linkage resolved `libgz-sim.so.10`, `libgz-rendering.so.10`, and `libgz-plugin.so.4`, with no missing dependencies or Harmonic simulator dependency.

Both the 45-second smoke script and the dynamic integration script returned zero. The logs record direct 320×240 `R8G8B8` frames from both cameras, independent 30 FPS / UDP 5606 and 15 FPS / UDP 5604 x264 streams, and both first-frame markers. Removing and respawning the model produced new camera entities and new first-frame markers. Changing `camera_front` to `L8` produced the expected rejection while `camera_down` continued to appsrc. SIGINT exited cleanly. Evidence is in the local `build-jetty/runtime-smoke.log` and `build-jetty/dynamic-cameras.log`, alongside configure/build/linkage, installation, and CTest logs.

Rendering used the available NVIDIA GeForce RTX 4060 and display environment. An earlier forced software EGL attempt failed during renderer initialization and does not count as a runtime pass. Software rendering is not claimed as validated. The optional NVENC factory was present, but usable encoder initialization was unavailable, so the hardware test skipped.

### Core and example evidence

A separate Debug core build with `-Wall -Wextra -Wpedantic -Werror` compiled without Gazebo and passed `core_config`, `core_pipeline`, and `cmake.backend_selection`; `core_nvenc` skipped. The software test observed encoded RTP packets at the payloader output with the UDP sink configured for localhost, but did not run a receiver.

All four distribution-specific `examples/{harmonic,jetty}/{model,world}.sdf` files passed the installed Jetty `gz sdf -k` schema checker. Both distribution-specific `server.config` files passed XML parsing. These are syntax checks, distinct from execution in either simulator.

### Remaining coverage

Harmonic CLI smoke execution, Harmonic camera removal/respawn and format isolation, hardware NVENC encoding, manual PX4 flight, receiver playback, long-duration soak testing, and pixel-quality comparison remain unverified by this local record. New results should identify the tested commit, Gazebo versions, renderer, commands, skipped tests, and available logs.
