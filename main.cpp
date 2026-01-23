#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <conio.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------
static void PrintDivider() { std::cout << "----------------------------------------------\n"; }
static void PrintDoubleDivider() { std::cout << "==============================================\n"; }

static void WaitForFence(ID3D12CommandQueue* queue, ID3D12Fence* fence, HANDLE eventHandle, UINT64& fenceValue) {
    queue->Signal(fence, fenceValue);
    if (fence->GetCompletedValue() < fenceValue) {
        fence->SetEventOnCompletion(fenceValue, eventHandle);
        WaitForSingleObject(eventHandle, INFINITE);
    }
    fenceValue++;
}

static ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heapType, size_t size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> res;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&res));
    return res;
}

// ------------------------------------------------------------
// Generic Bandwidth Test
// ------------------------------------------------------------
void RunBandwidthTest(
    const char* name,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator,
    ID3D12GraphicsCommandList* list,
    ID3D12Fence* fence,
    HANDLE fenceEvent,
    UINT64& fenceValue,
    ComPtr<ID3D12Resource> src,
    ComPtr<ID3D12Resource> dst,
    size_t bufferSize,
    int copiesPerBatch,
    int batches)
{
    std::cout << name << "\n";
    PrintDivider();

    D3D12_QUERY_HEAP_DESC qhd{}; qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qhd.Count = 2;
    ComPtr<ID3D12QueryHeap> queryHeap; device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queryHeap));
    auto queryReadback = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(UINT64) * 2, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 timestampFreq = 0; queue->GetTimestampFrequency(&timestampFreq);

    std::vector<double> bandwidths;
    int lastPercent = -1;

    for (int i = 0; i < batches; i++) {
        allocator->Reset();
        list->Reset(allocator, nullptr);

        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (int j = 0; j < copiesPerBatch; j++)
            list->CopyResource(dst.Get(), src.Get());
        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

        list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, queryReadback.Get(), 0);
        list->Close();

        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);

        UINT64* ts = nullptr;
        D3D12_RANGE range{ 0, sizeof(UINT64) * 2 };
        queryReadback->Map(0, &range, (void**)&ts);
        double seconds = double(ts[1] - ts[0]) / double(timestampFreq);
        queryReadback->Unmap(0, nullptr);

        double bytes = double(bufferSize) * copiesPerBatch;
        bandwidths.push_back((bytes / (1024.0 * 1024.0 * 1024.0)) / seconds);

        int percent = int((i + 1) * 100 / batches);
        if (percent != lastPercent) { std::cout << "\rProgress: " << percent << "% " << std::flush; lastPercent = percent; }
    }
    std::cout << "\n";

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min : " << *std::min_element(bandwidths.begin(), bandwidths.end()) << " GB/s\n";
    std::cout << "  Avg : " << std::accumulate(bandwidths.begin(), bandwidths.end(), 0.0) / bandwidths.size() << " GB/s\n";
    std::cout << "  Max : " << *std::max_element(bandwidths.begin(), bandwidths.end()) << " GB/s\n";
    PrintDoubleDivider();
}

// ------------------------------------------------------------
// Generic Latency Test
// ------------------------------------------------------------
void RunLatencyTest(
    const char* name,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator,
    ID3D12GraphicsCommandList* list,
    ID3D12Fence* fence,
    HANDLE fenceEvent,
    UINT64& fenceValue,
    ComPtr<ID3D12Resource> src,
    ComPtr<ID3D12Resource> dst,
    int iterations)
{
    std::cout << name << "\n";
    PrintDivider();

    std::vector<double> latencies; latencies.reserve(iterations);
    int lastPercent = -1;

    for (int i = 0; i < iterations; i++) {
        allocator->Reset();
        list->Reset(allocator, nullptr);

        auto start = std::chrono::high_resolution_clock::now();
        list->CopyResource(dst.Get(), src.Get());
        list->Close();

        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);
        auto end = std::chrono::high_resolution_clock::now();

        latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());

        int percent = int((i + 1) * 100 / iterations);
        if (percent != lastPercent) { std::cout << "\rProgress: " << percent << "% " << std::flush; lastPercent = percent; }
    }
    std::cout << "\n";

    std::sort(latencies.begin(), latencies.end());
    double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min        : " << latencies.front() << " us\n";
    std::cout << "  Avg        : " << avg << " us\n";
    std::cout << "  99% worst  : " << latencies[size_t(latencies.size() * 0.99)] << " us\n";
    std::cout << "  99.9% worst: " << latencies[size_t(latencies.size() * 0.999)] << " us\n";
    PrintDoubleDivider();
}

// ------------------------------------------------------------
// Command Submission Latency (CPU -> GPU)
// ------------------------------------------------------------
void RunCommandLatencyTest(
    ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator,
    ID3D12GraphicsCommandList* list,
    ID3D12Fence* fence,
    HANDLE fenceEvent,
    UINT64& fenceValue,
    int iterations)
{
    std::cout << "Test 3 - CPU -> GPU Command Submission Latency\n";
    PrintDivider();

    std::vector<double> latencies; latencies.reserve(iterations);
    int lastPercent = -1;

    for (int i = 0; i < iterations; i++) {
        allocator->Reset();
        list->Reset(allocator, nullptr);
        list->Close();

        auto start = std::chrono::high_resolution_clock::now();
        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        WaitForFence(queue, fence, fenceEvent, fenceValue);
        auto end = std::chrono::high_resolution_clock::now();

        latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());

        int percent = int((i + 1) * 100 / iterations);
        if (percent != lastPercent) { std::cout << "\rProgress: " << percent << "% " << std::flush; lastPercent = percent; }
    }
    std::cout << "\n";

    std::sort(latencies.begin(), latencies.end());
    double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();

    PrintDivider();
    std::cout << "Results:\n";
    std::cout << "  Min        : " << latencies.front() << " us\n";
    std::cout << "  Avg        : " << avg << " us\n";
    std::cout << "  99% worst  : " << latencies[size_t(latencies.size() * 0.99)] << " us\n";
    std::cout << "  99.9% worst: " << latencies[size_t(latencies.size() * 0.999)] << " us\n";
    PrintDoubleDivider();
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main() {
    constexpr size_t LargeSize = 256ull * 1024 * 1024;
    constexpr size_t SmallSize = 1;
    constexpr int CommandIters = 100000;
    constexpr int TransferIters = 10000; // 10x faster
    constexpr int CopiesPerBatch = 8;
    constexpr int BandwidthBatches = 32;

    ComPtr<IDXGIFactory6> factory; CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc; adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
    }

    DXGI_ADAPTER_DESC1 ad; adapter->GetDesc1(&ad);
    std::wcout << L"Using GPU: " << ad.Description << L"\n";
    PrintDoubleDivider();

    ComPtr<ID3D12Device> device; D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ComPtr<ID3D12CommandAllocator> allocator; device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    ComPtr<ID3D12GraphicsCommandList> list; device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)); list->Close();
    ComPtr<ID3D12Fence> fence; device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr); UINT64 fenceValue = 1;

    // Bandwidth Tests
    auto cpuUpload = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, LargeSize, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto gpuDefault = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, LargeSize, D3D12_RESOURCE_STATE_COPY_DEST);
    RunBandwidthTest("Test 1 - CPU -> GPU 256MB Transfer Bandwidth", device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, cpuUpload, gpuDefault, LargeSize, CopiesPerBatch, BandwidthBatches);

    auto gpuSrc = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, LargeSize, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto cpuReadback = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, LargeSize, D3D12_RESOURCE_STATE_COPY_DEST);
    RunBandwidthTest("Test 2 - GPU -> CPU 256MB Transfer Bandwidth", device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, gpuSrc, cpuReadback, LargeSize, CopiesPerBatch, BandwidthBatches);

    // Latency Tests
    RunCommandLatencyTest(queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, CommandIters);

    auto cpuSmall = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, SmallSize, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto gpuSmall = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, SmallSize, D3D12_RESOURCE_STATE_COPY_DEST);
    RunLatencyTest("Test 4 - CPU -> GPU 1B Transfer Latency", device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, cpuSmall, gpuSmall, TransferIters);

    auto gpuSmallSrc = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, SmallSize, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto cpuSmallDst = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, SmallSize, D3D12_RESOURCE_STATE_COPY_DEST);
    RunLatencyTest("Test 5 - GPU -> CPU 1B Transfer Latency", device.Get(), queue.Get(), allocator.Get(), list.Get(), fence.Get(), fenceEvent, fenceValue, gpuSmallSrc, cpuSmallDst, TransferIters);

    CloseHandle(fenceEvent);
    std::cout << "Press any key to exit..."; _getch();
    return 0;
}
