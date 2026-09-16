#include "gst_plane_camera/core/StreamPipeline.hh"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; return 1; \
} } while (false)

namespace gst_plane_camera
{
struct StreamPipelineTestAccess
{
  static GstElement *RefPipeline(const StreamPipeline &stream)
  { return stream.RefPipelineForTesting(); }
};
}

namespace
{
using namespace gst_plane_camera;
using namespace std::chrono_literals;
struct GstUnref { void operator()(GstElement *p) const { if (p) gst_object_unref(p); } };
using Element = std::unique_ptr<GstElement, GstUnref>;

bool Wait(const std::function<bool()> &condition, std::chrono::milliseconds timeout = 3000ms)
{
  const auto end = std::chrono::steady_clock::now() + timeout;
  do {
    if (condition())
      return true;
    std::this_thread::sleep_for(5ms);
  } while (std::chrono::steady_clock::now() < end);
  return condition();
}

Element Find(GstElement *pipeline, const char *factoryName)
{
  GstIterator *iterator = gst_bin_iterate_elements(GST_BIN(pipeline));
  GValue value = G_VALUE_INIT;
  Element result;
  while (gst_iterator_next(iterator, &value) == GST_ITERATOR_OK) {
    auto *element = GST_ELEMENT(g_value_get_object(&value));
    auto *factory = gst_element_get_factory(element);
    if (factory && std::string(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))) == factoryName) {
      result.reset(GST_ELEMENT(gst_object_ref(element)));
      g_value_unset(&value);
      break;
    }
    g_value_unset(&value);
  }
  gst_iterator_free(iterator);
  return result;
}

struct Sample
{
  GstClockTime pts;
  GstClockTime duration;
  std::vector<unsigned char> bytes;
  int stride;
};
struct Captures
{
  std::mutex mutex;
  std::condition_variable cv;
  bool block{false};
  bool release{false};
  std::vector<Sample> samples;
};

// A pad probe observes real appsrc output and can briefly hold a buffer so
// ownership and nonblocking submission are tested without scheduler assumptions.
class Probe
{
public:
  explicit Probe(GstElement *source) : captures(std::make_shared<Captures>())
  {
    pad = gst_element_get_static_pad(source, "src");
    id = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
      [](GstPad *, GstPadProbeInfo *info, gpointer data) {
        auto state = *static_cast<std::shared_ptr<Captures> *>(data);
        auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        GstMapInfo map = GST_MAP_INFO_INIT;
        Sample sample{GST_BUFFER_PTS(buffer), GST_BUFFER_DURATION(buffer), {}, 0};
        if (auto *meta = gst_buffer_get_video_meta(buffer))
          sample.stride = meta->stride[0];
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
          sample.bytes.assign(map.data, map.data + map.size);
          gst_buffer_unmap(buffer, &map);
        }
        std::unique_lock<std::mutex> lock(state->mutex);
        state->samples.push_back(std::move(sample));
        state->cv.notify_all();
        state->cv.wait(lock, [&] { return !state->block || state->release; });
        return GST_PAD_PROBE_OK;
      }, new std::shared_ptr<Captures>(captures),
      [](gpointer data) { delete static_cast<std::shared_ptr<Captures> *>(data); });
  }
  ~Probe()
  {
    Release();
    gst_pad_remove_probe(pad, id);
    gst_object_unref(pad);
  }
  void Release()
  {
    std::lock_guard<std::mutex> lock(captures->mutex);
    captures->release = true;
    captures->cv.notify_all();
  }
  void Block()
  {
    std::lock_guard<std::mutex> lock(captures->mutex);
    captures->block = true;
    captures->release = false;
  }
  std::size_t Count()
  {
    std::lock_guard<std::mutex> lock(captures->mutex);
    return captures->samples.size();
  }
  Sample Last()
  {
    std::lock_guard<std::mutex> lock(captures->mutex);
    return captures->samples.back();
  }
private:
  std::shared_ptr<Captures> captures;
  GstPad *pad{nullptr};
  gulong id{0};
};

void PostFailure(StreamPipeline &stream, bool eos)
{
  Element pipeline(StreamPipelineTestAccess::RefPipeline(stream));
  if (!pipeline)
    return;
  GstBus *bus = gst_element_get_bus(pipeline.get());
  GstMessage *message = nullptr;
  if (eos) {
    message = gst_message_new_eos(GST_OBJECT(pipeline.get()));
  } else {
    GError *error = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, "test fatal stream error");
    message = gst_message_new_error(GST_OBJECT(pipeline.get()), error, "injected on actual pipeline bus");
    g_error_free(error);
  }
  gst_bus_post(bus, message);
  gst_object_unref(bus);
}
} // namespace

int main(int argc, char **argv)
{
  gst_init(nullptr, nullptr);
  for (const char *factoryName : {"appsrc", "queue", "videoconvert", "capsfilter", "x264enc", "rtph264pay", "udpsink"}) {
    GstElementFactory *factory = gst_element_factory_find(factoryName);
    if (!factory) {
      std::cout << "SKIP: missing GStreamer runtime plugin " << factoryName << '\n';
      return 77;
    }
    gst_object_unref(factory);
  }
  const bool nvencOnly = argc > 1 && std::string(argv[1]) == "--nvenc";
  StreamConfig config;
  config.cameraName = "core_test";
  config.udpHost = "127.0.0.1";
  config.udpPort = 15600;
  config.rate = 60;
  config.bitrateKbps = 4096;
  config.x264SpeedPreset = 2;
  std::vector<unsigned char> pixels(64u * 48u * 3u, 42);
  FrameView frame{pixels.data(), pixels.size(), 64, 48, 3, PixelFormat::RGB8, {}};

  if (nvencOnly) {
    GstElementFactory *factory = gst_element_factory_find("nvh264enc");
    if (!factory) {
      std::cout << "SKIP: nvh264enc unavailable\n";
      return 77;
    }
    gst_object_unref(factory);
    config.useCuda = true;
    StreamPipeline stream(config);
    for (int i = 0; i < 40; ++i) {
      stream.SubmitFrame(frame);
      std::this_thread::sleep_for(50ms);
    }
    if (!stream.Stats().usingNvenc || stream.Stats().nvencDisabled) {
      std::cout << "SKIP: nvh264enc cannot initialize on this machine\n";
      return 77;
    }
    CHECK(stream.Stats().pushedFrames > 0);
    std::cout << "NVENC runtime passed\n";
    return 0;
  }

  StreamPipeline stream(config);
  FrameView invalid = frame;
  invalid.data = nullptr;
  CHECK(!stream.SubmitFrame(invalid));
  invalid = frame; invalid.size -= 1;
  CHECK(!stream.SubmitFrame(invalid));
  invalid = frame; invalid.width = 0;
  CHECK(!stream.SubmitFrame(invalid));
  invalid = frame; invalid.height = std::numeric_limits<unsigned>::max();
  CHECK(!stream.SubmitFrame(invalid));
  invalid = frame; invalid.channels = 4;
  CHECK(!stream.SubmitFrame(invalid));
  invalid = frame; invalid.format = static_cast<PixelFormat>(99);
  CHECK(!stream.SubmitFrame(invalid));
  invalid = frame; invalid.timestamp = -1ns;
  CHECK(!stream.SubmitFrame(invalid));
  CHECK(stream.Stats().acceptedFrames == 0);
  CHECK(stream.SubmitFrame(frame));
  CHECK(Wait([&] { return stream.Stats().pushedFrames == 1; }));

  {
    Element pipeline(StreamPipelineTestAccess::RefPipeline(stream));
    CHECK(pipeline);
    auto source = Find(pipeline.get(), "appsrc");
    auto queue = Find(pipeline.get(), "queue");
    auto encoder = Find(pipeline.get(), "x264enc");
    auto converter = Find(pipeline.get(), "videoconvert");
    auto filter = Find(pipeline.get(), "capsfilter");
    auto payloader = Find(pipeline.get(), "rtph264pay");
    auto sink = Find(pipeline.get(), "udpsink");
    CHECK(source && queue && encoder && converter && filter && payloader && sink);
    CHECK(!Find(pipeline.get(), "nvh264enc"));
    gboolean live = FALSE, timestamp = FALSE, block = TRUE;
    guint64 maxBytes = 0;
    g_object_get(source.get(), "is-live", &live, "do-timestamp", &timestamp,
                 "block", &block, "max-bytes", &maxBytes, nullptr);
    CHECK(live && timestamp && !block && maxBytes == pixels.size());
    GstCaps *caps = gst_app_src_get_caps(GST_APP_SRC(source.get()));
    CHECK(caps);
    CHECK(std::string(gst_structure_get_string(gst_caps_get_structure(caps, 0), "format")) == "RGB");
    int numerator = 0, denominator = 0;
    CHECK(gst_structure_get_fraction(gst_caps_get_structure(caps, 0), "framerate", &numerator, &denominator));
    CHECK(numerator == 60 && denominator == 1);
    gst_caps_unref(caps);
    g_object_get(filter.get(), "caps", &caps, nullptr);
    CHECK(caps);
    CHECK(std::string(gst_structure_get_string(gst_caps_get_structure(caps, 0), "format")) == "I420");
    gst_caps_unref(caps);
    guint bitrate = 0, keyInterval = 0, tune = 0, queueBuffers = 0, queueBytes = 1;
    gint preset = 0, leaky = 0;
    guint64 queueTime = 1;
    gboolean byteStream = FALSE, slicedThreads = FALSE;
    g_object_get(encoder.get(), "bitrate", &bitrate, "speed-preset", &preset,
                 "tune", &tune, "key-int-max", &keyInterval,
                 "byte-stream", &byteStream, "sliced-threads", &slicedThreads, nullptr);
    CHECK(bitrate == 4096 && preset == 2 && tune == 4 && keyInterval == 10 && byteStream && slicedThreads);
    g_object_get(queue.get(), "max-size-buffers", &queueBuffers, "max-size-bytes", &queueBytes,
                 "max-size-time", &queueTime, "leaky", &leaky, nullptr);
    CHECK(queueBuffers == 1 && queueBytes == 0 && queueTime == 0 && leaky == 2);
    guint mtu = 0, payloadType = 0;
    gint interval = 0, port = 0;
    gboolean sync = TRUE, async = TRUE;
    gchar *host = nullptr;
    g_object_get(payloader.get(), "mtu", &mtu, "pt", &payloadType, "config-interval", &interval, nullptr);
    CHECK(mtu == 1200 && payloadType == 96 && interval == -1);
    g_object_get(sink.get(), "host", &host, "port", &port, "sync", &sync, "async", &async, nullptr);
    const std::string actualHost(host ? host : "");
    g_free(host);
    CHECK(actualHost == "127.0.0.1" && port == 15600 && !sync && !async);

    Probe probe(source.get());
    probe.Block();
    CHECK(stream.SubmitFrame(frame));
    CHECK(Wait([&] { return probe.Count() >= 1; }));
    const auto first = probe.Last();
    CHECK(first.pts != GST_CLOCK_TIME_NONE && first.duration == GST_SECOND / 60);
    CHECK(first.stride == 64 * 3);
    std::fill(pixels.begin(), pixels.end(), 71);
    const auto before = stream.Stats().pushedFrames;
    CHECK(stream.SubmitFrame(frame));
    std::fill(pixels.begin(), pixels.end(), 255); // Original rendering memory immediately reused.
    CHECK(Wait([&] { return stream.Stats().pushedFrames > before; }));
    std::this_thread::sleep_for(30ms);
    probe.Release();
    CHECK(Wait([&] { return probe.Count() >= 2; }));
    const auto copied = probe.Last();
    CHECK(copied.bytes.size() == pixels.size());
    CHECK(std::all_of(copied.bytes.begin(), copied.bytes.end(), [](unsigned char b) { return b == 71; }));
    CHECK(copied.pts >= first.pts);
    frame.timestamp = 5s;
    const auto sampleCount = probe.Count();
    CHECK(stream.SubmitFrame(frame));
    CHECK(Wait([&] { return probe.Count() > sampleCount; }));
    CHECK(probe.Last().pts == 5 * GST_SECOND);
    frame.timestamp.reset();
  }

  // Each stream owns settings, worker, pipeline and retry state independently.
  StreamConfig secondConfig = config;
  secondConfig.cameraName = "independent";
  secondConfig.udpPort = 15602;
  secondConfig.rate = 30;
  secondConfig.bitrateKbps = 2048;
  StreamPipeline independent(secondConfig);
  CHECK(independent.SubmitFrame(frame));
  CHECK(Wait([&] { return independent.Stats().pushedFrames == 1; }));
  {
    Element pipeline(StreamPipelineTestAccess::RefPipeline(independent));
    auto sink = Find(pipeline.get(), "udpsink");
    auto encoder = Find(pipeline.get(), "x264enc");
    int port = 0; unsigned bitrate = 0;
    g_object_get(sink.get(), "port", &port, nullptr);
    g_object_get(encoder.get(), "bitrate", &bitrate, nullptr);
    CHECK(port == 15602 && bitrate == 2048);
  }

  for (bool eos : {true, false}) {
    const auto starts = stream.Stats().pipelineStarts;
    const auto failures = stream.Stats().pipelineFailures;
    PostFailure(stream, eos);
    // No frames arrive here: periodic worker polling must still consume the bus.
    CHECK(Wait([&] { return stream.Stats().pipelineFailures > failures && !stream.Stats().running; }, 500ms));
    CHECK(stream.SubmitFrame(frame));
    std::this_thread::sleep_for(100ms);
    CHECK(stream.Stats().pipelineStarts == starts);
    std::this_thread::sleep_for(1000ms);
    CHECK(stream.SubmitFrame(frame));
    CHECK(Wait([&] { return stream.Stats().pipelineStarts == starts + 1; }));
    CHECK(independent.Stats().running && independent.Stats().pipelineStarts == 1);
  }

  // Width 66 has a packed 198-byte row, different from GStreamer's default 200.
  std::vector<unsigned char> resized(66u * 48u * 3u, 64);
  FrameView resizedFrame{resized.data(), resized.size(), 66, 48, 3, PixelFormat::RGB8, {}};
  const auto starts = stream.Stats().pipelineStarts;
  CHECK(stream.SubmitFrame(resizedFrame));
  CHECK(Wait([&] { return stream.Stats().pipelineStarts == starts + 1; }));
  {
    Element pipeline(StreamPipelineTestAccess::RefPipeline(stream));
    auto source = Find(pipeline.get(), "appsrc");
    auto payloader = Find(pipeline.get(), "rtph264pay");
    Probe probe(source.get());
    Probe encoded(payloader.get());
    CHECK(stream.SubmitFrame(resizedFrame));
    CHECK(Wait([&] { return probe.Count() > 0; }));
    CHECK(probe.Last().stride == 198 && probe.Last().bytes.size() == resized.size());
    CHECK(Wait([&] { return encoded.Count() > 0; }));
    const auto rtp = encoded.Last().bytes;
    CHECK(rtp.size() >= 12 && rtp.size() <= 1200);
    CHECK((rtp[0] >> 6) == 2 && (rtp[1] & 0x7f) == 96);
    CHECK(stream.Stats().running);
  }

  // Force missing-factory fallback on every machine, including GPU runners.
  GstPluginFeature *savedNvenc = gst_registry_find_feature(gst_registry_get(), "nvh264enc", GST_TYPE_ELEMENT_FACTORY);
  if (savedNvenc)
    gst_registry_remove_feature(gst_registry_get(), savedNvenc);
  config.useCuda = true;
  config.cameraName = "fallback";
  StreamPipeline fallback(config);
  const bool accepted = fallback.SubmitFrame(frame);
  const bool fallbackStarted = Wait([&] { return fallback.Stats().pushedFrames > 0; });
  if (savedNvenc)
    gst_registry_add_feature(gst_registry_get(), savedNvenc); // Transfers the saved reference.
  CHECK(accepted && fallbackStarted);
  CHECK(fallback.Stats().nvencAttempts == 1 && fallback.Stats().nvencDisabled && !fallback.Stats().usingNvenc);

  std::atomic<bool> producing{true};
  std::thread producer([&] {
    while (producing.load())
      independent.SubmitFrame(frame);
  });
  std::this_thread::sleep_for(25ms);
  independent.Stop();
  producing = false;
  producer.join();
  independent.Stop();
  CHECK(!independent.SubmitFrame(frame) && !independent.Stats().running);
  stream.Stop();
  fallback.Stop();
  CHECK(!stream.Stats().running && !fallback.Stats().running);
  std::cout << "GStreamer configuration, ownership, timestamps, fallback, restart and shutdown passed\n";
}
