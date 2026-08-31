# RenoDX AMD bridge experiment

Experimental patch kit for adding an AMD FidelityFX upscaler backend path to RenoDX's existing Streamline/DLSS interception.

## Target

- Upstream: `clshortfuse/renodx`
- Original pinned baseline: `8d360960a8bdeccada1a1babf7f02215c15c82e8` (2026-08-28)
- Current target: DirectX 12 + Streamline DLSS input
- Backend: AMD FidelityFX upscaler API, using the installed provider supported by the AMD GPU/runtime

## What this is

RenoDX already intercepts the useful Streamline boundary: `slSetTag` / `slSetTagForFrame` expose depth, motion vectors, scaling input/output, exposure and related temporal resources, while `slEvaluateFeature` marks the point where DLSS executes.

This experiment translates that **DLSS API/input path** into AMD's FidelityFX upscaler API. It does **not** emulate NVIDIA hardware and it does not redistribute or execute NVIDIA's proprietary DLSS neural implementation on AMD.

The practical data flow is:

`game Streamline/DLSS inputs -> RenoDX hooks -> frame/viewport state -> FidelityFX DX12 upscaler -> game output`

## Implemented on this branch

### Scoped Streamline capture

`src/amd_bridge.hpp`

- frame-token + viewport keyed state rather than one global latest frame;
- legacy viewport-scoped `slSetTag` fallback;
- captures required color/depth/motion-vector resources plus exposure/reactive/composition hints;
- captures `sl::Constants` and viewport-scoped `sl::DLSSOptions`;
- overlays local `slEvaluateFeature` input-chain tags/constants/options;
- bounded state pruning;
- preflight/evaluate ownership split so NVIDIA fallback can happen only before AMD GPU commands begin.

### Typed FidelityFX runtime loader

`src/ffx_runtime_loader.hpp`

- uses official `ffx_api.h` function-pointer types when the SDK headers are present;
- dynamically loads `amd_fidelityfx_loader_dx12.dll`;
- validates `ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch`, `ffxQuery`, and `ffxConfigure`;
- keeps an older combined-DLL filename as a compatibility fallback.

### Concrete DX12 upscaler backend

`src/ffx_upscale_backend_dx12.hpp`

- AMD vendor-ID gate (`0x1002`) so NVIDIA's normal RenoDX path is not replaced;
- validates an installed FidelityFX upscaler provider before reporting the alternate backend ready;
- creates FidelityFX upscaler contexts lazily per Streamline viewport;
- current SDK create chain: upscaler -> upscaler version -> DX12 backend;
- maps Streamline color/depth/MV/exposure/reactive/composition/output resources;
- maps jitter, motion-vector scale, reset, near/far planes, FOV, HDR, auto exposure, and pre-exposure;
- maps DLSS quality modes to FidelityFX quality-mode queries for optimal render resolution;
- records D3D12 input/output barriers, dispatches FFX, records an output UAV barrier, and restores the game-provided resource states;
- recreates a viewport context when dimensions or creation flags require it.

### RenoDX integration patch

`patches/0001-streamline-amd-backend.patch`

The patch kit now describes integration with:

- `slSetD3DDevice` for AMD backend initialization;
- `slIsFeatureSupported` / `slIsFeatureLoaded` after provider validation;
- `slSetConstants`;
- legacy and frame-based resource tagging after RenoDX clone redirection;
- `slEvaluateFeature` takeover;
- AMD-backed `slDLSSSetOptions`, `slDLSSGetOptimalSettings`, and `slDLSSGetState` wrappers;
- `slShutdown` cleanup.

## Build

See `BUILD_INTEGRATION.md` for the required FidelityFX SDK include directories, runtime DLL layout, source-copy steps, and validation order.

## Current status

The experiment now contains a **concrete source-level FidelityFX dispatch path** instead of only an architectural handoff.

It has **not yet been compiled or validated in a real RenoDX game build from this staging repository**, so it should not be described as a working binary yet. The draft PR remains intentionally unmerged.

Highest-priority validation/remaining work:

- compile the patch against a RenoDX worktree with current FidelityFX headers;
- verify the AMD loader/provider DLL packaging;
- validate resource barriers against RenoDX clone/state tracking;
- source real per-frame delta instead of the 16.67 ms default;
- support non-zero resource-tag subrect offsets;
- map `DLSSOptions.exposureScale` if the selected provider needs equivalent behavior;
- validate FOV/infinite-far conventions per game;
- virtualize feature requirements/version where games depend on them;
- handle titles that query DLSS support before `slSetD3DDevice`;
- determine whether any title requires filtering the NVIDIA DLSS plugin out during `slInit`;
- test resize, dynamic resolution, HDR, camera cuts, multiple frames in flight, multiple viewports, and at least two unrelated Streamline games.

## Safety property

On a non-AMD device or when the FidelityFX runtime/provider is missing, the AMD backend never becomes ready and RenoDX remains on its existing Streamline/NVIDIA behavior.

The connected GitHub tooling cannot create a new upstream fork automatically, so the work remains staged on `experiment/renodx-amd-bridge` in this repository rather than changing `main` or upstream RenoDX directly.
