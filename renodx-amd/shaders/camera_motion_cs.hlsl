// Camera-motion reconstruction / completion for Streamline DLSS inputs.
//
// This mirrors Streamline's own mvec fallback. When native motion exists but
// Constants::cameraMotionIncluded is false, preserve valid native object motion
// and fill zero/invalid pixels from current depth + clipToPrevClip. When native
// motion is absent, every pixel uses camera reprojection. Output is always
// CURRENT -> PREVIOUS motion in render-resolution pixels for FidelityFX.

Texture2D<float4> NativeMotion : register(t0);
Texture2D<float> CurrentDepth : register(t1);
RWTexture2D<float2> OutputMotion : register(u0);

cbuffer CameraMotionConstants : register(b0) {
  // Streamline constants are row-major and clipToPrevClip is explicitly
  // current clip -> previous clip, with temporal jitter excluded.
  row_major float4x4 CurrentToPreviousClip;
  uint2 RenderSize;
  float2 NativeMotionScale;
  uint UseNativeMotion;
  uint3 Padding;
};

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
  const uint2 pixel = dispatchThreadId.xy;
  if (pixel.x >= RenderSize.x || pixel.y >= RenderSize.y) return;

  const float2 renderSize = float2(RenderSize);

  if (UseNativeMotion != 0u) {
    const float2 nativeVelocity = NativeMotion.Load(int3(pixel, 0)).xy;

    // Match Streamline's mvec.hlsl validity heuristic. Valid native vectors
    // represent object motion and should win; camera reprojection fills only
    // the pixels Streamline itself treats as missing/invalid.
    const bool nativeInvalid =
        any(nativeVelocity > 1.0f)
        || any(nativeVelocity < -1.0f)
        || all(nativeVelocity == 0.0f);
    if (!nativeInvalid) {
      OutputMotion[pixel] =
          -nativeVelocity * NativeMotionScale * renderSize;
      return;
    }
  }

  const float2 uvCurrent = (float2(pixel) + 0.5f) / renderSize;
  const float depth = CurrentDepth.Load(int3(pixel, 0));

  // Texture UV has +Y down; clip space has +Y up.
  const float4 currentClip = float4(
      uvCurrent.x * 2.0f - 1.0f,
      1.0f - uvCurrent.y * 2.0f,
      depth,
      1.0f);

  const float4 previousClip = mul(CurrentToPreviousClip, currentClip);
  if (abs(previousClip.w) < 1.0e-7f) {
    OutputMotion[pixel] = 0.0f;
    return;
  }

  const float2 previousNdc = previousClip.xy / previousClip.w;
  const float2 uvPrevious = float2(
      previousNdc.x * 0.5f + 0.5f,
      0.5f - previousNdc.y * 0.5f);

  OutputMotion[pixel] = (uvPrevious - uvCurrent) * renderSize;
}
