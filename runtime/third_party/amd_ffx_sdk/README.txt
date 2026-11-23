AMD FidelityFX SDK v1.1.4: the parts that make FSR 3 (the 3.1 upscaler) on Vulkan, built into the Linux runtime (runtime/cmake/ffx_sdk.cmake). MIT licence (LICENSE.txt).
Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK tag v1.1.4 (commit c6efa6b, 2025-05-08), same paths as there:
  ffx-api/include/ffx_api/           the FidelityFX API headers (ffx_api, ffx_upscale, ffx_framegeneration, vk/ffx_api_vk)
  ffx-api/src/                       ffx_api.cpp, ffx_provider.h, ffx_provider_fsr3upscale.*, backends.*, validation.*
  sdk/include/FidelityFX/host/       the SDK's host headers, and host/backends/vk/ffx_vk.h
  sdk/include/FidelityFX/gpu/        the shared GPU headers (ffx_*.h) and fsr3upscaler/, spd/, fsr1/, fsr3/, plus fsr2/ffx_fsr2_resources.h and
                                     frameinterpolation/ffx_frameinterpolation_resources.h (included by fsr3/ffx_fsr3_resources.h)
  sdk/src/backends/vk/               ffx_vk.cpp and shaders/fsr3upscaler/*.glsl
  sdk/src/backends/shared/           ffx_shader_blobs.*, blob_accessors/ffx_fsr3upscaler_shaderblobs.*
  sdk/src/components/fsr3upscaler/   ffx_fsr3upscaler.cpp, ffx_fsr3upscaler_private.h
  sdk/src/shared/                    ffx_assert, ffx_message, ffx_object_management, ffx_breadcrumbs_list
Changed from upstream (everything else is unmodified):
  ffx-api/src/ffx_provider.h, ffx-api/src/backends.h: <FidelityFx/host/...> spelled <FidelityFX/host/...> (case-sensitive file systems).
  sdk/include/FidelityFX/host/ffx_fsr3upscaler.h: FFX_FSR3UPSCALER_CONTEXT_SIZE wrapped in #ifndef, so the Linux build can raise it
  (4-byte wchar_t makes the private context larger than the 512 KB default).
  sdk/src/backends/vk/ffx_vk.cpp: EffectContext loses its alignas(32). The backend packs its scratch arrays at 4-byte offsets, so the
  contexts are not 32-byte aligned, and GCC copies an over-aligned struct with aligned SSE stores (a crash at context creation).
  sdk/src/backends/vk/ffx_vk.cpp: findMemoryTypeIndex skips the AMD device-coherent (uncached) memory types: they need
  VK_AMD_device_coherent_memory enabled on the device, which the runtime does not do (validation error VUID-vkAllocateMemory-deviceCoherentMemory-02790).
Not taken: ffx-api/src/ffx_provider.cpp (it also asks the AMD Windows driver for providers through D3D12), frame generation, FSR 2, the
DX12 backend, the samples and the Windows-only FidelityFX_SC.exe shader compiler.
Recompia's own files (MIT, like the SDK), in linux/:
  ffx_provider_linux.cpp   the provider list (the FSR 3 upscaler only) in place of ffx_provider.cpp
  ffx_linux_compat.h       force-included: the MSVC spellings the sources use (_countof, wcscpy_s, sprintf_s, __declspec, ...)
The shader permutation headers are generated at build time by runtime/scripts/gen_ffx_fsr3_shaders.py with glslangValidator.
