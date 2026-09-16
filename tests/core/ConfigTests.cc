// These expectations were recorded from ParseStreamConfig / SensorRate in the
// original Harmonic implementation before extracting the shared streaming core.
#include "gst_plane_camera/core/StreamConfig.hh"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; return 1; \
} } while (false)

int main()
{
  using namespace gst_plane_camera;
  const char *previous = std::getenv("PX4_VIDEO_HOST_IP");
  const std::string saved = previous ? previous : "";
  const bool hadPrevious = previous != nullptr;
  struct Restore {
    bool present;
    std::string value;
    ~Restore() { if (present) setenv("PX4_VIDEO_HOST_IP", value.c_str(), 1);
                 else unsetenv("PX4_VIDEO_HOST_IP"); }
  } restore{hadPrevious, saved};

  unsetenv("PX4_VIDEO_HOST_IP");
  StreamConfig config;
  CHECK(config.udpHost == "127.0.0.1");
  CHECK(config.udpPort == 5600 && config.rate == 30);
  CHECK(config.bitrateKbps == 16384 && config.x264SpeedPreset == 1);
  CHECK(!config.useCuda);
  CHECK(SensorRate(0) == 30 && SensorRate(-1) == 30);
  CHECK(SensorRate(std::numeric_limits<double>::infinity()) == 30);
  CHECK(SensorRate(std::numeric_limits<double>::quiet_NaN()) == 30);
  CHECK(SensorRate(0.1) == 1 && SensorRate(29.5) == 30);
  CHECK(SensorRate(29.49) == 29 && SensorRate(500) == 240);

  setenv("PX4_VIDEO_HOST_IP", "192.0.2.7", 1);
  CHECK(DefaultHost() == "192.0.2.7");
  CHECK(StreamConfig{}.udpHost == "192.0.2.7");
  config.udpHost = "198.51.100.9";
  NormalizeConfig(config, "camera");
  CHECK(config.cameraName == "camera" && config.udpHost == "198.51.100.9");
  config.cameraName = "explicit_camera";
  config.udpHost.clear();
  NormalizeConfig(config, "fallback_camera");
  CHECK(config.cameraName == "explicit_camera" && config.udpHost == "192.0.2.7");

  for (int port : {-1, 0, 65536}) {
    config.udpPort = port;
    NormalizeConfig(config, "camera");
    CHECK(config.udpPort == 5600);
  }
  for (int port : {1, 5606, 65535}) {
    config.udpPort = port;
    NormalizeConfig(config, "camera");
    CHECK(config.udpPort == port);
  }
  config.rate = 0;
  config.bitrateKbps = 0;
  config.x264SpeedPreset = -1;
  NormalizeConfig(config, "camera");
  CHECK(config.rate == 1 && config.bitrateKbps == 64 && config.x264SpeedPreset == 1);
  config.rate = 241;
  config.bitrateKbps = 200001;
  config.x264SpeedPreset = 11;
  NormalizeConfig(config, "camera");
  CHECK(config.rate == 240 && config.bitrateKbps == 200000 && config.x264SpeedPreset == 10);
  config.rate = 60;
  config.bitrateKbps = 4096;
  config.x264SpeedPreset = 5;
  NormalizeConfig(config, "camera");
  CHECK(config.rate == 60 && config.bitrateKbps == 4096 && config.x264SpeedPreset == 5);

  // Existing behavior intentionally distinguishes an unset and empty environment.
  setenv("PX4_VIDEO_HOST_IP", "", 1);
  CHECK(DefaultHost().empty());
  std::cout << "Configuration baseline passed\n";
}
