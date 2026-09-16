# GstPlaneCameraSystem

Direct rendering-camera streaming for PX4 SITL with **Gazebo Harmonic and Gazebo Jetty**. One Gazebo Sim adapter discovers configured cameras, copies RGB frames from `Camera::Copy()`, and passes them to one reusable GStreamer engine. Each camera has its own RTP/H.264 UDP stream, frame rate, bitrate, encoder choice, and retry state.

Gazebo Transport image subscriptions are not part of the frame path. PX4 is not a build dependency and this repository does not modify PX4 files.

## Compatibility and layout

| Build selection | Simulator | Rendering | Plugin registration |
| --- | --- | --- | --- |
| `-DGZ_DISTRO=harmonic` | `gz-sim8` | `gz-rendering8` | `gz-plugin2` |
| `-DGZ_DISTRO=jetty` | `gz-sim10` | `gz-rendering10` | `gz-plugin4` |

Both builds export `custom::GstPlaneCameraSystem` in `libGstPlaneCameraSystem.so`. They are **separate binaries**: a Harmonic binary links against `gz-sim8` and must never be loaded into Jetty; a Jetty binary must never be loaded into Harmonic. Select one distribution per environment, build directory, plugin search path, and installation prefix. CMake rejects unknown distributions and changing `GZ_DISTRO` in an existing cache.

```text
cmake/GazeboBackend.cmake            distribution/package/target selection
include/gst_plane_camera/core/       simulator-independent configuration and frame API
include/gst_plane_camera/adapters/   Gazebo Sim adapter declarations
src/core/                           shared GStreamer engine
src/adapters/gzsim/                  one adapter compiled for both distributions
examples/harmonic/                  standalone world, model and server.config
examples/jetty/                     equivalent Jetty examples
tests/core/                         tests without Gazebo
tests/cmake/                        backend/cache selection checks
tests/smoke/                        linkage and rendering smoke checks
tests/integration/                  camera removal and respawn checks
```

The original `GstPlaneCameraSystem.cpp/.hpp` responsibilities are split between the adapter and core. Gazebo lifecycle, entity discovery/removal, render events, scene and camera lookup, SDF parsing, and simulation-time frame scheduling stay in the adapter. GStreamer initialization, buffer ownership, appsrc, RGB conversion, x264/NVENC selection, timestamps, UDP output, errors/EOS, retries, workers, and shutdown belong to the core. Harmonic and Jetty use the same camera, event, and registration APIs; no empty compatibility layer or copied adapter is needed.

## Dependencies

Install the development packages for **one** Gazebo distribution: [Harmonic installation](https://gazebosim.org/docs/harmonic/install_ubuntu/) or [Jetty installation](https://gazebosim.org/docs/jetty/install_ubuntu/). On Ubuntu 24.04 the corresponding collections are `gz-harmonic` and `gz-jetty`. Jetty's installed CMake configuration may use unversioned package and imported target names; CMake resolves the exported targets and verifies their major versions.

Common build dependencies:

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
```

Runtime software encoding and the optional inspection/viewer commands:

```sh
sudo apt install -y gstreamer1.0-tools \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav

gst-inspect-1.0 x264enc
gst-inspect-1.0 nvh264enc  # Optional NVIDIA encoder
```

The GStreamer command-line tools are not required to compile the library. No CUDA SDK or GPU is required to build it or run the normal core tests.

## Build

Run from the repository root. Choose the command matching the installed simulator.

Harmonic:

```sh
cmake -S . -B build-harmonic \
  -DGZ_DISTRO=harmonic \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-harmonic -j"$(nproc)"
ctest --test-dir build-harmonic --output-on-failure
```

Jetty:

```sh
cmake -S . -B build-jetty \
  -DGZ_DISTRO=jetty \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-jetty -j"$(nproc)"
ctest --test-dir build-jetty --output-on-failure
```

Outputs are `build-harmonic/libGstPlaneCameraSystem.so` and `build-jetty/libGstPlaneCameraSystem.so`. An omitted `GZ_DISTRO` defaults to Harmonic for existing build commands. Never reuse a build cache between distributions.

Inspect linkage in the matching environment:

```sh
ldd build-harmonic/libGstPlaneCameraSystem.so | grep gz
ldd build-jetty/libGstPlaneCameraSystem.so | grep gz
```

The full `ldd` output must contain no `not found` entries. Harmonic must not link `gz-sim10`; Jetty must not link `gz-sim8`. The CTest `plugin.linkage` check enforces these checks automatically.

## Standalone examples

Each world is self-contained, with Physics, UserCommands, Sensors using Ogre2, the stream system, a ground plane, a visible target, and two cameras. No PX4 checkout, downloaded models, or external meshes are needed. `camera_front` sends to port `5606` at 30 FPS / 16384 kbit/s; `camera_down` sends to port `5604` at 15 FPS / 4096 kbit/s.

In a Harmonic terminal:

```sh
export GZ_SIM_SYSTEM_PLUGIN_PATH="$PWD/build-harmonic${GZ_SIM_SYSTEM_PLUGIN_PATH:+:$GZ_SIM_SYSTEM_PLUGIN_PATH}"
gz sim -s -r --headless-rendering examples/harmonic/world.sdf
```

Or, in a separate Jetty terminal:

```sh
export GZ_SIM_SYSTEM_PLUGIN_PATH="$PWD/build-jetty${GZ_SIM_SYSTEM_PLUGIN_PATH:+:$GZ_SIM_SYSTEM_PLUGIN_PATH}"
gz sim -s -r --headless-rendering examples/jetty/world.sdf
```

Use only the matching build path; do not add both directories to `GZ_SIM_SYSTEM_PLUGIN_PATH`. Headless rendering still needs a usable Ogre2 renderer and graphics drivers. For interactive inspection, omit `-s --headless-rendering`.

The standalone worlds embed their systems. The accompanying `server.config` files show equivalent ordering for applications that load systems from a server configuration; do not load the same world system twice.

## PX4 integration

In your PX4 checkout, edit:

```text
src/modules/simulation/gz_bridge/server.config
```

Replace the existing default streamer entry:

```xml
<plugin entity_name="*" entity_type="world"
        filename="libGstCameraSystem.so"
        name="custom::GstCameraSystem"/>
```

with the following, **after the Sensors system**:

```xml
<plugin entity_name="*" entity_type="world"
        filename="gz-sim-sensors-system" name="gz::sim::systems::Sensors">
  <render_engine>ogre2</render_engine>
</plugin>
<plugin entity_name="*" entity_type="world"
        filename="libGstPlaneCameraSystem.so"
        name="custom::GstPlaneCameraSystem"/>
```

Keep the existing Sensors entry and replace only the streamer entry; the expanded example illustrates their order. The custom entry is identical for Harmonic and Jetty. Launch PX4 from the terminal with the matching `GZ_SIM_SYSTEM_PLUGIN_PATH` exported, using a PX4 revision/model target that supports your simulator. For example, an existing Harmonic PX4 setup may use `make px4_sitl gz_rc_cessna`.

This plugin's original implementation was tested with PX4 v1.16 and Harmonic. That historical result does not constitute flight testing of this refactor or Jetty integration. The top-level `examples/model.sdf` and `examples/server.config` are retained as the original PX4 examples; the distribution-specific examples are the portable standalone starting points.

## Camera configuration

Add this configuration block to each camera sensor. In modern Gazebo the sensor block supplies configuration to the single world system; each sensor does not own a separate GStreamer system instance.

```xml
<sensor name="camera_front" type="camera">
  <always_on>true</always_on>
  <update_rate>30</update_rate>
  <visualize>false</visualize>
  <camera>
    <horizontal_fov>1.0</horizontal_fov>
    <image><width>1920</width><height>1080</height><format>R8G8B8</format></image>
    <clip><near>0.05</near><far>6000</far></clip>
  </camera>
  <plugin filename="libGstPlaneCameraSystem.so" name="custom::GstPlaneCameraSystem">
    <camera_name>camera_front</camera_name>
    <udp_host>127.0.0.1</udp_host>
    <udp_port>5606</udp_port>
    <rate>30</rate>
    <bitrate_kbps>16384</bitrate_kbps>
    <use_cuda>false</use_cuda>
    <x264_speed_preset>1</x264_speed_preset>
  </plugin>
</sensor>
```

Images must be `R8G8B8`. Unsupported formats are rejected, never reinterpreted as RGB. Add sensors with distinct names and UDP ports for additional streams. Cameras appearing after startup are discovered dynamically; removing a camera stops its pipeline, and respawning it creates a fresh independent stream.

| Parameter | Default | Validation / meaning |
| --- | --- | --- |
| `camera_name` | sensor name | Rendering camera name, optionally scoped |
| `udp_host` | `PX4_VIDEO_HOST_IP`, otherwise `127.0.0.1` | Explicit nonempty SDF host overrides the environment |
| `udp_port` | `5600` | Invalid values outside `1..65535` reset to `5600` |
| `rate` | rounded sensor update rate, otherwise `30` | Clamped to `1..240` FPS |
| `bitrate_kbps` | `16384` | Clamped to `64..200000` kbit/s |
| `use_cuda` | `false` | Attempt `nvh264enc`, with x264 fallback |
| `x264_speed_preset` | `1` | Clamped to `1..10` |

To choose a default remote destination, omit `udp_host` and set:

```sh
export PX4_VIDEO_HOST_IP=192.168.1.100
```

## Encoders, ownership, and lifecycle

`use_cuda=true` attempts NVIDIA `nvh264enc`. If it is absent, cannot initialize, or later reports a fatal error, the affected stream falls back to x264. Other cameras retain their own encoder and pipeline states. Errors/EOS stop the pipeline and schedule a retry after one second; they do not terminate the simulator. NVENC-specific integration checks skip when the encoder or usable hardware is unavailable.

RGB conversion to the encoder input format remains on the CPU. `use_cuda` does not make the renderer-to-appsrc path zero-copy.

Rendering access occurs on Gazebo's rendering callbacks. `Camera::Copy()` fills an adapter-owned image; `StreamPipeline::SubmitFrame()` copies its bytes into a GStreamer-owned buffer before returning. A bounded pending slot retains the newest frame if the worker falls behind. The worker transfers that buffer to appsrc without another full-frame copy and never holds a Gazebo image pointer. Default appsrc timestamps use its running clock, retaining the original wall-clock behavior; simulation time controls the adapter's capture rate. Teardown disconnects callbacks, stops frame submissions, joins stream workers, and releases pipeline resources.

## View a stream

```sh
gst-launch-1.0 -v \
  udpsrc port=5606 caps="application/x-rtp,media=video,encoding-name=H264,payload=96" \
  ! rtph264depay ! h264parse ! avdec_h264 ! videoconvert \
  ! autovideosink sync=false
```

Use port `5604` in another receiver for the second example camera.

## Testing

Core tests can be compiled and run without Gazebo:

```sh
cmake -S . -B build-core -DGST_PLANE_CAMERA_BUILD_PLUGIN=OFF
cmake --build build-core -j"$(nproc)"
ctest --test-dir build-core --output-on-failure
```

They check configuration/defaults, environment overrides, real GStreamer element settings, frame ownership and timestamps, retries after error/EOS, encoder fallback, multiple streams, and concurrent shutdown. No NVIDIA GPU is needed for the required tests.

Enable rendering smoke tests in either configured backend build:

```sh
cmake -S . -B build-jetty -DGZ_DISTRO=jetty -DGST_PLANE_CAMERA_RUNTIME_TESTS=ON
cmake --build build-jetty -j"$(nproc)"
ctest --test-dir build-jetty --output-on-failure
```

For Harmonic, substitute `build-harmonic` and `-DGZ_DISTRO=harmonic`. Runtime smoke tests require actual camera frames reaching appsrc (`APP_SRC_FIRST_FRAME`), not just a successful load. They may report a skip when the renderer is unavailable; a skip is not a runtime pass.

Exercise both streams, removal, rediscovery after respawn, rejection of a monochrome camera while the other stream continues, and clean shutdown:

```sh
python3 tests/integration/dynamic_cameras.py jetty build-jetty
# Or in the Harmonic environment:
python3 tests/integration/dynamic_cameras.py harmonic build-harmonic
```

CI uses isolated Ubuntu 24.04 jobs for Harmonic and Jetty, builds each adapter binary, runs core/linkage checks, and attempts rendering smoke tests. Compile-tested, runtime-smoke-tested, and manually flight-tested are distinct claims; see [validation evidence](docs/VALIDATION.md) for the evidence collected for this refactor and the [refactor report](docs/REFACTOR.md) for the full file layout and responsibility split.

## Install

Choose one distribution for a given prefix. For example:

```sh
cmake --install build-jetty --prefix "$HOME/.local"
export GZ_SIM_SYSTEM_PLUGIN_PATH="$HOME/.local/lib${GZ_SIM_SYSTEM_PLUGIN_PATH:+:$GZ_SIM_SYSTEM_PLUGIN_PATH}"
```

The default install directory follows CMake's normal library directory, usually `lib`; if your platform uses `lib64`, adjust the exported path or configure `-DCMAKE_INSTALL_LIBDIR=lib`. Installing a second distribution to the same prefix would overwrite the identically named binary. Use separate prefixes to retain both and expose only the matching prefix to Gazebo.

## License

BSD 3-Clause. Derived from the [PX4-Autopilot Gazebo GStreamer camera plugin](https://github.com/PX4/PX4-Autopilot/tree/v1.16.0/src/modules/simulation/gz_plugins/gstreamer).

Original PX4 implementation: Copyright (c) 2025 PX4 Development Team. Modifications in this repository: Copyright (c) 2026 Alex Chazov. See [LICENSE](LICENSE).
