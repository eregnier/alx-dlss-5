cmake_minimum_required(VERSION 3.20)
project(hlx_dlss5 VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable multi-processor compilation on MSVC
if(MSVC)
    add_compile_options(/W4 /WX- /MP /EHsc /utf-8)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openvr/headers
    ${CMAKE_CURRENT_SOURCE_DIR}/external/nvsdk_ngx
)

# 1. Main Proxy DLL (openvr_api.dll)
add_library(openvr_api SHARED
    src/openvr_proxy.cpp
)

set_target_properties(openvr_api PROPERTIES
    OUTPUT_NAME "openvr_api"
    PREFIX ""
    SUFFIX ".dll"
)

target_link_libraries(openvr_api PRIVATE
    d3d12
    dxgi
    gdi32
    user32
)

# 2. Automated Tests
enable_testing()

add_executable(test_vtable_hook tests/test_vtable_hook.cpp)
add_test(NAME VTableHookTest COMMAND test_vtable_hook)

add_executable(test_stereo_isolation tests/test_stereo_isolation.cpp)
add_test(NAME StereoIsolationTest COMMAND test_stereo_isolation)

add_executable(test_d3d12_device tests/test_d3d12_device.cpp)
target_link_libraries(test_d3d12_device PRIVATE d3d12 dxgi)
add_test(NAME D3D12DeviceTest COMMAND test_d3d12_device)

add_executable(test_dlss5_bridge tests/test_dlss5_bridge.cpp)
target_link_libraries(test_dlss5_bridge PRIVATE d3d12 dxgi)
add_test(NAME DLSS5BridgeTest COMMAND test_dlss5_bridge)

add_executable(test_config_resolution tests/test_config_resolution.cpp)
add_test(NAME ConfigResolutionTest COMMAND test_config_resolution)

add_executable(test_stereo_atlas_reuse tests/test_stereo_atlas_reuse.cpp)
add_test(NAME StereoAtlasReuseTest COMMAND test_stereo_atlas_reuse)

add_executable(test_openvr_submit_simulation tests/test_openvr_submit_simulation.cpp)
add_test(NAME OpenVRSubmitSimulationTest COMMAND test_openvr_submit_simulation)

add_executable(test_typeless_copy tests/test_typeless_copy.cpp)
target_link_libraries(test_typeless_copy PRIVATE d3d11)
add_test(NAME TypelessCopyTest COMMAND test_typeless_copy)

add_executable(test_inplace_pipeline tests/test_inplace_pipeline.cpp)
target_link_libraries(test_inplace_pipeline PRIVATE d3d11 d3d12 dxgi advapi32)
add_test(NAME InplacePipelineTest COMMAND test_inplace_pipeline)

add_executable(test_end_to_end_nr tests/test_end_to_end_nr.cpp)
target_link_libraries(test_end_to_end_nr PRIVATE d3d11 d3d12 dxgi advapi32)
add_test(NAME EndToEndNRTest COMMAND test_end_to_end_nr)

add_executable(test_render_target_scale tests/test_render_target_scale.cpp)
add_test(NAME RenderTargetScaleTest COMMAND test_render_target_scale)

add_executable(test_viewport_adaptation tests/test_viewport_adaptation.cpp)
add_test(NAME ViewportAdaptationTest COMMAND test_viewport_adaptation)

add_executable(test_perf_instrumentation tests/test_perf_instrumentation.cpp)
add_test(NAME PerfInstrumentationTest COMMAND test_perf_instrumentation)

add_executable(test_async_logger tests/test_async_logger.cpp)
add_test(NAME AsyncLoggerTest COMMAND test_async_logger)

add_executable(test_fidelity_and_budget tests/test_fidelity_and_budget.cpp)
target_link_libraries(test_fidelity_and_budget PRIVATE d3d11 d3d12 dxgi)
add_test(NAME FidelityAndBudgetTest COMMAND test_fidelity_and_budget)

add_executable(test_foveated_pipeline tests/test_foveated_pipeline.cpp)
target_link_libraries(test_foveated_pipeline PRIVATE d3d11 d3dcompiler dxgi)
add_test(NAME FoveatedPipelineTest COMMAND test_foveated_pipeline)

add_executable(test_vr_menu tests/test_vr_menu.cpp)
target_link_libraries(test_vr_menu PRIVATE d3d12 dxgi gdi32 user32)
add_test(NAME VRMenuTest COMMAND test_vr_menu)

add_executable(test_input_combos tests/test_input_combos.cpp)
add_test(NAME InputCombosTest COMMAND test_input_combos)
