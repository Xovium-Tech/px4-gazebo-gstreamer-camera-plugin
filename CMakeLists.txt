cmake_minimum_required(VERSION 3.16)
project(GstPlaneCameraSystem LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)
include(cmake/GazeboBackend.cmake)
gst_plane_camera_validate_backend()

option(GST_PLANE_CAMERA_BUILD_PLUGIN "Build the selected Gazebo adapter" ON)
option(GST_PLANE_CAMERA_RUNTIME_TESTS "Enable rendering-dependent simulator smoke tests" OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
  gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

# The streaming implementation has no Gazebo headers or link dependencies.
# PIC permits this static core to be linked into the shared Gazebo plugin.
add_library(gst_plane_camera_core STATIC
  src/core/StreamConfig.cc
  src/core/StreamPipeline.cc)
target_compile_features(gst_plane_camera_core PUBLIC cxx_std_17)
target_include_directories(gst_plane_camera_core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(gst_plane_camera_core PUBLIC
  PkgConfig::GSTREAMER Threads::Threads)
set_target_properties(gst_plane_camera_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(GST_PLANE_CAMERA_BUILD_PLUGIN)
  add_library(GstPlaneCameraSystem SHARED
    src/adapters/gzsim/GstPlaneCameraSystem.cc)
  target_link_libraries(GstPlaneCameraSystem PRIVATE gst_plane_camera_core)
  gst_plane_camera_configure_backend(GstPlaneCameraSystem)
  set_target_properties(GstPlaneCameraSystem PROPERTIES
    OUTPUT_NAME GstPlaneCameraSystem POSITION_INDEPENDENT_CODE ON)

  set(GST_PLANE_CAMERA_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}" CACHE PATH
    "Install directory for libGstPlaneCameraSystem.so")
  install(TARGETS GstPlaneCameraSystem
    LIBRARY DESTINATION "${GST_PLANE_CAMERA_INSTALL_DIR}")
endif()

if(BUILD_TESTING)
  add_subdirectory(tests/core)
  add_test(NAME cmake.backend_selection
    COMMAND "${CMAKE_COMMAND}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/backend-selection-test"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/BackendSelection.cmake")

  if(GST_PLANE_CAMERA_BUILD_PLUGIN AND UNIX)
    add_test(NAME plugin.linkage
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke/check_linkage.sh"
        "${GZ_DISTRO}" "$<TARGET_FILE:GstPlaneCameraSystem>")
    if(GST_PLANE_CAMERA_RUNTIME_TESTS)
      add_test(NAME plugin.runtime
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke/run.sh"
          "${GZ_DISTRO}" "$<TARGET_FILE_DIR:GstPlaneCameraSystem>")
      set_tests_properties(plugin.runtime PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 90 LABELS "runtime")
    endif()
  endif()
endif()
