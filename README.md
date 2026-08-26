# GstPlaneCameraSystem

GStreamer camera streaming for PX4 SITL with Gazebo Harmonic.

The plugin reads frames directly from Gazebo's rendering camera and sends them as RTP/H.264 over UDP. By bypassing the `gz.msgs.Image` and Gazebo Transport path used by the default PX4 Harmonic GStreamer plugin, it avoids unnecessary frame handling and copying, reducing overhead in the camera-to-stream path and improving streaming performance.

Unlike the default PX4 Harmonic GStreamer plugin, which initializes a single camera stream per plugin instance, `GstPlaneCameraSystem` can stream multiple cameras at the same time. Several cameras can be mounted on the same aircraft, with independent UDP ports, frame rates, bitrates, and encoder settings.

Features:

* multiple cameras on the same aircraft
* independent stream settings and UDP ports for each camera
* direct access to Gazebo rendering cameras
* reduced overhead compared with the Gazebo Transport image path
* x264 software encoding
* optional NVIDIA `nvh264enc`
* dynamic camera discovery and cleanup
* automatic pipeline restart after GStreamer errors

Tested with PX4 v1.16 and Gazebo Harmonic.

## Multiple camera streams

`GstPlaneCameraSystem` can stream multiple cameras from the same aircraft simultaneously. Each camera has its own UDP port and streaming configuration.

For example:

```
camera_down  -> UDP 5604
camera_front -> UDP 5606
```

<p align="center">
  <img src="images/two-camera-streams.png" width="1000" alt="Two simultaneous camera streams in PX4 Gazebo">
</p>

<p align="center">
  <em>Two camera streams running simultaneously from the same <code>rc_cessna</code> aircraft: one forward-facing and one downward-facing.</em>
</p>

## Requirements

Install the GStreamer packages:

```
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav
```

The build also needs `gz-sim8`, `gz-rendering8`, and `gz-plugin2`.

If PX4 v1.16 SITL with Gazebo Harmonic already builds on your machine, these are usually installed already.

Check available encoders:

```
gst-inspect-1.0 x264enc
gst-inspect-1.0 nvh264enc
```

`nvh264enc` is optional.

## Build

```
git clone https://github.com/syvixi/GstPlaneCameraSystem.git
cd GstPlaneCameraSystem

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The library will be created at `build/libGstPlaneCameraSystem.so`.

Add the build directory to Gazebo's plugin path:

```
export GZ_SIM_SYSTEM_PLUGIN_PATH="$PWD/build:${GZ_SIM_SYSTEM_PLUGIN_PATH:-}"
```

Launch PX4 from the same terminal.

## PX4 setup

Open:

`<PX4_PATH>/src/modules/simulation/gz_bridge/server.config`

Replace the default PX4 GStreamer plugin:

```
<plugin entity_name="*"
        entity_type="world"
        filename="libGstCameraSystem.so"
        name="custom::GstCameraSystem"/>
```

with:

```
<plugin entity_name="*"
        entity_type="world"
        filename="libGstPlaneCameraSystem.so"
        name="custom::GstPlaneCameraSystem"/>
```

The plugin must be loaded after the Gazebo Sensors system:

```
<plugin entity_name="*"
        entity_type="world"
        filename="gz-sim-sensors-system"
        name="gz::sim::systems::Sensors">
  <render_engine>ogre2</render_engine>
</plugin>

<plugin entity_name="*"
        entity_type="world"
        filename="libGstPlaneCameraSystem.so"
        name="custom::GstPlaneCameraSystem"/>
```

See `examples/server.config` for a complete example.

Do not keep both `GstCameraSystem` and `GstPlaneCameraSystem` enabled unless you intentionally want both streamers running.

## Camera setup

Add a `GstPlaneCameraSystem` block to every camera you want to stream:

```
<sensor name="camera_front" type="camera">
  <pose>0.55 0.0 0.02 0 0 0</pose>

  <camera>
    <horizontal_fov>1.0</horizontal_fov>

    <image>
      <format>R8G8B8</format>
      <width>1920</width>
      <height>1080</height>
    </image>

    <clip>
      <near>0.05</near>
      <far>6000</far>
    </clip>
  </camera>

  <always_on>true</always_on>
  <update_rate>30</update_rate>
  <visualize>false</visualize>

  <plugin filename="libGstPlaneCameraSystem.so"
          name="custom::GstPlaneCameraSystem">
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

The camera image format must be `R8G8B8`.

See `examples/model.sdf` for a complete model example.

For additional cameras, add another camera sensor with a different name and UDP port.

For example:

```
<sensor name="camera_down" type="camera">
  ...
  <plugin filename="libGstPlaneCameraSystem.so"
          name="custom::GstPlaneCameraSystem">
    <camera_name>camera_down</camera_name>
    <udp_host>127.0.0.1</udp_host>
    <udp_port>5604</udp_port>
    <rate>30</rate>
    <bitrate_kbps>16384</bitrate_kbps>
    <use_cuda>false</use_cuda>
    <x264_speed_preset>1</x264_speed_preset>
  </plugin>
</sensor>
```

Each stream should use its own UDP port.

## Start PX4

```
cd <PX4_PATH>
make px4_sitl gz_rc_cessna
```

Replace `gz_rc_cessna` with your PX4 Gazebo model target.

A successful startup should contain messages similar to:

```
[GstPlaneCameraSystem] active WORLD direct-rendering-camera streamer loaded
[GstPlaneCameraSystem] discovered camera entity ...
[GstPlaneCameraSystem] attached DIRECTLY to Gazebo rendering camera ...
[GstPlaneCameraSystem] STREAMING [camera_front] ... -> 127.0.0.1:5606 using x264
[GstPlaneCameraSystem] first appsrc frame pushed [camera_front]
```

## View the stream

For UDP port `5606`:

```
gst-launch-1.0 -v \
  udpsrc port=5606 caps="application/x-rtp,media=video,encoding-name=H264,payload=96" \
  ! rtph264depay \
  ! h264parse \
  ! avdec_h264 \
  ! videoconvert \
  ! autovideosink sync=false
```

To view another camera, run the same pipeline with its UDP port.

For example, for `camera_down` on port `5604`:

```
gst-launch-1.0 -v \
  udpsrc port=5604 caps="application/x-rtp,media=video,encoding-name=H264,payload=96" \
  ! rtph264depay \
  ! h264parse \
  ! avdec_h264 \
  ! videoconvert \
  ! autovideosink sync=false
```

## Streaming to another computer

For local streaming:

```
<udp_host>127.0.0.1</udp_host>
```

To send the stream to another computer, set `udp_host` to the receiver's IP:

```
<udp_host>192.168.1.100</udp_host>
```

You can also omit `udp_host` from the camera configuration and set:

```
export PX4_VIDEO_HOST_IP=192.168.1.100
```

An unset or empty `PX4_VIDEO_HOST_IP` uses `127.0.0.1`.

## Configuration

| Parameter           |             Default | Description                           |
| ------------------- | ------------------: | ------------------------------------- |
| `camera_name`       |         sensor name | Gazebo rendering camera               |
| `udp_host`          |         `127.0.0.1` | destination IP                        |
| `udp_port`          |              `5600` | destination UDP port                  |
| `rate`              | sensor rate or `30` | stream FPS                            |
| `bitrate_kbps`      |             `16384` | H.264 bitrate                         |
| `use_cuda`          |             `false` | use NVIDIA `nvh264enc` when available |
| `x264_speed_preset` |                 `1` | x264 speed preset                     |

`udp_port` is limited to `1..65535`, `rate` to `1..240`, `bitrate_kbps` to `64..200000`, and `x264_speed_preset` to `1..10`.

## NVIDIA encoding

Set:

```
<use_cuda>true</use_cuda>
```

The plugin will try to use `nvh264enc`. If it is unavailable or fails to start, it falls back to x264.

If an active GStreamer pipeline reports a fatal error or EOS, the stream is stopped and retried after a short delay. If NVENC fails at runtime, that camera falls back to x264 for the rest of the plugin instance.

`use_cuda=true` uses `nvh264enc` for H.264 encoding. RGB-to-NV12 conversion still runs through `videoconvert` on the CPU; Gazebo rendering and the frame copy are not CUDA zero-copy.

## How it works

The default PX4 Harmonic GStreamer path is roughly:

```
Gazebo Camera
    |
    v
gz.msgs.Image
    |
    v
Gazebo Transport
    |
    v
GStreamer
```

`GstPlaneCameraSystem` uses:

```
Gazebo Rendering Camera
    |
    v
Camera::Copy()
    |
    v
GStreamer appsrc
    |
    v
H.264 encoder
    |
    v
RTP / UDP
```

This removes the Gazebo Transport image-message subscription from the video path.

The plugin is loaded once at world level. It discovers camera sensors containing a `GstPlaneCameraSystem` block and creates a separate GStreamer pipeline for each camera.

Camera streams are discovered dynamically. When a camera or aircraft is removed from the simulation, its pipeline is stopped and its stream state is cleaned up. Respawned cameras can then be discovered again.

## Install locally

Instead of exporting the build directory every time:

```
cmake --install build --prefix "$HOME/.local"
```

Then add:

```
export GZ_SIM_SYSTEM_PLUGIN_PATH="$HOME/.local/lib:${GZ_SIM_SYSTEM_PLUGIN_PATH:-}"
```

You can add this line to your shell configuration if you want the plugin path to persist between terminals.

## License

BSD 3-Clause.

This project is derived from the [PX4-Autopilot Gazebo GStreamer camera plugin](https://github.com/PX4/PX4-Autopilot/tree/v1.16.0/src/modules/simulation/gz_plugins/gstreamer).

Original PX4 implementation: Copyright (c) 2025 PX4 Development Team.

Modifications in this repository: Copyright (c) 2026 Alex Chazov.

See `LICENSE`.

