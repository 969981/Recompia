Snapdragon Game Super Resolution 1 (SGSR 1), Qualcomm Innovation Center, Inc.
Source: https://github.com/SnapdragonStudios/snapdragon-gsr (commit d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e), sgsr/v1/include/glsl/sgsr1_shader_mobile_edge_direction.frag, unmodified.
Licence: BSD-3-Clause (LICENSE). Used by scripts/gen_sgsr_shaders.py, which wraps its functions into a compute shader.

Snapdragon Game Super Resolution 2 (SGSR 2), same source and commit: v2/sgsr2_convert.comp and v2/sgsr2_upscale.comp are
the 2-pass compute shaders of sgsr/v2/include/glsl_2_pass_cs (GLSL), sgsr2_convert.comp and sgsr2_upscale.comp, unmodified. Licence: BSD-3-Clause (v2/LICENSE).
Used by scripts/gen_sgsr_shaders.py, which edits them at generation time (Vulkan bindings, the runtime's motion vectors, reversed-Z).
