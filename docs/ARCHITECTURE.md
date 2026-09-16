# Architecture

`GstPlaneCameraSystem` uses one Gazebo Sim adapter and one simulator-independent
GStreamer core. Harmonic and Jetty compile the same adapter source against their
own library versions. Both export `custom::GstPlaneCameraSystem` from
`libGstPlaneCameraSystem.so`; the resulting binaries are not ABI-compatible.

## Frame path

```text
Gazebo rendering camera
    │ Camera::Copy()
    ▼
Adapter-owned RGB image
    │ StreamPipeline::SubmitFrame(FrameView)
    ▼
GStreamer-owned buffer → latest-frame slot → appsrc → queue
    → videoconvert → I420 / NV12 → x264enc / nvh264enc → rtph264pay → udpsink
```

There is no Gazebo Transport image subscriber in this path. The supported
camera image format is `R8G8B8`; the adapter rejects other formats explicitly.

## Gazebo Sim adapter

The world system discovers camera sensors containing a `GstPlaneCameraSystem`
configuration block. It parses their SDF values into `StreamConfig` and creates
an independent `StreamPipeline` for each camera. Sensor-level plugin instances
remain inactive; the world instance owns discovery and streaming.

`PostUpdate` discovers additions and removals and schedules capture using
simulation time. Rendering-scene lookup, camera lookup, grouped `Render` /
`PostRender` calls, and `Camera::Copy()` happen in the rendering callback.
Simulation time controls capture cadence, while appsrc uses its running clock
for default frame timestamps.

The adapter handles cameras appearing after startup and rediscovers respawned
cameras. Removal stops the affected stream and retires its camera reference on
the render thread. A shared callback lifetime gate prevents an event callback
from accessing a destroyed system. Render teardown releases active rendering
references and stops stream workers.

The camera, event, and registration APIs used here compile unchanged on both
supported Gazebo distributions. Package and imported-target differences are
handled in `cmake/GazeboBackend.cmake`.

## Shared streaming core

The core has no Gazebo or SDF includes, pointers, or link dependencies. Its API
consists of configuration values, a borrowed `FrameView`, and a stream object.

Each stream owns its destination, rate, bitrate, encoder choice, worker, pipeline,
appsrc, bus, retry deadline, and NVENC failure state. GStreamer initialization is
shared, but mutable pipeline state is per stream.

`SubmitFrame` validates dimensions, buffer size, channel count, and format, then
copies the frame into a `GstBuffer` before returning. The rendering image can be
reused immediately. RGB stride metadata describes tightly packed rows, including
widths that do not align to GStreamer's default four-byte row boundary.

The pending slot keeps the latest frame when the worker falls behind. The worker
passes the same owned buffer to appsrc without another full-frame copy. Queues
are bounded for low latency. The core supplies frame duration from the configured
rate and preserves appsrc's wall-clock timestamp behavior by default.

## Encoder recovery and shutdown

The pipeline converts RGB to I420 for x264 or NV12 for NVENC, encodes H.264,
packetizes it as RTP, and sends it over UDP. NVIDIA encoding is discovered at
runtime through GStreamer; CUDA libraries are not a compile-time requirement.

When requested NVENC is unavailable, cannot initialize, or reports a fatal
runtime failure, the stream falls back to x264. That stream's NVENC failure
state lasts for the lifetime of the stream instance.

The worker polls the bus even while frames are not arriving. Fatal errors and
EOS stop the pipeline and impose a one-second retry delay; a subsequent frame
starts the replacement pipeline. A resolution change also rebuilds the pipeline.
Other cameras retain their own pipelines and retry state.

`Stop()` rejects further submissions, clears the pending buffer, joins the
worker, and releases GStreamer resources. It is idempotent and may run
concurrently with `SubmitFrame`; callers remain responsible for the stream
object's lifetime.

## Source layout

```text
cmake/GazeboBackend.cmake             distribution and imported-target selection
include/gst_plane_camera/core/        StreamConfig, FrameView, StreamPipeline APIs
src/core/                            shared configuration and streaming implementation
include/gst_plane_camera/adapters/    Gazebo Sim adapter declarations
src/adapters/gzsim/                   one world-system implementation
examples/harmonic/                    standalone world, model, and server.config
examples/jetty/                       equivalent Jetty examples
tests/core/                          configuration and real GStreamer tests
tests/cmake/                         distribution and cache selection tests
tests/smoke/                         linkage and rendering smoke tests
tests/integration/                   camera lifecycle and format tests
```

See [TESTING.md](TESTING.md) for test commands and the distinction between
compile checks, runtime camera tests, and manual PX4 flight testing. Build,
configuration, and PX4 integration instructions are in the [README](../README.md).
