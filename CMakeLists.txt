cmake_minimum_required(VERSION 3.16)

project(GstPlaneCameraSystem LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(
    GSTREAMER
    REQUIRED
    IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
)

if(DEFINED GZ_SIM_TARGET AND TARGET "${GZ_SIM_TARGET}")
    set(GST_PLANE_GZ_SIM_TARGET "${GZ_SIM_TARGET}")
else()
    find_package(gz-sim8 REQUIRED)
    set(GST_PLANE_GZ_SIM_TARGET gz-sim8::gz-sim8)
endif()

if(DEFINED GZ_PLUGIN_TARGET AND TARGET "${GZ_PLUGIN_TARGET}")
    set(GST_PLANE_GZ_PLUGIN_TARGET "${GZ_PLUGIN_TARGET}")
else()
    find_package(gz-plugin2 REQUIRED COMPONENTS register)
    set(GST_PLANE_GZ_PLUGIN_TARGET gz-plugin2::gz-plugin2)
endif()

find_package(gz-rendering8 REQUIRED)

add_library(${PROJECT_NAME} SHARED
    GstPlaneCameraSystem.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ${GST_PLANE_GZ_SIM_TARGET}
        ${GST_PLANE_GZ_PLUGIN_TARGET}
        gz-rendering8::gz-rendering8
        PkgConfig::GSTREAMER
        Threads::Threads
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME GstPlaneCameraSystem
    POSITION_INDEPENDENT_CODE ON
)

set(
    GST_PLANE_CAMERA_INSTALL_DIR
    "${CMAKE_INSTALL_LIBDIR}"
    CACHE PATH
    "Install directory for libGstPlaneCameraSystem.so"
)

install(
    TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION "${GST_PLANE_CAMERA_INSTALL_DIR}"
)

if(BUILD_TESTING)
    add_executable(GstPlaneCameraConfigTest
        tests/GstPlaneCameraConfigTest.cpp
    )
    target_compile_features(GstPlaneCameraConfigTest PRIVATE cxx_std_17)
    target_include_directories(GstPlaneCameraConfigTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME GstPlaneCameraConfigTest COMMAND GstPlaneCameraConfigTest)
endif()
