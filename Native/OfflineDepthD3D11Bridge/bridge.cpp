#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "depth_pipeline.h"

namespace
{
constexpr int kOk = 0;
constexpr int kError = -1;
constexpr int kBridgeApiVersion = 2;
constexpr int kCaptureEventBase = 0x10000;
constexpr UINT kReadbackTimeoutMs = 30000;
constexpr uint64_t kMiB = 1024ull * 1024;
constexpr size_t kMaxPoolSlots = 4;
#ifdef ORS_TESTING
bool g_testDeferNonBlockingReadbacks = false;
bool g_testFailDsvRecording = false;
#endif

enum ContextVTableIndex
{
    kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20, kDrawInstanced = 21,
    kOMSetRenderTargets = 33, kOMSetRenderTargetsAndUAV = 34, kDrawAuto = 38,
    kDrawIndexedInstancedIndirect = 39, kDrawInstancedIndirect = 40, kClearDepthStencilView = 53
};
enum DeviceVTableIndex { kCreateDeferredContext = 27 };
struct HookSlot { void** slot = nullptr; void* original = nullptr; };

enum class CapturePhase { Prepared, Capturing, Queued, Saved, Failed, Cancelled };
struct CaptureSettings
{
    UINT budgetMiB = 512;
    bool memoryLog = true;
    bool reversedZ = true;
    bool diagnostics = false;
};
struct CaptureRequest
{
    int id = 0;
    int width = 0, height = 0;
    std::wstring label, path, failure;
    CapturePhase phase = CapturePhase::Prepared;
    CaptureSettings settings;
};
struct DsvStats
{
    uintptr_t dsv = 0, resource = 0;
    ID3D11Texture2D* retainedTexture = nullptr;
    ID3D11DepthStencilView* retainedView = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT sampleCount = 0, sampleQuality = 0, bindFlags = 0, miscFlags = 0;
    UINT bindCount = 0, clearCount = 0, drawCount = 0, lastBindSerial = 0;
    bool hasDepthClear = false;
    float clearDepth = 0;
};
struct ContentStats
{
    uint64_t pixels = 0, valid = 0, nonClear = 0;
    float minValue = std::numeric_limits<float>::infinity();
    float maxValue = -std::numeric_limits<float>::infinity();
    void add(float value, float clearValue)
    {
        ++pixels;
        if (!std::isfinite(value) || value < 0 || value > 1) return;
        ++valid;
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
        if (value != clearValue) ++nonClear;
    }
};
struct PendingDepthReadback
{
    ors::ReadbackSlot* slot = nullptr;
    std::shared_ptr<CaptureRequest> request;
    uintptr_t candidate = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    float clearValue = 0;
    ULONGLONG queuedAt = 0;
};

using OMSetRenderTargetsFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OMSetRenderTargetsAndUAVFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using ClearDepthStencilViewFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using DrawIndexedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawAutoFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndexedInstancedIndirectFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DrawInstancedIndirectFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using CreateDeferredContextFn = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);

std::mutex g_mutex, g_logMutex, g_errorMutex;
std::wofstream g_log;
std::wstring g_lastError;
std::vector<HookSlot> g_hooks;
bool g_initialized = false;
std::atomic<bool> g_captureActive{false};
std::atomic<bool> g_captureRecordingFailed{false};
int g_captureWidth = 0, g_captureHeight = 0, g_activeId = 0, g_nextId = 0;
UINT g_bindSerial = 0, g_omSetCalls = 0, g_omSetNullDsvCalls = 0, g_clearDsvCalls = 0, g_drawCalls = 0;
std::unordered_map<uintptr_t, DsvStats> g_dsvStats;
std::unordered_map<ID3D11DeviceContext*, uintptr_t> g_contextDsv;
std::unordered_map<int, std::shared_ptr<CaptureRequest>> g_requests;
CaptureSettings g_settings;
ID3D11Device* g_unityDevice = nullptr;
ors::ComPtr<ID3D11Device> g_renderDevice;
ors::DepthPipeline g_pipeline;
ors::FloatTarget g_output, g_sampleOutput;
ors::ReadbackSlot g_sampleSlot;
std::vector<std::unique_ptr<ors::ReadbackSlot>> g_slots;
std::vector<PendingDepthReadback> g_pendingDepthReadbacks;

OMSetRenderTargetsFn g_OMSetRenderTargets = nullptr;
OMSetRenderTargetsAndUAVFn g_OMSetRenderTargetsAndUAV = nullptr;
ClearDepthStencilViewFn g_ClearDepthStencilView = nullptr;
DrawIndexedFn g_DrawIndexed = nullptr;
DrawFn g_Draw = nullptr;
DrawIndexedInstancedFn g_DrawIndexedInstanced = nullptr;
DrawInstancedFn g_DrawInstanced = nullptr;
DrawAutoFn g_DrawAuto = nullptr;
DrawIndexedInstancedIndirectFn g_DrawIndexedInstancedIndirect = nullptr;
DrawInstancedIndirectFn g_DrawInstancedIndirect = nullptr;
CreateDeferredContextFn g_CreateDeferredContext = nullptr;

bool InstallHooksForContextNoLock(ID3D11DeviceContext*, const wchar_t*);
bool InstallHooksForDeviceNoLock(ID3D11Device*, const wchar_t*);

void SetLastErrorText(const std::wstring& message)
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_lastError = message;
}
void LogLineNoLock(const std::wstring& line)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log.is_open()) return;
    SYSTEMTIME st; GetLocalTime(&st);
    g_log << L"[" << st.wHour << L":" << st.wMinute << L":" << st.wSecond << L"."
          << st.wMilliseconds << L"] " << line << std::endl;
}
void SetPhase(const std::shared_ptr<CaptureRequest>& request, CapturePhase phase, const std::wstring& failure = L"")
{
    std::lock_guard<std::mutex> lock(g_mutex);
    request->phase = phase;
    request->failure = failure;
}
void FailCapture(const std::shared_ptr<CaptureRequest>& request, const std::wstring& reason)
{
    SetPhase(request, CapturePhase::Failed, reason);
    SetLastErrorText(reason);
    LogLineNoLock(L"capture failed id=" + std::to_wstring(request->id) + L" reason=" + reason);
}
void ClearDsvStatsNoLock()
{
    for (auto& kv : g_dsvStats)
    {
        if (kv.second.retainedView) kv.second.retainedView->Release();
        if (kv.second.retainedTexture) kv.second.retainedTexture->Release();
    }
    g_dsvStats.clear();
    g_contextDsv.clear();
}
DsvStats* RetainDsvNoLock(ID3D11DepthStencilView* dsv)
{
    if (!dsv) return nullptr;
#ifdef ORS_TESTING
    if (g_testFailDsvRecording) throw std::bad_alloc();
#endif
    auto key = reinterpret_cast<uintptr_t>(dsv);
    auto found = g_dsvStats.find(key);
    if (found != g_dsvStats.end()) return &found->second;
    ors::ComPtr<ID3D11Resource> resource;
    dsv->GetResource(resource.put());
    ors::ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put()))))
        return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    auto& stats = g_dsvStats[key];
    stats.dsv = key; stats.resource = reinterpret_cast<uintptr_t>(resource.get());
    texture->AddRef(); stats.retainedTexture = texture.get();
    dsv->AddRef(); stats.retainedView = dsv;
    stats.width = desc.Width; stats.height = desc.Height; stats.format = desc.Format;
    stats.sampleCount = desc.SampleDesc.Count; stats.sampleQuality = desc.SampleDesc.Quality;
    stats.bindFlags = desc.BindFlags; stats.miscFlags = desc.MiscFlags;
    return &stats;
}
void RecordOmSetNoLock(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv)
{
    if (!g_captureActive) return;
    ++g_omSetCalls;
    if (!dsv) ++g_omSetNullDsvCalls;
    g_contextDsv[ctx] = reinterpret_cast<uintptr_t>(dsv);
    if (auto* s = RetainDsvNoLock(dsv)) { ++s->bindCount; s->lastBindSerial = ++g_bindSerial; }
}
void RecordDrawNoLock(ID3D11DeviceContext* ctx)
{
    if (!g_captureActive) return;
    ++g_drawCalls;
    auto binding = g_contextDsv.find(ctx);
    if (binding == g_contextDsv.end()) return;
    auto found = g_dsvStats.find(binding->second);
    if (found != g_dsvStats.end()) ++found->second.drawCount;
}
void STDMETHODCALLTYPE Hook_OMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    try { if (g_captureActive) { std::lock_guard<std::mutex> lock(g_mutex); RecordOmSetNoLock(ctx, dsv); } }
    catch (const std::bad_alloc&) { g_captureRecordingFailed = true; g_captureActive = false; }
    g_OMSetRenderTargets(ctx, n, rtvs, dsv);
}
void STDMETHODCALLTYPE Hook_OMSetRenderTargetsAndUAV(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs,
    ID3D11DepthStencilView* dsv, UINT start, UINT count, ID3D11UnorderedAccessView* const* uavs, const UINT* initial)
{
    try { if (g_captureActive) { std::lock_guard<std::mutex> lock(g_mutex); RecordOmSetNoLock(ctx, dsv); } }
    catch (const std::bad_alloc&) { g_captureRecordingFailed = true; g_captureActive = false; }
    g_OMSetRenderTargetsAndUAV(ctx, n, rtvs, dsv, start, count, uavs, initial);
}
void STDMETHODCALLTYPE Hook_ClearDepthStencilView(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
{
    try
    {
        if (g_captureActive)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_captureActive && (flags & D3D11_CLEAR_DEPTH))
            {
                ++g_clearDsvCalls;
                if (auto* s = RetainDsvNoLock(dsv))
                { ++s->clearCount; s->hasDepthClear = true; s->clearDepth = depth; }
            }
        }
    }
    catch (const std::bad_alloc&) { g_captureRecordingFailed = true; g_captureActive = false; }
    // A clear does not change the context's actual DSV binding.
    g_ClearDepthStencilView(ctx, dsv, flags, depth, stencil);
}
void RecordDraw(ID3D11DeviceContext* ctx)
{
    if (g_captureActive) { std::lock_guard<std::mutex> lock(g_mutex); RecordDrawNoLock(ctx); }
}
void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* c, UINT a, UINT b, INT d)
{ RecordDraw(c); g_DrawIndexed(c, a, b, d); }
void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* c, UINT a, UINT b)
{ RecordDraw(c); g_Draw(c, a, b); }
void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f)
{ RecordDraw(c); g_DrawIndexedInstanced(c, a, b, d, e, f); }
void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e)
{ RecordDraw(c); g_DrawInstanced(c, a, b, d, e); }
void STDMETHODCALLTYPE Hook_DrawAuto(ID3D11DeviceContext* c)
{ RecordDraw(c); g_DrawAuto(c); }
void STDMETHODCALLTYPE Hook_DrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT offset)
{ RecordDraw(c); g_DrawIndexedInstancedIndirect(c, b, offset); }
void STDMETHODCALLTYPE Hook_DrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT offset)
{ RecordDraw(c); g_DrawInstancedIndirect(c, b, offset); }
HRESULT STDMETHODCALLTYPE Hook_CreateDeferredContext(ID3D11Device* device, UINT flags, ID3D11DeviceContext** context)
{
    const auto hr = g_CreateDeferredContext(device, flags, context);
    if (SUCCEEDED(hr) && context && *context)
    {
        try
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            InstallHooksForContextNoLock(*context, L"CreateDeferredContext");
        }
        catch (const std::bad_alloc&) { g_captureRecordingFailed = true; g_captureActive = false; }
    }
    return hr;
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

            if (*slot == replacement)
                return true;

            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
                return false;

            *slot = replacement;
            VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

            std::wstringstream ss;
            ss << L"repaired D3D11 vtable hook index=" << index << L" slot=0x" << std::hex << reinterpret_cast<uintptr_t>(slot);
            LogLineNoLock(ss.str());
            return true;
        }
    }

    if (*slot == replacement)
        return true;

    HookSlot hook;
    hook.slot = slot;
    hook.original = *slot;
    // Allocate bookkeeping before patching a callable entry point.
    g_hooks.push_back(hook);
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    { g_hooks.pop_back(); return false; }

    if (originalOut != nullptr)
        *originalOut = hook.original;
    *slot = replacement;
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

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
    INSTALL_CONTEXT_HOOK(kClearDepthStencilView, Hook_ClearDepthStencilView, ClearDepthStencilViewFn, g_ClearDepthStencilView);
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

bool InstallHooksForDeviceNoLock(ID3D11Device* device, const wchar_t* source)
{
    if (device == nullptr)
        return false;

    void** vtable = *reinterpret_cast<void***>(device);
    void* original = nullptr;
    const bool ok = ReplaceVTableSlot(vtable, kCreateDeferredContext, reinterpret_cast<void*>(&Hook_CreateDeferredContext), &original);
    if (ok && g_CreateDeferredContext == nullptr && original != nullptr)
        g_CreateDeferredContext = reinterpret_cast<CreateDeferredContextFn>(original);

    std::wstringstream ss;
    ss << L"device hooks " << (ok ? L"installed" : L"failed") << L" for " << source
       << L" device=0x" << std::hex << reinterpret_cast<uintptr_t>(device);
    LogLineNoLock(ss.str());

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context != nullptr)
    {
        InstallHooksForContextNoLock(context, source);
        context->Release();
    }

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
    ok &= InstallHooksForDeviceNoLock(device, L"dummy D3D11");

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

DXGI_FORMAT GetDepthSrvFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
int ScoreCandidate(const DsvStats& s)
{
    return int(std::min<UINT>(s.drawCount, 64)) * 3 + int(std::min<UINT>(s.bindCount, 16)) +
        (s.clearCount > 0 ? 10 : 0);
}
uint64_t PoolBytes()
{
    uint64_t bytes = uint64_t(g_output.width) * g_output.height * sizeof(float);
    for (const auto& slot : g_slots) bytes += slot->bytes();
    bytes += uint64_t(g_sampleOutput.width) * g_sampleOutput.height * sizeof(float) + g_sampleSlot.bytes();
    return bytes;
}
void LogMemory(const std::shared_ptr<CaptureRequest>& request, const wchar_t* stage, bool force = false)
{
    if (!request->settings.memoryLog && !force) return;
    const auto m = ors::ReadMemory(g_renderDevice.get());
    uint64_t pendingBytes = 0;
    for (const auto& pending : g_pendingDepthReadbacks) pendingBytes += pending.slot->bytes();
    std::wstringstream ss;
    ss << L"memory id=" << request->id << L" stage=" << stage
       << L" ramValid=" << m.systemValid << L" availableRamMiB=" << m.availableRam / kMiB
       << L" availableCommitMiB=" << m.availableCommit / kMiB << L" processPrivateMiB=" << m.privateBytes / kMiB
       << L" gpuValid=" << m.gpuValid << L" gpuUsageMiB=" << m.gpuUsage / kMiB
       << L" gpuBudgetMiB=" << m.gpuBudget / kMiB << L" sharedUsageMiB=" << m.sharedUsage / kMiB
       << L" sharedBudgetMiB=" << m.sharedBudget / kMiB << L" pressure=" << m.pressure()
       << L" poolMiB=" << PoolBytes() / kMiB << L" pending=" << g_pendingDepthReadbacks.size()
       << L" pendingMiB=" << pendingBytes / kMiB << L" textureAllocations=" << g_pipeline.textureAllocations;
    LogLineNoLock(ss.str());
}
std::wstring HrFailure(ID3D11Device* device, const std::wstring& operation, HRESULT hr)
{
    std::wstringstream ss;
    ss << operation << L" hr=0x" << std::hex << unsigned(hr);
    if (device) ss << L" deviceRemovedReason=0x" << unsigned(device->GetDeviceRemovedReason());
    return ss.str();
}
std::wstring PipelineFailure()
{
    return HrFailure(g_renderDevice.get(), g_pipeline.lastOperation, g_pipeline.lastHr);
}
enum class ReadyResult { Ready, Pending, Failed };
ReadyResult WaitForSlot(ID3D11DeviceContext* ctx, ors::ReadbackSlot& slot, bool wait, std::wstring& reason)
{
#ifdef ORS_TESTING
    if (!wait && g_testDeferNonBlockingReadbacks) return ReadyResult::Pending;
#endif
    const auto start = GetTickCount64();
    if (wait) ctx->Flush();
    for (;;)
    {
        BOOL complete = FALSE;
        HRESULT hr = ctx->GetData(slot.complete.get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (FAILED(hr)) { reason = HrFailure(g_renderDevice.get(), L"completion_query", hr); return ReadyResult::Failed; }
        if (hr == S_OK && complete) return ReadyResult::Ready;
        if (!wait) return ReadyResult::Pending;
        if (GetTickCount64() - start >= kReadbackTimeoutMs)
        {
            reason = L"gpu_completion_timeout_30000ms";
            return ReadyResult::Failed;
        }
        Sleep(1);
    }
}
bool WriteReadySlot(ID3D11DeviceContext* ctx, PendingDepthReadback& pending, std::wstring& reason)
{
    auto& slot = *pending.slot;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto mapStart = GetTickCount64();
    const auto hr = ctx->Map(slot.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (FAILED(hr))
    {
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { reason = L"map_still_drawing"; return false; }
        reason = HrFailure(g_renderDevice.get(), L"map_ready_staging", hr);
        return false;
    }
    const auto& request = pending.request;
    const auto temp = request->path + L".capture" + std::to_wstring(request->id) + L".tmp";
    FILE* file = _wfopen(temp.c_str(), L"wb");
    ContentStats stats;
    bool wrote = file != nullptr;
    const auto rowBytes = size_t(slot.width) * sizeof(float);
    const float clearValue = pending.clearValue;
    const auto* base = static_cast<const unsigned char*>(mapped.pData);
    for (UINT y = 0; y < slot.height; ++y)
    {
        const auto* row = reinterpret_cast<const float*>(base + size_t(mapped.RowPitch) * y);
        for (UINT x = 0; x < slot.width; ++x) stats.add(row[x], clearValue);
        if (file && std::fwrite(row, 1, rowBytes, file) != rowBytes) wrote = false;
    }
    if (file && std::fclose(file) != 0) wrote = false;
    ctx->Unmap(slot.staging.get(), 0);
    if (!wrote) reason = L"depth_file_write_failed win32=" + std::to_wstring(GetLastError());
    else if (stats.valid != stats.pixels) reason = L"full_depth_contains_invalid_values";
    else if (stats.nonClear == 0) reason = L"full_depth_is_all_clear";
    else if (!MoveFileExW(temp.c_str(), request->path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        reason = L"depth_file_commit_failed win32=" + std::to_wstring(GetLastError());
    if (!reason.empty()) { DeleteFileW(temp.c_str()); return false; }
    std::wstringstream ss;
    ss << L"D3D11 depth rfloat saved: " << request->path << L" captureId=" << request->id
       << L" candidate=0x" << std::hex << pending.candidate << std::dec
       << L" size=" << slot.width << L"x" << slot.height << L" format=" << unsigned(pending.format)
       << L" valid=" << stats.valid << L" nonClear=" << stats.nonClear
       << L" min=" << stats.minValue << L" max=" << stats.maxValue
       << L" latencyMs=" << GetTickCount64() - pending.queuedAt
       << L" mapAndWriteMs=" << GetTickCount64() - mapStart;
    LogLineNoLock(ss.str());
    return true;
}
bool DrainOne(ID3D11DeviceContext* ctx, size_t index, bool wait)
{
    auto& pending = g_pendingDepthReadbacks[index];
    std::wstring reason;
    const auto ready = WaitForSlot(ctx, *pending.slot, wait, reason);
    if (ready == ReadyResult::Pending) return false;
    bool ok = ready == ReadyResult::Ready && WriteReadySlot(ctx, pending, reason);
    const auto waitStarted = GetTickCount64();
    while (!ok && reason == L"map_still_drawing")
    {
        if (!wait) return false;
        if (GetTickCount64() - waitStarted >= kReadbackTimeoutMs)
        { reason = L"gpu_map_timeout_30000ms"; break; }
        Sleep(1);
        reason.clear();
        ok = WriteReadySlot(ctx, pending, reason);
    }
    if (ok) SetPhase(pending.request, CapturePhase::Saved);
    else
    {
        FailCapture(pending.request, reason);
        LogMemory(pending.request, L"readback_failed", true);
        // A timed-out slot must not be reused while its GPU copy is still in flight.
        pending.slot->staging.reset();
        pending.slot->complete.reset();
        pending.slot->width = pending.slot->height = 0;
    }
    pending.slot->busy = false;
    g_pendingDepthReadbacks.erase(g_pendingDepthReadbacks.begin() + std::ptrdiff_t(index));
    return true;
}
void DrainPending(ID3D11DeviceContext* ctx, bool wait)
{
    for (size_t i = 0; i < g_pendingDepthReadbacks.size();)
    {
        if (!DrainOne(ctx, i, wait)) ++i;
    }
}
void ResetGpuResources()
{
    g_slots.clear();
    g_output.reset();
    g_sampleOutput.reset();
    g_sampleSlot = ors::ReadbackSlot{};
    g_pipeline.reset();
}
size_t PoolLimit(const std::shared_ptr<CaptureRequest>& request)
{
    const uint64_t bytes = uint64_t(request->width) * request->height * sizeof(float);
    const uint64_t budget = uint64_t(request->settings.budgetMiB) * kMiB;
    if (bytes == 0 || budget < bytes * 2 + 2 * kMiB) return 0;
    const size_t capacity = size_t(std::min<uint64_t>(kMaxPoolSlots, (budget - bytes - 2 * kMiB) / bytes));
    return ors::ReadMemory(g_renderDevice.get()).pressure() ? std::min<size_t>(capacity, 1) : capacity;
}
ors::ReadbackSlot* AcquireSlot(ID3D11DeviceContext* ctx, const std::shared_ptr<CaptureRequest>& request)
{
    DrainPending(ctx, false);
    const size_t limit = PoolLimit(request);
    if (limit == 0)
    {
        FailCapture(request, L"readback_budget_too_small: need two full-resolution float textures plus scratch; increase readback budget or lower supersampling");
        return nullptr;
    }
    if (g_output.width != UINT(request->width) || g_output.height != UINT(request->height))
    {
        DrainPending(ctx, true);
        g_slots.clear();
        g_output.reset();
    }
    // Trim idle slots first. Under pressure, wait for old work before allocating anything.
    while (g_slots.size() > limit)
    {
        auto idle = std::find_if(g_slots.begin(), g_slots.end(), [](const auto& s) { return !s->busy; });
        if (idle != g_slots.end()) g_slots.erase(idle);
        else if (!g_pendingDepthReadbacks.empty()) DrainOne(ctx, 0, true);
        else break;
    }
    if (!g_pipeline.ensureTarget(g_renderDevice.get(), request->width, request->height, g_output))
    {
        DrainPending(ctx, true);
        g_slots.clear();
        if (!g_pipeline.ensureTarget(g_renderDevice.get(), request->width, request->height, g_output))
        { FailCapture(request, PipelineFailure()); LogMemory(request, L"output_allocation_failed", true); return nullptr; }
    }
    auto idle = std::find_if(g_slots.begin(), g_slots.end(), [](const auto& s) { return !s->busy; });
    if (idle == g_slots.end() && g_slots.size() >= limit)
    {
        const auto start = GetTickCount64();
        if (!g_pendingDepthReadbacks.empty()) DrainOne(ctx, 0, true);
        LogLineNoLock(L"readback backpressure id=" + std::to_wstring(request->id) +
            L" waitMs=" + std::to_wstring(GetTickCount64() - start));
        idle = std::find_if(g_slots.begin(), g_slots.end(), [](const auto& s) { return !s->busy; });
    }
    if (idle == g_slots.end())
    {
        g_slots.push_back(std::make_unique<ors::ReadbackSlot>());
        idle = g_slots.end() - 1;
    }
    auto* slot = idle->get();
    if (!g_pipeline.ensureSlot(g_renderDevice.get(), request->width, request->height, *slot))
    {
        DrainPending(ctx, true);
        if (!g_pipeline.ensureSlot(g_renderDevice.get(), request->width, request->height, *slot))
        { FailCapture(request, PipelineFailure()); LogMemory(request, L"staging_allocation_failed", true); return nullptr; }
    }
    return slot;
}
bool ProbeCandidate(ID3D11DeviceContext* ctx, const DsvStats& candidate, UINT width,
    const std::shared_ptr<CaptureRequest>& request, ContentStats& stats, std::wstring& reason)
{
    const UINT height = std::max<UINT>(1, std::min<UINT>(512, UINT(uint64_t(candidate.height) * width / candidate.width)));
    if (!g_pipeline.ensureTarget(g_renderDevice.get(), width, height, g_sampleOutput) ||
        !g_pipeline.ensureSlot(g_renderDevice.get(), width, height, g_sampleSlot) ||
        !g_pipeline.copy(g_renderDevice.get(), ctx, candidate.retainedTexture,
            GetDepthSrvFormat(candidate.format), g_sampleOutput, g_sampleSlot))
    { reason = PipelineFailure(); return false; }
    if (WaitForSlot(ctx, g_sampleSlot, true, reason) != ReadyResult::Ready) return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto hr = ctx->Map(g_sampleSlot.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (FAILED(hr)) { reason = HrFailure(g_renderDevice.get(), L"map_candidate_probe", hr); return false; }
    const auto* base = static_cast<const unsigned char*>(mapped.pData);
    const float clear = candidate.hasDepthClear ? candidate.clearDepth : (request->settings.reversedZ ? 0.0f : 1.0f);
    for (UINT y = 0; y < height; ++y)
    {
        const auto* row = reinterpret_cast<const float*>(base + size_t(mapped.RowPitch) * y);
        for (UINT x = 0; x < width; ++x) stats.add(row[x], clear);
    }
    ctx->Unmap(g_sampleSlot.staging.get(), 0);
    if (stats.valid != stats.pixels) { reason = L"candidate_probe_invalid_values_or_incomplete_draw"; return false; }
    return true;
}
const DsvStats* SelectValidated(ID3D11DeviceContext* ctx, const std::shared_ptr<CaptureRequest>& request)
{
    std::vector<const DsvStats*> candidates;
    UINT wrongSize = 0, msaa = 0, unreadable = 0, noDraw = 0;
    for (const auto& kv : g_dsvStats)
    {
        const auto& s = kv.second;
        if (s.width != UINT(request->width) || s.height != UINT(request->height)) { ++wrongSize; continue; }
        if (s.sampleCount != 1) { ++msaa; continue; }
        if (!(s.bindFlags & D3D11_BIND_SHADER_RESOURCE) || GetDepthSrvFormat(s.format) == DXGI_FORMAT_UNKNOWN)
        { ++unreadable; continue; }
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        s.retainedView->GetDesc(&view);
        if (view.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice != 0)
        { ++unreadable; continue; }
        // Drivers may switch optimized Draw vtable entries after state changes.
        // A clear in this capture plus non-clear sampled content also proves fresh work.
        if (s.drawCount == 0 && s.clearCount == 0) { ++noDraw; continue; }
        candidates.push_back(&s);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto* a, const auto* b) {
        if (ScoreCandidate(*a) != ScoreCandidate(*b)) return ScoreCandidate(*a) > ScoreCandidate(*b);
        return a->lastBindSerial < b->lastBindSerial;
    });
    UINT empty = 0, failed = 0;
    const DsvStats* best = nullptr;
    std::wstring lastFailure;
    for (const auto* s : candidates)
    {
        ContentStats stats;
        std::wstring reason;
        bool sampled = ProbeCandidate(ctx, *s, 128, request, stats, reason);
        if (sampled && stats.nonClear == 0)
        {
            stats = ContentStats{};
            sampled = ProbeCandidate(ctx, *s, 512, request, stats, reason);
        }
        std::wstringstream ss;
        ss << L"candidate id=" << request->id << L" dsv=0x" << std::hex << s->dsv << std::dec
           << L" size=" << s->width << L"x" << s->height << L" draws=" << s->drawCount
           << L" clears=" << s->clearCount << L" valid=" << stats.valid << L"/" << stats.pixels
           << L" nonClear=" << stats.nonClear << L" min=" << stats.minValue << L" max=" << stats.maxValue;
        if (!sampled)
        {
            ++failed; lastFailure = reason;
            ss << L" rejected=probe_failed reason=" << reason;
        }
        else if (stats.nonClear == 0) { ++empty; ss << L" rejected=all_clear"; }
        else { ss << L" accepted=true"; if (!best) best = s; }
        LogLineNoLock(ss.str());
        if (best && !request->settings.diagnostics) break;
    }
    if (!best)
    {
        std::wstringstream ss;
        ss << L"no_valid_depth candidates=" << g_dsvStats.size() << L" exactReadableFresh=" << candidates.size()
           << L" wrongSize=" << wrongSize << L" msaa=" << msaa << L" unreadable=" << unreadable
           << L" noDraw=" << noDraw << L" allClear=" << empty << L" probeFailed=" << failed;
        if (!lastFailure.empty()) ss << L" lastProbeError=" << lastFailure;
        FailCapture(request, ss.str());
        LogMemory(request, L"candidate_selection_failed", true);
    }
    return best;
}
void WriteCaptureSummary(const std::shared_ptr<CaptureRequest>& request)
{
    std::wstringstream ss;
    ss << L"capture end id=" << request->id << L" label=" << request->label
       << L" renderThread=" << GetCurrentThreadId() << L" OMSet=" << g_omSetCalls
       << L" nullDSV=" << g_omSetNullDsvCalls << L" clear=" << g_clearDsvCalls
       << L" draw=" << g_drawCalls << L" candidates=" << g_dsvStats.size();
    LogLineNoLock(ss.str());
    for (const auto& kv : g_dsvStats)
    {
        const auto& s = kv.second;
        if (!request->settings.diagnostics && s.width == UINT(request->width) && s.height == UINT(request->height) &&
            s.sampleCount == 1 && (s.bindFlags & D3D11_BIND_SHADER_RESOURCE)) continue;
        std::wstringstream line;
        line << L"  dsv=0x" << std::hex << s.dsv << L" resource=0x" << s.resource << std::dec
             << L" size=" << s.width << L"x" << s.height << L" format=" << unsigned(s.format)
             << L" samples=" << s.sampleCount << L" bindFlags=0x" << std::hex << s.bindFlags << std::dec
             << L" draws=" << s.drawCount << L" clears=" << s.clearCount;
        LogLineNoLock(line.str());
    }
}
void BeginOnRenderThread(const std::shared_ptr<CaptureRequest>& request)
{
    if (g_captureActive)
    {
        FailCapture(request, L"capture_begin_while_previous_camera_is_active");
        return;
    }
    ors::ComPtr<ID3D11Device> device;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_unityDevice) { g_unityDevice->AddRef(); *device.put() = g_unityDevice; }
    }
    if (!device) { FailCapture(request, L"unity_device_unavailable"); return; }
    if (g_renderDevice.get() != device.get())
    {
        if (g_renderDevice)
        {
            ors::ComPtr<ID3D11DeviceContext> oldContext;
            g_renderDevice->GetImmediateContext(oldContext.put());
            DrainPending(oldContext.get(), true);
        }
        ResetGpuResources();
        g_renderDevice = std::move(device);
    }
    ors::ComPtr<ID3D11DeviceContext> context;
    g_renderDevice->GetImmediateContext(context.put());
    if (!context) { FailCapture(request, L"unity_immediate_context_unavailable"); return; }
    bool hooksInstalled;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        hooksInstalled = InstallHooksForDeviceNoLock(g_renderDevice.get(), L"render capture begin");
    }
    if (!hooksInstalled)
    { FailCapture(request, L"context_rehook_failed"); return; }
    // Repeated Ctrl+F10 paths are serialized before deleting the previous result.
    for (size_t i = 0; i < g_pendingDepthReadbacks.size();)
    {
        if (g_pendingDepthReadbacks[i].request->path == request->path) DrainOne(context.get(), i, true);
        else ++i;
    }
    if (!DeleteFileW(request->path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
    { FailCapture(request, L"cannot_remove_previous_depth win32=" + std::to_wstring(GetLastError())); return; }
    ClearDsvStatsNoLock();
    g_captureWidth = request->width; g_captureHeight = request->height; g_activeId = request->id;
    g_bindSerial = g_omSetCalls = g_omSetNullDsvCalls = g_clearDsvCalls = g_drawCalls = 0;
    g_captureRecordingFailed = false;
    SetPhase(request, CapturePhase::Capturing);
    g_captureActive = true;
    ors::ComPtr<ID3D11DepthStencilView> current;
    context->OMGetRenderTargets(0, nullptr, current.put());
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        RecordOmSetNoLock(context.get(), current.get());
    }
    LogLineNoLock(L"capture begin id=" + std::to_wstring(request->id) + L" label=" + request->label +
        L" requestedSize=" + std::to_wstring(request->width) + L"x" + std::to_wstring(request->height) +
        L" renderThread=" + std::to_wstring(GetCurrentThreadId()));
    LogMemory(request, L"camera_begin");
}
void EndOnRenderThread(const std::shared_ptr<CaptureRequest>& request, bool cancel)
{
    if (g_activeId == request->id && g_captureRecordingFailed)
    {
        g_captureActive = false;
        ClearDsvStatsNoLock();
        g_activeId = 0;
        FailCapture(request, L"cpu_allocation_failed_recording_depth_views");
        LogMemory(request, L"dsv_recording_failed", true);
        return;
    }
    if (g_activeId != request->id || !g_captureActive)
    {
        if (!cancel)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (request->phase == CapturePhase::Failed) return;
        }
        FailCapture(request, L"capture_end_without_matching_render_begin");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_captureActive = false;
    }
    WriteCaptureSummary(request);
    if (cancel) SetPhase(request, CapturePhase::Cancelled, L"camera_capture_cancelled");
    else
    {
        ors::ComPtr<ID3D11DeviceContext> context;
        g_renderDevice->GetImmediateContext(context.put());
        const auto started = GetTickCount64();
        if (!g_pipeline.initialize(g_renderDevice.get()))
        { FailCapture(request, PipelineFailure()); LogMemory(request, L"pipeline_initialization_failed", true); }
        else if (const auto* candidate = SelectValidated(context.get(), request))
        {
            if (auto* slot = AcquireSlot(context.get(), request))
            {
                if (!g_pipeline.copy(g_renderDevice.get(), context.get(), candidate->retainedTexture,
                    GetDepthSrvFormat(candidate->format), g_output, *slot))
                { FailCapture(request, PipelineFailure()); LogMemory(request, L"depth_conversion_failed", true); }
                else
                {
                    slot->busy = true;
                    SetPhase(request, CapturePhase::Queued);
                    const float clear = candidate->hasDepthClear ? candidate->clearDepth :
                        (request->settings.reversedZ ? 0.0f : 1.0f);
                    g_pendingDepthReadbacks.push_back({slot, request, candidate->dsv, candidate->format, clear, GetTickCount64()});
                    context->Flush();
                    LogLineNoLock(L"D3D11 depth rfloat readback queued: " + request->path +
                        L" captureId=" + std::to_wstring(request->id) + L" pending=" +
                        std::to_wstring(g_pendingDepthReadbacks.size()) + L" queueMs=" +
                        std::to_wstring(GetTickCount64() - started));
                    DrainPending(context.get(), false);
                }
            }
        }
        LogMemory(request, L"camera_end");
    }
    ClearDsvStatsNoLock();
    g_activeId = 0;
}
void FlushPending()
{
    if (!g_renderDevice) return;
    ors::ComPtr<ID3D11DeviceContext> context;
    g_renderDevice->GetImmediateContext(context.put());
    const size_t before = g_pendingDepthReadbacks.size();
    DrainPending(context.get(), true);
    LogLineNoLock(L"Depth readback flush finished before=" + std::to_wstring(before) +
        L" remaining=" + std::to_wstring(g_pendingDepthReadbacks.size()));
    if (g_pendingDepthReadbacks.empty() && ors::ReadMemory(g_renderDevice.get()).pressure())
    {
        g_slots.clear();
        g_output.reset();
        LogLineNoLock(L"Released idle full-resolution texture pool under memory pressure");
    }
}
void HandleRenderEvent(int eventId)
{
    if (eventId == 2003) { FlushPending(); return; }
    if (eventId < kCaptureEventBase) { SetLastErrorText(L"unsupported_event_or_mismatched_bridge_build"); return; }
    const int id = (eventId - kCaptureEventBase) / 4;
    const int action = (eventId - kCaptureEventBase) % 4;
    std::shared_ptr<CaptureRequest> request;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto found = g_requests.find(id);
        if (found == g_requests.end()) { LogLineNoLock(L"ignored unknown capture event id=" + std::to_wstring(id)); return; }
        request = found->second;
    }
    if (action == 0) BeginOnRenderThread(request);
    else if (action == 1 || action == 2) EndOnRenderThread(request, action == 2);
}

void STDMETHODCALLTYPE OnRenderEvent(int eventId)
{
    try
    {
        HandleRenderEvent(eventId);
    }
    catch (const std::exception&)
    {
        // Never unwind a C++ allocation exception into Unity's rendering thread.
        // Already queued readbacks retain their own resources and can still finish.
        g_captureActive = false;
        ClearDsvStatsNoLock();
        g_activeId = 0;
        try
        {
            const int id = eventId >= kCaptureEventBase ? (eventId - kCaptureEventBase) / 4 : 0;
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto found = g_requests.find(id);
            if (found != g_requests.end() && found->second->phase != CapturePhase::Queued &&
                found->second->phase != CapturePhase::Saved)
                found->second->phase = CapturePhase::Failed;
            SetLastErrorText(L"native_exception_or_cpu_allocation_failure_during_render_event");
            LogLineNoLock(L"native exception contained; captureId=" + std::to_wstring(id));
        }
        catch (...) {}
    }
}
}

using UnityRenderingEvent = void (STDMETHODCALLTYPE*)(int);
extern "C" __declspec(dllexport) int ORS_GetBridgeApiVersion() { return kBridgeApiVersion; }
extern "C" __declspec(dllexport) UnityRenderingEvent ORS_GetRenderEventFunc() { return OnRenderEvent; }
extern "C" __declspec(dllexport) int ORS_Initialize(const wchar_t* logPath) try
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return kOk;
    if (!logPath || !*logPath) return kError;
    g_log.open(logPath, std::ios::out | std::ios::app);
    if (!g_log.is_open()) return kError;
    LogLineNoLock(L"initialize build=kk-depth-pressure-v1 api=2");
    g_slots.reserve(kMaxPoolSlots);
    g_pendingDepthReadbacks.reserve(kMaxPoolSlots);
    g_hooks.reserve(128);
    if (!InstallHooks()) return kError;
    g_initialized = true;
    return kOk;
}
catch (const std::bad_alloc&) { return kError; }
extern "C" __declspec(dllexport) int ORS_ConfigureReadback(int budgetMiB, int memoryLogging, int reversedZ)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_settings.budgetMiB = UINT(std::max(64, std::min(4096, budgetMiB)));
    g_settings.memoryLog = memoryLogging != 0;
    g_settings.reversedZ = reversedZ != 0;
    return kOk;
}
extern "C" __declspec(dllexport) int ORS_SetCandidateDiagnosticsEnabled(int enabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_settings.diagnostics = enabled != 0;
    return kOk;
}
extern "C" __declspec(dllexport) int ORS_SetUnityD3D11Texture(void* nativeTexture)
{
    if (!nativeTexture) return kError;
    ors::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(reinterpret_cast<IUnknown*>(nativeTexture)->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(texture.put())))) return kError;
    ID3D11Device* device = nullptr;
    texture->GetDevice(&device);
    if (!device) return kError;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_unityDevice) g_unityDevice->Release();
    g_unityDevice = device;
    return kOk;
}
extern "C" __declspec(dllexport) int ORS_BeginCapture(int width, int height, const wchar_t* label) try
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized || width <= 0 || height <= 0 || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || g_nextId >= (INT_MAX - kCaptureEventBase) / 4 - 1)
        return kError;
    // Keep recent completion statuses without retaining every frame of a long export.
    for (auto it = g_requests.begin(); it != g_requests.end();)
    {
        const auto phase = it->second->phase;
        if (g_nextId - it->first > 128 &&
            (phase == CapturePhase::Saved || phase == CapturePhase::Failed || phase == CapturePhase::Cancelled))
            it = g_requests.erase(it);
        else ++it;
    }
    auto request = std::make_shared<CaptureRequest>();
    request->id = ++g_nextId; request->width = width; request->height = height;
    request->label = label ? label : L""; request->settings = g_settings;
    g_requests[request->id] = request;
    return request->id;
}
catch (const std::bad_alloc&) { return kError; }
extern "C" __declspec(dllexport) int ORS_SetDepthRFloatOutputPath(int captureId, const wchar_t* path) try
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_requests.find(captureId);
    if (found == g_requests.end() || found->second->phase != CapturePhase::Prepared || !path || !*path) return kError;
    found->second->path = path;
    return kOk;
}
catch (const std::bad_alloc&) { return kError; }
extern "C" __declspec(dllexport) int ORS_GetDepthCaptureStatus(int captureId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_requests.find(captureId);
    if (found == g_requests.end()) return kError;
    switch (found->second->phase)
    {
    case CapturePhase::Queued: return 1;
    case CapturePhase::Saved: return 2;
    case CapturePhase::Failed:
    case CapturePhase::Cancelled: return -1;
    default: return 0;
    }
}
extern "C" __declspec(dllexport) int ORS_EndCapture()
{
    // Legacy CPU notification is deliberately not allowed to close a render-thread window.
    return kOk;
}
extern "C" __declspec(dllexport) const wchar_t* ORS_GetLastError()
{
    thread_local std::wstring copy;
    std::lock_guard<std::mutex> lock(g_errorMutex);
    copy = g_lastError;
    return copy.c_str();
}
extern "C" __declspec(dllexport) int ORS_Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& kv : g_requests)
        if (kv.second->phase == CapturePhase::Capturing || kv.second->phase == CapturePhase::Queued ||
            kv.second->phase == CapturePhase::Prepared)
        { SetLastErrorText(L"shutdown_requires_completed_capture_and_render_thread_flush"); return kError; }
    g_captureActive = false;
    ClearDsvStatsNoLock();
    ResetGpuResources();
    g_renderDevice.reset();
    if (g_unityDevice) { g_unityDevice->Release(); g_unityDevice = nullptr; }
    RemoveHooks();
    g_requests.clear();
    g_initialized = false;
    LogLineNoLock(L"shutdown");
    g_log.close();
    return kOk;
}
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
