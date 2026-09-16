include_guard(GLOBAL)

function(gst_plane_camera_validate_backend)
  set(GZ_DISTRO "harmonic" CACHE STRING "Gazebo backend: harmonic or jetty")
  set_property(CACHE GZ_DISTRO PROPERTY STRINGS harmonic jetty)
  if(NOT GZ_DISTRO MATCHES "^(harmonic|jetty)$")
    message(FATAL_ERROR "Unknown GZ_DISTRO='${GZ_DISTRO}'. Choose harmonic or jetty.")
  endif()
  if(DEFINED GST_PLANE_CAMERA_CONFIGURED_DISTRO AND
      NOT GST_PLANE_CAMERA_CONFIGURED_DISTRO STREQUAL GZ_DISTRO)
    message(FATAL_ERROR
      "This build directory was configured for '${GST_PLANE_CAMERA_CONFIGURED_DISTRO}', "
      "but GZ_DISTRO is now '${GZ_DISTRO}'. Use a separate build-${GZ_DISTRO} directory; "
      "Gazebo major versions must never share a CMake cache or plugin binary.")
  endif()
  set(GST_PLANE_CAMERA_CONFIGURED_DISTRO "${GZ_DISTRO}" CACHE INTERNAL
    "Gazebo distribution originally selected for this build directory")
endfunction()

# Find-package version files are not uniformly strict about major versions.
# Verify the resolved config version as well as requesting the correct family.
function(gst_plane_camera_require_major package expected_major)
  set(version "${${package}_VERSION}")
  if(NOT version MATCHES "^${expected_major}([.]|$)")
    message(FATAL_ERROR
      "GZ_DISTRO=${GZ_DISTRO} requires ${package} major ${expected_major}; "
      "the selected package reports '${version}'. Check CMAKE_PREFIX_PATH and package *_DIR entries.")
  endif()
endfunction()

function(gst_plane_camera_resolve_target output)
  foreach(candidate IN LISTS ARGN)
    if(TARGET "${candidate}")
      set("${output}" "${candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "Package did not export any supported imported target: ${ARGN}")
endfunction()

function(gst_plane_camera_configure_backend target)
  if(GZ_DISTRO STREQUAL "harmonic")
    find_package(gz-sim8 8 REQUIRED CONFIG)
    find_package(gz-rendering8 8 REQUIRED CONFIG)
    find_package(gz-plugin2 2 REQUIRED CONFIG COMPONENTS register)
    gst_plane_camera_require_major(gz-sim8 8)
    gst_plane_camera_require_major(gz-rendering8 8)
    gst_plane_camera_require_major(gz-plugin2 2)
    target_link_libraries(${target} PRIVATE
      gz-sim8::gz-sim8 gz-rendering8::gz-rendering8 gz-plugin2::register)
    target_compile_definitions(${target} PRIVATE GST_PLANE_CAMERA_GZ_HARMONIC=1)
  elseif(GZ_DISTRO STREQUAL "jetty")
    # Jetty installs unversioned configs and imported targets. NAMES also
    # accommodates installations which retain versioned package names.
    find_package(gz-sim 10 REQUIRED CONFIG NAMES gz-sim gz-sim10)
    find_package(gz-rendering 10 REQUIRED CONFIG NAMES gz-rendering gz-rendering10)
    find_package(gz-plugin 4 REQUIRED CONFIG NAMES gz-plugin gz-plugin4 COMPONENTS register)
    gst_plane_camera_require_major(gz-sim 10)
    gst_plane_camera_require_major(gz-rendering 10)
    gst_plane_camera_require_major(gz-plugin 4)
    gst_plane_camera_resolve_target(sim_target gz-sim::gz-sim gz-sim10::gz-sim10)
    gst_plane_camera_resolve_target(rendering_target
      gz-rendering::gz-rendering gz-rendering10::gz-rendering10)
    gst_plane_camera_resolve_target(register_target
      gz-plugin::register gz-plugin::gz-plugin-register
      gz-plugin4::register gz-plugin4::gz-plugin4-register)
    target_link_libraries(${target} PRIVATE
      "${sim_target}" "${rendering_target}" "${register_target}")
    target_compile_definitions(${target} PRIVATE GST_PLANE_CAMERA_GZ_JETTY=1)
  endif()
  message(STATUS "Building GstPlaneCameraSystem for Gazebo ${GZ_DISTRO}")
endfunction()
