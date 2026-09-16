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


#include "gst_plane_camera/adapters/gzsim/GstPlaneCameraSystem.hh"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <iostream>
#include <optional>

#include <gz/common/Console.hh>
#include <gz/plugin/Register.hh>
#include <gz/rendering/RenderEngine.hh>
#include <gz/rendering/RenderingIface.hh>
#include <gz/rendering/Sensor.hh>
#include <gz/rendering/PixelFormat.hh>
#include <gz/sim/EntityComponentManager.hh>
#include <sdf/Element.hh>
#include <gz/sim/EventManager.hh>
#include <gz/sim/components/Camera.hh>
#include <gz/sim/components/Name.hh>
#include <gz/sim/components/World.hh>
#include <gz/sim/rendering/Events.hh>

#include <sdf/Plugin.hh>

#include "gst_plane_camera/core/FrameView.hh"

using namespace custom;

namespace
{
bool IsGstConfigPlugin(const sdf::Plugin &_plugin)
{
    return _plugin.Name() == "custom::GstPlaneCameraSystem"
           || _plugin.Filename().find("GstPlaneCameraSystem") != std::string::npos;
}

template<typename T>
bool PluginValue(const sdf::Plugin &_plugin, const std::string &_name, T &_value)
{
    for (const auto &elem : _plugin.Contents()) {
        if (elem && elem->GetName() == _name) {
            try {
                _value = elem->Get<T>();
                return true;
            } catch (const std::exception &e) {
                gzwarn << "GstPlaneCameraSystem: invalid <" << _name
                       << "> value: " << e.what() << std::endl;
                return false;
            }
        }
    }

    return false;
}

bool NameMatches(const std::string &_actual, const std::string &_wanted)
{
    if (_actual == _wanted) {
        return true;
    }

    if (_actual.size() > _wanted.size()
        && _actual.compare(_actual.size() - _wanted.size(), _wanted.size(), _wanted) == 0) {
        const size_t separator = _actual.size() - _wanted.size();
        return separator >= 2 && _actual[separator - 1] == ':' && _actual[separator - 2] == ':';
    }

    return false;
}

} // namespace

//////////////////////////////////////////////////
GstPlaneCameraSystem::GstPlaneCameraSystem()
    : _callbacks(std::make_shared<CallbackState>())
{
    _callbacks->owner = this;
}

//////////////////////////////////////////////////
GstPlaneCameraSystem::~GstPlaneCameraSystem()
{
    _shuttingDown.store(true, std::memory_order_release);

    // Event dispatch may already hold a copy of a callback when disconnected.
    // The shared gate outlives this object and waits for any active callback.
    {
        std::lock_guard<std::mutex> lock(_callbacks->mutex);
        _callbacks->owner = nullptr;
    }
    _renderConnection.reset();
    _renderTeardownConnection.reset();
    _eventManager = nullptr;

    std::lock_guard<std::mutex> renderLock(_renderExecutionMutex);

    StreamList streams;
    {
        std::lock_guard<std::mutex> lock(_streamsMutex);
        streams.swap(_streams);
        streams.insert(streams.end(), _retiredStreams.begin(), _retiredStreams.end());
        _retiredStreams.clear();
        _knownCameraEntities.clear();
    }

    for (auto &stream : streams) {
        stream->removed.store(true, std::memory_order_release);
        stream->frameRequested.store(false, std::memory_order_release);
        stream->camera.reset();
        stream->pipeline->Stop();
    }

    _scene.reset();
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::Configure(const gz::sim::Entity &_entity,
                const std::shared_ptr<const sdf::Element> &,
                gz::sim::EntityComponentManager &_ecm,
                gz::sim::EventManager &_eventMgr)
{
    if (!_ecm.EntityHasComponentType(_entity, gz::sim::components::World::typeId)) {
        return;
    }

    _eventManager = &_eventMgr;
    _activeWorldInstance = true;

    const auto callbacks = _callbacks;
    _renderConnection = _eventMgr.Connect<gz::sim::events::Render>([callbacks] {
        std::lock_guard<std::mutex> lock(callbacks->mutex);
        if (callbacks->owner) { callbacks->owner->OnRender(); }
    });
    _renderTeardownConnection = _eventMgr.Connect<gz::sim::events::RenderTeardown>([callbacks] {
        std::lock_guard<std::mutex> lock(callbacks->mutex);
        if (callbacks->owner) { callbacks->owner->OnRenderTeardown(); }
    });

    std::cerr << "[GstPlaneCameraSystem] active WORLD direct-rendering-camera streamer loaded; "
             "waiting for camera entities" << std::endl;
}

//////////////////////////////////////////////////
bool GstPlaneCameraSystem::ParseStreamConfig(const sdf::Sensor &_sensor,
                                            gst_plane_camera::StreamConfig &_config) const
{
    const sdf::Plugin *gstPlugin = nullptr;
    for (const auto &plugin : _sensor.Plugins()) {
        if (IsGstConfigPlugin(plugin)) { gstPlugin = &plugin; break; }
    }
    if (!gstPlugin) { return false; }

    _config = gst_plane_camera::StreamConfig{};
    _config.cameraName = _sensor.Name();
    _config.rate = gst_plane_camera::SensorRate(_sensor.UpdateRate());
    PluginValue(*gstPlugin, "camera_name", _config.cameraName);
    PluginValue(*gstPlugin, "udp_host", _config.udpHost);
    PluginValue(*gstPlugin, "udp_port", _config.udpPort);
    PluginValue(*gstPlugin, "rate", _config.rate);
    PluginValue(*gstPlugin, "bitrate_kbps", _config.bitrateKbps);
    PluginValue(*gstPlugin, "x264_speed_preset", _config.x264SpeedPreset);
    PluginValue(*gstPlugin, "use_cuda", _config.useCuda);
    gst_plane_camera::NormalizeConfig(_config, _sensor.Name());
    return true;
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::DiscoverStreams(const gz::sim::EntityComponentManager &_ecm)
{
    _ecm.Each<gz::sim::components::Camera, gz::sim::components::Name>(
        [this](const gz::sim::Entity &_entity,
               const gz::sim::components::Camera *_cameraComp,
               const gz::sim::components::Name *_nameComp) -> bool {
            {
                std::lock_guard<std::mutex> lock(_streamsMutex);
                if (_knownCameraEntities.find(_entity) != _knownCameraEntities.end()) {
                    return true;
                }
            }

            gst_plane_camera::StreamConfig config;
            const sdf::Sensor &sensorSdf = _cameraComp->Data();
            if (!ParseStreamConfig(sensorSdf, config)) {
                return true;
            }

            auto stream = std::make_shared<StreamState>();
            stream->entity = _entity;
            stream->config = config;
            stream->pipeline = std::make_unique<gst_plane_camera::StreamPipeline>(config);

            {
                std::lock_guard<std::mutex> lock(_streamsMutex);
                if (_knownCameraEntities.find(_entity) != _knownCameraEntities.end()) {
                    return true;
                }

                for (const auto &existing : _streams) {
                    if (existing->config.udpPort == stream->config.udpPort && existing->config.udpHost == stream->config.udpHost) {
                        gzwarn << "GstPlaneCameraSystem: cameras [" << existing->config.cameraName
                               << "] and [" << stream->config.cameraName << "] share destination "
                               << stream->config.udpHost << ':' << stream->config.udpPort << std::endl;
                    }
                }

                _knownCameraEntities.insert(_entity);
                _streams.emplace_back(stream);
            }


            std::fprintf(stderr,
                "[GstPlaneCameraSystem] discovered camera entity %llu [%s] -> %s:%d @ %u FPS (%s)\n",
                static_cast<unsigned long long>(_entity), _nameComp->Data().c_str(),
                stream->config.udpHost.c_str(), stream->config.udpPort,
                stream->config.rate, stream->config.useCuda ? "NVENC requested" : "x264");

            return true;
        });
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::RemoveStreams(const gz::sim::EntityComponentManager &_ecm)
{
    StreamList removedStreams;

    _ecm.EachRemoved<gz::sim::components::Camera>(
        [this, &removedStreams](const gz::sim::Entity &_entity,
                    const gz::sim::components::Camera *) -> bool {
            std::lock_guard<std::mutex> lock(_streamsMutex);

            if (_knownCameraEntities.erase(_entity) == 0u) {
                return true;
            }

            auto it = std::remove_if(_streams.begin(), _streams.end(),
                [&_entity, &removedStreams](const StreamPtr &_stream) {
                    if (_stream->entity != _entity) {
                        return false;
                    }

                    _stream->removed.store(true, std::memory_order_release);
                    _stream->frameRequested.store(false, std::memory_order_release);
                    removedStreams.emplace_back(_stream);
                    return true;
                });

            _streams.erase(it, _streams.end());
            return true;
        });

    for (auto &stream : removedStreams) {
        stream->pipeline->Stop();
        std::fprintf(stderr, "[GstPlaneCameraSystem] removed camera entity %llu [%s]\n",
            static_cast<unsigned long long>(stream->entity), stream->config.cameraName.c_str());
    }

    if (!removedStreams.empty()) {
        std::lock_guard<std::mutex> lock(_streamsMutex);
        _retiredStreams.insert(_retiredStreams.end(),
            removedStreams.begin(), removedStreams.end());
    }
}

//////////////////////////////////////////////////
GstPlaneCameraSystem::StreamList GstPlaneCameraSystem::StreamSnapshot()
{
    std::lock_guard<std::mutex> lock(_streamsMutex);
    return _streams;
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::ReleaseRetiredStreamsOnRenderThread()
{
    StreamList retired;
    {
        std::lock_guard<std::mutex> lock(_streamsMutex);
        retired.swap(_retiredStreams);
    }

    for (auto &stream : retired) {
        stream->camera.reset();
    }
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::PostUpdate(const gz::sim::UpdateInfo &_info,
                 const gz::sim::EntityComponentManager &_ecm)
{
    if (!_activeWorldInstance || _shuttingDown.load(std::memory_order_acquire)) {
        return;
    }

    DiscoverStreams(_ecm);
    RemoveStreams(_ecm);

    auto streams = StreamSnapshot();
    if (_info.paused || !_eventManager || streams.empty()) {
        return;
    }

    ScheduleRender(_info, streams);
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::ScheduleRender(const gz::sim::UpdateInfo &_info,
                      const StreamList &_streams)
{
    for (const auto &stream : _streams) {
        if (stream->removed.load(std::memory_order_acquire)) {
            continue;
        }

        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(
                static_cast<int64_t>(1000000000ULL / stream->config.rate)));

        if (!stream->scheduleInitialized || _info.simTime < stream->lastSimTime) {
            stream->nextFrameTime = _info.simTime;
            stream->scheduleInitialized = true;
        }
        stream->lastSimTime = _info.simTime;

        if (_info.simTime >= stream->nextFrameTime) {
            stream->frameRequested.store(true, std::memory_order_release);

            const auto overdue = _info.simTime - stream->nextFrameTime;
            const auto periodsToAdvance = overdue / period + 1;
            stream->nextFrameTime += period * periodsToAdvance;
        }
    }

}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::OnRender()
{
    std::lock_guard<std::mutex> renderLock(_renderExecutionMutex);

    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }

    ReleaseRetiredStreamsOnRenderThread();
    auto streams = StreamSnapshot();
    if (streams.empty() || !FindScene()) {
        return;
    }

    static std::atomic<bool> renderCallbackReported{false};
    if (!renderCallbackReported.exchange(true, std::memory_order_acq_rel)) {
        std::cerr << "[GstPlaneCameraSystem] events::Render callback reached" << std::endl;
    }

    StreamList dueStreams;
    dueStreams.reserve(streams.size());

    for (auto &stream : streams) {
        if (stream->removed.load(std::memory_order_acquire)
            || !stream->frameRequested.exchange(false, std::memory_order_acq_rel)) {
            continue;
        }

        if (!FindExistingCamera(*stream)) {
            continue;
        }

        dueStreams.emplace_back(stream);
    }

    for (auto &stream : dueStreams) {
        if (!stream->renderCallReported) {
            std::cerr << "[GstPlaneCameraSystem] first grouped Render/PostRender/Copy ["
                  << stream->config.cameraName << "]" << std::endl;
            stream->renderCallReported = true;
        }
        stream->camera->Render();
    }

    for (auto &stream : dueStreams) {
        stream->camera->PostRender();
    }

    for (auto &stream : dueStreams) {
        CopyCameraFrame(*stream);
    }
}

//////////////////////////////////////////////////
bool GstPlaneCameraSystem::FindScene()
{
    if (_scene && _scene->IsInitialized() && _scene->RootVisual()) {
        return true;
    }

    _scene.reset();
    const auto engines = gz::rendering::loadedEngines();

    for (const auto &engineName : engines) {
        auto *engine = gz::rendering::engine(engineName);
        if (!engine || engine->SceneCount() == 0u) {
            continue;
        }

        for (unsigned int i = 0; i < engine->SceneCount(); ++i) {
            auto scene = engine->SceneByIndex(i);
            if (scene && scene->IsInitialized() && scene->RootVisual()) {
                _scene = scene;
                std::cerr << "[GstPlaneCameraSystem] using rendering scene ["
                      << _scene->Name() << "] engine [" << engineName << ']'
                      << std::endl;
                return true;
            }
        }
    }

    return false;
}

//////////////////////////////////////////////////
bool GstPlaneCameraSystem::FindExistingCamera(StreamState &_stream)
{
    if (_stream.camera) {
        return true;
    }

    if (!_scene) {
        return false;
    }

    gz::rendering::SensorPtr sensor = _scene->SensorByName(_stream.config.cameraName);

    if (!sensor) {
        for (unsigned int i = 0; i < _scene->SensorCount(); ++i) {
            auto candidate = _scene->SensorByIndex(i);
            if (candidate && NameMatches(candidate->Name(), _stream.config.cameraName)) {
                sensor = candidate;
                break;
            }
        }
    }

    if (!sensor) {
        if (!_stream.cameraMissingReported) {
            std::cerr << "[GstPlaneCameraSystem] built-in rendering camera ["
                  << _stream.config.cameraName << "] not available yet" << std::endl;
            _stream.cameraMissingReported = true;
        }
        return false;
    }

    auto camera = std::dynamic_pointer_cast<gz::rendering::Camera>(sensor);
    if (!camera) {
        gzerr << "GstPlaneCameraSystem: rendering sensor [" << sensor->Name()
              << "] is not a camera" << std::endl;
        return false;
    }

    _stream.camera = camera;
    _stream.image = camera->CreateImage();
    _stream.cameraMissingReported = false;

    std::cerr << "[GstPlaneCameraSystem] attached DIRECTLY to Gazebo rendering camera ["
          << camera->Name() << "] " << camera->ImageWidth() << 'x'
          << camera->ImageHeight()
          << " (no gz.msgs.Image subscription)" << std::endl;
    return true;
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::CopyCameraFrame(StreamState &_stream)
{
    if (!_stream.camera) {
        return;
    }

    if (_stream.camera->ImageFormat() != gz::rendering::PF_R8G8B8) {
        if (!_stream.formatWarningReported) {
            gzwarn << "GstPlaneCameraSystem: camera [" << _stream.config.cameraName
                   << "] requires R8G8B8; unsupported rendering format "
                   << gz::rendering::PixelUtil::Name(_stream.camera->ImageFormat()) << std::endl;
            _stream.formatWarningReported = true;
        }
        return;
    }
    // A resolution/format change requires a matching rendering-owned image.
    if (_stream.image.Width() != _stream.camera->ImageWidth()
        || _stream.image.Height() != _stream.camera->ImageHeight()
        || _stream.image.Format() != _stream.camera->ImageFormat()) {
        _stream.image = _stream.camera->CreateImage();
    }
    _stream.camera->Copy(_stream.image);

    const void *data = _stream.image.Data();
    if (!data) {
        if (!_stream.formatWarningReported) {
            gzwarn << "GstPlaneCameraSystem: Camera::Copy returned no data for ["
                   << _stream.config.cameraName << ']' << std::endl;
            _stream.formatWarningReported = true;
        }
        return;
    }

    const unsigned int width = _stream.image.Width();
    const unsigned int height = _stream.image.Height();
    const unsigned int depth = _stream.image.Depth();
    const uint64_t expectedBytes = static_cast<uint64_t>(width) * height * depth;

    if (_stream.image.MemorySize() < expectedBytes) {
        if (!_stream.formatWarningReported) {
            gzwarn << "GstPlaneCameraSystem: Camera::Copy returned an undersized image for ["
                   << _stream.config.cameraName << "]: " << _stream.image.MemorySize()
                   << " bytes, expected at least " << expectedBytes << std::endl;
            _stream.formatWarningReported = true;
        }
        return;
    }

    if (_stream.image.Format() != gz::rendering::PF_R8G8B8 || depth != 3u) { return; }
    // SubmitFrame copies synchronously into GstBuffer-owned storage. Neither
    // the worker nor GStreamer retains this rendering image's data pointer.
    const gst_plane_camera::FrameView frame{data, _stream.image.MemorySize(),
        width, height, depth, gst_plane_camera::PixelFormat::RGB8, std::nullopt};
    if (_stream.pipeline->SubmitFrame(frame) && !_stream.rawFrameReported) {
        std::fprintf(stderr, "[GstPlaneCameraSystem] copied camera frame [%s] %ux%u source=[Camera::Copy R8G8B8]\n",
            _stream.config.cameraName.c_str(), width, height);
        _stream.rawFrameReported = true;
    }
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::OnRenderTeardown()
{
    _shuttingDown.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> renderLock(_renderExecutionMutex);
    ReleaseRetiredStreamsOnRenderThread();
    for (auto &stream : StreamSnapshot()) {
        stream->frameRequested.store(false, std::memory_order_release);
        stream->camera.reset();
        stream->pipeline->Stop();
    }
    _scene.reset();
}

GZ_ADD_PLUGIN(GstPlaneCameraSystem,
              gz::sim::System,
              GstPlaneCameraSystem::ISystemConfigure,
              GstPlaneCameraSystem::ISystemPostUpdate)

GZ_ADD_PLUGIN_ALIAS(GstPlaneCameraSystem, "custom::GstPlaneCameraSystem")
