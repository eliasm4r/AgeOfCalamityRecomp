# AMD FidelityFX SDK 1.1.4 (subset)

From <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4>, MIT (`LICENSE.txt`), via Savage Wind
(<https://github.com/Revivedx/SavageWindRecomp>, MIT, `third_party/FidelityFX`) and TotK Recomp
(<https://github.com/valentderah/totk-pc-recomp> @ `afae80c908ab9fc9cdbafa9ed5295c946d80aa21`, MIT,
`runtime/third_party/FidelityFX`), of which this is the FSR 3.1 upscaler's part. `runtime/vk_fsr3.cpp` drives the
upscaler on the SDK's Vulkan backend directly, without the SDK's ffx-api DLL layer.

What is here:

- `sdk/include/FidelityFX/host`: the SDK's public headers; `gpu`: the shader headers of the FSR 3 upscaler, SPD (which
  the upscaler includes) and FSR 1 (the upscaler's RCAS). The host headers of the other effects stay, unchanged,
  because the SDK's own headers include each other.
- `sdk/src/components/fsr3upscaler`: the upscaler.
- `sdk/src/backends/vk/ffx_vk.cpp` and its `shaders/fsr3upscaler` (GLSL).
- `sdk/src/shared`: the SDK's assert, message and object-management helpers.
- `sdk/src/backends/shared/ffx_shader_blobs.h`: the declaration of the blob lookup, which `runtime/vk_fsr3.cpp`
  implements.
- `spirv/ffx_fsr3upscaler_vk.h`: the upscaler's shaders compiled to SPIR-V with their bindings by
  `tools/ffx_shaders.py` (glslangValidator from MSYS2 UCRT64 or the Vulkan SDK) in place of the SDK's Windows-only
  FidelityFX_SC.exe, one permutation per pass: HDR input, render-size motion vectors without jitter, inverted
  depth (Age of Calamity's; TotK Recomp's header is built for standard depth), the reference Lanczos and Savage Wind's Catmull-Rom history reprojection, in 32-bit and 16-bit floats (the
  16-bit ones where the SDK asks for them and the device has shaderFloat16 and shaderInt16). Committed like
  `runtime/*_spv.h`: AMD's shaders, no game data. Regenerate after changing the GLSL: `python tools/ffx_shaders.py`
  (with `FU_DEPTH_INVERTED = 0` the output is identical to TotK Recomp's header).
- `ffx_compat.h`: force-included into the SDK's sources (`tools/build.py`, rule `cc_ffx`) so they build with clang.

Changes to the SDK's files (marked "Savage Wind" in them):

- `sdk/src/backends/vk/ffx_vk.cpp`: the arrays the backend lays out in its scratch memory are aligned to 64 bytes
  instead of 4, and start 64-byte aligned after its context. With 4, clang's aligned SSE stores into them (`movdqa`)
  faulted; MSVC emits unaligned ones.
- `sdk/include/FidelityFX/host/ffx_types.h`: the contexts' default size is doubled outside Windows, where `wchar_t`
  (their resources' debug names) is 4 bytes instead of 2.
- `sdk/src/backends/vk/ffx_vk.cpp`: the global descriptor pool is created with all six of the sizes it lists. It passed
  five, leaving out the storage buffers (the validation layer reports it).
- `sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_reproject.h`: the history reprojected with a Catmull-Rom
  filter from 5 bilinear samples (`FFX_FSR3UPSCALER_OPTION_REPROJECT_CATMULL_ROM_5`), a Savage Wind addition.

Changes of our own (marked "TotK Recomp"):

- `sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h`: the luma history's storage image is
  declared `rgba16f`, the format of its images (R16G16B16A16_FLOAT in `ffx_fsr3upscaler.cpp`). It was `rgba8`, which
  the Vulkan validation layer reports as a format mismatch with undefined loads and stores.
