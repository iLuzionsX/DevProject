# Port status

## Goal

Use RenoDX's existing DirectX/Streamline interception to make a game's DLSS temporal-upscaling path consumable by AMD's FidelityFX upscaler API.

This is **DLSS API/input compatibility**, not an attempt to execute NVIDIA's proprietary DLSS model on AMD hardware.

## Upstream seam

RenoDX's Streamline v2 hook already intercepts the right boundary:

- `slSetTag` / `slSetTagForFrame`
- `slSetConstants`
- `slEvaluateFeature`
- `slGetFeatureFunction`
- native D3D12 device / command-list handling
- resource clone redirection

That means the game has already supplied most of the temporal information an alternate upscaler needs before RenoDX decides which backend consumes it.

## Implemented on this branch

### 1. Frame/viewport-scoped bridge

`src/amd_bridge.hpp`

The original global latest-frame scaffold has been replaced.

Current behavior:

- keyed by Streamline frame index + viewport;
- legacy viewport-scoped state for deprecated `slSetTag`;
- bounded state retention;
- captures depth, motion vectors, scaling input/output, exposure, reactive mask, transparency/composition mask, and HUD-less color;
- captures `sl::Constants` per frame/viewport;
- captures `sl::DLSSOptions` per viewport;
- walks local `slEvaluateFeature` structure chains and overlays local resource tags/constants/options;
- refuses an ambiguous evaluate call rather than mixing two viewports;
- separates backend readiness from callback registration;
- has a preflight/evaluate ownership split so the bridge can fall through to NVIDIA only before AMD GPU commands are recorded.

### 2. Typed FidelityFX loader

`src/ffx_runtime_loader.hpp`

- dynamically loads `amd_fidelityfx_loader_dx12.dll`;
- legacy combined-DLL filename fallback remains for older layouts;
- uses official `PfnFfx*` types when `ffx_api.h` is available;
- validates the five public FidelityFX API exports:
  - `ffxCreateContext`
  - `ffxDestroyContext`
  - `ffxDispatch`
  - `ffxQuery`
  - `ffxConfigure`

### 3. Concrete FidelityFX DX12 backend

`src/ffx_upscale_backend_dx12.hpp`

Implemented source path:

- accepts only a device whose DXGI adapter vendor ID is AMD (`0x1002`);
- validates an upscaler provider with `ffxQueryDescGetVersions` before marking the alternate backend ready;
- creates one FFX upscaler context per Streamline viewport;
- current SDK context chain is:
  - `ffxCreateContextDescUpscale`
  - `ffxCreateContextDescUpscaleVersion` using `FFX_UPSCALER_VERSION`
  - `ffxCreateBackendDX12Desc`
- chooses the installed FidelityFX provider rather than hard-coding one GPU generation;
- recreates a context when required dimensions or creation flags change;
- uses `ffxApiGetResourceDX12` for D3D12 resources;
- maps required Streamline inputs to FFX color/output/depth/MV;
- maps optional exposure/reactive/composition resources;
- maps jitter, reset, depth inversion, jittered MVs, near/far planes and FOV;
- converts Streamline's normalized motion-vector scale to FFX pixel-space scale using the render dimensions;
- uses viewport DLSS options for HDR, auto exposure and pre-exposure;
- maps DLAA/Quality/Balanced/Performance/Ultra Performance to AMD quality-mode queries;
- supplies AMD-backed `DLSSOptimalSettings` for the provider-recommended render resolution;
- records D3D12 transitions to compute-read/UAV, an output UAV barrier, and restores the original Streamline-provided resource states;
- maps FFX return codes back to Streamline result codes.

### 4. RenoDX hook integration patch

`patches/0001-streamline-amd-backend.patch`

The patch kit now describes:

- AMD backend initialization after RenoDX unwraps the native `ID3D12Device` in `slSetD3DDevice`;
- provider-gated `slIsFeatureSupported` / `slIsFeatureLoaded` virtualization;
- frame/viewport-aware `slSetConstants` capture;
- resource capture after RenoDX clone redirection in both tag APIs;
- AMD evaluation before the NVIDIA Streamline evaluation;
- stable wrappers for `slDLSSSetOptions`, `slDLSSGetOptimalSettings`, and `slDLSSGetState` even when the NVIDIA DLSS plugin cannot return those symbols on AMD;
- backend shutdown before Streamline shutdown.

### 5. Reproducible integration notes

`BUILD_INTEGRATION.md`

Documents the source-copy steps, official FidelityFX include paths, expected loader/provider DLL layout, resource-state behavior, known PoC limits, and validation order.

## FidelityFX inputs currently mapped

### Resources

- scaling input color -> FFX color
- scaling output color -> FFX output
- depth / hi-res depth / linear depth -> FFX depth
- motion vectors -> FFX motion vectors
- exposure -> optional FFX exposure
- reactive mask hint -> optional FFX reactive
- transparency/composition mask hint -> optional FFX transparency/composition

### Streamline constants

- jitter -> jitter offset
- `mvecScale * render dimensions` -> FFX motion-vector scale
- reset -> reset/camera cut
- near/far -> camera near/far
- FOV -> vertical FOV, with configurable convention
- depth inverted -> FFX create flag
- jittered MVs -> FFX jitter-cancellation flag

### DLSS options

- `preExposure` -> FFX pre-exposure
- `colorBuffersHDR` -> HDR create flag
- `useAutoExposure` -> FFX auto-exposure path
- DLSS mode -> FFX quality-mode query

`DLSSOptions.exposureScale` is **not** mapped yet.

## What is no longer just a plan

The branch now contains a real source-level `ffxCreateContext` + `ffxDispatch` path and D3D12 resource-state handling. The previous major architectural gaps—global frame state, generic callback only, untyped runtime entry points, and missing context/dispatch mapping—have been implemented in source.

## What is still not proven

This staging repository does not itself build RenoDX, so the new code is **not yet compile-validated or game-validated**.

Do not call it a working release binary until the next gates pass.

## Current blockers / hardening work

### Build/package integration

- apply the patch to a real RenoDX worktree/fork;
- add FidelityFX SDK include paths;
- package `amd_fidelityfx_loader_dx12.dll` plus an upscaler provider DLL;
- compile against the exact RenoDX + current FidelityFX header versions.

### Streamline exposure edge cases

- a title that asks `slIsFeatureSupported` before `slSetD3DDevice` can still reject DLSS before an AMD provider can safely be validated;
- `slGetFeatureRequirements` and feature-version virtualization are not implemented;
- `slInit` still allows normal Streamline feature loading; titles that insist on initializing NVIDIA's DLSS plugin before device setup may require feature filtering or a deeper interposer layer.

### Frame/render correctness

- per-frame delta still defaults to 16.67 ms instead of being measured from RenoDX/game timing;
- non-zero Streamline resource subrect offsets currently fail preflight;
- FOV convention and infinite-far-plane behavior require runtime validation;
- provider-specific dynamic-resolution min/max settings are not exposed yet;
- optional resource requirements should be queried/validated across FSR4 and legacy providers;
- resource-state restoration must be checked against RenoDX clone-state tracking in live games.

### Compatibility

- test provider selection on FSR 4.x-capable AMD hardware;
- test an older AMD path where the loader selects a legacy upscaler provider;
- verify no-backend/NVIDIA behavior remains unchanged;
- test multiple frames in flight and multiple viewports;
- test HDR, camera cuts, resize, alt-tab, and dynamic resolution;
- test at least two unrelated Streamline titles.

## Validation gates

1. Compile RenoDX with the bridge sources present and AMD runtime absent.
2. Confirm NVIDIA/no-backend fallthrough remains intact.
3. Compile with current FidelityFX SDK headers.
4. Validate AMD vendor and provider gating.
5. Validate the first FFX context creation.
6. Record the first successful FFX upscale dispatch in a fixed-resolution title.
7. Validate camera cuts/reset and resource-state restoration.
8. Validate resize/dynamic resolution/HDR.
9. Validate multiple frames in flight and multiple viewports.
10. Validate a second unrelated game before calling the bridge generic.

## DLSS 5 distinction

An RTX 40-series unlock can still call NVIDIA's own NGX/DLSS runtime because the hardware remains NVIDIA. This AMD experiment is different: it translates a game's DLSS-facing temporal input path into an AMD-capable backend.

If an independently runnable, legally distributable DLSS model/backend ever exists, the bridge's backend boundary could host it. Nothing in this branch claims that such a runtime exists today.
