# Refactor validation

Validation recorded on 2026-09-16. Evidence is collected separately for each Gazebo distribution. A successful CMake configure is not compilation evidence; loading a system is not evidence that a camera delivered frames.

| Target | Compile and linkage | Camera/appsrc runtime smoke | Removal and respawn | Manual PX4 flight |
| --- | --- | --- | --- | --- |
| Harmonic | Passed | Passed with native Server harness: both cameras reached appsrc | Not performed on Harmonic | Not performed |
| Jetty | Passed | Passed: both cameras reached appsrc | Passed, including invalid-format isolation | Not performed |

## Harmonic

The local machine's default simulator is Jetty. An existing isolated Harmonic package tree at `/tmp/dynamic-terrain-harmonic/root/usr` supplied `gz-sim8` 8.15.0, `gz-rendering8` 8.2.3, and `gz-plugin2` 2.0.4. The same adapter source built against those real headers and libraries:

```sh
cmake -S . -B build-harmonic -DGZ_DISTRO=harmonic \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/tmp/dynamic-terrain-harmonic/root/usr
cmake --build build-harmonic -j2
LD_LIBRARY_PATH=/tmp/dynamic-terrain-harmonic/root/usr/lib/x86_64-linux-gnu \
  ldd build-harmonic/libGstPlaneCameraSystem.so
LD_LIBRARY_PATH=/tmp/dynamic-terrain-harmonic/root/usr/lib/x86_64-linux-gnu \
  ctest --test-dir build-harmonic --output-on-failure
```

The resulting `build-harmonic/libGstPlaneCameraSystem.so` resolves `libgz-sim8.so.8`, `libgz-rendering8.so.8`, and `libgz-plugin2.so.2`, with no missing libraries or `gz-sim10` dependency. Backend selection, linkage, configuration, and software pipeline tests passed; optional NVENC hardware execution skipped. Build and linkage evidence is in `build-harmonic/{configure,build,linkage}.log`; CTest evidence is in `build-harmonic/Testing/Temporary/LastTest.log`.

The extracted package tree does not contain the Harmonic CLI. Runtime validation therefore used a small native C++ harness linked to `gz-sim8::gz-sim8`, calling `gz::sim::Server` on the standalone example world, running for 20 seconds and then calling `Stop()`. It loaded the real plugin, attached both rendering cameras, copied 320×240 `R8G8B8` images, and emitted both `APP_SRC_FIRST_FRAME` markers on independent x264 streams (30 FPS / UDP 5606 and 15 FPS / UDP 5604). The process returned zero; see `build-harmonic/runtime-server.log`.

The harness selected the versioned `gz-rendering8-ogre2` engine and used `DISPLAY=:1` to avoid finding the host's identically named Jetty renderer. Its final configuration selected the available `gz-physics7-bullet-featherstone-plugin`; the partial package tree lacks the default DART dependency. These are test-environment adjustments, not changes to the repository examples or adapter. The normal Harmonic `gz sim` command and the full CI image have not been executed locally, and camera removal/respawn has only been exercised on Jetty.

The executed launcher is `bash /tmp/gst-plane-harmonic-server/run.sh`; its source, CMake file, and adjusted world remain in that temporary directory. It points the library, system-plugin, rendering-plugin, physics-engine, and resource search paths at the extracted Harmonic tree, and launches `/tmp/gst-plane-harmonic-server/build/server /tmp/gst-plane-harmonic-server/world.sdf` under a 35-second watchdog. The final Bullet/Ogre2 run returned zero with no error/failed log entries and clean render-thread shutdown.

Installation to `/tmp/gst-plane-harmonic-install` succeeded, and the installed library passed the linkage check in the Harmonic environment.

## Jetty

Configured and compiled the final shared adapter with installed `gz-sim` 10.5.0, `gz-rendering` 10.0.2, and `gz-plugin` 4.0.0:

```sh
cmake -S . -B build-jetty -DGZ_DISTRO=jetty -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-jetty -j2
ldd build-jetty/libGstPlaneCameraSystem.so
python3 tests/integration/dynamic_cameras.py jetty build-jetty
```

The build produced `build-jetty/libGstPlaneCameraSystem.so`. Linkage resolved without `not found` entries, using `libgz-sim.so.10`, `libgz-rendering.so.10`, and `libgz-plugin.so.4`; no `gz-sim8` was present. See `build-jetty/configure.log`, `build-jetty/build.log`, and `build-jetty/ldd.log` in the local build tree.

The dynamic integration command returned zero and printed `PASS`. `build-jetty/dynamic-cameras.log` records:

- The world system loaded without SystemLoader errors.
- Both cameras attached directly to rendering cameras and supplied 320×240 `R8G8B8` frames through `Camera::Copy()`.
- `camera_front` used 30 FPS and UDP `127.0.0.1:5606`; `camera_down` used 15 FPS and UDP `127.0.0.1:5604`.
- Both x264 pipelines emitted `APP_SRC_FIRST_FRAME`.
- Removing `stream_camera` stopped both streams; respawning it created new camera entities and both emitted `APP_SRC_FIRST_FRAME` again.
- A further respawn changed `camera_front` to `L8`: the adapter reported that it requires `R8G8B8`, rejected that stream, and continued delivering `camera_down` to appsrc.
- SIGINT produced a clean simulator exit, without forced termination.

Rendering worked with the available NVIDIA GeForce RTX 4060 / display environment. An initial forced software EGL run failed in renderer initialization before it could validate camera delivery; that attempt is not counted as a runtime pass. Software rendering is therefore not claimed as validated here.

The integrated command `ctest --test-dir build-jetty --output-on-failure` returned zero: `cmake.backend_selection`, `plugin.linkage`, `core_config`, and `core_pipeline` passed; optional `core_nvenc` skipped because the encoder factory was present but usable NVENC initialization was unavailable.

The separate CI-style command `bash tests/smoke/run.sh jetty build-jetty` completed with exit zero after its 45-second run and confirmed both cameras, pipelines, and appsrc frames. The summary is in `build-jetty/smoke-result.log` and raw simulator output in `build-jetty/runtime-smoke.log`.

Installing with `cmake --install build-jetty --prefix /tmp/gst-plane-camera-install-jetty` succeeded and produced `/tmp/gst-plane-camera-install-jetty/lib/libGstPlaneCameraSystem.so`; see `build-jetty/install.log`.

## Shared core tests

A separate build at `/tmp/gst-plane-core-verified`, configured with `GST_PLANE_CAMERA_BUILD_PLUGIN=OFF` and warnings treated as errors, compiled and ran without linking Gazebo. Three tests passed and the optional NVENC hardware test skipped. The core tests cover normalized defaults and ranges, `PX4_VIDEO_HOST_IP` precedence, real pipeline element settings, ownership of copied frames, timestamps and duration, independent streams, resolution changes, error/EOS retries, fallback logic, and concurrent stop/submission.

```sh
cmake -S . -B /tmp/gst-plane-core-verified \
  -DGST_PLANE_CAMERA_BUILD_PLUGIN=OFF -DGZ_DISTRO=jetty \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Werror"
cmake --build /tmp/gst-plane-core-verified -j2
ctest --test-dir /tmp/gst-plane-core-verified --output-on-failure
```

The software pipeline test observed encoded RTP packets at the payloader output and checked their RTP version, payload type and size, with the UDP sink configured for localhost. It did not run a UDP receiver. It also exercised tightly packed RGB rows at 66-pixel width, which are not aligned to the default four-byte video stride. The hardware NVENC skip does not invalidate the tested software and fallback paths, and it is not claimed as successful NVIDIA encoding.

## Example validation

All four new `examples/{harmonic,jetty}/{model,world}.sdf` files passed the installed Jetty `gz sdf -k` schema checker. Both `server.config` files passed XML parsing. This is syntax evidence, separate from each simulator's runtime checks.

## Limits

No manual PX4 flight, remote receiver, long-duration soak, or pixel-quality comparison has been performed. A first-frame appsrc marker proves direct camera acquisition and appsrc acceptance, not end-to-end receiver playback. Hardware NVIDIA encoding was not validated; its optional test skipped. Dynamic removal/respawn and monochrome-format isolation have been validated on Jetty only. The GitHub Actions workflow was added but has not been run by GitHub in this workspace.
