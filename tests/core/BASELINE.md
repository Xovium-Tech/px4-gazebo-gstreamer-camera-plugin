# Behavior captured before extraction

`ConfigTests.cc` was added against the original `ParseStreamConfig`, `DefaultHost`
and `SensorRate` behavior, before implementing the shared-core equivalents.
The original build requires Harmonic, which is not installed on this workstation
(Jetty is installed), so this is an extracted behavior baseline, not a claim that
an original Harmonic binary was executed.

- `PX4_VIDEO_HOST_IP`, when set, supplies the default host; explicit nonempty
  `udp_host` wins. The empty environment value is preserved as before.
- UDP defaults to 5600; ports outside 1–65535 reset to 5600.
- Sensor update rate defaults to 30 if nonfinite or nonpositive; otherwise round
  after clamping to 1–240. Explicit plugin rates clamp directly to 1–240.
- Bitrate defaults to 16384 kbit/s and clamps to 64–200000.
- x264 speed preset defaults to 1 and clamps to 1–10.
- Appsrc uses live RGB, wall-clock `do-timestamp=true`, duration 1/rate,
  nonblocking delivery, and a downstream-leaky one-buffer queue.
- x264 uses I420, zerolatency tune (4), keyframe interval 10, byte-stream and
  sliced threads. NVENC uses NV12, zero B frames / lookahead, the existing
  low-latency presets and a two-frame VBV budget with a 256-kbit minimum.
- RTP is H264, payload type 96, MTU 1200, config interval -1. UDP is unsynchronized
  and asynchronous preroll is disabled; send buffer is 4 MiB where supported.
- Missing / failed NVENC falls back to x264 per stream. Runtime ERROR or EOS
  stops the pipeline and schedules retry after one second, on the next frame.

The runtime tests inspect real GStreamer elements and appsrc buffers, without
Gazebo or an NVIDIA GPU. They retain these baseline expectations while exercising
ownership, resolution changes, independent streams, bus failures and shutdown.
