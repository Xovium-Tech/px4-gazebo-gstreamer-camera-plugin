/****************************************************************************
 *
 *   Copyright (c) 2025 PX4 Development Team. All rights reserved.
 *   Copyright (c) 2026 Alex Chazov. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be used to
 *    endorse or promote products derived from this software without specific
 *    prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/


#include "gst_plane_camera/core/StreamPipeline.hh"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace gst_plane_camera
{
namespace
{
constexpr auto kPipelineRetryDelay = std::chrono::seconds(1);
constexpr auto kBusPollInterval = std::chrono::milliseconds(20);

void ConfigureLeakyQueue(GstElement *_queue)
{
	if (!_queue) {
		return;
	}

	g_object_set(G_OBJECT(_queue),
		"max-size-buffers", 1u,
		"max-size-bytes", 0u,
		"max-size-time", static_cast<guint64>(0),
		"leaky", 2,
		"silent", TRUE,
		nullptr);
}

//////////////////////////////////////////////////
void SetUintIfPresent(GstElement *_element,
				       const char *_property,
				       guint _value)
{
	if (_element && g_object_class_find_property(G_OBJECT_GET_CLASS(_element), _property)) {
		g_object_set(G_OBJECT(_element), _property, _value, nullptr);
	}
}

//////////////////////////////////////////////////
void SetIntIfPresent(GstElement *_element,
				       const char *_property,
				       gint _value)
{
	if (_element && g_object_class_find_property(G_OBJECT_GET_CLASS(_element), _property)) {
		g_object_set(G_OBJECT(_element), _property, _value, nullptr);
	}
}

//////////////////////////////////////////////////
bool SetEnumNickIfPresent(
    GstElement *_element,
    const char *_property,
    const char *_nick)
{
    if (!_element || !_property || !_nick) {
        return false;
    }

    GObjectClass *objectClass = G_OBJECT_GET_CLASS(_element);
    GParamSpec *pspec =
        g_object_class_find_property(objectClass, _property);

    if (!pspec || !G_IS_PARAM_SPEC_ENUM(pspec)) {
        return false;
    }

    GType enumType = G_PARAM_SPEC_VALUE_TYPE(pspec);
    GEnumClass *enumClass =
        G_ENUM_CLASS(g_type_class_ref(enumType));

    if (!enumClass) {
        return false;
    }

    const GEnumValue *value =
        g_enum_get_value_by_nick(enumClass, _nick);

    if (!value) {
        g_type_class_unref(enumClass);
        return false;
    }

    const gint enumValue = value->value;
    g_type_class_unref(enumClass);

    g_object_set(
        G_OBJECT(_element),
        _property,
        enumValue,
        nullptr);

    return true;
}

//////////////////////////////////////////////////
void SetBoolIfPresent(GstElement *_element,
				       const char *_property,
				       gboolean _value)
{
	if (_element && g_object_class_find_property(G_OBJECT_GET_CLASS(_element), _property)) {
		g_object_set(G_OBJECT(_element), _property, _value, nullptr);
	}
}

} // namespace

struct StreamPipeline::Impl
{
  explicit Impl(const StreamConfig &requested) : config(requested)
  {
    NormalizeConfig(config, config.cameraName);
    static std::once_flag init;
    std::call_once(init, [] { gst_init(nullptr, nullptr); });
    worker = std::thread([this] { Run(); });
  }

  StreamConfig config;
  std::mutex frameMutex;
  std::condition_variable frameCv;
  GstBuffer *pending{nullptr};
  unsigned int pendingWidth{0};
  unsigned int pendingHeight{0};
  bool stopping{false};
  std::mutex stopMutex;
  std::thread worker;

  // Only the worker mutates Gst state. The lock also permits the test peer to
  // acquire a safe object reference while observing a running pipeline.
  mutable std::mutex pipelineMutex;
  GstElement *pipeline{nullptr};
  GstElement *source{nullptr};
  GstBus *bus{nullptr};
  unsigned int width{0};
  unsigned int height{0};
  bool usingNvenc{false};
  bool nvencFailed{false};
  bool firstFrameReported{false};
  std::chrono::steady_clock::time_point nextPipelineRetry{};
  mutable std::mutex statsMutex;
  PipelineStats stats;

  bool Submit(const FrameView &frame)
  {
    if (!frame.data || !frame.width || !frame.height || frame.channels != 3 ||
        frame.format != PixelFormat::RGB8 ||
        frame.width > static_cast<unsigned>(std::numeric_limits<int>::max()) ||
        frame.height > static_cast<unsigned>(std::numeric_limits<int>::max()) ||
        (frame.timestamp && frame.timestamp->count() < 0))
      return false;
    const std::size_t widthBytes = static_cast<std::size_t>(frame.width) * 3u;
    if (widthBytes > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        widthBytes / 3u != frame.width ||
        frame.height > std::numeric_limits<std::size_t>::max() / widthBytes)
      return false;
    const std::size_t bytes = widthBytes * frame.height;
    if (frame.size < bytes)
      return false;

    // The only copy after Gazebo Camera::Copy: the owned GstBuffer is passed
    // straight through the pending slot and transferred to appsrc by the worker.
    // No rendering buffer or adapter-owned image is referenced asynchronously.
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, bytes, nullptr);
    if (!buffer)
      return false;
    if (gst_buffer_fill(buffer, 0, frame.data, bytes) != bytes) {
      gst_buffer_unref(buffer);
      return false;
    }
    // RGB camera images are tightly packed. Default raw-video strides round
    // rows to four bytes; explicit video metadata avoids that assumption and
    // avoids a second copy for camera widths that are not multiples of four.
    gsize offsets[GST_VIDEO_MAX_PLANES] = {};
    gint strides[GST_VIDEO_MAX_PLANES] = {static_cast<gint>(widthBytes)};
    if (!gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE,
          GST_VIDEO_FORMAT_RGB, frame.width, frame.height, 1, offsets, strides)) {
      gst_buffer_unref(buffer);
      return false;
    }
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, config.rate);
    if (frame.timestamp)
      GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(frame.timestamp->count());

    {
      std::lock_guard<std::mutex> lock(frameMutex);
      if (stopping) {
        gst_buffer_unref(buffer);
        return false;
      }
      std::lock_guard<std::mutex> statsLock(statsMutex);
      if (pending) {
        gst_buffer_unref(pending);
        ++stats.droppedFrames;
      }
      pending = buffer;
      pendingWidth = frame.width;
      pendingHeight = frame.height;
      ++stats.acceptedFrames;
    }
    frameCv.notify_one();
    return true;
  }

  void Stop()
  {
    // Serializing join makes repeated/concurrent Stop calls deterministic.
    std::lock_guard<std::mutex> stopLock(stopMutex);
    {
      std::lock_guard<std::mutex> lock(frameMutex);
      stopping = true;
      if (pending) {
        gst_buffer_unref(pending);
        pending = nullptr;
      }
    }
    frameCv.notify_all();
    if (worker.joinable())
      worker.join();
  }

  void Run()
  {
    for (;;) {
      GstBuffer *buffer = nullptr;
      unsigned int frameWidth = 0;
      unsigned int frameHeight = 0;
      {
        std::unique_lock<std::mutex> lock(frameMutex);
        // Bus errors are consumed even while the simulator is paused or a
        // camera no longer supplies frames. No render-thread bus callback exists.
        frameCv.wait_for(lock, kBusPollInterval,
                         [this] { return stopping || pending; });
        if (stopping)
          break;
        buffer = std::exchange(pending, nullptr);
        frameWidth = pendingWidth;
        frameHeight = pendingHeight;
      }
      std::lock_guard<std::mutex> lock(pipelineMutex);
      PollBus();
      if (!buffer)
        continue;
      if (!StartPipeline(frameWidth, frameHeight)) {
        gst_buffer_unref(buffer);
        continue;
      }
      const GstFlowReturn result = gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
      // push_buffer consumes the reference for all flow results.
      if (result == GST_FLOW_OK) {
        {
          std::lock_guard<std::mutex> statsLock(statsMutex);
          ++stats.pushedFrames;
        }
        if (!firstFrameReported) {
          // One stdio write keeps markers intact across camera worker threads.
          std::fprintf(stderr, "[GstPlaneCameraSystem] APP_SRC_FIRST_FRAME [%s]\n",
                       config.cameraName.c_str());
          firstFrameReported = true;
        }
      } else if (result != GST_FLOW_FLUSHING) {
        std::cerr << "GstPlaneCameraSystem: appsrc push failed for ["
                  << config.cameraName << "]: " << result << std::endl;
        HandleFailure(usingNvenc);
      }
    }
    std::lock_guard<std::mutex> lock(pipelineMutex);
    StopPipeline();
  }

  bool StartPipeline(unsigned int frameWidth, unsigned int frameHeight)
  {
    if (pipeline) {
      if (width == frameWidth && height == frameHeight)
        return true;
      StopPipeline();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < nextPipelineRetry)
      return false;
    if (config.useCuda && !nvencFailed) {
      {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++stats.nvencAttempts;
      }
      if (BuildPipeline(frameWidth, frameHeight, true))
        return true;
      nvencFailed = true;
      {
        std::lock_guard<std::mutex> lock(statsMutex);
        stats.nvencDisabled = true;
      }
      std::cerr << "GstPlaneCameraSystem: nvh264enc unavailable or failed for ["
                << config.cameraName << "], falling back to x264" << std::endl;
    }
    if (BuildPipeline(frameWidth, frameHeight, false))
      return true;
    {
      std::lock_guard<std::mutex> lock(statsMutex);
      ++stats.pipelineFailures;
    }
    nextPipelineRetry = now + kPipelineRetryDelay;
    return false;
  }

  bool BuildPipeline(unsigned int _width, unsigned int _height, bool _useNvenc)
{
	GstElement *pipeline = gst_pipeline_new(nullptr);
	GstElement *source = gst_element_factory_make("appsrc", nullptr);
	GstElement *queue = gst_element_factory_make("queue", nullptr);
	GstElement *converter = gst_element_factory_make("videoconvert", nullptr);
	GstElement *capsFilter = gst_element_factory_make("capsfilter", nullptr);
	GstElement *encoder = gst_element_factory_make(_useNvenc ? "nvh264enc" : "x264enc", nullptr);
	GstElement *payloader = gst_element_factory_make("rtph264pay", nullptr);
	GstElement *sink = gst_element_factory_make("udpsink", nullptr);

	if (!pipeline || !source || !queue || !converter || !capsFilter
	    || !encoder || !payloader || !sink) {
		if (!_useNvenc) {
			std::cerr << "GstPlaneCameraSystem: failed to create GStreamer elements for ["
			      << config.cameraName << "]" << std::endl;
		}

		if (source) { gst_object_unref(source); }
		if (queue) { gst_object_unref(queue); }
		if (converter) { gst_object_unref(converter); }
		if (capsFilter) { gst_object_unref(capsFilter); }
		if (encoder) { gst_object_unref(encoder); }
		if (payloader) { gst_object_unref(payloader); }
		if (sink) { gst_object_unref(sink); }
		if (pipeline) { gst_object_unref(pipeline); }
		return false;
	}

	ConfigureLeakyQueue(queue);

	const guint64 rawFrameBytes = static_cast<guint64>(_width) * _height * 3u;
	GstCaps *sourceCaps = gst_caps_new_simple("video/x-raw",
		"format", G_TYPE_STRING, "RGB",
		"width", G_TYPE_INT, static_cast<int>(_width),
		"height", G_TYPE_INT, static_cast<int>(_height),
		"framerate", GST_TYPE_FRACTION, config.rate, 1,
		nullptr);

	GstCaps *encoderCaps = gst_caps_new_simple("video/x-raw",
		"format", G_TYPE_STRING, _useNvenc ? "NV12" : "I420",
		"width", G_TYPE_INT, static_cast<int>(_width),
		"height", G_TYPE_INT, static_cast<int>(_height),
		"framerate", GST_TYPE_FRACTION, config.rate, 1,
		nullptr);

	if (!sourceCaps || !encoderCaps) {
		std::cerr << "GstPlaneCameraSystem: failed to create video caps for ["
		      << config.cameraName << "]" << std::endl;
		if (sourceCaps) { gst_caps_unref(sourceCaps); }
		if (encoderCaps) { gst_caps_unref(encoderCaps); }
		gst_object_unref(source);
		gst_object_unref(queue);
		gst_object_unref(converter);
		gst_object_unref(capsFilter);
		gst_object_unref(encoder);
		gst_object_unref(payloader);
		gst_object_unref(sink);
		gst_object_unref(pipeline);
		return false;
	}

	g_object_set(G_OBJECT(source),
		"caps", sourceCaps,
		"is-live", TRUE,
		"do-timestamp", TRUE,
		"stream-type", GST_APP_STREAM_TYPE_STREAM,
		"format", GST_FORMAT_TIME,
		"block", FALSE,
		"emit-signals", FALSE,
		"max-bytes", rawFrameBytes,
		nullptr);
	gst_caps_unref(sourceCaps);

	GObjectClass *sourceClass = G_OBJECT_GET_CLASS(source);
	if (g_object_class_find_property(sourceClass, "max-buffers")) {
		g_object_set(G_OBJECT(source), "max-buffers", static_cast<guint64>(1), nullptr);
	}
	if (g_object_class_find_property(sourceClass, "leaky-type")) {
		g_object_set(G_OBJECT(source), "leaky-type", 2, nullptr);
	}

	g_object_set(G_OBJECT(capsFilter), "caps", encoderCaps, nullptr);
	gst_caps_unref(encoderCaps);

	if (_useNvenc) {
		SetUintIfPresent(encoder, "bitrate", config.bitrateKbps);
		SetIntIfPresent(encoder, "gop-size", 10);
		SetUintIfPresent(encoder, "bframes", 0);
		SetUintIfPresent(encoder, "rc-lookahead", 0);
		SetBoolIfPresent(encoder, "zerolatency", TRUE);
		SetBoolIfPresent(encoder, "repeat-sequence-header", TRUE);
		SetBoolIfPresent(encoder, "strict-gop", TRUE);

		if (!SetEnumNickIfPresent(encoder, "preset", "p4")) {
			SetEnumNickIfPresent(encoder, "preset", "low-latency-hq");
		}
		SetEnumNickIfPresent(encoder, "tune", "ultra-low-latency");
		SetEnumNickIfPresent(encoder, "rc-mode", "cbr");
		SetEnumNickIfPresent(encoder, "multi-pass", "disabled");

		const guint vbvKbits = std::max<guint>(256u,
			static_cast<guint>((static_cast<uint64_t>(config.bitrateKbps) * 2u)
				/ std::max(1u, config.rate)));
		SetUintIfPresent(encoder, "vbv-buffer-size", vbvKbits);
	} else {
		g_object_set(G_OBJECT(encoder),
			"bitrate", config.bitrateKbps,
			"speed-preset", config.x264SpeedPreset,
			"tune", 4,
			"key-int-max", 10,
			nullptr);
		SetBoolIfPresent(encoder, "byte-stream", TRUE);
		SetBoolIfPresent(encoder, "sliced-threads", TRUE);
	}

	g_object_set(G_OBJECT(payloader),
		"config-interval", -1,
		"mtu", 1200u,
		"pt", 96u,
		nullptr);

	g_object_set(G_OBJECT(sink),
		"host", config.udpHost.c_str(),
		"port", config.udpPort,
		"sync", FALSE,
		"async", FALSE,
		nullptr);
	SetIntIfPresent(sink, "buffer-size", 4 * 1024 * 1024);
	SetBoolIfPresent(sink, "qos", FALSE);

	gst_bin_add_many(GST_BIN(pipeline), source, queue, converter, capsFilter,
		encoder, payloader, sink, nullptr);

	if (!gst_element_link_many(source, queue, converter, capsFilter, encoder,
		payloader, sink, nullptr)) {
		if (!_useNvenc) {
			std::cerr << "GstPlaneCameraSystem: failed to link GStreamer pipeline for ["
			      << config.cameraName << "]" << std::endl;
		}
		gst_element_set_state(pipeline, GST_STATE_NULL);
		gst_object_unref(pipeline);
		return false;
	}

	if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		if (!_useNvenc) {
			std::cerr << "GstPlaneCameraSystem: failed to start GStreamer pipeline for ["
			      << config.cameraName << "]" << std::endl;
		}
		gst_element_set_state(pipeline, GST_STATE_NULL);
		gst_object_unref(pipeline);
		return false;
	}

	this->pipeline = pipeline;
	this->source = source;
	this->bus = gst_element_get_bus(pipeline);
	width = _width;
	height = _height;
	firstFrameReported = false;
	usingNvenc = _useNvenc;
	nextPipelineRetry = {};

	{
    std::lock_guard<std::mutex> lock(statsMutex);
    ++stats.pipelineStarts;
    stats.running = true;
    stats.usingNvenc = _useNvenc;
  }
  std::cerr << "[GstPlaneCameraSystem] STREAMING [" << config.cameraName << "] "
		  << _width << 'x' << _height << " @ " << config.rate << " FPS -> "
		  << config.udpHost << ':' << config.udpPort << " using "
		  << (_useNvenc ? "nvh264enc" : "x264") << std::endl;
	return true;
}


  void StopPipeline()
  {
    source = nullptr;
    if (pipeline)
      gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus) {
      gst_object_unref(bus);
      bus = nullptr;
    }
    if (pipeline) {
      gst_object_unref(pipeline);
      pipeline = nullptr;
    }
    width = height = 0;
    usingNvenc = false;
    std::lock_guard<std::mutex> lock(statsMutex);
    stats.running = false;
    stats.usingNvenc = false;
  }

  void HandleFailure(bool disableNvenc)
  {
    if (disableNvenc && usingNvenc) {
      nvencFailed = true;
      std::cerr << "GstPlaneCameraSystem: disabling NVENC for ["
                << config.cameraName << "] after a runtime encoder failure" << std::endl;
    }
    StopPipeline();
    nextPipelineRetry = std::chrono::steady_clock::now() + kPipelineRetryDelay;
    std::lock_guard<std::mutex> lock(statsMutex);
    ++stats.pipelineFailures;
    stats.nvencDisabled = nvencFailed;
  }

  void PollBus()
  {
    if (!bus)
      return;
    bool fatal = false;
    while (GstMessage *message = gst_bus_pop_filtered(bus,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS))) {
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        std::cerr << "GstPlaneCameraSystem: EOS [" << config.cameraName << "]" << std::endl;
        fatal = true;
      } else {
        GError *error = nullptr;
        gchar *debug = nullptr;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
          gst_message_parse_error(message, &error, &debug);
          fatal = true;
        } else {
          gst_message_parse_warning(message, &error, &debug);
        }
        std::cerr << "GstPlaneCameraSystem GStreamer "
                  << (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR ? "error" : "warning")
                  << " [" << config.cameraName << "]: "
                  << (error ? error->message : "unknown") << std::endl;
        if (debug)
          std::cerr << "  debug: " << debug << std::endl;
        if (error)
          g_error_free(error);
        g_free(debug);
      }
      gst_message_unref(message);
    }
    if (fatal)
      HandleFailure(usingNvenc);
  }
};

StreamPipeline::StreamPipeline(const StreamConfig &config)
  : impl(std::make_unique<Impl>(config)) {}
StreamPipeline::~StreamPipeline() { Stop(); }
bool StreamPipeline::SubmitFrame(const FrameView &frame) { return impl->Submit(frame); }
void StreamPipeline::Stop() { impl->Stop(); }
PipelineStats StreamPipeline::Stats() const
{
  std::lock_guard<std::mutex> lock(impl->statsMutex);
  return impl->stats;
}
_GstElement *StreamPipeline::RefPipelineForTesting() const
{
  std::lock_guard<std::mutex> lock(impl->pipelineMutex);
  return impl->pipeline ? GST_ELEMENT(gst_object_ref(impl->pipeline)) : nullptr;
}
} // namespace gst_plane_camera
