#pragma once

#include <string>

namespace gst_plane_camera
{
std::string DefaultHost();
unsigned int SensorRate(double updateRate);

// Parsed values only: no SDF or simulator types cross the adapter boundary.
struct StreamConfig
{
  std::string cameraName;
  std::string udpHost{DefaultHost()};
  int udpPort{5600};
  unsigned int rate{30};
  unsigned int bitrateKbps{16384};
  int x264SpeedPreset{1};
  bool useCuda{false};
};

void NormalizeConfig(StreamConfig &config, const std::string &defaultCameraName);
} // namespace gst_plane_camera
