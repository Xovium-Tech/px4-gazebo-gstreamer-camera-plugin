#include "gst_plane_camera/core/StreamConfig.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace gst_plane_camera
{
std::string DefaultHost()
{
  const char *host = std::getenv("PX4_VIDEO_HOST_IP");
  return host ? std::string(host) : std::string("127.0.0.1");
}

unsigned int SensorRate(double updateRate)
{
  if (!std::isfinite(updateRate) || updateRate <= 0.0)
    return 30;
  return static_cast<unsigned int>(std::lround(std::clamp(updateRate, 1.0, 240.0)));
}

void NormalizeConfig(StreamConfig &config, const std::string &defaultCameraName)
{
  if (config.cameraName.empty())
    config.cameraName = defaultCameraName;
  if (config.udpHost.empty())
    config.udpHost = DefaultHost();
  if (config.udpPort < 1 || config.udpPort > 65535) {
    std::cerr << "GstPlaneCameraSystem: invalid UDP port " << config.udpPort
              << " for [" << config.cameraName << "], using 5600\n";
    config.udpPort = 5600;
  }
  const auto requestedRate = config.rate;
  config.rate = std::clamp(config.rate, 1u, 240u);
  if (config.rate != requestedRate)
    std::cerr << "GstPlaneCameraSystem: rate " << requestedRate << " for ["
              << config.cameraName << "] is outside 1..240 FPS; using " << config.rate << '\n';
  const auto requestedBitrate = config.bitrateKbps;
  config.bitrateKbps = std::clamp(config.bitrateKbps, 64u, 200000u);
  if (config.bitrateKbps != requestedBitrate)
    std::cerr << "GstPlaneCameraSystem: bitrate " << requestedBitrate << " kbit/s for ["
              << config.cameraName << "] is outside 64..200000; using " << config.bitrateKbps << '\n';
  config.x264SpeedPreset = std::clamp(config.x264SpeedPreset, 1, 10);
}
} // namespace gst_plane_camera
