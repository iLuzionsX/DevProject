// Camera-only motion reconstruction for frames without native velocity data.
//
// Contract:
//   - matrices are normalized by the capture adapter to row-vector convention;
//   - output motion is CURRENT -> PREVIOUS in render-resolution pixels;
//   - confidence falls toward zero when previous depth disagrees with the
//     reprojected surface (disocclusion / dynamic-object warning);
//   - this is not object motion. A later optical-flow pass may refine/replace
//     low-confidence regions.

Texture2D<float> CurrentDepth : register(t0);
Texture2D<float> PreviousDepth : register(t1);
RWTexture2D<float2> OutputMotion : register(u0);
RWTexture2D<float> OutputConfidence : register(u1);

cbuffer CameraMotionConstants : register(b0) {
  row_major float4x4 InvCurrentViewProjection;
  row_major float4x4 PreviousViewProjection;

  uint2 RenderSize;
  float DepthThreshold;
  uint DepthMinusOneToOne;

  float2 CurrentJitterPixels;
  float2 PreviousJitterPixels;
  uint RemoveJitter;
  float ConfidenceFloor;
  uint2 Padding;
};

float RawDepthToNdc(float depth) {
  return DepthMinusOneToOne != 0u ? depth * 2.0f - 1.0f : depth;
}

float NdcDepthToRaw(float depth) {
  return DepthMinusOneToOne != 0u ? depth * 0.5f + 0.5f : depth;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
  const uint2 pixel = dispatchThreadId.xy;
  if (pixel.x >= RenderSize.x || pixel.y >= RenderSize.y) return;

  const float2 renderSize = float2(RenderSize);
  const float2 uv = (float2(pixel) + 0.5f) / renderSize;
  const float currentDepth = CurrentDepth.Load(int3(pixel, 0));

  // Texture UV has +Y down. Convert to canonical clip/NDC with +Y up.
  const float2 currentNdcXY = float2(
      uv.x * 2.0f - 1.0f,
      1.0f - uv.y * 2.0f);

  float4 world = mul(
      float4(currentNdcXY, RawDepthToNdc(currentDepth), 1.0f),
      InvCurrentViewProjection);

  if (abs(world.w) < 1.0e-7f) {
    OutputMotion[pixel] = 0.0f;
    OutputConfidence[pixel] = 0.0f;
    return;
  }
  world /= world.w;

  const float4 previousClip = mul(world, PreviousViewProjection);
  if (previousClip.w <= 1.0e-7f) {
    OutputMotion[pixel] = 0.0f;
    OutputConfidence[pixel] = 0.0f;
    return;
  }

  const float3 previousNdc = previousClip.xyz / previousClip.w;
  const float2 previousUv = float2(
      previousNdc.x * 0.5f + 0.5f,
      0.5f - previousNdc.y * 0.5f);

  float2 motionPixels = (previousUv - uv) * renderSize;
  if (RemoveJitter != 0u) {
    // Reprojection through jittered matrices contains previous-current jitter.
    // Remove that component for providers expecting de-jittered motion.
    motionPixels -= PreviousJitterPixels - CurrentJitterPixels;
  }

  OutputMotion[pixel] = motionPixels;

  if (any(previousUv < 0.0f) || any(previousUv > 1.0f)) {
    OutputConfidence[pixel] = 0.0f;
    return;
  }

  const uint2 previousPixel = min(
      uint2(previousUv * renderSize),
      RenderSize - uint2(1u, 1u));
  const float previousDepth = PreviousDepth.Load(int3(previousPixel, 0));
  const float projectedPreviousDepth = NdcDepthToRaw(previousNdc.z);
  const float depthError = abs(previousDepth - projectedPreviousDepth);
  const float threshold = max(DepthThreshold, 1.0e-6f);
  const float depthConfidence = saturate(1.0f - depthError / threshold);

  OutputConfidence[pixel] = max(saturate(ConfidenceFloor), depthConfidence);
}
