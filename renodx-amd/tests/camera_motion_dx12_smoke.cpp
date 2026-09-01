#define NOMINMAX

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "universal/camera_motion_dx12.hpp"

using Microsoft::WRL::ComPtr;
using namespace renodx::universal;

namespace {

constexpr uint32_t kWidth = 16u;
constexpr uint32_t kHeight = 16u;

bool Check(HRESULT result, const char* operation) {
  if (SUCCEEDED(result)) return true;
  std::cerr << operation << " failed: 0x" << std::hex
            << static_cast<uint32_t>(result) << std::dec << '\n';
  return false;
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = type;
  properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  properties.CreationNodeMask = 1u;
  properties.VisibleNodeMask = 1u;
  return properties;
}

D3D12_RESOURCE_DESC Texture2DDesc(DXGI_FORMAT format) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.DepthOrArraySize = 1u;
  desc.MipLevels = 1u;
  desc.Format = format;
  desc.SampleDesc.Count = 1u;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  return desc;
}

D3D12_RESOURCE_DESC BufferDesc(uint64_t size) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1u;
  desc.DepthOrArraySize = 1u;
  desc.MipLevels = 1u;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1u;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  return desc;
}

struct UploadedTexture {
  ComPtr<ID3D12Resource> texture;
  ComPtr<ID3D12Resource> upload;
};

bool CreateUploadedTexture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* command_list,
    DXGI_FORMAT format,
    const void* pixel_value,
    uint32_t pixel_size,
    UploadedTexture& output) {
  if (device == nullptr || command_list == nullptr || pixel_value == nullptr || pixel_size == 0u) {
    return false;
  }

  const auto desc = Texture2DDesc(format);
  const auto default_heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
  if (!Check(
          device->CreateCommittedResource(
              &default_heap,
              D3D12_HEAP_FLAG_NONE,
              &desc,
              D3D12_RESOURCE_STATE_COPY_DEST,
              nullptr,
              IID_PPV_ARGS(output.texture.GetAddressOf())),
          "Create uploaded texture")) {
    return false;
  }

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0u;
  UINT64 row_size = 0u;
  UINT64 upload_size = 0u;
  device->GetCopyableFootprints(
      &desc,
      0u,
      1u,
      0u,
      &footprint,
      &rows,
      &row_size,
      &upload_size);
  if (rows != kHeight || row_size < static_cast<UINT64>(kWidth) * pixel_size) {
    std::cerr << "Unexpected upload footprint\n";
    return false;
  }

  const auto upload_heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
  const auto upload_desc = BufferDesc(upload_size);
  if (!Check(
          device->CreateCommittedResource(
              &upload_heap,
              D3D12_HEAP_FLAG_NONE,
              &upload_desc,
              D3D12_RESOURCE_STATE_GENERIC_READ,
              nullptr,
              IID_PPV_ARGS(output.upload.GetAddressOf())),
          "Create upload buffer")) {
    return false;
  }

  uint8_t* mapped = nullptr;
  const D3D12_RANGE no_read{0u, 0u};
  if (!Check(
          output.upload->Map(0u, &no_read, reinterpret_cast<void**>(&mapped)),
          "Map upload buffer")) {
    return false;
  }
  for (uint32_t y = 0u; y < kHeight; ++y) {
    uint8_t* row = mapped + footprint.Offset
        + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
    for (uint32_t x = 0u; x < kWidth; ++x) {
      std::memcpy(row + static_cast<size_t>(x) * pixel_size, pixel_value, pixel_size);
    }
  }
  output.upload->Unmap(0u, nullptr);

  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = output.texture.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  destination.SubresourceIndex = 0u;
  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = output.upload.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint = footprint;
  command_list->CopyTextureRegion(&destination, 0u, 0u, 0u, &source, nullptr);

  D3D12_RESOURCE_BARRIER to_read{};
  to_read.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_read.Transition.pResource = output.texture.Get();
  to_read.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  to_read.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  to_read.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1u, &to_read);
  return true;
}

bool ExecuteAndWait(
    ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Fence* fence,
    HANDLE fence_event,
    uint64_t fence_value) {
  if (!Check(command_list->Close(), "Close command list")) return false;
  ID3D12CommandList* lists[] = {command_list};
  queue->ExecuteCommandLists(1u, lists);
  if (!Check(queue->Signal(fence, fence_value), "Signal fence")) return false;
  if (fence->GetCompletedValue() < fence_value) {
    if (!Check(fence->SetEventOnCompletion(fence_value, fence_event), "Set fence event")) {
      return false;
    }
    if (WaitForSingleObject(fence_event, INFINITE) != WAIT_OBJECT_0) {
      std::cerr << "Fence wait failed\n";
      return false;
    }
  }
  return true;
}

UniversalFrame MakeFrame(ID3D12Resource* depth) {
  UniversalFrame frame{};
  frame.api = GraphicsApi::kD3D12;
  frame.render_extent = {kWidth, kHeight};
  frame.depth.resource = ResourceHandle{
      .api = GraphicsApi::kD3D12,
      .native = depth,
  };
  frame.depth.extent = {kWidth, kHeight};
  frame.depth.semantic = ResourceSemantic::kDepth;
  frame.depth.provenance = Provenance::kNativeTagged;
  frame.depth.confidence = 1.0f;
  frame.camera.layout = MatrixLayout::kRowMajor;
  frame.camera.provenance = Provenance::kNativeTagged;
  frame.camera.confidence = 1.0f;
  return frame;
}

void SetIdentity(std::array<float, 16>& matrix) {
  matrix.fill(0.0f);
  matrix[0] = 1.0f;
  matrix[5] = 1.0f;
  matrix[10] = 1.0f;
  matrix[15] = 1.0f;
}

struct MotionReadback {
  ComPtr<ID3D12Resource> resource;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
};

bool CreateMotionReadback(
    ID3D12Device* device,
    ID3D12Resource* motion,
    MotionReadback& output) {
  const auto desc = motion->GetDesc();
  UINT rows = 0u;
  UINT64 row_size = 0u;
  UINT64 readback_size = 0u;
  device->GetCopyableFootprints(
      &desc,
      0u,
      1u,
      0u,
      &output.footprint,
      &rows,
      &row_size,
      &readback_size);
  if (rows != kHeight || row_size < static_cast<UINT64>(kWidth) * 4u) {
    std::cerr << "Unexpected motion footprint\n";
    return false;
  }

  const auto readback_heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
  const auto readback_desc = BufferDesc(readback_size);
  return Check(
      device->CreateCommittedResource(
          &readback_heap,
          D3D12_HEAP_FLAG_NONE,
          &readback_desc,
          D3D12_RESOURCE_STATE_COPY_DEST,
          nullptr,
          IID_PPV_ARGS(output.resource.GetAddressOf())),
      "Create motion readback");
}

void RecordMotionReadback(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* motion,
    const MotionReadback& readback) {
  D3D12_RESOURCE_BARRIER to_copy{};
  to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_copy.Transition.pResource = motion;
  to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  command_list->ResourceBarrier(1u, &to_copy);

  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = motion;
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  source.SubresourceIndex = 0u;
  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = readback.resource.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint = readback.footprint;
  command_list->CopyTextureRegion(&destination, 0u, 0u, 0u, &source, nullptr);

  std::swap(to_copy.Transition.StateBefore, to_copy.Transition.StateAfter);
  command_list->ResourceBarrier(1u, &to_copy);
}

bool ReadFirstMotion(
    const MotionReadback& readback,
    uint16_t& x,
    uint16_t& y) {
  const D3D12_RANGE range{
      static_cast<SIZE_T>(readback.footprint.Offset),
      static_cast<SIZE_T>(readback.footprint.Offset + 4u),
  };
  uint8_t* mapped = nullptr;
  if (!Check(
          readback.resource->Map(0u, &range, reinterpret_cast<void**>(&mapped)),
          "Map motion readback")) {
    return false;
  }
  std::memcpy(&x, mapped + readback.footprint.Offset, sizeof(x));
  std::memcpy(&y, mapped + readback.footprint.Offset + sizeof(x), sizeof(y));
  const D3D12_RANGE written{0u, 0u};
  readback.resource->Unmap(0u, &written);
  return true;
}

bool ResetCommands(
    ID3D12CommandAllocator* allocator,
    ID3D12GraphicsCommandList* command_list) {
  return Check(allocator->Reset(), "Reset command allocator")
      && Check(command_list->Reset(allocator, nullptr), "Reset command list");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: camera_motion_dx12_smoke <camera_motion_cs.dxil>\n";
    return 2;
  }

  ComPtr<IDXGIFactory4> factory;
  if (!Check(CreateDXGIFactory2(0u, IID_PPV_ARGS(factory.GetAddressOf())), "Create DXGI factory")) {
    return 1;
  }

  ComPtr<IDXGIAdapter> warp_adapter;
  if (!Check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp_adapter.GetAddressOf())), "Get WARP adapter")) {
    return 1;
  }

  ComPtr<ID3D12Device> device;
  if (!Check(
          D3D12CreateDevice(
              warp_adapter.Get(),
              D3D_FEATURE_LEVEL_11_0,
              IID_PPV_ARGS(device.GetAddressOf())),
          "Create WARP D3D12 device")) {
    return 1;
  }

  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  if (!Check(
          device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.GetAddressOf())),
          "Create command queue")) {
    return 1;
  }

  ComPtr<ID3D12CommandAllocator> allocator;
  if (!Check(
          device->CreateCommandAllocator(
              D3D12_COMMAND_LIST_TYPE_DIRECT,
              IID_PPV_ARGS(allocator.GetAddressOf())),
          "Create command allocator")) {
    return 1;
  }

  ComPtr<ID3D12GraphicsCommandList> command_list;
  if (!Check(
          device->CreateCommandList(
              0u,
              D3D12_COMMAND_LIST_TYPE_DIRECT,
              allocator.Get(),
              nullptr,
              IID_PPV_ARGS(command_list.GetAddressOf())),
          "Create command list")) {
    return 1;
  }

  ComPtr<ID3D12Fence> fence;
  if (!Check(
          device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())),
          "Create fence")) {
    return 1;
  }
  const HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (fence_event == nullptr) {
    std::cerr << "CreateEventW failed\n";
    return 1;
  }

  const float depth_value = 0.5f;
  UploadedTexture depth{};
  if (!CreateUploadedTexture(
          device.Get(),
          command_list.Get(),
          DXGI_FORMAT_R32_FLOAT,
          &depth_value,
          sizeof(depth_value),
          depth)) {
    CloseHandle(fence_event);
    return 1;
  }

  // Half-float {0.125, 0}. With mvecScale.x = -1 and a 16 px render width,
  // Streamline's native-preservation branch should output exactly +2 px.
  const std::array<uint16_t, 2> native_value{0x3000u, 0x0000u};
  UploadedTexture native_motion{};
  if (!CreateUploadedTexture(
          device.Get(),
          command_list.Get(),
          DXGI_FORMAT_R16G16_FLOAT,
          native_value.data(),
          static_cast<uint32_t>(sizeof(native_value)),
          native_motion)) {
    CloseHandle(fence_event);
    return 1;
  }

  if (!camera_motion_dx12::Initialize(device.Get(), std::filesystem::path(argv[1]))) {
    std::cerr << "Camera-motion executor initialization failed\n";
    CloseHandle(fence_event);
    return 1;
  }

  auto frame = MakeFrame(depth.texture.Get());
  SetIdentity(frame.camera.clip_to_previous_clip);
  auto* motion = camera_motion_dx12::Dispatch(
      command_list.Get(),
      0u,
      frame,
      depth.texture.Get());
  if (motion == nullptr) {
    std::cerr << "Identity camera-motion dispatch failed\n";
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }

  MotionReadback readback{};
  if (!CreateMotionReadback(device.Get(), motion, readback)) {
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }
  RecordMotionReadback(command_list.Get(), motion, readback);

  if (!ExecuteAndWait(queue.Get(), command_list.Get(), fence.Get(), fence_event, 1u)) {
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }

  uint16_t motion_x = 0u;
  uint16_t motion_y = 0u;
  if (!ReadFirstMotion(readback, motion_x, motion_y)) {
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }
  if ((motion_x & 0x7fffu) != 0u || (motion_y & 0x7fffu) != 0u) {
    std::cerr << "Identity transform produced non-zero motion: 0x"
              << std::hex << motion_x << ", 0x" << motion_y << std::dec << '\n';
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }

  if (!ResetCommands(allocator.Get(), command_list.Get())) {
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }

  SetIdentity(frame.camera.clip_to_previous_clip);
  // previousClip.x = currentClip.x + 0.25 * w. NDC-to-UV halves that
  // displacement, so at 16 px render width the expected motion is +2 px.
  frame.camera.clip_to_previous_clip[3] = 0.25f;
  motion = camera_motion_dx12::Dispatch(
      command_list.Get(), 0u, frame, depth.texture.Get());
  if (motion == nullptr) {
    std::cerr << "Translated camera-motion dispatch failed\n";
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }
  RecordMotionReadback(command_list.Get(), motion, readback);

  if (!ExecuteAndWait(queue.Get(), command_list.Get(), fence.Get(), fence_event, 2u)) {
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }

  if (!ReadFirstMotion(readback, motion_x, motion_y)) {
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }
  if (motion_x != 0x4000u || (motion_y & 0x7fffu) != 0u) {
    std::cerr << "Translated transform expected +2 px (half 0x4000), got: 0x"
              << std::hex << motion_x << ", 0x" << motion_y << std::dec << '\n';
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }

  if (!ResetCommands(allocator.Get(), command_list.Get())) {
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }

  // Validate Streamline-style completion: the identity camera transform would
  // produce zero, so +2 px proves the valid native object vector was preserved.
  SetIdentity(frame.camera.clip_to_previous_clip);
  frame.motion_vectors.resource = ResourceHandle{
      .api = GraphicsApi::kD3D12,
      .native = native_motion.texture.Get(),
  };
  frame.motion_vectors.extent = {kWidth, kHeight};
  frame.motion_vectors.semantic = ResourceSemantic::kMotionVectors;
  frame.motion_vectors.provenance = Provenance::kNativeTagged;
  frame.motion_vectors.confidence = 1.0f;
  frame.motion.camera_motion = CameraMotionCoverage::kMissing;
  frame.motion.scale_x = -1.0f;
  frame.motion.scale_y = 1.0f;

  motion = camera_motion_dx12::Dispatch(
      command_list.Get(),
      0u,
      frame,
      depth.texture.Get(),
      native_motion.texture.Get());
  if (motion == nullptr) {
    std::cerr << "Native camera-completion dispatch failed\n";
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }
  RecordMotionReadback(command_list.Get(), motion, readback);

  if (!ExecuteAndWait(queue.Get(), command_list.Get(), fence.Get(), fence_event, 3u)) {
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }

  if (!ReadFirstMotion(readback, motion_x, motion_y)) {
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }
  if (motion_x != 0x4000u || (motion_y & 0x7fffu) != 0u) {
    std::cerr << "Native completion expected +2 px (half 0x4000), got: 0x"
              << std::hex << motion_x << ", 0x" << motion_y << std::dec << '\n';
    camera_motion_dx12::Shutdown();
    CloseHandle(fence_event);
    return 1;
  }

  camera_motion_dx12::Shutdown();
  CloseHandle(fence_event);
  std::cout << "D3D12 WARP camera-motion smoke test passed\n";
  return 0;
}
