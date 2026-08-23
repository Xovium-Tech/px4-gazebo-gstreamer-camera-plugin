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


#include "GstPlaneCameraSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>

#include <gz/common/Console.hh>
#include <gz/plugin/Register.hh>
#include <gz/rendering/RenderEngine.hh>
#include <gz/rendering/RenderingIface.hh>
#include <gz/rendering/Sensor.hh>
#include <gz/sim/EventManager.hh>
#include <gz/sim/components/Camera.hh>
#include <gz/sim/components/Name.hh>
#include <gz/sim/components/World.hh>
#include <gz/sim/rendering/Events.hh>

#include <sdf/Plugin.hh>

#include <gst/app/gstappsrc.h>

using namespace custom;

namespace
{
constexpr unsigned int kDefaultRate = 30;
constexpr unsigned int kMinRate = 1;
constexpr unsigned int kMaxRate = 240;
constexpr unsigned int kDefaultBitrateKbps = 16384;
constexpr unsigned int kMinBitrateKbps = 64;
constexpr unsigned int kMaxBitrateKbps = 200000;
constexpr int kDefaultUdpPort = 5600;
constexpr int kDefaultX264Preset = 1;
constexpr auto kPipelineRetryDelay = std::chrono::seconds(1);

std::string DefaultHost()
{
	const char *host = std::getenv("PX4_VIDEO_HOST_IP");
	return host ? std::string(host) : std::string("127.0.0.1");
}

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

unsigned int SensorRate(const sdf::Sensor &_sensor)
{
	const double hz = _sensor.UpdateRate();
	if (!std::isfinite(hz) || hz <= 0.0) {
		return kDefaultRate;
	}

	const double clamped = std::clamp(hz,
		static_cast<double>(kMinRate),
		static_cast<double>(kMaxRate));
	return static_cast<unsigned int>(std::lround(clamped));
}
} // namespace

//////////////////////////////////////////////////
GstPlaneCameraSystem::GstPlaneCameraSystem()
{
	static std::once_flag gstInitFlag;
	std::call_once(gstInitFlag, []() { gst_init(nullptr, nullptr); });
}

//////////////////////////////////////////////////
GstPlaneCameraSystem::~GstPlaneCameraSystem()
{
	_shuttingDown.store(true, std::memory_order_release);

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
		StopFrameWorker(*stream);
		StopPipeline(*stream);
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

	_renderConnection = _eventMgr.Connect<gz::sim::events::Render>(
		std::bind(&GstPlaneCameraSystem::OnRender, this));
	_renderTeardownConnection = _eventMgr.Connect<gz::sim::events::RenderTeardown>(
		std::bind(&GstPlaneCameraSystem::OnRenderTeardown, this));

	std::cerr << "[GstPlaneCameraSystem] active WORLD direct-rendering-camera streamer loaded; "
		     "waiting for camera entities" << std::endl;
}

//////////////////////////////////////////////////
bool GstPlaneCameraSystem::ParseStreamConfig(const sdf::Sensor &_sensor,
					StreamState &_stream) const
{
	const sdf::Plugin *gstPlugin = nullptr;

	for (const auto &plugin : _sensor.Plugins()) {
		if (IsGstConfigPlugin(plugin)) {
			gstPlugin = &plugin;
			break;
		}
	}

	if (!gstPlugin) {
		return false;
	}

	_stream.cameraName = _sensor.Name();
	_stream.udpHost = DefaultHost();
	_stream.udpPort = kDefaultUdpPort;
	_stream.rate = SensorRate(_sensor);
	_stream.bitrateKbps = kDefaultBitrateKbps;
	_stream.x264SpeedPreset = kDefaultX264Preset;
	_stream.useCuda = false;

	PluginValue(*gstPlugin, "camera_name", _stream.cameraName);
	PluginValue(*gstPlugin, "udp_host", _stream.udpHost);
	PluginValue(*gstPlugin, "udp_port", _stream.udpPort);
	PluginValue(*gstPlugin, "rate", _stream.rate);
	PluginValue(*gstPlugin, "bitrate_kbps", _stream.bitrateKbps);
	PluginValue(*gstPlugin, "x264_speed_preset", _stream.x264SpeedPreset);
	PluginValue(*gstPlugin, "use_cuda", _stream.useCuda);

	if (_stream.cameraName.empty()) {
		_stream.cameraName = _sensor.Name();
	}

	if (_stream.udpHost.empty()) {
		_stream.udpHost = DefaultHost();
	}

	if (_stream.udpPort < 1 || _stream.udpPort > 65535) {
		gzwarn << "GstPlaneCameraSystem: invalid UDP port " << _stream.udpPort
		       << " for [" << _stream.cameraName << "], using "
		       << kDefaultUdpPort << std::endl;
		_stream.udpPort = kDefaultUdpPort;
	}

	const unsigned int requestedRate = _stream.rate;
	_stream.rate = std::clamp(_stream.rate, kMinRate, kMaxRate);
	if (_stream.rate != requestedRate) {
		gzwarn << "GstPlaneCameraSystem: rate " << requestedRate << " for ["
		       << _stream.cameraName << "] is outside " << kMinRate << ".."
		       << kMaxRate << " FPS; using " << _stream.rate << std::endl;
	}

	const unsigned int requestedBitrate = _stream.bitrateKbps;
	_stream.bitrateKbps = std::clamp(_stream.bitrateKbps,
		kMinBitrateKbps, kMaxBitrateKbps);
	if (_stream.bitrateKbps != requestedBitrate) {
		gzwarn << "GstPlaneCameraSystem: bitrate " << requestedBitrate << " kbit/s for ["
		       << _stream.cameraName << "] is outside " << kMinBitrateKbps << ".."
		       << kMaxBitrateKbps << "; using " << _stream.bitrateKbps << std::endl;
	}

	_stream.x264SpeedPreset = std::clamp(_stream.x264SpeedPreset, 1, 10);
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

			StreamState config;
			const sdf::Sensor &sensorSdf = _cameraComp->Data();
			if (!ParseStreamConfig(sensorSdf, config)) {
				return true;
			}

			auto stream = std::make_shared<StreamState>();
			stream->entity = _entity;
			stream->cameraName = config.cameraName.empty() ? _nameComp->Data() : config.cameraName;
			stream->udpHost = config.udpHost;
			stream->udpPort = config.udpPort;
			stream->rate = config.rate;
			stream->bitrateKbps = config.bitrateKbps;
			stream->x264SpeedPreset = config.x264SpeedPreset;
			stream->useCuda = config.useCuda;

			{
				std::lock_guard<std::mutex> lock(_streamsMutex);
				if (_knownCameraEntities.find(_entity) != _knownCameraEntities.end()) {
					return true;
				}

				for (const auto &existing : _streams) {
					if (existing->udpPort == stream->udpPort && existing->udpHost == stream->udpHost) {
						gzwarn << "GstPlaneCameraSystem: cameras [" << existing->cameraName
						       << "] and [" << stream->cameraName << "] share destination "
						       << stream->udpHost << ':' << stream->udpPort << std::endl;
					}
				}

				_knownCameraEntities.insert(_entity);
				_streams.emplace_back(stream);
			}

			StartFrameWorker(stream);

			std::cerr << "[GstPlaneCameraSystem] discovered camera entity " << _entity
				  << " [" << _nameComp->Data() << "] -> "
				  << stream->udpHost << ':' << stream->udpPort
				  << " @ " << stream->rate << " FPS"
				  << (stream->useCuda ? " (NVENC requested)" : " (x264)")
				  << std::endl;

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
		StopFrameWorker(*stream);
		StopPipeline(*stream);
		std::cerr << "[GstPlaneCameraSystem] removed camera entity " << stream->entity
			  << " [" << stream->cameraName << "]" << std::endl;
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
	for (auto &stream : streams) {
		if (!stream->removed.load(std::memory_order_acquire)) {
			PollBus(*stream);
		}
	}

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
				static_cast<int64_t>(1000000000ULL / stream->rate)));

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
				  << stream->cameraName << "]" << std::endl;
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

	gz::rendering::SensorPtr sensor = _scene->SensorByName(_stream.cameraName);

	if (!sensor) {
		for (unsigned int i = 0; i < _scene->SensorCount(); ++i) {
			auto candidate = _scene->SensorByIndex(i);
			if (candidate && NameMatches(candidate->Name(), _stream.cameraName)) {
				sensor = candidate;
				break;
			}
		}
	}

	if (!sensor) {
		if (!_stream.cameraMissingReported) {
			std::cerr << "[GstPlaneCameraSystem] built-in rendering camera ["
				  << _stream.cameraName << "] not available yet" << std::endl;
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

	_stream.camera->Copy(_stream.image);

	const void *data = _stream.image.Data();
	if (!data) {
		if (!_stream.formatWarningReported) {
			gzwarn << "GstPlaneCameraSystem: Camera::Copy returned no data for ["
			       << _stream.cameraName << ']' << std::endl;
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
			       << _stream.cameraName << "]: " << _stream.image.MemorySize()
			       << " bytes, expected at least " << expectedBytes << std::endl;
			_stream.formatWarningReported = true;
		}
		return;
	}

	OnNewFrame(_stream, data, width, height, depth, "Camera::Copy");
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::StartFrameWorker(const StreamPtr &_stream)
{
	if (!_stream || _stream->frameWorker.joinable()) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(_stream->frameMutex);
		_stream->frameWorkerStop = false;
		_stream->frameReady = false;
	}

	_stream->frameWorker = std::thread([this, _stream]() { FrameWorker(_stream); });
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::StopFrameWorker(StreamState &_stream)
{
	{
		std::lock_guard<std::mutex> lock(_stream.frameMutex);
		_stream.frameWorkerStop = true;
		_stream.frameReady = false;
	}
	_stream.frameCv.notify_all();

	if (_stream.frameWorker.joinable()) {
		_stream.frameWorker.join();
	}

	std::lock_guard<std::mutex> lock(_stream.frameMutex);
	_stream.pendingFrame.clear();
	_stream.pendingWidth = 0;
	_stream.pendingHeight = 0;
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::FrameWorker(const StreamPtr &_stream)
{
	std::vector<uint8_t> frame;

	while (!_shuttingDown.load(std::memory_order_acquire)) {
		unsigned int width = 0;
		unsigned int height = 0;

		{
			std::unique_lock<std::mutex> lock(_stream->frameMutex);
			_stream->frameCv.wait(lock, [this, _stream]() {
				return _stream->frameWorkerStop || _stream->frameReady
			       || _shuttingDown.load(std::memory_order_acquire);
			});

			if (_stream->frameWorkerStop || _shuttingDown.load(std::memory_order_acquire)) {
				break;
			}

			frame.swap(_stream->pendingFrame);
			width = _stream->pendingWidth;
			height = _stream->pendingHeight;
			_stream->frameReady = false;
		}

		if (_stream->removed.load(std::memory_order_acquire) || frame.empty()) {
			continue;
		}

		if (!StartPipeline(*_stream, width, height)) {
			continue;
		}

		GstBuffer *buffer = gst_buffer_new_allocate(nullptr,
			static_cast<gsize>(frame.size()), nullptr);
		if (!buffer) {
			continue;
		}

		if (gst_buffer_fill(buffer, 0, frame.data(), frame.size()) != frame.size()) {
			gst_buffer_unref(buffer);
			continue;
		}

		GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, _stream->rate);
		_stream->frameIndex.fetch_add(1, std::memory_order_relaxed);

		GstElement *source = nullptr;
		{
			std::lock_guard<std::mutex> lock(_stream->gstMutex);
			if (_stream->source) {
				source = GST_ELEMENT(gst_object_ref(_stream->source));
			}
		}

		if (!source) {
			gst_buffer_unref(buffer);
			continue;
		}

		const GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
		gst_object_unref(source);

		if (ret != GST_FLOW_OK && ret != GST_FLOW_FLUSHING) {
			gzerr << "GstPlaneCameraSystem: appsrc push failed for [" << _stream->cameraName
			      << "]: " << ret << std::endl;
			HandlePipelineFailure(*_stream, false);
		}
	}
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::OnNewFrame(StreamState &_stream,
				 const void *_image,
				 unsigned int _width,
				 unsigned int _height,
				 unsigned int _depth,
				 const std::string &_format)
{
	if (_shuttingDown.load(std::memory_order_acquire)
	    || _stream.removed.load(std::memory_order_acquire) || !_image) {
		return;
	}

	if (!_stream.rawFrameReported) {
		std::cerr << "[GstPlaneCameraSystem] copied camera frame [" << _stream.cameraName
			  << "] " << _width << 'x' << _height << " depth=" << _depth
			  << " source=[" << _format << "]" << std::endl;
		_stream.rawFrameReported = true;
	}

	if (_depth != 3u) {
		if (!_stream.formatWarningReported) {
			gzwarn << "GstPlaneCameraSystem: camera [" << _stream.cameraName
			       << "] returned depth " << _depth << " format [" << _format << ']'
			       << std::endl;
			_stream.formatWarningReported = true;
		}
		return;
	}

	const size_t bytes = static_cast<size_t>(_width) * _height * 3u;
	{
		std::lock_guard<std::mutex> lock(_stream.frameMutex);
		if (_stream.frameWorkerStop) {
			return;
		}

		if (_stream.frameReady) {
			_stream.droppedFrames.fetch_add(1, std::memory_order_relaxed);
		}

		_stream.pendingFrame.resize(bytes);
		std::memcpy(_stream.pendingFrame.data(), _image, bytes);
		_stream.pendingWidth = _width;
		_stream.pendingHeight = _height;
		_stream.frameReady = true;
	}

	_stream.frameCv.notify_one();
}

//////////////////////////////////////////////////
bool GstPlaneCameraSystem::StartPipeline(StreamState &_stream,
				    unsigned int _width,
				    unsigned int _height)
{
	std::lock_guard<std::mutex> lock(_stream.gstMutex);

	if (_stream.removed.load(std::memory_order_acquire)) {
		return false;
	}

	if (_stream.pipeline) {
		if (_stream.width == _width && _stream.height == _height) {
			return true;
		}

		StopPipelineLocked(_stream);
	}

	const auto now = std::chrono::steady_clock::now();
	if (_stream.nextPipelineRetry != std::chrono::steady_clock::time_point{}
	    && now < _stream.nextPipelineRetry) {
		return false;
	}

	if (_stream.useCuda && !_stream.nvencFailed) {
		if (BuildPipelineLocked(_stream, _width, _height, true)) {
			return true;
		}

		_stream.nvencFailed = true;
		gzwarn << "GstPlaneCameraSystem: nvh264enc unavailable or failed for ["
		       << _stream.cameraName << "], falling back to x264" << std::endl;
	}

	if (BuildPipelineLocked(_stream, _width, _height, false)) {
		return true;
	}

	_stream.nextPipelineRetry = now + kPipelineRetryDelay;
	return false;
}

//////////////////////////////////////////////////
bool GstPlaneCameraSystem::BuildPipelineLocked(StreamState &_stream,
					       unsigned int _width,
					       unsigned int _height,
					       bool _useNvenc)
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
			gzerr << "GstPlaneCameraSystem: failed to create GStreamer elements for ["
			      << _stream.cameraName << "]" << std::endl;
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
		"framerate", GST_TYPE_FRACTION, _stream.rate, 1,
		nullptr);

	GstCaps *encoderCaps = gst_caps_new_simple("video/x-raw",
		"format", G_TYPE_STRING, _useNvenc ? "NV12" : "I420",
		"width", G_TYPE_INT, static_cast<int>(_width),
		"height", G_TYPE_INT, static_cast<int>(_height),
		"framerate", GST_TYPE_FRACTION, _stream.rate, 1,
		nullptr);

	if (!sourceCaps || !encoderCaps) {
		gzerr << "GstPlaneCameraSystem: failed to create video caps for ["
		      << _stream.cameraName << "]" << std::endl;
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
		SetUintIfPresent(encoder, "bitrate", _stream.bitrateKbps);
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
			static_cast<guint>((static_cast<uint64_t>(_stream.bitrateKbps) * 2u)
				/ std::max(1u, _stream.rate)));
		SetUintIfPresent(encoder, "vbv-buffer-size", vbvKbits);
	} else {
		g_object_set(G_OBJECT(encoder),
			"bitrate", _stream.bitrateKbps,
			"speed-preset", _stream.x264SpeedPreset,
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
		"host", _stream.udpHost.c_str(),
		"port", _stream.udpPort,
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
			gzerr << "GstPlaneCameraSystem: failed to link GStreamer pipeline for ["
			      << _stream.cameraName << "]" << std::endl;
		}
		gst_element_set_state(pipeline, GST_STATE_NULL);
		gst_object_unref(pipeline);
		return false;
	}

	if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		if (!_useNvenc) {
			gzerr << "GstPlaneCameraSystem: failed to start GStreamer pipeline for ["
			      << _stream.cameraName << "]" << std::endl;
		}
		gst_element_set_state(pipeline, GST_STATE_NULL);
		gst_object_unref(pipeline);
		return false;
	}

	_stream.pipeline = pipeline;
	_stream.source = source;
	_stream.bus = gst_element_get_bus(pipeline);
	_stream.width = _width;
	_stream.height = _height;
	_stream.frameIndex.store(0, std::memory_order_relaxed);
	_stream.usingNvenc = _useNvenc;
	_stream.nextPipelineRetry = {};

	std::cerr << "[GstPlaneCameraSystem] STREAMING [" << _stream.cameraName << "] "
		  << _width << 'x' << _height << " @ " << _stream.rate << " FPS -> "
		  << _stream.udpHost << ':' << _stream.udpPort << " using "
		  << (_useNvenc ? "nvh264enc" : "x264") << std::endl;
	return true;
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::StopPipeline(StreamState &_stream)
{
	std::lock_guard<std::mutex> lock(_stream.gstMutex);
	StopPipelineLocked(_stream);
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::StopPipelineLocked(StreamState &_stream)
{
	_stream.source = nullptr;

	if (_stream.pipeline) {
		gst_element_set_state(_stream.pipeline, GST_STATE_NULL);
	}

	if (_stream.bus) {
		gst_object_unref(_stream.bus);
		_stream.bus = nullptr;
	}

	if (_stream.pipeline) {
		gst_object_unref(_stream.pipeline);
		_stream.pipeline = nullptr;
	}

	_stream.width = 0;
	_stream.height = 0;
	_stream.frameIndex.store(0, std::memory_order_relaxed);
	_stream.usingNvenc = false;
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::HandlePipelineFailure(StreamState &_stream, bool _disableNvenc)
{
	std::lock_guard<std::mutex> lock(_stream.gstMutex);

	if (_disableNvenc && _stream.usingNvenc) {
		_stream.nvencFailed = true;
	}

	StopPipelineLocked(_stream);
	_stream.nextPipelineRetry = std::chrono::steady_clock::now() + kPipelineRetryDelay;
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::PollBus(StreamState &_stream)
{
	GstBus *bus = nullptr;
	bool busUsingNvenc = false;
	{
		std::lock_guard<std::mutex> lock(_stream.gstMutex);
		if (_stream.bus) {
			bus = GST_BUS(gst_object_ref(_stream.bus));
			busUsingNvenc = _stream.usingNvenc;
		}
	}

	if (!bus) {
		return;
	}

	bool fatal = false;

	while (GstMessage *msg = gst_bus_pop_filtered(bus,
		static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS))) {
		if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
			GError *error = nullptr;
			gchar *debug = nullptr;
			gst_message_parse_error(msg, &error, &debug);
			gzerr << "GstPlaneCameraSystem GStreamer error [" << _stream.cameraName
			      << "]: " << (error ? error->message : "unknown") << std::endl;
			if (debug) {
				gzerr << "  debug: " << debug << std::endl;
			}
			if (error) { g_error_free(error); }
			if (debug) { g_free(debug); }
			fatal = true;
		} else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
			GError *error = nullptr;
			gchar *debug = nullptr;
			gst_message_parse_warning(msg, &error, &debug);
			gzwarn << "GstPlaneCameraSystem GStreamer warning [" << _stream.cameraName
			       << "]: " << (error ? error->message : "unknown") << std::endl;
			if (debug) {
				gzwarn << "  debug: " << debug << std::endl;
			}
			if (error) { g_error_free(error); }
			if (debug) { g_free(debug); }
		} else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
			gzwarn << "GstPlaneCameraSystem: EOS [" << _stream.cameraName << ']' << std::endl;
			fatal = true;
		}
		gst_message_unref(msg);
	}

	bool disabledNvenc = false;
	if (fatal) {
		std::lock_guard<std::mutex> lock(_stream.gstMutex);

		if (_stream.bus == bus) {
			if (busUsingNvenc) {
				_stream.nvencFailed = true;
				disabledNvenc = true;
			}

			StopPipelineLocked(_stream);
			_stream.nextPipelineRetry =
				std::chrono::steady_clock::now() + kPipelineRetryDelay;
		}
	}

	gst_object_unref(bus);

	if (disabledNvenc) {
		gzwarn << "GstPlaneCameraSystem: disabling NVENC for [" << _stream.cameraName
		       << "] after a runtime encoder failure" << std::endl;
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
	}

	_scene.reset();
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::ConfigureLeakyQueue(GstElement *_queue)
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
void GstPlaneCameraSystem::SetUintIfPresent(GstElement *_element,
				       const char *_property,
				       guint _value)
{
	if (_element && g_object_class_find_property(G_OBJECT_GET_CLASS(_element), _property)) {
		g_object_set(G_OBJECT(_element), _property, _value, nullptr);
	}
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::SetIntIfPresent(GstElement *_element,
				       const char *_property,
				       gint _value)
{
	if (_element && g_object_class_find_property(G_OBJECT_GET_CLASS(_element), _property)) {
		g_object_set(G_OBJECT(_element), _property, _value, nullptr);
	}
}

//////////////////////////////////////////////////
void GstPlaneCameraSystem::SetEnumIfPresent(GstElement *_element,
				        const char *_property,
				        gint _value)
{
	if (_element && g_object_class_find_property(G_OBJECT_GET_CLASS(_element), _property)) {
		g_object_set(G_OBJECT(_element), _property, _value, nullptr);
	}
}

bool GstPlaneCameraSystem::SetEnumNickIfPresent(
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
void GstPlaneCameraSystem::SetBoolIfPresent(GstElement *_element,
				       const char *_property,
				       gboolean _value)
{
	if (_element && g_object_class_find_property(G_OBJECT_GET_CLASS(_element), _property)) {
		g_object_set(G_OBJECT(_element), _property, _value, nullptr);
	}
}

GZ_ADD_PLUGIN(GstPlaneCameraSystem,
	      gz::sim::System,
	      GstPlaneCameraSystem::ISystemConfigure,
	      GstPlaneCameraSystem::ISystemPostUpdate)

GZ_ADD_PLUGIN_ALIAS(GstPlaneCameraSystem, "custom::GstPlaneCameraSystem")
