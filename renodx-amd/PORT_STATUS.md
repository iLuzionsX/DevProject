# Port status

## Goal

Use RenoDX's existing DirectX/Streamline interception to make a game's DLSS temporal inputs consumable by a non-NVIDIA backend on AMD hardware.

## What is already proven by the upstream code

RenoDX's current Streamline v2 hook already intercepts:

- `slSetTag` and `slSetTagForFrame`
- `slEvaluateFeature`
- `slGetFeatureFunction`
- D3D12 device / command-list / queue wrapping

Its tag hook sees the exact resource classes needed by temporal upscalers, including depth, motion vectors, scaling input/output, exposure, reactive mask and transparency/composition mask. It also rewrites tags when RenoDX has upgraded/cloned a resource.

That makes the Streamline hook the correct translation boundary.

## Implemented on this branch

### 1. Backend-neutral capture/handoff

`src/amd_bridge.hpp`

- Captures the final Streamline resources by value after any RenoDX resource redirection.
- Captures `sl::Constants` for temporal data needed by FSR (jitter, motion-vector scale, camera/reset data, etc.).
- Requires depth + motion vectors + input + output + constants before attempting an override.
- Exposes `SetEvaluateOverride()` so an AMD backend can consume `sl::kFeatureDLSS`.
- Falls through to the original Streamline path when no backend is registered.

### 2. AMD FSR runtime loader

`src/ffx_runtime_loader.hpp`

- Loads `amd_fidelityfx_loader_dx12.dll` first (FSR SDK 2.x).
- Falls back to the legacy combined `amd_fidelityfx_dx12.dll`.
- Verifies the five FSR API exports: `ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch`, `ffxQuery`, `ffxConfigure`.
- Does not copy or freeze AMD ABI descriptor definitions; the real dispatcher should compile against AMD's official `ffx_api` headers.

### 3. RenoDX integration patch

`patches/0001-streamline-amd-backend.patch`

- Calls the capture layer from both Streamline tag APIs.
- Calls the alternate backend before the real DLSS evaluation.
- Does not change normal RenoDX behavior when the backend is absent.

## Still required before the port can render a frame

### A. Frame/viewport-scoped state

The first scaffold keeps the latest state globally. Production code must key captures by viewport and, for frame-based tagging, frame token so multiple viewports / frames in flight cannot cross-contaminate resources.

### B. FSR descriptor mapping

Build `ffxCreateContext` and `ffxDispatch` descriptors using AMD's official FSR API headers. Map:

- Streamline scaling input -> FSR color input
- Streamline scaling output -> FSR output
- depth -> FSR depth
- motion vectors -> FSR motion vectors
- exposure -> exposure when present
- reactive mask -> reactive mask when present
- transparency/composition mask -> transparency/composition mask when present
- `sl::Constants` -> jitter, motion-vector scale, near/far/FOV, reset/camera-cut metadata

### C. Feature exposure on AMD

Once (and only once) the FSR backend can successfully create a context, Streamline must conditionally report DLSS input support to the game. The safe version should cover `slInit`, `slIsFeatureSupported`, `slIsFeatureLoaded`, and any requirements/version query a game relies on. Do not report DLSS available merely because an AMD GPU is detected.

### D. Resource barriers / states

FSR dispatch must transition all D3D12 resources to the states required by the AMD API and restore/hand back states expected by the game. RenoDX already tracks resource state information, which should be reused rather than creating a parallel tracker.

### E. Lifecycle

Create/destroy FSR contexts on:

- device init/destroy
- render-size changes
- output-size changes
- swapchain recreation
- relevant HDR/format changes

### F. Hardware-specific backend choice

The translation layer should not assume FSR4 everywhere. Backend selection should be capability-driven:

- FSR4/FSR4.1 where the installed AMD runtime/GPU supports it
- FSR3.1 fallback for older AMD GPUs

That preserves the main objective—DLSS game inputs on AMD—even when the newest FSR model is unavailable.

## Important distinction: DLSS 5 vs DLSS input compatibility

The RTX 40-series unlock work can reuse NVIDIA's own NGX/Streamline implementation because the GPU is still NVIDIA. On AMD, RenoDX cannot simply call the proprietary NVIDIA DLSS implementation and expect it to execute. The engineering target here is therefore API/input compatibility: make a game that only exposes DLSS feed those temporal inputs into an AMD-capable neural/upscaling backend.

If an independently runnable DLSS 5 model/backend ever becomes legally and technically available, the callback boundary in this experiment is intentionally generic enough to host that instead of FSR.

## Validation gates

1. Bridge compiles in RenoDX with backend disabled.
2. NVIDIA regression: byte-for-byte behavior path remains fall-through.
3. AMD backend loads only when required exports exist.
4. DLSS feature is not spoofed until context creation succeeds.
5. One fixed-resolution DX12 Streamline game renders correctly.
6. Dynamic resolution + resize.
7. Camera cuts / reset.
8. HDR formats.
9. Multiple frames in flight.
10. At least two unrelated games before calling the bridge generic.
