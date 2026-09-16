#pragma once

#include "gst_plane_camera/core/FrameView.hh"
#include "gst_plane_camera/core/StreamConfig.hh"

#include <cstdint>
#include <memory>

struct _GstElement;

namespace gst_plane_camera
{
struct PipelineStats
{
  std::uint64_t acceptedFrames{0};
  std::uint64_t droppedFrames{0};
  std::uint64_t pushedFrames{0};
  std::uint64_t pipelineStarts{0};
  std::uint64_t pipelineFailures{0};
  std::uint64_t nvencAttempts{0};
  bool running{false};
  bool usingNvenc{false};
  bool nvencDisabled{false};
};

class StreamPipeline
{
public:
  explicit StreamPipeline(const StreamConfig &config);
  ~StreamPipeline();
  StreamPipeline(const StreamPipeline &) = delete;
  StreamPipeline &operator=(const StreamPipeline &) = delete;

  // Validates and makes one copy into GstBuffer-owned memory before returning.
  // Replaces a pending frame when the worker falls behind; never waits for an
  // encoder or keeps a borrowed rendering-camera pointer.
  bool SubmitFrame(const FrameView &frame);
  // Idempotent. Stops accepting frames, joins the worker, and frees all Gst state.
  // Stop may run concurrently with SubmitFrame; object lifetime remains caller-owned.
  void Stop();
  PipelineStats Stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
  // The test peer obtains a ref under the worker state lock to inspect actual
  // Gst elements and inject bus messages; no alternate production pipeline path.
  friend struct StreamPipelineTestAccess;
  _GstElement *RefPipelineForTesting() const;
};
} // namespace gst_plane_camera
