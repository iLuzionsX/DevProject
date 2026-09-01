// Camera-only motion reconstruction for frames without native velocity data.
//
// This contract intentionally mirrors Streamline's own DLSS camera-motion
// fallback: use the unjittered current-clip -> previous-clip transform together
// with the current depth buffer and emit CURRENT -> PREVIOUS motion in
// render-resolution pixels. No previous-depth history texture is required.

Texture2D<float> CurrentDepth : register(t0);
RWTexture2D<float2> OutputMotion : register(u0);

cbuffer CameraMotionConstants : register(b0) {
  // Streamline constants are row-major and clipToPrevClip is explicitly
  // current clip -> previous clip. The depth resource is required by Streamline
  // to be compatible with this transform, so keep depth in its native clip
  // convention rather than guessing/re-linearizing it here.
  row_major float4x4 CurrentToPreviousClip;
  uint2 RenderSize;
  uint2 Padding;
};

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
  const uint2 pixel = dispatchThreadId.xy;
  if (pixel.x >= RenderSize.x || pixel.y >= RenderSize.y) return;

  const float2 renderSize = float2(RenderSize);
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
