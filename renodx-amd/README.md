# RenoDX AMD bridge experiment

Experimental patch kit for adding a non-NVIDIA upscaler backend path to RenoDX.

## Target

- Upstream: `clshortfuse/renodx`
- Baseline commit: `8d360960a8bdeccada1a1babf7f02215c15c82e8` (2026-08-28)
- First target: DirectX 12 + Streamline DLSS input

## What this is

RenoDX already intercepts the useful Streamline boundary: `slSetTag`/`slSetTagForFrame` expose depth, motion vectors, scaling input/output, exposure and related resources, while `slEvaluateFeature` is the point where DLSS is dispatched.

The first port therefore does **not** try to emulate NVIDIA hardware or call NGX on an AMD GPU. Instead it adds a small backend handoff at those two interception points so an AMD-capable implementation can consume the same game-provided temporal inputs.

That backend can be:

1. AMD FSR API (`amd_fidelityfx_loader_dx12.dll` + `amd_fidelityfx_upscaler_dx12.dll`), or
2. an OptiScaler-style provider while the bridge is being validated.

This is the practical path toward "DLSS input on AMD." It is not a redistribution or reimplementation of NVIDIA's proprietary DLSS neural model.

## Files

- `src/amd_bridge.hpp` — small callback/data boundary designed to drop into `src/utils/dlss/` in RenoDX.
- `patches/0001-streamline-amd-backend.patch` — first integration patch against the pinned RenoDX baseline.
- `PORT_STATUS.md` — implementation phases, validation criteria, and known blockers.

## Phase 1 behavior

The patch captures the final resource tags *after RenoDX clone redirection* and offers an override before the real Streamline `slEvaluateFeature` call. If no AMD backend is registered, behavior is unchanged and RenoDX falls through to the original Streamline implementation.

This is intentionally fail-open and keeps the attach/detach/runtime path compatible with existing RenoDX behavior.

## Next implementation step

Wire an FSR API DX12 provider into `SetEvaluateOverride()`. Required minimum inputs for temporal upscaling are expected to be:

- scaling input color
- scaling output color
- depth
- motion vectors
- jitter / render-size / reset state from Streamline constants or evaluation inputs
- optional exposure / reactive / transparency-composition masks

The bridge already captures the tagged resources. The next patch has to capture the non-resource per-frame constants and map them to `ffxDispatch` descriptors.

## Safety / testing

Do not ship this as a general compatibility mod until it passes:

- no-backend regression test on NVIDIA
- AMD feature-support spoof/override test
- resize / alt-tab / swapchain recreation
- dynamic resolution
- camera cuts / reset
- HDR resource formats
- at least two Streamline games

The connected GitHub tooling cannot create a new repository/fork, so this is staged on an isolated branch here instead of changing this repository's `main` branch.
