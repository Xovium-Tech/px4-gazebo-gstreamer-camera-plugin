#pragma once

#include <chrono>
#include <cstddef>
#include <optional>

namespace gst_plane_camera
{
enum class PixelFormat { RGB8 };

// Borrowed, tightly packed RGB bytes, valid for the duration of SubmitFrame.
struct FrameView
{
  const void *data{nullptr};
  std::size_t size{0};
  unsigned int width{0};
  unsigned int height{0};
  unsigned int channels{3};
  PixelFormat format{PixelFormat::RGB8};
  // Optional pipeline running-time PTS. Unset preserves wall-clock appsrc
  // do-timestamp behavior; adapters must not insert simulator time by default.
  std::optional<std::chrono::nanoseconds> timestamp;
};
} // namespace gst_plane_camera
