# AMD FidelityFX SDK v1.1.4 (MIT, third_party/amd_ffx_sdk): the FSR 3 upscaler and its Vulkan backend built from source, for the
# platforms without AMD's signed DLLs (Linux). Included by gfx.cmake; defines nxrt_ffx_sdk, which provides the FidelityFX API
# (ffxCreateContext, ffxDispatch, ...) that extras/temporal_ffx_linked.cpp hands to temporal_ffx.cpp.
#
# The SDK compiles its shader permutations with FidelityFX_SC.exe (Windows only); scripts/gen_ffx_fsr3_shaders.py makes the same
# headers with glslangValidator (Vulkan SDK / the glslang-tools package). Without it nxrt_ffx_sdk is not defined and FSR 3 is left out.
find_program(NXRT_GLSLANG glslangValidator)
find_package(Python3 QUIET COMPONENTS Interpreter)
if(NOT NXRT_GLSLANG OR NOT Python3_EXECUTABLE)
    message(STATUS "nxrt: FSR 3 left out: glslangValidator or Python 3 not found (FidelityFX SDK shaders)")
    return()
endif()

set(_ffx "${CMAKE_CURRENT_SOURCE_DIR}/third_party/amd_ffx_sdk")
set(_ffx_gen "${CMAKE_CURRENT_BINARY_DIR}/ffx_fsr3_shaders")
file(GLOB _ffx_glsl CONFIGURE_DEPENDS "${_ffx}/sdk/src/backends/vk/shaders/fsr3upscaler/*.glsl")
file(GLOB_RECURSE _ffx_gpu CONFIGURE_DEPENDS "${_ffx}/sdk/include/FidelityFX/gpu/*.h")
add_custom_command(
    OUTPUT "${_ffx_gen}/stamp"
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_ffx_fsr3_shaders.py" "${NXRT_GLSLANG}" "${_ffx}" "${_ffx_gen}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_ffx_gen}/stamp"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_ffx_fsr3_shaders.py" ${_ffx_glsl} ${_ffx_gpu}
    COMMENT "FidelityFX SDK: FSR 3 upscaler shader permutations (glslang)"
    VERBATIM)
add_custom_target(nxrt_ffx_shaders DEPENDS "${_ffx_gen}/stamp")

add_library(nxrt_ffx_sdk STATIC
    "${_ffx}/ffx-api/src/ffx_api.cpp"
    "${_ffx}/ffx-api/src/ffx_provider_fsr3upscale.cpp"
    "${_ffx}/ffx-api/src/backends.cpp"
    "${_ffx}/ffx-api/src/validation.cpp"
    "${_ffx}/linux/ffx_provider_linux.cpp"
    "${_ffx}/sdk/src/backends/vk/ffx_vk.cpp"
    "${_ffx}/sdk/src/backends/shared/ffx_shader_blobs.cpp"
    "${_ffx}/sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp"
    "${_ffx}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    "${_ffx}/sdk/src/shared/ffx_assert.cpp"
    "${_ffx}/sdk/src/shared/ffx_message.cpp"
    "${_ffx}/sdk/src/shared/ffx_object_management.cpp"
    "${_ffx}/sdk/src/shared/ffx_breadcrumbs_list.cpp")
add_dependencies(nxrt_ffx_sdk nxrt_ffx_shaders)
set_target_properties(nxrt_ffx_sdk PROPERTIES CXX_STANDARD 17 CXX_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
target_include_directories(nxrt_ffx_sdk SYSTEM PRIVATE "${_ffx}/ffx-api/include" "${_ffx}/ffx-api/src" "${_ffx}/sdk/include" "${_ffx}/sdk/src/shared"
    "${_ffx}/sdk/src/components" "${_ffx}/sdk/src/backends/shared" "${_ffx_gen}")
# ffx_linux_compat.h: the MSVC spellings the SDK uses. volk.h first: the Vulkan backend calls instance functions (vkGetPhysicalDevice*) by
# name, and in this runtime those are volk's pointers, loaded by vk_device.cpp. 4-byte wchar_t names make the upscaler's private context
# 838 KB, past the SDK's 512 KB default (static_assert in ffx_fsr3upscaler.cpp).
target_compile_options(nxrt_ffx_sdk PRIVATE -w "SHELL:-include ${_ffx}/linux/ffx_linux_compat.h" "SHELL:-include volk.h")
target_compile_definitions(nxrt_ffx_sdk PRIVATE FFX_BACKEND_VK FFX_FSR3UPSCALER FFX_GCC "FFX_FSR3UPSCALER_CONTEXT_SIZE=(1024*256)")
target_link_libraries(nxrt_ffx_sdk PRIVATE nxrt_volk)
