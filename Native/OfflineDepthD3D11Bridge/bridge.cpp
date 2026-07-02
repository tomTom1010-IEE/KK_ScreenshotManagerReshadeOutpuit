#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr int kOk = 0;
constexpr int kError = 1;

enum ContextVTableIndex
{
    kDrawIndexed = 12,
    kDraw = 13,
    kDrawIndexedInstanced = 20,
    kDrawInstanced = 21,
    kOMSetRenderTargets = 33,
    kOMSetRenderTargetsAndUAV = 34,
    kOMSetDepthStencilState = 36,
    kDrawAuto = 38,
    kDrawIndexedInstancedIndirect = 39,
    kDrawInstancedIndirect = 40,
    kRSSetViewports = 44,
    kCopySubresourceRegion = 46,
    kCopyResource = 47,
    kClearRenderTargetView = 50,
    kClearDepthStencilView = 53,
    kResolveSubresource = 57,
};

struct HookSlot
{
    void** slot = nullptr;
    void* original = nullptr;
};

struct DsvStats
{
    uintptr_t dsv = 0;
    uintptr_t resource = 0;
    ID3D11Texture2D* retainedTexture = nullptr;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT sampleCount = 0;
    UINT sampleQuality = 0;
    UINT bindFlags = 0;
    UINT miscFlags = 0;
    UINT bindCount = 0;
    UINT clearCount = 0;
    UINT drawCount = 0;
    UINT lastBindSerial = 0;
};

struct PendingDepthReadback
{
    ID3D11Texture2D* staging = nullptr;
    UINT width = 0;
    UINT height = 0;
    std::wstring path;
    UINT sequence = 0;
    uintptr_t candidate = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct CandidateContentStats
{
    bool sampled = false;
    uint64_t finite = 0;
    uint64_t valid = 0;
    uint64_t nonZero = 0;
    float minValue = 0.0f;
    float maxValue = 0.0f;
    double meanNonZero = 0.0;
};

struct Texture2DInfo
{
    uintptr_t view = 0;
    uintptr_t resource = 0;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT sampleCount = 0;
    UINT sampleQuality = 0;
    UINT bindFlags = 0;
    UINT miscFlags = 0;
};

struct RenderPassStats
{
    UINT serial = 0;
    Texture2DInfo rtv0;
    Texture2DInfo dsv;
    bool hasViewport = false;
    D3D11_VIEWPORT viewport{};
    uintptr_t depthState = 0;
    bool hasDepthState = false;
    BOOL depthEnable = TRUE;
    D3D11_DEPTH_WRITE_MASK depthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    D3D11_COMPARISON_FUNC depthFunc = D3D11_COMPARISON_LESS;
    UINT stencilRef = 0;
    UINT bindCount = 0;
    UINT drawCount = 0;
    UINT clearRtvCount = 0;
    UINT clearDsvCount = 0;
    UINT lastDrawSerial = 0;
    UINT lastClearRtvSerial = 0;
    UINT lastClearDsvSerial = 0;
};

struct ResourceTransferStats
{
    UINT serial = 0;
    const wchar_t* op = L"";
    Texture2DInfo dst;
    Texture2DInfo src;
    DXGI_FORMAT resolveFormat = DXGI_FORMAT_UNKNOWN;
};

using OMSetRenderTargetsFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OMSetRenderTargetsAndUAVFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using OMSetDepthStencilStateFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
using RSSetViewportsFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using ClearRenderTargetViewFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using ClearDepthStencilViewFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using CopyResourceFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using CopySubresourceRegionFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using ResolveSubresourceFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
using DrawIndexedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawAutoFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndexedInstancedIndirectFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DrawInstancedIndirectFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);

std::mutex g_mutex;
std::wofstream g_log;
std::wstring g_logPath;
std::wstring g_lastError;
std::vector<HookSlot> g_hooks;
bool g_initialized = false;
bool g_captureActive = false;
int g_captureWidth = 0;
int g_captureHeight = 0;
UINT g_bindSerial = 0;
uintptr_t g_currentDsv = 0;
uintptr_t g_currentRtv0 = 0;
std::wstring g_captureLabel;
std::unordered_map<uintptr_t, DsvStats> g_dsvStats;
std::vector<RenderPassStats> g_renderPassStats;
std::vector<ResourceTransferStats> g_resourceTransferStats;
UINT g_passSerial = 0;
UINT g_currentPassSerial = 0;
UINT g_operationSerial = 0;
D3D11_VIEWPORT g_currentViewport{};
bool g_hasCurrentViewport = false;
uintptr_t g_currentDepthState = 0;
bool g_hasCurrentDepthState = false;
BOOL g_currentDepthEnable = TRUE;
D3D11_DEPTH_WRITE_MASK g_currentDepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
D3D11_COMPARISON_FUNC g_currentDepthFunc = D3D11_COMPARISON_LESS;
UINT g_currentStencilRef = 0;
UINT g_omSetCalls = 0;
UINT g_omSetNullDsvCalls = 0;
UINT g_clearDsvCalls = 0;
UINT g_drawCalls = 0;
ID3D11Device* g_unityDevice = nullptr;
bool g_unityContextHooksInstalled = false;
std::wstring g_pendingDepthRFloatPath;
std::vector<PendingDepthReadback> g_pendingDepthReadbacks;
UINT g_depthReadbackSequence = 0;
int g_lastDepthQueueResult = 0;
bool g_candidateDiagnosticsEnabled = false;
constexpr UINT kDepthReadbackMapDelay = 1;
constexpr size_t kMaxPendingDepthReadbacks = 4;

OMSetRenderTargetsFn g_OMSetRenderTargets = nullptr;
OMSetRenderTargetsAndUAVFn g_OMSetRenderTargetsAndUAV = nullptr;
OMSetDepthStencilStateFn g_OMSetDepthStencilState = nullptr;
RSSetViewportsFn g_RSSetViewports = nullptr;
ClearRenderTargetViewFn g_ClearRenderTargetView = nullptr;
ClearDepthStencilViewFn g_ClearDepthStencilView = nullptr;
CopyResourceFn g_CopyResource = nullptr;
CopySubresourceRegionFn g_CopySubresourceRegion = nullptr;
ResolveSubresourceFn g_ResolveSubresource = nullptr;
DrawIndexedFn g_DrawIndexed = nullptr;
DrawFn g_Draw = nullptr;
DrawIndexedInstancedFn g_DrawIndexedInstanced = nullptr;
DrawInstancedFn g_DrawInstanced = nullptr;
DrawAutoFn g_DrawAuto = nullptr;
DrawIndexedInstancedIndirectFn g_DrawIndexedInstancedIndirect = nullptr;
DrawInstancedIndirectFn g_DrawInstancedIndirect = nullptr;

void SetLastErrorText(const std::wstring& message)
{
    g_lastError = message;
}

std::wstring FormatToString(DXGI_FORMAT format)
{
    wchar_t buffer[32];
    std::swprintf(buffer, 32, L"%u", static_cast<unsigned>(format));
    return buffer;
}

void LogLineNoLock(const std::wstring& line)
{
    if (!g_log.is_open())
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    g_log << L"[" << st.wHour << L":" << st.wMinute << L":" << st.wSecond << L"." << st.wMilliseconds << L"] " << line << std::endl;
}

bool IsCaptureActiveNoLock()
{
    return g_initialized && g_captureActive;
}

bool FillTexture2DInfoFromResourceNoLock(ID3D11Resource* resource, Texture2DInfo* info)
{
    if (resource == nullptr || info == nullptr)
        return false;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    if (FAILED(hr) || texture == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    info->resource = reinterpret_cast<uintptr_t>(resource);
    info->width = desc.Width;
    info->height = desc.Height;
    info->format = desc.Format;
    info->sampleCount = desc.SampleDesc.Count;
    info->sampleQuality = desc.SampleDesc.Quality;
    info->bindFlags = desc.BindFlags;
    info->miscFlags = desc.MiscFlags;
    texture->Release();
    return true;
}

bool FillTexture2DInfoFromRtvNoLock(ID3D11RenderTargetView* rtv, Texture2DInfo* info)
{
    if (rtv == nullptr || info == nullptr)
        return false;

    info->view = reinterpret_cast<uintptr_t>(rtv);
    ID3D11Resource* resource = nullptr;
    rtv->GetResource(&resource);
    if (resource == nullptr)
        return false;

    const bool ok = FillTexture2DInfoFromResourceNoLock(resource, info);
    resource->Release();
    return ok;
}

bool FillTexture2DInfoFromDsvNoLock(ID3D11DepthStencilView* dsv, Texture2DInfo* info)
{
    if (dsv == nullptr || info == nullptr)
        return false;

    info->view = reinterpret_cast<uintptr_t>(dsv);
    ID3D11Resource* resource = nullptr;
    dsv->GetResource(&resource);
    if (resource == nullptr)
        return false;

    const bool ok = FillTexture2DInfoFromResourceNoLock(resource, info);
    resource->Release();
    return ok;
}

void ClearDsvStatsNoLock()
{
    for (auto& kv : g_dsvStats)
    {
        if (kv.second.retainedTexture != nullptr)
        {
            kv.second.retainedTexture->Release();
            kv.second.retainedTexture = nullptr;
        }
    }
    g_dsvStats.clear();
    g_renderPassStats.clear();
    g_resourceTransferStats.clear();
    g_currentDsv = 0;
    g_currentRtv0 = 0;
    g_currentPassSerial = 0;
    g_passSerial = 0;
    g_operationSerial = 0;
    g_hasCurrentViewport = false;
    g_currentViewport = D3D11_VIEWPORT{};
    g_currentDepthState = 0;
    g_hasCurrentDepthState = false;
    g_currentDepthEnable = TRUE;
    g_currentDepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    g_currentDepthFunc = D3D11_COMPARISON_LESS;
    g_currentStencilRef = 0;
}

void RecordDsvNoLock(ID3D11DepthStencilView* dsv)
{
    if (!IsCaptureActiveNoLock() || dsv == nullptr)
    {
        g_currentDsv = reinterpret_cast<uintptr_t>(dsv);
        return;
    }

    g_currentDsv = reinterpret_cast<uintptr_t>(dsv);

    ID3D11Resource* resource = nullptr;
    dsv->GetResource(&resource);
    if (resource == nullptr)
        return;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    if (FAILED(hr) || texture == nullptr)
    {
        resource->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    const auto key = reinterpret_cast<uintptr_t>(dsv);
    auto& stats = g_dsvStats[key];
    stats.dsv = key;
    stats.resource = reinterpret_cast<uintptr_t>(resource);
    if (stats.retainedTexture == nullptr)
    {
        texture->AddRef();
        stats.retainedTexture = texture;
    }
    stats.width = desc.Width;
    stats.height = desc.Height;
    stats.format = desc.Format;
    stats.sampleCount = desc.SampleDesc.Count;
    stats.sampleQuality = desc.SampleDesc.Quality;
    stats.bindFlags = desc.BindFlags;
    stats.miscFlags = desc.MiscFlags;
    stats.bindCount++;
    stats.lastBindSerial = ++g_bindSerial;

    texture->Release();
    resource->Release();
}

RenderPassStats* FindCurrentPassNoLock()
{
    if (g_currentPassSerial == 0)
        return nullptr;

    for (auto& pass : g_renderPassStats)
    {
        if (pass.serial == g_currentPassSerial)
            return &pass;
    }

    return nullptr;
}

void RecordOmSetNoLock(UINT numRtvs, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    if (IsCaptureActiveNoLock())
    {
        g_omSetCalls++;
        if (dsv == nullptr)
            g_omSetNullDsvCalls++;
    }

    RecordDsvNoLock(dsv);

    if (!IsCaptureActiveNoLock())
        return;

    Texture2DInfo rtvInfo{};
    if (rtvs != nullptr && numRtvs > 0 && rtvs[0] != nullptr)
        FillTexture2DInfoFromRtvNoLock(rtvs[0], &rtvInfo);

    Texture2DInfo dsvInfo{};
    if (dsv != nullptr)
        FillTexture2DInfoFromDsvNoLock(dsv, &dsvInfo);

    g_currentRtv0 = rtvInfo.view;

    RenderPassStats pass{};
    pass.serial = ++g_passSerial;
    pass.rtv0 = rtvInfo;
    pass.dsv = dsvInfo;
    pass.hasViewport = g_hasCurrentViewport;
    pass.viewport = g_currentViewport;
    pass.depthState = g_currentDepthState;
    pass.hasDepthState = g_hasCurrentDepthState;
    pass.depthEnable = g_currentDepthEnable;
    pass.depthWriteMask = g_currentDepthWriteMask;
    pass.depthFunc = g_currentDepthFunc;
    pass.stencilRef = g_currentStencilRef;
    pass.bindCount = 1;
    g_currentPassSerial = pass.serial;
    g_renderPassStats.push_back(pass);
}

void RecordDrawNoLock()
{
    if (!IsCaptureActiveNoLock())
        return;

    g_drawCalls++;
    if (g_currentDsv == 0)
        return;

    auto it = g_dsvStats.find(g_currentDsv);
    if (it != g_dsvStats.end())
        it->second.drawCount++;

    auto* pass = FindCurrentPassNoLock();
    if (pass != nullptr)
    {
        pass->drawCount++;
        pass->lastDrawSerial = ++g_operationSerial;
    }
}

void RecordClearNoLock(ID3D11DepthStencilView* dsv)
{
    if (!IsCaptureActiveNoLock() || dsv == nullptr)
        return;

    g_clearDsvCalls++;
    RecordDsvNoLock(dsv);
    auto it = g_dsvStats.find(reinterpret_cast<uintptr_t>(dsv));
    if (it != g_dsvStats.end())
        it->second.clearCount++;

    auto* pass = FindCurrentPassNoLock();
    if (pass != nullptr && pass->dsv.view == reinterpret_cast<uintptr_t>(dsv))
    {
        pass->clearDsvCount++;
        pass->lastClearDsvSerial = ++g_operationSerial;
    }
}

void STDMETHODCALLTYPE Hook_OMSetRenderTargets(ID3D11DeviceContext* ctx, UINT numViews, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        RecordOmSetNoLock(numViews, rtvs, dsv);
    }
    g_OMSetRenderTargets(ctx, numViews, rtvs, dsv);
}

void STDMETHODCALLTYPE Hook_OMSetRenderTargetsAndUAV(ID3D11DeviceContext* ctx, UINT numRtvs, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv, UINT uavStartSlot, UINT numUavs, ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        RecordOmSetNoLock(numRtvs, rtvs, dsv);
    }
    g_OMSetRenderTargetsAndUAV(ctx, numRtvs, rtvs, dsv, uavStartSlot, numUavs, uavs, initialCounts);
}

void STDMETHODCALLTYPE Hook_OMSetDepthStencilState(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* depthStencilState, UINT stencilRef)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_currentDepthState = reinterpret_cast<uintptr_t>(depthStencilState);
        g_currentStencilRef = stencilRef;
        g_hasCurrentDepthState = true;

        if (depthStencilState != nullptr)
        {
            D3D11_DEPTH_STENCIL_DESC desc{};
            depthStencilState->GetDesc(&desc);
            g_currentDepthEnable = desc.DepthEnable;
            g_currentDepthWriteMask = desc.DepthWriteMask;
            g_currentDepthFunc = desc.DepthFunc;
        }
        else
        {
            g_currentDepthEnable = TRUE;
            g_currentDepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            g_currentDepthFunc = D3D11_COMPARISON_LESS;
        }

        auto* pass = FindCurrentPassNoLock();
        if (pass != nullptr)
        {
            pass->depthState = g_currentDepthState;
            pass->hasDepthState = true;
            pass->depthEnable = g_currentDepthEnable;
            pass->depthWriteMask = g_currentDepthWriteMask;
            pass->depthFunc = g_currentDepthFunc;
            pass->stencilRef = g_currentStencilRef;
        }
    }
    g_OMSetDepthStencilState(ctx, depthStencilState, stencilRef);
}

void STDMETHODCALLTYPE Hook_RSSetViewports(ID3D11DeviceContext* ctx, UINT numViewports, const D3D11_VIEWPORT* viewports)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (numViewports > 0 && viewports != nullptr)
        {
            g_currentViewport = viewports[0];
            g_hasCurrentViewport = true;

            auto* pass = FindCurrentPassNoLock();
            if (pass != nullptr)
            {
                pass->viewport = g_currentViewport;
                pass->hasViewport = true;
            }
        }
    }
    g_RSSetViewports(ctx, numViewports, viewports);
}

void STDMETHODCALLTYPE Hook_ClearRenderTargetView(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const FLOAT colorRGBA[4])
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (IsCaptureActiveNoLock())
        {
            auto* pass = FindCurrentPassNoLock();
            if (pass != nullptr && pass->rtv0.view == reinterpret_cast<uintptr_t>(rtv))
            {
                pass->clearRtvCount++;
                pass->lastClearRtvSerial = ++g_operationSerial;
            }
        }
    }
    g_ClearRenderTargetView(ctx, rtv, colorRGBA);
}

void STDMETHODCALLTYPE Hook_ClearDepthStencilView(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT clearFlags, FLOAT depth, UINT8 stencil)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        RecordClearNoLock(dsv);
    }
    g_ClearDepthStencilView(ctx, dsv, clearFlags, depth, stencil);
}

void RecordTransferNoLock(const wchar_t* op, ID3D11Resource* dst, ID3D11Resource* src, DXGI_FORMAT resolveFormat = DXGI_FORMAT_UNKNOWN)
{
    if (!IsCaptureActiveNoLock())
        return;

    ResourceTransferStats transfer{};
    transfer.serial = ++g_operationSerial;
    transfer.op = op;
    transfer.resolveFormat = resolveFormat;
    FillTexture2DInfoFromResourceNoLock(dst, &transfer.dst);
    FillTexture2DInfoFromResourceNoLock(src, &transfer.src);

    if (transfer.dst.resource != 0 || transfer.src.resource != 0)
    {
        g_resourceTransferStats.push_back(transfer);
        if (g_resourceTransferStats.size() > 96)
            g_resourceTransferStats.erase(g_resourceTransferStats.begin());
    }
}

void STDMETHODCALLTYPE Hook_CopyResource(ID3D11DeviceContext* ctx, ID3D11Resource* dstResource, ID3D11Resource* srcResource)
{
    { std::lock_guard<std::mutex> lock(g_mutex); RecordTransferNoLock(L"CopyResource", dstResource, srcResource); }
    g_CopyResource(ctx, dstResource, srcResource);
}

void STDMETHODCALLTYPE Hook_CopySubresourceRegion(ID3D11DeviceContext* ctx, ID3D11Resource* dstResource, UINT dstSubresource, UINT dstX, UINT dstY, UINT dstZ, ID3D11Resource* srcResource, UINT srcSubresource, const D3D11_BOX* srcBox)
{
    { std::lock_guard<std::mutex> lock(g_mutex); RecordTransferNoLock(L"CopySubresourceRegion", dstResource, srcResource); }
    g_CopySubresourceRegion(ctx, dstResource, dstSubresource, dstX, dstY, dstZ, srcResource, srcSubresource, srcBox);
}

void STDMETHODCALLTYPE Hook_ResolveSubresource(ID3D11DeviceContext* ctx, ID3D11Resource* dstResource, UINT dstSubresource, ID3D11Resource* srcResource, UINT srcSubresource, DXGI_FORMAT format)
{
    { std::lock_guard<std::mutex> lock(g_mutex); RecordTransferNoLock(L"ResolveSubresource", dstResource, srcResource, format); }
    g_ResolveSubresource(ctx, dstResource, dstSubresource, srcResource, srcSubresource, format);
}

void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndexLocation, INT baseVertexLocation)
{
    { std::lock_guard<std::mutex> lock(g_mutex); RecordDrawNoLock(); }
    g_DrawIndexed(ctx, indexCount, startIndexLocation, baseVertexLocation);
}

void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* ctx, UINT vertexCount, UINT startVertexLocation)
{
    { std::lock_guard<std::mutex> lock(g_mutex); RecordDrawNoLock(); }
    g_Draw(ctx, vertexCount, startVertexLocation);
}

void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT indexCountPerInstance, UINT instanceCount, UINT startIndexLocation, INT baseVertexLocation, UINT startInstanceLocation)
{
    { std::lock_guard<std::mutex> lock(g_mutex); RecordDrawNoLock(); }
    g_DrawIndexedInstanced(ctx, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
}

void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* ctx, UINT vertexCountPerInstance, UINT instanceCount, UINT startVertexLocation, UINT startInstanceLocation)
{
    { std::lock_guard<std::mutex> lock(g_mutex); RecordDrawNoLock(); }
    g_DrawInstanced(ctx, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
}

void STDMETHODCALLTYPE Hook_DrawAuto(ID3D11DeviceContext* ctx)
{
    { std::lock_guard<std::mutex> lock(g_mutex); RecordDrawNoLock(); }
    g_DrawAuto(ctx);
}

void STDMETHODCALLTYPE Hook_DrawIndexedInstancedIndirect(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT alignedByteOffsetForArgs)
{
    { std::lock_guard<std::mutex> lock(g_mutex); RecordDrawNoLock(); }
    g_DrawIndexedInstancedIndirect(ctx, args, alignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE Hook_DrawInstancedIndirect(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT alignedByteOffsetForArgs)
{
    { std::lock_guard<std::mutex> lock(g_mutex); RecordDrawNoLock(); }
    g_DrawInstancedIndirect(ctx, args, alignedByteOffsetForArgs);
}

bool ReplaceVTableSlot(void** vtable, int index, void* replacement, void** originalOut)
{
    void** slot = &vtable[index];
    for (const auto& hook : g_hooks)
    {
        if (hook.slot == slot)
        {
            if (originalOut != nullptr)
                *originalOut = hook.original;
            return true;
        }
    }

    if (*slot == replacement)
        return true;

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    if (originalOut != nullptr)
        *originalOut = *slot;
    *slot = replacement;
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

    HookSlot hook;
    hook.slot = slot;
    hook.original = originalOut != nullptr ? *originalOut : nullptr;
    g_hooks.push_back(hook);
    return true;
}

bool InstallHooksForContextNoLock(ID3D11DeviceContext* context, const wchar_t* source)
{
    if (context == nullptr)
        return false;

    void** vtable = *reinterpret_cast<void***>(context);
    bool ok = true;
    void* original = nullptr;

#define INSTALL_CONTEXT_HOOK(slotIndex, hookFn, fnType, fnStorage) \
    original = nullptr; \
    ok &= ReplaceVTableSlot(vtable, slotIndex, reinterpret_cast<void*>(&hookFn), &original); \
    if (ok && fnStorage == nullptr && original != nullptr) \
        fnStorage = reinterpret_cast<fnType>(original)

    INSTALL_CONTEXT_HOOK(kOMSetRenderTargets, Hook_OMSetRenderTargets, OMSetRenderTargetsFn, g_OMSetRenderTargets);
    INSTALL_CONTEXT_HOOK(kOMSetRenderTargetsAndUAV, Hook_OMSetRenderTargetsAndUAV, OMSetRenderTargetsAndUAVFn, g_OMSetRenderTargetsAndUAV);
    INSTALL_CONTEXT_HOOK(kOMSetDepthStencilState, Hook_OMSetDepthStencilState, OMSetDepthStencilStateFn, g_OMSetDepthStencilState);
    INSTALL_CONTEXT_HOOK(kRSSetViewports, Hook_RSSetViewports, RSSetViewportsFn, g_RSSetViewports);
    INSTALL_CONTEXT_HOOK(kCopySubresourceRegion, Hook_CopySubresourceRegion, CopySubresourceRegionFn, g_CopySubresourceRegion);
    INSTALL_CONTEXT_HOOK(kCopyResource, Hook_CopyResource, CopyResourceFn, g_CopyResource);
    INSTALL_CONTEXT_HOOK(kClearRenderTargetView, Hook_ClearRenderTargetView, ClearRenderTargetViewFn, g_ClearRenderTargetView);
    INSTALL_CONTEXT_HOOK(kClearDepthStencilView, Hook_ClearDepthStencilView, ClearDepthStencilViewFn, g_ClearDepthStencilView);
    INSTALL_CONTEXT_HOOK(kResolveSubresource, Hook_ResolveSubresource, ResolveSubresourceFn, g_ResolveSubresource);
    INSTALL_CONTEXT_HOOK(kDrawIndexed, Hook_DrawIndexed, DrawIndexedFn, g_DrawIndexed);
    INSTALL_CONTEXT_HOOK(kDraw, Hook_Draw, DrawFn, g_Draw);
    INSTALL_CONTEXT_HOOK(kDrawIndexedInstanced, Hook_DrawIndexedInstanced, DrawIndexedInstancedFn, g_DrawIndexedInstanced);
    INSTALL_CONTEXT_HOOK(kDrawInstanced, Hook_DrawInstanced, DrawInstancedFn, g_DrawInstanced);
    INSTALL_CONTEXT_HOOK(kDrawAuto, Hook_DrawAuto, DrawAutoFn, g_DrawAuto);
    INSTALL_CONTEXT_HOOK(kDrawIndexedInstancedIndirect, Hook_DrawIndexedInstancedIndirect, DrawIndexedInstancedIndirectFn, g_DrawIndexedInstancedIndirect);
    INSTALL_CONTEXT_HOOK(kDrawInstancedIndirect, Hook_DrawInstancedIndirect, DrawInstancedIndirectFn, g_DrawInstancedIndirect);

#undef INSTALL_CONTEXT_HOOK

    std::wstringstream ss;
    ss << L"hooks " << (ok ? L"installed" : L"failed") << L" for " << source
       << L" context=0x" << std::hex << reinterpret_cast<uintptr_t>(context);
    LogLineNoLock(ss.str());
    return ok;
}

bool InstallHooks()
{
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL selected{};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &device, &selected, &context);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &device, &selected, &context);

    if (FAILED(hr) || context == nullptr)
    {
        std::wstringstream ss;
        ss << L"D3D11CreateDevice failed: 0x" << std::hex << static_cast<unsigned>(hr);
        SetLastErrorText(ss.str());
        return false;
    }

    bool ok = InstallHooksForContextNoLock(context, L"dummy D3D11");

    context->Release();
    device->Release();

    if (!ok)
        SetLastErrorText(L"Failed to replace one or more D3D11 context vtable slots");
    return ok;
}

void RemoveHooks()
{
    for (auto it = g_hooks.rbegin(); it != g_hooks.rend(); ++it)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(it->slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            *it->slot = it->original;
            VirtualProtect(it->slot, sizeof(void*), oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), it->slot, sizeof(void*));
        }
    }
    g_hooks.clear();
}

int ScoreCandidate(const DsvStats& stats)
{
    int score = 0;
    if (static_cast<int>(stats.width) == g_captureWidth && static_cast<int>(stats.height) == g_captureHeight)
        score += 100;
    if ((stats.bindFlags & D3D11_BIND_SHADER_RESOURCE) != 0)
        score += 40;
    if (stats.sampleCount == 1)
        score += 30;
    if (stats.bindCount > 0)
        score += 50;
    if (stats.drawCount > 0)
        score += 25;
    score += static_cast<int>(std::min<UINT>(stats.drawCount, 64)) * 3;
    score += static_cast<int>(std::min<UINT>(stats.bindCount, 16));
    score += static_cast<int>(std::min<UINT>(stats.clearCount, 4)) * 2;
    if (stats.clearCount < stats.bindCount + stats.drawCount)
        score += 10;
    if (stats.format == DXGI_FORMAT_R24G8_TYPELESS || stats.format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
        stats.format == DXGI_FORMAT_R32_TYPELESS || stats.format == DXGI_FORMAT_D32_FLOAT)
        score += 10;
    return score;
}

DXGI_FORMAT GetDepthSrvFormat(DXGI_FORMAT textureFormat)
{
    switch (textureFormat)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

const DsvStats* SelectBestReadableDepthNoLock()
{
    const DsvStats* best = nullptr;
    int bestScore = -9999;
    for (const auto& kv : g_dsvStats)
    {
        const auto& stats = kv.second;
        if (stats.retainedTexture == nullptr)
            continue;
        if ((stats.bindFlags & D3D11_BIND_SHADER_RESOURCE) == 0)
            continue;
        if (stats.sampleCount != 1)
            continue;
        if (GetDepthSrvFormat(stats.format) == DXGI_FORMAT_UNKNOWN)
            continue;

        const int score = ScoreCandidate(stats);
        if (score > bestScore ||
            (score == bestScore && best != nullptr && stats.drawCount > best->drawCount) ||
            (score == bestScore && best != nullptr && stats.drawCount == best->drawCount && stats.lastBindSerial < best->lastBindSerial))
        {
            best = &stats;
            bestScore = score;
        }
    }
    return best;
}

std::vector<const DsvStats*> GetReadableDepthCandidatesNoLock()
{
    std::vector<const DsvStats*> candidates;
    for (const auto& kv : g_dsvStats)
    {
        const auto& stats = kv.second;
        if (stats.retainedTexture == nullptr)
            continue;
        if ((stats.bindFlags & D3D11_BIND_SHADER_RESOURCE) == 0)
            continue;
        if (stats.sampleCount != 1)
            continue;
        if (GetDepthSrvFormat(stats.format) == DXGI_FORMAT_UNKNOWN)
            continue;
        candidates.push_back(&stats);
    }

    std::sort(candidates.begin(), candidates.end(), [](const DsvStats* a, const DsvStats* b) {
        const int scoreA = ScoreCandidate(*a);
        const int scoreB = ScoreCandidate(*b);
        if (scoreA != scoreB)
            return scoreA > scoreB;
        if (a->drawCount != b->drawCount)
            return a->drawCount > b->drawCount;
        return a->lastBindSerial < b->lastBindSerial;
    });
    return candidates;
}

bool CompileShaderNoLock(const char* source, const char* entry, const char* target, ID3DBlob** blob)
{
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, blob, &errors);
    if (FAILED(hr))
    {
        std::wstringstream ss;
        ss << L"D3DCompile failed for " << entry << L"/" << target << L" hr=0x" << std::hex << static_cast<unsigned>(hr);
        if (errors != nullptr)
        {
            ss << L" error=";
            const char* text = static_cast<const char*>(errors->GetBufferPointer());
            if (text != nullptr)
            {
                while (*text != '\0')
                    ss << static_cast<wchar_t>(*text++);
            }
            errors->Release();
        }
        SetLastErrorText(ss.str());
        LogLineNoLock(g_lastError);
        return false;
    }

    if (errors != nullptr)
        errors->Release();
    return true;
}

void LogCandidateDiagnosticsNoLock()
{
    const auto candidates = GetReadableDepthCandidatesNoLock();
    std::wstringstream begin;
    begin << L"D3D11 candidate diagnostic begin count=" << candidates.size();
    LogLineNoLock(begin.str());

    if (candidates.empty())
        return;

    ID3D11Device* device = nullptr;
    candidates[0]->retainedTexture->GetDevice(&device);
    if (device == nullptr)
    {
        LogLineNoLock(L"D3D11 candidate diagnostic skipped because depth texture GetDevice returned null");
        return;
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context == nullptr)
    {
        device->Release();
        LogLineNoLock(L"D3D11 candidate diagnostic skipped because GetImmediateContext returned null");
        return;
    }

    const char* shaderSource =
        "Texture2D<float> DepthTex : register(t0);\n"
        "SamplerState PointSampler : register(s0);\n"
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) {\n"
        "  VSOut o;\n"
        "  float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv;\n"
        "  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "  return o;\n"
        "}\n"
        "float ps(VSOut i) : SV_Target { return DepthTex.SampleLevel(PointSampler, i.uv, 0); }\n";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* sampler = nullptr;

    do
    {
        if (!CompileShaderNoLock(shaderSource, "vs", "vs_4_0", &vsBlob))
            break;
        if (!CompileShaderNoLock(shaderSource, "ps", "ps_4_0", &psBlob))
            break;

        HRESULT hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
        if (FAILED(hr))
        {
            LogLineNoLock(L"D3D11 candidate diagnostic CreateVertexShader failed");
            break;
        }

        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
        if (FAILED(hr))
        {
            LogLineNoLock(L"D3D11 candidate diagnostic CreatePixelShader failed");
            break;
        }

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&samplerDesc, &sampler);
        if (FAILED(hr))
        {
            LogLineNoLock(L"D3D11 candidate diagnostic CreateSamplerState failed");
            break;
        }

        UINT index = 0;
        for (const auto* candidate : candidates)
        {
            ID3D11ShaderResourceView* srv = nullptr;
            ID3D11Texture2D* output = nullptr;
            ID3D11RenderTargetView* rtv = nullptr;
            ID3D11Texture2D* staging = nullptr;

            const UINT sampleWidth = 256;
            UINT sampleHeight = candidate->width > 0
                ? static_cast<UINT>((static_cast<uint64_t>(candidate->height) * sampleWidth + candidate->width / 2) / candidate->width)
                : 144;
            sampleHeight = std::max<UINT>(1, std::min<UINT>(256, sampleHeight));

            do
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = GetDepthSrvFormat(candidate->format);
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;
                hr = device->CreateShaderResourceView(candidate->retainedTexture, &srvDesc, &srv);
                if (FAILED(hr))
                {
                    std::wstringstream ss;
                    ss << L"D3D11 candidate diagnostic " << index << L" CreateShaderResourceView failed hr=0x"
                       << std::hex << static_cast<unsigned>(hr) << std::dec
                       << L" dsv=0x" << std::hex << candidate->dsv << std::dec
                       << L" format=" << FormatToString(candidate->format)
                       << L" srvFormat=" << FormatToString(srvDesc.Format);
                    LogLineNoLock(ss.str());
                    break;
                }

                D3D11_TEXTURE2D_DESC outDesc{};
                outDesc.Width = sampleWidth;
                outDesc.Height = sampleHeight;
                outDesc.MipLevels = 1;
                outDesc.ArraySize = 1;
                outDesc.Format = DXGI_FORMAT_R32_FLOAT;
                outDesc.SampleDesc.Count = 1;
                outDesc.Usage = D3D11_USAGE_DEFAULT;
                outDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
                hr = device->CreateTexture2D(&outDesc, nullptr, &output);
                if (FAILED(hr))
                {
                    LogLineNoLock(L"D3D11 candidate diagnostic Create output texture failed");
                    break;
                }

                hr = device->CreateRenderTargetView(output, nullptr, &rtv);
                if (FAILED(hr))
                {
                    LogLineNoLock(L"D3D11 candidate diagnostic CreateRenderTargetView failed");
                    break;
                }

                D3D11_TEXTURE2D_DESC stagingDesc = outDesc;
                stagingDesc.Usage = D3D11_USAGE_STAGING;
                stagingDesc.BindFlags = 0;
                stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
                if (FAILED(hr))
                {
                    LogLineNoLock(L"D3D11 candidate diagnostic Create staging texture failed");
                    break;
                }

                D3D11_VIEWPORT viewport{};
                viewport.Width = static_cast<float>(sampleWidth);
                viewport.Height = static_cast<float>(sampleHeight);
                viewport.MinDepth = 0.0f;
                viewport.MaxDepth = 1.0f;

                ID3D11RenderTargetView* rtvs[] = { rtv };
                context->OMSetRenderTargets(1, rtvs, nullptr);
                context->RSSetViewports(1, &viewport);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(vs, nullptr, 0);
                context->PSSetShader(ps, nullptr, 0);
                context->PSSetShaderResources(0, 1, &srv);
                context->PSSetSamplers(0, 1, &sampler);
                context->Draw(3, 0);

                ID3D11ShaderResourceView* nullSrv[] = { nullptr };
                context->PSSetShaderResources(0, 1, nullSrv);
                context->CopyResource(staging, output);
                context->Flush();

                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
                if (FAILED(hr))
                {
                    std::wstringstream ss;
                    ss << L"D3D11 candidate diagnostic " << index << L" Map failed hr=0x"
                       << std::hex << static_cast<unsigned>(hr);
                    LogLineNoLock(ss.str());
                    break;
                }

                uint64_t finite = 0;
                uint64_t valid = 0;
                uint64_t nonZero = 0;
                double sum = 0.0;
                float minValue = 3.402823466e+38F;
                float maxValue = -3.402823466e+38F;
                const auto* base = static_cast<const unsigned char*>(mapped.pData);
                for (UINT y = 0; y < sampleHeight; y++)
                {
                    const auto* row = reinterpret_cast<const float*>(base + static_cast<size_t>(mapped.RowPitch) * y);
                    for (UINT x = 0; x < sampleWidth; x++)
                    {
                        const float value = row[x];
                        if (!std::isfinite(value))
                            continue;
                        finite++;
                        if (value < -0.0001f || value > 1.0001f)
                            continue;
                        valid++;
                        if (std::fabs(value) > 0.0000001f)
                        {
                            nonZero++;
                            sum += value;
                            if (value < minValue)
                                minValue = value;
                            if (value > maxValue)
                                maxValue = value;
                        }
                    }
                }
                context->Unmap(staging, 0);

                if (nonZero == 0)
                {
                    minValue = 0.0f;
                    maxValue = 0.0f;
                }

                std::wstringstream ss;
                ss << L"D3D11 candidate diagnostic " << index
                   << L" dsv=0x" << std::hex << candidate->dsv
                   << L" resource=0x" << candidate->resource << std::dec
                   << L" score=" << ScoreCandidate(*candidate)
                   << L" size=" << candidate->width << L"x" << candidate->height
                   << L" sample=" << sampleWidth << L"x" << sampleHeight
                   << L" format=" << FormatToString(candidate->format)
                   << L" binds=" << candidate->bindCount
                   << L" clears=" << candidate->clearCount
                   << L" draws=" << candidate->drawCount
                   << L" lastBind=" << candidate->lastBindSerial
                   << L" finite=" << finite
                   << L" valid01=" << valid
                   << L" nonZero=" << nonZero
                   << L" min=" << minValue
                   << L" max=" << maxValue
                   << L" meanNonZero=" << (nonZero > 0 ? sum / static_cast<double>(nonZero) : 0.0);
                LogLineNoLock(ss.str());
            } while (false);

            if (staging) staging->Release();
            if (rtv) rtv->Release();
            if (output) output->Release();
            if (srv) srv->Release();
            index++;
        }
    } while (false);

    if (sampler) sampler->Release();
    if (ps) ps->Release();
    if (vs) vs->Release();
    if (psBlob) psBlob->Release();
    if (vsBlob) vsBlob->Release();
    context->Release();
    device->Release();
    LogLineNoLock(L"D3D11 candidate diagnostic end");
}

bool SampleDepthCandidateContentNoLock(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11VertexShader* vs,
    ID3D11PixelShader* ps,
    ID3D11SamplerState* sampler,
    const DsvStats* candidate,
    UINT sampleWidth,
    CandidateContentStats* stats)
{
    if (device == nullptr || context == nullptr || vs == nullptr || ps == nullptr || sampler == nullptr ||
        candidate == nullptr || candidate->retainedTexture == nullptr || stats == nullptr)
        return false;

    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D* output = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* staging = nullptr;
    bool ok = false;

    UINT sampleHeight = candidate->width > 0
        ? static_cast<UINT>((static_cast<uint64_t>(candidate->height) * sampleWidth + candidate->width / 2) / candidate->width)
        : 64;
    sampleHeight = std::max<UINT>(1, std::min<UINT>(256, sampleHeight));

    do
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = GetDepthSrvFormat(candidate->format);
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        HRESULT hr = device->CreateShaderResourceView(candidate->retainedTexture, &srvDesc, &srv);
        if (FAILED(hr))
            break;

        D3D11_TEXTURE2D_DESC outDesc{};
        outDesc.Width = sampleWidth;
        outDesc.Height = sampleHeight;
        outDesc.MipLevels = 1;
        outDesc.ArraySize = 1;
        outDesc.Format = DXGI_FORMAT_R32_FLOAT;
        outDesc.SampleDesc.Count = 1;
        outDesc.Usage = D3D11_USAGE_DEFAULT;
        outDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = device->CreateTexture2D(&outDesc, nullptr, &output);
        if (FAILED(hr))
            break;

        hr = device->CreateRenderTargetView(output, nullptr, &rtv);
        if (FAILED(hr))
            break;

        D3D11_TEXTURE2D_DESC stagingDesc = outDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
        if (FAILED(hr))
            break;

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(sampleWidth);
        viewport.Height = static_cast<float>(sampleHeight);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        ID3D11RenderTargetView* rtvs[] = { rtv };
        context->OMSetRenderTargets(1, rtvs, nullptr);
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, &sampler);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrv[] = { nullptr };
        context->PSSetShaderResources(0, 1, nullSrv);
        context->CopyResource(staging, output);
        context->Flush();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
            break;

        uint64_t finite = 0;
        uint64_t valid = 0;
        uint64_t nonZero = 0;
        double sum = 0.0;
        float minValue = 3.402823466e+38F;
        float maxValue = -3.402823466e+38F;
        const auto* base = static_cast<const unsigned char*>(mapped.pData);
        for (UINT y = 0; y < sampleHeight; y++)
        {
            const auto* row = reinterpret_cast<const float*>(base + static_cast<size_t>(mapped.RowPitch) * y);
            for (UINT x = 0; x < sampleWidth; x++)
            {
                const float value = row[x];
                if (!std::isfinite(value))
                    continue;
                finite++;
                if (value < -0.0001f || value > 1.0001f)
                    continue;
                valid++;
                if (std::fabs(value) > 0.0000001f)
                {
                    nonZero++;
                    sum += value;
                    if (value < minValue)
                        minValue = value;
                    if (value > maxValue)
                        maxValue = value;
                }
            }
        }
        context->Unmap(staging, 0);

        if (nonZero == 0)
        {
            minValue = 0.0f;
            maxValue = 0.0f;
        }

        stats->sampled = true;
        stats->finite = finite;
        stats->valid = valid;
        stats->nonZero = nonZero;
        stats->minValue = minValue;
        stats->maxValue = maxValue;
        stats->meanNonZero = nonZero > 0 ? sum / static_cast<double>(nonZero) : 0.0;
        ok = true;
    } while (false);

    if (staging) staging->Release();
    if (rtv) rtv->Release();
    if (output) output->Release();
    if (srv) srv->Release();
    return ok;
}

const DsvStats* SelectBestReadableDepthValidatedNoLock()
{
    const auto candidates = GetReadableDepthCandidatesNoLock();
    if (candidates.empty())
        return nullptr;

    ID3D11Device* device = nullptr;
    candidates[0]->retainedTexture->GetDevice(&device);
    if (device == nullptr)
        return SelectBestReadableDepthNoLock();

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context == nullptr)
    {
        device->Release();
        return SelectBestReadableDepthNoLock();
    }

    const char* shaderSource =
        "Texture2D<float> DepthTex : register(t0);\n"
        "SamplerState PointSampler : register(s0);\n"
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) {\n"
        "  VSOut o;\n"
        "  float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv;\n"
        "  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "  return o;\n"
        "}\n"
        "float ps(VSOut i) : SV_Target { return DepthTex.SampleLevel(PointSampler, i.uv, 0); }\n";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    const DsvStats* bestExactNonZero = nullptr;
    const DsvStats* bestAnyNonZero = nullptr;
    CandidateContentStats bestExactStats{};
    CandidateContentStats bestAnyStats{};
    bool sampledAnyCandidate = false;

    do
    {
        if (!CompileShaderNoLock(shaderSource, "vs", "vs_4_0", &vsBlob))
            break;
        if (!CompileShaderNoLock(shaderSource, "ps", "ps_4_0", &psBlob))
            break;

        HRESULT hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
        if (FAILED(hr))
            break;
        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
        if (FAILED(hr))
            break;

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&samplerDesc, &sampler);
        if (FAILED(hr))
            break;

        for (const auto* candidate : candidates)
        {
            CandidateContentStats stats{};
            if (!SampleDepthCandidateContentNoLock(device, context, vs, ps, sampler, candidate, 128, &stats))
                continue;
            sampledAnyCandidate = true;

            std::wstringstream line;
            line << L"D3D11 candidate validation dsv=0x" << std::hex << candidate->dsv << std::dec
                 << L" score=" << ScoreCandidate(*candidate)
                 << L" size=" << candidate->width << L"x" << candidate->height
                 << L" nonZero=" << stats.nonZero
                 << L" min=" << stats.minValue
                 << L" max=" << stats.maxValue
                 << L" meanNonZero=" << stats.meanNonZero;
            LogLineNoLock(line.str());

            if (stats.nonZero == 0)
                continue;

            const bool exactSize = static_cast<int>(candidate->width) == g_captureWidth &&
                static_cast<int>(candidate->height) == g_captureHeight;

            if (exactSize && (bestExactNonZero == nullptr ||
                ScoreCandidate(*candidate) > ScoreCandidate(*bestExactNonZero) ||
                (ScoreCandidate(*candidate) == ScoreCandidate(*bestExactNonZero) && stats.nonZero > bestExactStats.nonZero)))
            {
                bestExactNonZero = candidate;
                bestExactStats = stats;
            }

            if (bestAnyNonZero == nullptr ||
                ScoreCandidate(*candidate) > ScoreCandidate(*bestAnyNonZero) ||
                (ScoreCandidate(*candidate) == ScoreCandidate(*bestAnyNonZero) && stats.nonZero > bestAnyStats.nonZero))
            {
                bestAnyNonZero = candidate;
                bestAnyStats = stats;
            }
        }
    } while (false);

    if (sampler) sampler->Release();
    if (ps) ps->Release();
    if (vs) vs->Release();
    if (psBlob) psBlob->Release();
    if (vsBlob) vsBlob->Release();
    context->Release();
    device->Release();

    if (bestExactNonZero != nullptr)
    {
        std::wstringstream ss;
        ss << L"D3D11 depth candidate validation selected dsv=0x" << std::hex << bestExactNonZero->dsv << std::dec
           << L" nonZero=" << bestExactStats.nonZero
           << L" exactSize=true";
        LogLineNoLock(ss.str());
        return bestExactNonZero;
    }

    if (!sampledAnyCandidate)
    {
        LogLineNoLock(L"D3D11 depth candidate validation could not sample any readable candidate; falling back to static score");
        return SelectBestReadableDepthNoLock();
    }

    if (bestAnyNonZero != nullptr)
    {
        std::wstringstream ss;
        ss << L"D3D11 depth candidate validation found nonzero candidates, but none matched requested size "
           << g_captureWidth << L"x" << g_captureHeight
           << L"; bestNonExact dsv=0x" << std::hex << bestAnyNonZero->dsv << std::dec
           << L" size=" << bestAnyNonZero->width << L"x" << bestAnyNonZero->height
           << L" nonZero=" << bestAnyStats.nonZero;
        SetLastErrorText(ss.str());
        LogLineNoLock(g_lastError);
        return nullptr;
    }

    SetLastErrorText(L"D3D11 depth candidate validation found only empty readable candidates");
    LogLineNoLock(g_lastError);
    return nullptr;
}

bool WriteMappedRFloatNoLock(ID3D11DeviceContext* context, ID3D11Texture2D* staging, UINT width, UINT height, const std::wstring& path, UINT mapFlags, bool* stillDrawing)
{
    if (stillDrawing != nullptr)
        *stillDrawing = false;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, mapFlags, &mapped);
    if (FAILED(hr))
    {
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            if (stillDrawing != nullptr)
                *stillDrawing = true;
            return false;
        }

        std::wstringstream ss;
        ss << L"Map staging RFloat failed hr=0x" << std::hex << static_cast<unsigned>(hr);
        SetLastErrorText(ss.str());
        LogLineNoLock(g_lastError);
        return false;
    }

    FILE* file = _wfopen(path.c_str(), L"wb");
    if (file == nullptr)
    {
        context->Unmap(staging, 0);
        SetLastErrorText(L"Failed to open D3D11 depth output file");
        LogLineNoLock(g_lastError);
        return false;
    }

    const auto rowBytes = static_cast<size_t>(width) * sizeof(float);
    const auto* src = static_cast<const unsigned char*>(mapped.pData);
    for (UINT y = 0; y < height; y++)
        std::fwrite(src + static_cast<size_t>(mapped.RowPitch) * y, 1, rowBytes, file);

    std::fclose(file);
    context->Unmap(staging, 0);
    return true;
}

void ReleasePendingDepthReadbacksNoLock()
{
    for (auto& pending : g_pendingDepthReadbacks)
    {
        if (pending.staging != nullptr)
        {
            pending.staging->Release();
            pending.staging = nullptr;
        }
    }
    g_pendingDepthReadbacks.clear();
}

size_t RemovePendingDepthReadbacksForPathNoLock(const std::wstring& path)
{
    size_t removed = 0;
    for (size_t i = 0; i < g_pendingDepthReadbacks.size();)
    {
        auto& pending = g_pendingDepthReadbacks[i];
        if (pending.path != path)
        {
            ++i;
            continue;
        }

        if (pending.staging != nullptr)
            pending.staging->Release();
        g_pendingDepthReadbacks.erase(g_pendingDepthReadbacks.begin() + static_cast<std::ptrdiff_t>(i));
        ++removed;
    }

    if (removed > 0)
    {
        std::wstringstream ss;
        ss << L"D3D11 depth pending readback replaced for path: " << path
           << L" removed=" << removed;
        LogLineNoLock(ss.str());
    }

    return removed;
}

size_t DrainPendingDepthReadbacksNoLock(ID3D11DeviceContext* context, bool block, UINT minAge)
{
    size_t saved = 0;
    for (size_t i = 0; i < g_pendingDepthReadbacks.size();)
    {
        auto& pending = g_pendingDepthReadbacks[i];
        if (!block && g_depthReadbackSequence - pending.sequence < minAge)
        {
            ++i;
            continue;
        }

        bool stillDrawing = false;
        const bool ok = WriteMappedRFloatNoLock(
            context,
            pending.staging,
            pending.width,
            pending.height,
            pending.path,
            block ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT,
            &stillDrawing);

        if (stillDrawing)
        {
            ++i;
            continue;
        }

        if (ok)
        {
            std::wstringstream ss;
            ss << L"D3D11 depth rfloat saved: " << pending.path
               << L" candidate=0x" << std::hex << pending.candidate << std::dec
               << L" size=" << pending.width << L"x" << pending.height
               << L" format=" << FormatToString(pending.format)
               << L" sequence=" << pending.sequence
               << L" block=" << (block ? L"true" : L"false");
            LogLineNoLock(ss.str());
            ++saved;
        }
        else if (!g_lastError.empty())
        {
            LogLineNoLock(L"D3D11 depth rfloat save failed: " + g_lastError);
        }

        if (pending.staging != nullptr)
            pending.staging->Release();
        g_pendingDepthReadbacks.erase(g_pendingDepthReadbacks.begin() + static_cast<std::ptrdiff_t>(i));
    }

    return saved;
}

bool EnqueueBestDepthRFloatReadbackNoLock(const std::wstring& path)
{
    const DsvStats* candidate = SelectBestReadableDepthValidatedNoLock();
    if (candidate == nullptr)
    {
        SetLastErrorText(L"No readable non-MSAA DSV candidate is available");
        LogLineNoLock(g_lastError);
        return false;
    }

    ID3D11Device* device = nullptr;
    candidate->retainedTexture->GetDevice(&device);
    if (device == nullptr)
    {
        SetLastErrorText(L"Depth texture GetDevice returned null");
        LogLineNoLock(g_lastError);
        return false;
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context == nullptr)
    {
        device->Release();
        SetLastErrorText(L"Depth texture GetImmediateContext returned null");
        LogLineNoLock(g_lastError);
        return false;
    }

    const char* shaderSource =
        "Texture2D<float> DepthTex : register(t0);\n"
        "SamplerState PointSampler : register(s0);\n"
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "VSOut vs(uint id : SV_VertexID) {\n"
        "  VSOut o;\n"
        "  float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv;\n"
        "  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "  return o;\n"
        "}\n"
        "float ps(VSOut i) : SV_Target { return DepthTex.SampleLevel(PointSampler, i.uv, 0); }\n";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11Texture2D* output = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* staging = nullptr;
    bool ok = false;

    do
    {
        if (!CompileShaderNoLock(shaderSource, "vs", "vs_4_0", &vsBlob))
            break;
        if (!CompileShaderNoLock(shaderSource, "ps", "ps_4_0", &psBlob))
            break;

        HRESULT hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
        if (FAILED(hr))
        {
            SetLastErrorText(L"CreateVertexShader failed");
            break;
        }

        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
        if (FAILED(hr))
        {
            SetLastErrorText(L"CreatePixelShader failed");
            break;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = GetDepthSrvFormat(candidate->format);
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = device->CreateShaderResourceView(candidate->retainedTexture, &srvDesc, &srv);
        if (FAILED(hr))
        {
            std::wstringstream ss;
            ss << L"CreateShaderResourceView for depth failed hr=0x" << std::hex << static_cast<unsigned>(hr)
               << L" format=" << FormatToString(candidate->format)
               << L" srvFormat=" << FormatToString(srvDesc.Format);
            SetLastErrorText(ss.str());
            break;
        }

        D3D11_TEXTURE2D_DESC outDesc{};
        outDesc.Width = candidate->width;
        outDesc.Height = candidate->height;
        outDesc.MipLevels = 1;
        outDesc.ArraySize = 1;
        outDesc.Format = DXGI_FORMAT_R32_FLOAT;
        outDesc.SampleDesc.Count = 1;
        outDesc.Usage = D3D11_USAGE_DEFAULT;
        outDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = device->CreateTexture2D(&outDesc, nullptr, &output);
        if (FAILED(hr))
        {
            SetLastErrorText(L"Create output R32_FLOAT texture failed");
            break;
        }

        hr = device->CreateRenderTargetView(output, nullptr, &rtv);
        if (FAILED(hr))
        {
            SetLastErrorText(L"Create output R32_FLOAT RTV failed");
            break;
        }

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&samplerDesc, &sampler);
        if (FAILED(hr))
        {
            SetLastErrorText(L"Create point sampler failed");
            break;
        }

        D3D11_TEXTURE2D_DESC stagingDesc = outDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
        if (FAILED(hr))
        {
            SetLastErrorText(L"Create staging R32_FLOAT texture failed");
            break;
        }

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(candidate->width);
        viewport.Height = static_cast<float>(candidate->height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        ID3D11RenderTargetView* rtvs[] = { rtv };
        context->OMSetRenderTargets(1, rtvs, nullptr);
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, &sampler);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrv[] = { nullptr };
        context->PSSetShaderResources(0, 1, nullSrv);
        context->CopyResource(staging, output);
        context->Flush();

        RemovePendingDepthReadbacksForPathNoLock(path);

        PendingDepthReadback pending{};
        pending.staging = staging;
        pending.width = candidate->width;
        pending.height = candidate->height;
        pending.path = path;
        pending.sequence = ++g_depthReadbackSequence;
        pending.candidate = candidate->dsv;
        pending.format = candidate->format;
        g_pendingDepthReadbacks.push_back(pending);
        staging = nullptr;

        std::wstringstream ss;
        ss << L"D3D11 depth rfloat readback queued: " << path
           << L" candidate=0x" << std::hex << candidate->dsv << std::dec
           << L" size=" << candidate->width << L"x" << candidate->height
           << L" format=" << FormatToString(candidate->format)
           << L" sequence=" << pending.sequence
           << L" pending=" << g_pendingDepthReadbacks.size();
        LogLineNoLock(ss.str());

        DrainPendingDepthReadbacksNoLock(context, false, kDepthReadbackMapDelay);
        while (g_pendingDepthReadbacks.size() > kMaxPendingDepthReadbacks)
        {
            LogLineNoLock(L"D3D11 depth pending ring is full; blocking on oldest readback");
            DrainPendingDepthReadbacksNoLock(context, true, 0);
            break;
        }
        ok = true;
    } while (false);

    if (!ok && !g_lastError.empty())
        LogLineNoLock(L"D3D11 depth rfloat queue failed: " + g_lastError);

    if (staging) staging->Release();
    if (rtv) rtv->Release();
    if (output) output->Release();
    if (sampler) sampler->Release();
    if (srv) srv->Release();
    if (ps) ps->Release();
    if (vs) vs->Release();
    if (psBlob) psBlob->Release();
    if (vsBlob) vsBlob->Release();
    context->Release();
    device->Release();
    return ok;
}

void FlushPendingDepthReadbacksNoLock()
{
    if (g_unityDevice == nullptr)
    {
        LogLineNoLock(L"Depth readback flush skipped because Unity D3D11 device is unavailable");
        return;
    }

    ID3D11DeviceContext* context = nullptr;
    g_unityDevice->GetImmediateContext(&context);
    if (context == nullptr)
    {
        LogLineNoLock(L"Depth readback flush skipped because GetImmediateContext returned null");
        return;
    }

    const auto before = g_pendingDepthReadbacks.size();
    const auto saved = DrainPendingDepthReadbacksNoLock(context, true, 0);
    std::wstringstream ss;
    ss << L"Depth readback flush finished saved=" << saved
       << L" before=" << before
       << L" remaining=" << g_pendingDepthReadbacks.size();
    LogLineNoLock(ss.str());
    context->Release();
}

void WriteCaptureSummaryNoLock()
{
    LogLineNoLock(L"capture end: " + g_captureLabel);
    std::wstringstream counters;
    counters << L"  hook counters: OMSet=" << g_omSetCalls
             << L" nullDSV=" << g_omSetNullDsvCalls
             << L" ClearDepthStencilView=" << g_clearDsvCalls
             << L" Draw=" << g_drawCalls;
    LogLineNoLock(counters.str());

    if (g_dsvStats.empty())
    {
        LogLineNoLock(L"  no DSVs observed");
    }
    else
    {
        std::vector<DsvStats> stats;
        stats.reserve(g_dsvStats.size());
        for (const auto& kv : g_dsvStats)
            stats.push_back(kv.second);

        std::sort(stats.begin(), stats.end(), [](const DsvStats& a, const DsvStats& b) {
            return a.lastBindSerial < b.lastBindSerial;
        });

        int bestScore = -9999;
        uintptr_t best = 0;
        for (const auto& s : stats)
        {
            const int score = ScoreCandidate(s);
            if (score > bestScore)
            {
                bestScore = score;
                best = s.dsv;
            }

            std::wstringstream line;
            line << L"  dsv=0x" << std::hex << s.dsv
                 << L" resource=0x" << s.resource << std::dec
                 << L" size=" << s.width << L"x" << s.height
                 << L" format=" << FormatToString(s.format)
                 << L" samples=" << s.sampleCount << L":" << s.sampleQuality
                 << L" bindFlags=0x" << std::hex << s.bindFlags
                 << L" miscFlags=0x" << s.miscFlags << std::dec
                 << L" binds=" << s.bindCount
                 << L" clears=" << s.clearCount
                 << L" draws=" << s.drawCount
                 << L" lastBind=" << s.lastBindSerial
                 << L" score=" << score;
            LogLineNoLock(line.str());
        }

        std::wstringstream bestLine;
        bestLine << L"  bestCandidate=0x" << std::hex << best << std::dec << L" score=" << bestScore
                 << L" requestedSize=" << g_captureWidth << L"x" << g_captureHeight;
        LogLineNoLock(bestLine.str());
    }

    std::wstringstream passHeader;
    passHeader << L"  render passes observed=" << g_renderPassStats.size();
    LogLineNoLock(passHeader.str());

    for (const auto& pass : g_renderPassStats)
    {
        std::wstringstream line;
        line << L"  pass#" << pass.serial
             << L" rtv=0x" << std::hex << pass.rtv0.view
             << L" rtvRes=0x" << pass.rtv0.resource << std::dec
             << L" rtvSize=" << pass.rtv0.width << L"x" << pass.rtv0.height
             << L" rtvFmt=" << FormatToString(pass.rtv0.format)
             << L" rtvSamples=" << pass.rtv0.sampleCount << L":" << pass.rtv0.sampleQuality
             << L" dsv=0x" << std::hex << pass.dsv.view
             << L" dsvRes=0x" << pass.dsv.resource << std::dec
             << L" dsvSize=" << pass.dsv.width << L"x" << pass.dsv.height
             << L" dsvFmt=" << FormatToString(pass.dsv.format)
             << L" dsvSamples=" << pass.dsv.sampleCount << L":" << pass.dsv.sampleQuality
             << L" draws=" << pass.drawCount
             << L" clearRTV=" << pass.clearRtvCount
             << L" clearDSV=" << pass.clearDsvCount
             << L" lastDrawOp=" << pass.lastDrawSerial
             << L" lastClearRTVOp=" << pass.lastClearRtvSerial
             << L" lastClearDSVOp=" << pass.lastClearDsvSerial;
        if (pass.hasViewport)
        {
            line << L" viewport=" << pass.viewport.TopLeftX << L"," << pass.viewport.TopLeftY
                 << L"," << pass.viewport.Width << L"x" << pass.viewport.Height
                 << L" depthRange=" << pass.viewport.MinDepth << L"-" << pass.viewport.MaxDepth;
        }
        else
        {
            line << L" viewport=<unset>";
        }
        line << L" depthState=0x" << std::hex << pass.depthState << std::dec
             << L" depthKnown=" << (pass.hasDepthState ? L"true" : L"false")
             << L" depthEnable=" << pass.depthEnable
             << L" depthWriteMask=" << static_cast<int>(pass.depthWriteMask)
             << L" depthFunc=" << static_cast<int>(pass.depthFunc)
             << L" stencilRef=" << pass.stencilRef;
        LogLineNoLock(line.str());
    }

    std::wstringstream transferHeader;
    transferHeader << L"  resource transfers observed=" << g_resourceTransferStats.size();
    LogLineNoLock(transferHeader.str());
    for (const auto& transfer : g_resourceTransferStats)
    {
        std::wstringstream line;
        line << L"  transfer#" << transfer.serial
             << L" op=" << transfer.op
             << L" dst=0x" << std::hex << transfer.dst.resource
             << L" src=0x" << transfer.src.resource << std::dec
             << L" dstSize=" << transfer.dst.width << L"x" << transfer.dst.height
             << L" dstFmt=" << FormatToString(transfer.dst.format)
             << L" dstSamples=" << transfer.dst.sampleCount << L":" << transfer.dst.sampleQuality
             << L" srcSize=" << transfer.src.width << L"x" << transfer.src.height
             << L" srcFmt=" << FormatToString(transfer.src.format)
             << L" srcSamples=" << transfer.src.sampleCount << L":" << transfer.src.sampleQuality;
        if (transfer.resolveFormat != DXGI_FORMAT_UNKNOWN)
            line << L" resolveFmt=" << FormatToString(transfer.resolveFormat);
        LogLineNoLock(line.str());
    }
}

void StoreUnityDeviceNoLock(void* device, int deviceType, int eventType)
{
    std::wstringstream ss;
    ss << L"UnitySetGraphicsDevice device=0x" << std::hex << reinterpret_cast<uintptr_t>(device)
       << std::dec << L" deviceType=" << deviceType << L" eventType=" << eventType;
    LogLineNoLock(ss.str());

    if (g_unityDevice != nullptr)
    {
        g_unityDevice->Release();
        g_unityDevice = nullptr;
        g_unityContextHooksInstalled = false;
    }

    if (device == nullptr)
        return;

    ID3D11Device* d3dDevice = nullptr;
    HRESULT hr = reinterpret_cast<IUnknown*>(device)->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&d3dDevice));
    if (FAILED(hr) || d3dDevice == nullptr)
    {
        std::wstringstream fail;
        fail << L"Unity device is not ID3D11Device hr=0x" << std::hex << static_cast<unsigned>(hr);
        LogLineNoLock(fail.str());
        return;
    }

    g_unityDevice = d3dDevice;
    LogLineNoLock(L"Unity D3D11 device stored");
}

void TryInstallUnityContextHooksNoLock(const wchar_t* source)
{
    if (g_unityDevice == nullptr)
    {
        LogLineNoLock(L"Unity D3D11 device is not available for context hook");
        return;
    }

    ID3D11DeviceContext* context = nullptr;
    g_unityDevice->GetImmediateContext(&context);
    if (context == nullptr)
    {
        LogLineNoLock(L"Unity D3D11 GetImmediateContext returned null");
        return;
    }

    g_unityContextHooksInstalled = InstallHooksForContextNoLock(context, source);
    context->Release();
}

bool StoreUnityDeviceFromTextureNoLock(void* nativeTexture)
{
    std::wstringstream ss;
    ss << L"SetUnityD3D11Texture texture=0x" << std::hex << reinterpret_cast<uintptr_t>(nativeTexture);
    LogLineNoLock(ss.str());

    if (nativeTexture == nullptr)
    {
        SetLastErrorText(L"Native texture pointer is null");
        return false;
    }

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = reinterpret_cast<IUnknown*>(nativeTexture)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    if (FAILED(hr) || texture == nullptr)
    {
        std::wstringstream fail;
        fail << L"Native texture is not ID3D11Texture2D hr=0x" << std::hex << static_cast<unsigned>(hr);
        SetLastErrorText(fail.str());
        LogLineNoLock(fail.str());
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    ID3D11Device* device = nullptr;
    texture->GetDevice(&device);
    texture->Release();

    if (device == nullptr)
    {
        SetLastErrorText(L"ID3D11Texture2D::GetDevice returned null");
        LogLineNoLock(g_lastError);
        return false;
    }

    if (g_unityDevice != nullptr)
    {
        if (g_unityDevice == device)
        {
            device->Release();
            std::wstringstream same;
            same << L"D3D11 device already stored from native texture size=" << desc.Width << L"x" << desc.Height
                 << L" format=" << FormatToString(desc.Format);
            LogLineNoLock(same.str());
            return true;
        }

        g_unityDevice->Release();
        g_unityContextHooksInstalled = false;
    }

    g_unityDevice = device;
    std::wstringstream ok;
    ok << L"D3D11 device stored from native texture size=" << desc.Width << L"x" << desc.Height
       << L" format=" << FormatToString(desc.Format)
       << L" bindFlags=0x" << std::hex << desc.BindFlags;
    LogLineNoLock(ok.str());
    TryInstallUnityContextHooksNoLock(L"native texture device");
    return true;
}

void STDMETHODCALLTYPE OnRenderEvent(int eventId)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::wstringstream ss;
        ss << L"render event id=" << eventId
           << L" thread=" << GetCurrentThreadId()
           << L" unityDevice=0x" << std::hex << reinterpret_cast<uintptr_t>(g_unityDevice);
        LogLineNoLock(ss.str());

        if (eventId != 2002 && eventId != 2003)
        {
            TryInstallUnityContextHooksNoLock(L"Unity render event");
            return;
        }

        if (eventId == 2003)
        {
            g_captureActive = false;
            LogLineNoLock(L"Depth readback flush event starting");
        }
    }

    if (eventId == 2003)
    {
        FlushPendingDepthReadbacksNoLock();
        std::lock_guard<std::mutex> lock(g_mutex);
        LogLineNoLock(L"Depth readback flush event finished");
        return;
    }

    std::wstring readbackPath;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pendingDepthRFloatPath.empty())
        {
            LogLineNoLock(L"Depth readback event skipped because output path is empty");
            return;
        }

        readbackPath = g_pendingDepthRFloatPath;
        g_captureActive = false;
        LogLineNoLock(L"Depth readback queue event starting");
    }

    if (g_candidateDiagnosticsEnabled)
        LogCandidateDiagnosticsNoLock();

    const bool queued = EnqueueBestDepthRFloatReadbackNoLock(readbackPath);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_lastDepthQueueResult = queued ? 1 : -1;
        LogLineNoLock(L"Depth readback queue event finished");
        return;
    }
}
}

using UnityRenderingEvent = void (STDMETHODCALLTYPE*)(int);

extern "C" __declspec(dllexport) void UnitySetGraphicsDevice(void* device, int deviceType, int eventType)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_log.is_open())
        StoreUnityDeviceNoLock(device, deviceType, eventType);
}

extern "C" __declspec(dllexport) int ORS_SetUnityD3D11Texture(void* nativeTexture)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        SetLastErrorText(L"Bridge is not initialized");
        return kError;
    }

    return StoreUnityDeviceFromTextureNoLock(nativeTexture) ? kOk : kError;
}

extern "C" __declspec(dllexport) int ORS_SetDepthRFloatOutputPath(const wchar_t* path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        SetLastErrorText(L"Bridge is not initialized");
        return kError;
    }

    if (path == nullptr || path[0] == L'\0')
    {
        SetLastErrorText(L"Depth output path is empty");
        return kError;
    }

    g_pendingDepthRFloatPath = path;
    g_lastDepthQueueResult = 0;
    LogLineNoLock(L"Depth RFloat output path set: " + g_pendingDepthRFloatPath);
    return kOk;
}

extern "C" __declspec(dllexport) int ORS_SetCandidateDiagnosticsEnabled(int enabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        SetLastErrorText(L"Bridge is not initialized");
        return kError;
    }

    g_candidateDiagnosticsEnabled = enabled != 0;
    LogLineNoLock(std::wstring(L"Candidate diagnostics ") + (g_candidateDiagnosticsEnabled ? L"enabled" : L"disabled"));
    return kOk;
}

extern "C" __declspec(dllexport) int ORS_GetLastDepthQueueResult()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_lastDepthQueueResult;
}

extern "C" __declspec(dllexport) void UnityPluginLoad(void* unityInterfaces)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_log.is_open())
    {
        std::wstringstream ss;
        ss << L"UnityPluginLoad unityInterfaces=0x" << std::hex << reinterpret_cast<uintptr_t>(unityInterfaces);
        LogLineNoLock(ss.str());
    }
}

extern "C" __declspec(dllexport) void UnityPluginUnload()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_log.is_open())
        LogLineNoLock(L"UnityPluginUnload");
}

extern "C" __declspec(dllexport) UnityRenderingEvent ORS_GetRenderEventFunc()
{
    return OnRenderEvent;
}

extern "C" __declspec(dllexport) int ORS_Initialize(const wchar_t* logPath)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized)
        return kOk;

    if (logPath == nullptr || logPath[0] == L'\0')
    {
        SetLastErrorText(L"Missing log path");
        return kError;
    }

    g_logPath = logPath;
    g_log.open(g_logPath.c_str(), std::ios::out | std::ios::app);
    if (!g_log.is_open())
    {
        SetLastErrorText(L"Failed to open log file");
        return kError;
    }

    LogLineNoLock(L"initialize");
    if (!InstallHooks())
    {
        LogLineNoLock(L"hook install failed: " + g_lastError);
        return kError;
    }

    g_initialized = true;
    LogLineNoLock(L"hooks installed");
    return kOk;
}

extern "C" __declspec(dllexport) int ORS_Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
        return kOk;

    g_captureActive = false;
    ClearDsvStatsNoLock();
    ReleasePendingDepthReadbacksNoLock();
    RemoveHooks();
    if (g_unityDevice != nullptr)
    {
        g_unityDevice->Release();
        g_unityDevice = nullptr;
    }
    g_unityContextHooksInstalled = false;
    g_pendingDepthRFloatPath.clear();
    LogLineNoLock(L"shutdown");
    g_log.close();
    g_initialized = false;
    return kOk;
}

extern "C" __declspec(dllexport) int ORS_BeginCapture(int width, int height, const wchar_t* label)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        SetLastErrorText(L"Bridge is not initialized");
        return kError;
    }

    g_captureWidth = width;
    g_captureHeight = height;
    g_captureLabel = label != nullptr ? label : L"";
    g_bindSerial = 0;
    g_currentDsv = 0;
    ClearDsvStatsNoLock();
    g_omSetCalls = 0;
    g_omSetNullDsvCalls = 0;
    g_clearDsvCalls = 0;
    g_drawCalls = 0;
    g_pendingDepthRFloatPath.clear();
    g_lastDepthQueueResult = 0;
    g_captureActive = true;

    std::wstringstream ss;
    ss << L"capture begin: " << g_captureLabel << L" requestedSize=" << width << L"x" << height;
    LogLineNoLock(ss.str());
    return kOk;
}

extern "C" __declspec(dllexport) int ORS_EndCapture()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        SetLastErrorText(L"Bridge is not initialized");
        return kError;
    }

    g_captureActive = false;
    WriteCaptureSummaryNoLock();
    return kOk;
}

extern "C" __declspec(dllexport) const wchar_t* ORS_GetLastError()
{
    return g_lastError.c_str();
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH)
    {
        if (g_initialized)
            ORS_Shutdown();
    }
    return TRUE;
}
