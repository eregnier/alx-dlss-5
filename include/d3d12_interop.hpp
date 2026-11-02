#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include "hlx_logger.hpp"

namespace hlx {

using Microsoft::WRL::ComPtr;

class D3D12Interop {
public:
    static D3D12Interop& Instance() {
        static D3D12Interop instance;
        return instance;
    }

    bool Init() {
        if (isInitialized_) return true;

        // Create DXGI Factory
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory_));
        if (FAILED(hr)) {
            HLX_ERROR("Failed to CreateDXGIFactory1: 0x%08X", hr);
            return false;
        }

        // Find primary NVIDIA adapter
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; dxgiFactory_->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            if (desc.VendorId == 0x10DE) { // NVIDIA
                HLX_INFO("Found NVIDIA adapter: %ls (VRAM: %zu MB)", desc.Description, desc.DedicatedVideoMemory / (1024 * 1024));
                break;
            }
        }

        if (!adapter) {
            HLX_WARN("NVIDIA adapter not explicitly identified, selecting default adapter 0.");
            dxgiFactory_->EnumAdapters1(0, &adapter);
        }

        // Create D3D12 Device
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_));
        if (FAILED(hr)) {
            HLX_ERROR("Failed to D3D12CreateDevice: 0x%08X", hr);
            return false;
        }

        // Create Command Queue
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue_));
        if (FAILED(hr)) {
            HLX_ERROR("Failed to CreateCommandQueue: 0x%08X", hr);
            return false;
        }

        // Create Command Allocator & List
        hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator_));
        if (FAILED(hr)) {
            HLX_ERROR("Failed to CreateCommandAllocator: 0x%08X", hr);
            return false;
        }

        hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator_.Get(), nullptr, IID_PPV_ARGS(&commandList_));
        if (FAILED(hr)) {
            HLX_ERROR("Failed to CreateCommandList: 0x%08X", hr);
            return false;
        }

        // Create Fence
        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(hr)) {
            HLX_ERROR("Failed to CreateFence: 0x%08X", hr);
            return false;
        }
        fenceValue_ = 1;
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        isInitialized_ = true;
        HLX_INFO("D3D12Interop successfully initialized private D3D12 device and command queue.");
        return true;
    }

    void Shutdown() {
        if (isInitialized_) {
            WaitForGpu();
            if (fenceEvent_) {
                CloseHandle(fenceEvent_);
                fenceEvent_ = nullptr;
            }
            d3d11Fence_.Reset();
            d3d12SharedFence_.Reset();
            hasSharedFence_ = false;
            fence_.Reset();
            commandList_.Reset();
            commandAllocator_.Reset();
            commandQueue_.Reset();
            device_.Reset();
            dxgiFactory_.Reset();
            isInitialized_ = false;
            HLX_INFO("D3D12Interop shutdown complete.");
        }
    }

    bool InitSharedFence(ID3D11Device* d3d11Device) {
        if (!d3d11Device || !device_) return false;
        if (hasSharedFence_) return true;

        ComPtr<ID3D11Device5> dev11_5;
        if (FAILED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dev11_5)))) {
            HLX_INFO("D3D11 device does not support ID3D11Device5; using standard fallback.");
            return false;
        }

        HRESULT hr = dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3d11Fence_));
        if (FAILED(hr)) {
            HLX_WARN("CreateFence failed in D3D11: 0x%08X", hr);
            return false;
        }

        HANDLE hFence = nullptr;
        hr = d3d11Fence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hFence);
        if (FAILED(hr)) {
            HLX_WARN("CreateSharedHandle failed for D3D11 fence: 0x%08X", hr);
            return false;
        }

        hr = device_->OpenSharedHandle(hFence, IID_PPV_ARGS(&d3d12SharedFence_));
        CloseHandle(hFence);
        if (FAILED(hr)) {
            HLX_WARN("OpenSharedHandle failed in D3D12 for shared fence: 0x%08X", hr);
            return false;
        }

        hasSharedFence_ = true;
        sharedFenceValue_ = 100;
        HLX_INFO("Cheeky Transport: D3D11 <-> D3D12 Shared Hardware Fence initialized successfully (zero CPU wait)!");
        return true;
    }

    [[nodiscard]] bool HasSharedFence() const noexcept { return hasSharedFence_; }
    [[nodiscard]] ID3D11Fence* GetD3D11Fence() const noexcept { return d3d11Fence_.Get(); }
    [[nodiscard]] ID3D12Fence* GetD3D12SharedFence() const noexcept { return d3d12SharedFence_.Get(); }
    [[nodiscard]] uint64_t NextSharedFenceValue() noexcept { return ++sharedFenceValue_; }

    bool CreateTexture(
        UINT width,
        UINT height,
        DXGI_FORMAT format,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initialState,
        ComPtr<ID3D12Resource>& outResource,
        const wchar_t* debugName = L"HLX_Texture"
    ) {
        if (!device_) return false;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = flags;

        HRESULT hr = device_->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&outResource)
        );

        if (FAILED(hr)) {
            HLX_ERROR("CreateCommittedResource failed for %ls: 0x%08X", debugName, hr);
            return false;
        }

        if (debugName) {
            outResource->SetName(debugName);
        }
        return true;
    }

    void ExecuteAndSync() {
        if (!commandList_ || !commandQueue_) return;
        commandList_->Close();
        ID3D12CommandList* lists[] = { commandList_.Get() };
        commandQueue_->ExecuteCommandLists(1, lists);

        const UINT64 fence = fenceValue_++;
        commandQueue_->Signal(fence_.Get(), fence);
        if (fence_->GetCompletedValue() < fence) {
            fence_->SetEventOnCompletion(fence, fenceEvent_);
            WaitForSingleObject(fenceEvent_, 1000);
        }
        commandAllocator_->Reset();
        commandList_->Reset(commandAllocator_.Get(), nullptr);
    }

    void WaitForGpu() {
        if (!commandQueue_ || !fence_ || !fenceEvent_) return;

        const UINT64 fence = fenceValue_++;
        commandQueue_->Signal(fence_.Get(), fence);
        if (fence_->GetCompletedValue() < fence) {
            fence_->SetEventOnCompletion(fence, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    [[nodiscard]] ID3D12Device* GetDevice() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D12CommandQueue* GetCommandQueue() const noexcept { return commandQueue_.Get(); }
    [[nodiscard]] ID3D12GraphicsCommandList* GetCommandList() const noexcept { return commandList_.Get(); }
    [[nodiscard]] ID3D12CommandAllocator* GetCommandAllocator() const noexcept { return commandAllocator_.Get(); }
    [[nodiscard]] bool IsInitialized() const noexcept { return isInitialized_; }

private:
    D3D12Interop() = default;
    ~D3D12Interop() { Shutdown(); }

    bool isInitialized_ = false;
    ComPtr<IDXGIFactory4> dxgiFactory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> commandQueue_;
    ComPtr<ID3D12CommandAllocator> commandAllocator_;
    ComPtr<ID3D12GraphicsCommandList> commandList_;
    ComPtr<ID3D12Fence> fence_;
    UINT64 fenceValue_ = 0;
    HANDLE fenceEvent_ = nullptr;

    // Cheeky-inspired D3D11 <-> D3D12 Shared Hardware Fence
    bool hasSharedFence_ = false;
    ComPtr<ID3D11Fence> d3d11Fence_;
    ComPtr<ID3D12Fence> d3d12SharedFence_;
    uint64_t sharedFenceValue_ = 0;
};

} // namespace hlx
