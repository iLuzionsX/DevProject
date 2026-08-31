# Build integration

This experiment is a source patch kit for RenoDX. It is not a redistributable binary bundle and it does not vendor AMD's FidelityFX SDK or NVIDIA binaries.

## 1. Copy the bridge sources into RenoDX

Copy these files into `src/utils/dlss/` in a RenoDX worktree/fork:

- `amd_bridge.hpp`
- `ffx_runtime_loader.hpp`
- `ffx_upscale_backend_dx12.hpp`

Then apply the `patches/0001-streamline-amd-backend.patch` changes to `src/utils/dlss/streamline_v2.hpp`.

## 2. Add FidelityFX SDK include paths

The backend compiles against AMD's official current public API headers. Add these FidelityFX SDK directories to the target include path:

- `Kits/FidelityFX/api/include`
- `Kits/FidelityFX/upscalers/include`

The source expects these headers to resolve:

- `ffx_api.h`
- `ffx_upscale.h`
- `dx12/ffx_api_dx12.h`

If the headers are not present, `ffx_upscale_backend_dx12.hpp` compiles to a disabled backend and `Initialize()` returns false.

## 3. Runtime DLL layout

Place the AMD FidelityFX runtime next to the RenoDX/mod DLL or otherwise in the Windows DLL search path:

- `amd_fidelityfx_loader_dx12.dll`
- `amd_fidelityfx_upscaler_dx12.dll`

The experiment loads `amd_fidelityfx_loader_dx12.dll` dynamically and validates the five public API exports:

- `ffxCreateContext`
- `ffxDestroyContext`
- `ffxDispatch`
- `ffxQuery`
- `ffxConfigure`

The loader then resolves the installed upscaler provider. A legacy `amd_fidelityfx_dx12.dll` filename remains a compatibility fallback in the source, but current SDK integration should use the loader/provider layout above.

No static FidelityFX library link is required by this experiment.

## 4. Current ownership/lifecycle

`slSetD3DDevice` provides the native D3D12 device to the AMD backend. Initialization succeeds only when:

1. the device resolves to AMD vendor ID `0x1002`,
2. the FidelityFX loader is present and exposes the required API functions, and
3. `ffxQueryDescGetVersions` reports an upscaler provider for that device.

Only after those checks does the bridge report DLSS support through the alternate path.

Upscaler contexts are created lazily per Streamline viewport after the first complete frame exposes render/output dimensions. Contexts are destroyed and recreated when required dimensions or creation flags change. `slShutdown` shuts the backend down before Streamline shutdown.

## 5. Streamline -> FidelityFX mapping currently implemented

Required tagged resources:

- `kBufferTypeScalingInputColor` -> `ffxDispatchDescUpscale.color`
- `kBufferTypeScalingOutputColor` -> `output`
- depth / hi-res depth / linear depth -> `depth`
- `kBufferTypeMotionVectors` -> `motionVectors`

Optional tagged resources:

- exposure -> `exposure`
- reactive mask hint -> `reactive`
- transparency/composition mask hint -> `transparencyAndComposition`

Common constants:

- jitter -> `jitterOffset`
- Streamline `mvecScale * render dimensions` -> FFX pixel-space `motionVectorScale`
- `reset` -> camera-cut/reset
- near/far plane -> camera near/far
- FOV -> vertical FOV (configurable convention)
- depth-inverted and jittered-MV flags -> FFX context flags

DLSS options:

- `preExposure` -> FFX `preExposure`
- `colorBuffersHDR` -> HDR context flag
- `useAutoExposure` -> auto-exposure behavior
- DLSS mode -> FidelityFX quality-mode query for optimal render resolution

## 6. D3D12 states

Before dispatch the backend currently transitions:

- color/depth/MV/optional masks/exposure -> `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE`
- output -> `D3D12_RESOURCE_STATE_UNORDERED_ACCESS`

It records an output UAV barrier after dispatch and restores the states supplied in the Streamline `sl::Resource` tags.

This must be validated against RenoDX clone/state tracking in a real title before release.

## 7. Important PoC limits

This source is not release-ready yet. In particular:

- no build/CI validation has been completed in this staging repository;
- non-zero Streamline tagged-resource subrect offsets fail preflight;
- frame time is currently a configured default instead of measured per frame;
- `exposureScale` is not mapped yet;
- FOV convention can require a per-game override;
- infinite-far-plane handling needs validation in games that use it;
- `slGetFeatureRequirements` / feature-version virtualization is not implemented;
- a title that checks DLSS support before `slSetD3DDevice` can still reject the feature before the AMD provider has been validated;
- `slInit` does not yet filter NVIDIA DLSS plugin loading on AMD;
- optimal-settings min/max values currently equal the provider-recommended resolution instead of exposing provider-specific dynamic-resolution limits;
- multiple games and multi-frame-in-flight behavior still require runtime validation.

## 8. Validation order

1. Compile RenoDX with the backend headers present but FidelityFX runtime absent. Confirm NVIDIA behavior remains unchanged.
2. Compile with FidelityFX SDK headers and runtime DLLs present.
3. Verify AMD provider query succeeds and `IsBackendAvailable()` is false on NVIDIA, true only on a supported AMD device.
4. Test a fixed-resolution D3D12 Streamline DLSS title.
5. Validate camera cut/reset, resize, alt-tab, HDR, and dynamic resolution.
6. Validate two or more frames in flight and more than one viewport.
7. Validate a second unrelated title before treating the translation layer as generic.

## Licensing

RenoDX is MIT-licensed. AMD's public FidelityFX SDK source/headers are distributed under AMD's permissive license terms in the SDK repository. Keep required license notices when copying or redistributing SDK material. This experiment intentionally does not redistribute NVIDIA's proprietary DLSS implementation.
