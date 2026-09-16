# Refactor report

The supported targets are Gazebo Harmonic and Jetty. Both compile the same modern Gazebo Sim adapter against a simulator-independent GStreamer core. Each distribution produces its own `libGstPlaneCameraSystem.so`, exporting the unchanged `custom::GstPlaneCameraSystem` name.

## Final source layout

```text
CMakeLists.txt
cmake/GazeboBackend.cmake
include/gst_plane_camera/
  core/
    FrameView.hh
    StreamConfig.hh
    StreamPipeline.hh
  adapters/gzsim/GstPlaneCameraSystem.hh
src/
  core/
    StreamConfig.cc
    StreamPipeline.cc
  adapters/gzsim/GstPlaneCameraSystem.cc
examples/
  harmonic/{model.sdf,world.sdf,server.config}
  jetty/{model.sdf,world.sdf,server.config}
  model.sdf                   original PX4 model retained
  server.config               original PX4 configuration retained
tests/
  core/{CMakeLists.txt,BASELINE.md,ConfigTests.cc,PipelineTests.cc}
  cmake/BackendSelection.cmake
  smoke/{check_linkage.sh,run.sh}
  integration/dynamic_cameras.py
.github/workflows/build.yml
docs/{REFACTOR.md,VALIDATION.md}
README.md
```

## Files moved and added

The Gazebo responsibilities formerly in `GstPlaneCameraSystem.cpp` moved to `src/adapters/gzsim/GstPlaneCameraSystem.cc`; its adapter declarations moved from `GstPlaneCameraSystem.hpp` to `include/gst_plane_camera/adapters/gzsim/GstPlaneCameraSystem.hh`. The original top-level source/header were removed after their responsibilities were extracted.

`StreamConfig`, `FrameView`, and `StreamPipeline` headers and implementations are new. The backend CMake module, tests, workflow, distribution-specific examples, and these reports are new. The root `CMakeLists.txt`, `.gitignore`, and README were updated. No PX4 source or configuration file was edited.

No empty `EncoderSelection.hh`, `GzSimCompat.hh`, or other placeholder abstraction was created. Encoder selection is part of the shared pipeline implementation; the verified modern API calls do not require distribution-specific source wrappers.

## Shared core

The core owns GStreamer initialization, normalized stream configuration and defaults, encoder availability/fallback, pipeline construction, RGB conversion, x264 and NVENC settings, RTP payloading and UDP destination settings, appsrc, buffer duration and timestamps, error/EOS handling, delayed restart, worker scheduling, and shutdown. Each stream owns its pipeline, pending buffer, counters, retry deadline, and NVENC failure state.

`FrameView` borrows tightly packed RGB bytes only during `SubmitFrame`. That call validates the frame and copies its bytes into GStreamer-owned storage; the worker transfers the same buffer to appsrc. Explicit video metadata handles RGB row strides without a second frame copy. The core includes no Gazebo or SDF types or headers.

## Harmonic and Jetty adapter

The one adapter retains world-level configuration and post-update lifecycle, dynamic camera discovery, SDF parsing, simulation-time capture scheduling, rendering-thread scene/camera discovery, grouped Render/PostRender calls, direct `Camera::Copy()`, format rejection, camera removal cleanup, and respawn discovery. Configured cameras retain independent settings and pipelines.

Render callbacks use a shared lifetime gate to prevent stale callbacks from accessing a destroyed plugin. Rendering references are released during render teardown or retirement; stream shutdown joins its worker. The supported format remains `R8G8B8`.

The Jetty port changes dependency resolution to the `gz-sim10` / `gz-rendering10` / `gz-plugin4` family. Installed Jetty configurations expose unversioned imported targets in this environment; CMake supports those and versioned alternatives while verifying the requested major versions. The existing adapter API compiled against Jetty without changing its rendering, camera copying, event, or registration APIs. Distribution definitions stay in the build; duplicated source implementations are unnecessary.

## Exact build commands and outputs

Harmonic:

```sh
cmake -S . -B build-harmonic \
  -DGZ_DISTRO=harmonic \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-harmonic -j"$(nproc)"
```

Output: `build-harmonic/libGstPlaneCameraSystem.so`.

Jetty:

```sh
cmake -S . -B build-jetty \
  -DGZ_DISTRO=jetty \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-jetty -j"$(nproc)"
```

Output: `build-jetty/libGstPlaneCameraSystem.so`.

Use a separate build directory and runtime environment for each. Do not put both build directories in `GZ_SIM_SYSTEM_PLUGIN_PATH`, install both binaries to the same prefix, or reuse a cache after changing distributions.

## Validation and limitations

[VALIDATION.md](VALIDATION.md) records the actual compile, `ldd`, appsrc runtime, removal/respawn, core test, and optional hardware results separately. The build and linkage checks are required; a runtime test needs camera frames, and an unavailable renderer may produce a documented skip. Manual PX4 flight and remote receiver playback are separate activities and have not been claimed.

See the [README](../README.md) for dependencies, standalone world commands, exact PX4 `server.config` replacement, camera SDF, stream reception, and local installation.
