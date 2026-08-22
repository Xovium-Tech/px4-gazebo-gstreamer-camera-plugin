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

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gz/common/Event.hh>
#include <gz/rendering/Camera.hh>
#include <gz/rendering/Image.hh>
#include <gz/rendering/Scene.hh>
#include <gz/sim/Entity.hh>
#include <gz/sim/System.hh>

#include <sdf/Sensor.hh>

#include <gst/gst.h>

namespace custom
{

class GstPlaneCameraSystem :
	public gz::sim::System,
	public gz::sim::ISystemConfigure,
	public gz::sim::ISystemPostUpdate
{
public:
	GstPlaneCameraSystem();
	~GstPlaneCameraSystem() override;

	void Configure(const gz::sim::Entity &_entity,
		       const std::shared_ptr<const sdf::Element> &_sdf,
		       gz::sim::EntityComponentManager &_ecm,
		       gz::sim::EventManager &_eventMgr) override;

	void PostUpdate(const gz::sim::UpdateInfo &_info,
			const gz::sim::EntityComponentManager &_ecm) override;

private:
	struct StreamState {
		gz::sim::Entity entity{gz::sim::kNullEntity};

		std::string cameraName;
		std::string udpHost{"127.0.0.1"};
		int udpPort{5600};
		unsigned int rate{30};
		unsigned int bitrateKbps{16384};
		int x264SpeedPreset{1};
		bool useCuda{false};

		gz::rendering::CameraPtr camera;
		gz::rendering::Image image;

		std::mutex gstMutex;
		GstElement *pipeline{nullptr};
		GstElement *source{nullptr};
		GstBus *bus{nullptr};
		unsigned int width{0};
		unsigned int height{0};
		std::atomic<uint64_t> frameIndex{0};
		bool usingNvenc{false};
		bool usingGpuConvert{false};
		bool gpuConvertFailed{false};
		bool nvencFailed{false};
		std::chrono::steady_clock::time_point nextPipelineRetry{};

		std::mutex frameMutex;
		std::condition_variable frameCv;
		std::vector<uint8_t> pendingFrame;
		unsigned int pendingWidth{0};
		unsigned int pendingHeight{0};
		bool frameReady{false};
		bool frameWorkerStop{false};
		std::thread frameWorker;
		std::atomic<uint64_t> droppedFrames{0};

		std::chrono::steady_clock::duration nextFrameTime{};
		std::chrono::steady_clock::duration lastSimTime{};
		bool scheduleInitialized{false};
		std::atomic<bool> frameRequested{false};
		std::atomic<bool> removed{false};

		bool formatWarningReported{false};
		bool renderCallReported{false};
		bool cameraMissingReported{false};
		bool rawFrameReported{false};
	};

	using StreamPtr = std::shared_ptr<StreamState>;
	using StreamList = std::vector<StreamPtr>;

	void DiscoverStreams(const gz::sim::EntityComponentManager &_ecm);
	void RemoveStreams(const gz::sim::EntityComponentManager &_ecm);
	StreamList StreamSnapshot();
	void ReleaseRetiredStreamsOnRenderThread();
	bool ParseStreamConfig(const sdf::Sensor &_sensor, StreamState &_stream) const;

	void ScheduleRender(const gz::sim::UpdateInfo &_info, const StreamList &_streams);
	void OnRender();
	void OnRenderTeardown();
	bool FindScene();
	bool FindExistingCamera(StreamState &_stream);
	void RenderCamera(StreamState &_stream);

	void StartFrameWorker(const StreamPtr &_stream);
	void StopFrameWorker(StreamState &_stream);
	void FrameWorker(const StreamPtr &_stream);

	void OnNewFrame(StreamState &_stream,
			const void *_image,
			unsigned int _width,
			unsigned int _height,
			unsigned int _depth,
			const std::string &_format);

	bool StartPipeline(StreamState &_stream,
			   unsigned int _width,
			   unsigned int _height);
	bool BuildPipelineLocked(StreamState &_stream,
				 unsigned int _width,
				 unsigned int _height,
				 bool _useNvenc,
				 bool _gpuConvert);
	void StopPipeline(StreamState &_stream);
	void StopPipelineLocked(StreamState &_stream);
	void HandlePipelineFailure(StreamState &_stream, bool _disableNvenc);
	void PollBus(StreamState &_stream);

	static void ConfigureLeakyQueue(GstElement *_queue);
	static void SetUintIfPresent(GstElement *_element,
				     const char *_property,
				     guint _value);
	static void SetIntIfPresent(GstElement *_element,
				    const char *_property,
				    gint _value);
	static void SetEnumIfPresent(GstElement *_element,
				     const char *_property,
				     gint _value);
        static bool SetEnumNickIfPresent(GstElement *_element,
                                 const char *_property,
                                 const char *_nick);
	static void SetBoolIfPresent(GstElement *_element,
				     const char *_property,
				     gboolean _value);

	gz::sim::EventManager *_eventManager{nullptr};
	gz::rendering::ScenePtr _scene;

	std::mutex _streamsMutex;
	StreamList _streams;
	StreamList _retiredStreams;
	std::unordered_set<gz::sim::Entity> _knownCameraEntities;

	std::mutex _renderExecutionMutex;
	std::mutex _renderConnectionMutex;
	gz::common::ConnectionPtr _renderConnection;
	gz::common::ConnectionPtr _renderTeardownConnection;
	std::atomic<bool> _renderPending{false};
	std::atomic<bool> _renderCallbackDone{false};
	std::atomic<bool> _shuttingDown{false};
	bool _activeWorldInstance{false};
};

} // namespace custom
