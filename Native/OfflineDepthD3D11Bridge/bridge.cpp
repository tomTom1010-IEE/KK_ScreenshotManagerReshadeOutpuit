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
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "depth_pipeline.h"
#include "write_evidence.h"
#include <MinHook.h>

namespace
{
constexpr int kOk = 0;
constexpr int kError = -1;
constexpr int kBridgeApiVersion = 4;
constexpr int kCaptureEventBase = 0x10000;
constexpr int kCameraEventBase = 0x40000000;
constexpr UINT kReadbackTimeoutMs = 30000;
constexpr uint64_t kMiB = 1024ull * 1024;
constexpr size_t kMaxPoolSlots = 4;
constexpr size_t kMaxStageSamples = 64;
#ifdef ORS_TESTING
bool g_testDeferNonBlockingReadbacks = false;
bool g_testFailDsvRecording = false;
#endif

enum ContextVTableIndex
{
    kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20, kDrawInstanced = 21,
    kOMSetRenderTargets = 33, kOMSetRenderTargetsAndUAV = 34, kDrawAuto = 38,
    kDrawIndexedInstancedIndirect = 39, kDrawInstancedIndirect = 40, kClearDepthStencilView = 53,
    kOMSetDepthStencilState = 36, kCopySubresourceRegion = 46, kCopyResource = 47,
    kResolveSubresource = 57, kExecuteCommandList = 58, kClearState = 110
};
enum DeviceVTableIndex { kCreateDeferredContext = 27 };
struct HookSlot { void** slot = nullptr; void* original = nullptr; void* replacement = nullptr; };

enum class CapturePhase { Prepared, Capturing, Queued, Saved, Failed, Cancelled };
struct CaptureSettings
{
    UINT budgetMiB = 512;
    bool memoryLog = true;
    bool reversedZ = true;
    bool diagnostics = false;
    bool stageDiagnostics = false;
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
    bool writeFresh = false;
    uint64_t gpuPassedSamples = 0;
    bool hasDepthClear = false;
    float clearDepth = 0;
};
struct ContentStats
{
    uint64_t pixels = 0, valid = 0, nonClear = 0;
    uint64_t fingerprint = 14695981039346656037ull;
    bool hashEnabled = false;
    float minValue = std::numeric_limits<float>::infinity();
    float maxValue = -std::numeric_limits<float>::infinity();
    void add(float value, float clearValue)
    {
        ++pixels;
        if (hashEnabled)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            fingerprint = (fingerprint ^ bits) * 1099511628211ull;
        }
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
struct CameraStageEvent
{
    int captureId = 0, cameraIndex = -1;
    bool ending = false;
    std::wstring label;
};
struct StageSample
{
    size_t slotIndex = 0;
    size_t writeVersion = std::numeric_limits<size_t>::max();
    DsvStats source;
    std::wstring stage, camera;
    int cameraIndex = -1;
    ULONGLONG observedAt = 0;
    float clearValue = 0, nextClearValue = 0;
};
struct InternalDrawGuard
{
    bool& flag;
    bool previous;
    explicit InternalDrawGuard(bool& value) : flag(value), previous(value) { flag = true; }
    ~InternalDrawGuard() { flag = previous; }
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
using OMSetDepthStencilStateFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
using CopyResourceFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using CopySubresourceRegionFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using ResolveSubresourceFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
using ExecuteCommandListFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using ClearStateFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*);

std::mutex g_mutex, g_logMutex, g_errorMutex;
std::wofstream g_log;
std::wstring g_lastError;
std::vector<HookSlot> g_hooks;
std::vector<ors::ComPtr<IUnknown>> g_hookOwners;
std::atomic<uint64_t> g_hookEpoch{0};
struct EntryRoute { void* target = nullptr; void* original = nullptr; void* wrapper = nullptr; };
std::array<std::array<EntryRoute, 32>, 128> g_entryRoutes{};
std::array<std::unordered_set<void*>, 128> g_skippedEntries;
std::unordered_set<void*> g_sharedEntryTargets;
thread_local void* g_entryOriginal[128]{};
thread_local UINT g_forwarding[128]{};
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
ors::WriteEvidence g_writeEvidence;
ors::FloatTarget g_output, g_sampleOutput;
ors::ReadbackSlot g_sampleSlot;
std::vector<std::unique_ptr<ors::ReadbackSlot>> g_slots;
std::vector<PendingDepthReadback> g_pendingDepthReadbacks;
std::shared_ptr<CaptureRequest> g_stageRequest;
std::unordered_map<int, CameraStageEvent> g_cameraEvents;
int g_nextCameraEvent = 0, g_stageCameraIndex = -1;
std::wstring g_stageCamera;
std::atomic<DWORD> g_captureThread{0};
thread_local bool g_internalDiagnosticDraw = false;
ors::FloatTarget g_stageOutput;
std::vector<std::unique_ptr<ors::ReadbackSlot>> g_stageSlots;
std::vector<StageSample> g_stageSamples;
bool g_stageLimitLogged = false;

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
OMSetDepthStencilStateFn g_OMSetDepthStencilState = nullptr;
CopyResourceFn g_CopyResource = nullptr;
CopySubresourceRegionFn g_CopySubresourceRegion = nullptr;
ResolveSubresourceFn g_ResolveSubresource = nullptr;
ExecuteCommandListFn g_ExecuteCommandList = nullptr;
ClearStateFn g_ClearState = nullptr;

bool InstallHooksForContextNoLock(ID3D11DeviceContext*, const wchar_t*);
bool InstallHooksForDeviceNoLock(ID3D11Device*, const wchar_t*);
void QueueStageDiagnostic(ID3D11DeviceContext*, const DsvStats&, const wchar_t*, float = 0) noexcept;
void LogLineNoLock(const std::wstring&);
void RefreshEntryHooks(ID3D11DeviceContext*) noexcept;

// Dummy, WARP and game contexts need not share the same implementation.
template<class Fn, class Object> Fn OriginalFor(Object* object, int index, Fn fallback)
{
    if (g_entryOriginal[index]) return reinterpret_cast<Fn>(g_entryOriginal[index]);
    struct Cache { void** table = nullptr; uint64_t epoch = 0; void* methods[128]{}; };
    thread_local Cache cache;
    auto** table = *reinterpret_cast<void***>(object);
    const auto epoch = g_hookEpoch.load();
    if (cache.table != table || cache.epoch != epoch)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        cache = Cache{}; cache.table = table; cache.epoch = epoch;
        for (const auto& hook : g_hooks)
            for (int method = 0; method < 128; ++method)
                if (hook.slot == table + method) cache.methods[method] = hook.original;
    }
    return cache.methods[index] ? reinterpret_cast<Fn>(cache.methods[index]) : fallback;
}
template<int Method, size_t Slot, class Fn> struct EntryThunk;
template<int Method, size_t Slot, class R, class Object, class... Args>
struct EntryThunk<Method, Slot, R (STDMETHODCALLTYPE*)(Object*, Args...)>
{
    static R STDMETHODCALLTYPE call(Object* object, Args... args)
    {
        const auto& route = g_entryRoutes[Method][Slot];
        const auto original = reinterpret_cast<R (STDMETHODCALLTYPE*)(Object*, Args...)>(route.original);
        if (g_forwarding[Method] || !g_captureActive || g_internalDiagnosticDraw) return original(object, args...);
        struct Scope
        {
            void* previous;
            Scope(void* value) : previous(g_entryOriginal[Method]) { g_entryOriginal[Method] = value; }
            ~Scope() { g_entryOriginal[Method] = previous; }
        } scope(route.original);
        return reinterpret_cast<R (STDMETHODCALLTYPE*)(Object*, Args...)>(route.wrapper)(object, args...);
    }
};
template<int Method, class Fn, size_t... Indices>
std::array<void*, sizeof...(Indices)> EntryAddresses(std::index_sequence<Indices...>)
{
    return {{reinterpret_cast<void*>(&EntryThunk<Method, Indices, Fn>::call)...}};
}
template<int Method, class Fn> bool InstallEntryHook(void* target, Fn wrapper)
{
    auto& routes = g_entryRoutes[Method];
    for (const auto& route : routes) if (route.target == target) return true;
    const auto thunks = EntryAddresses<Method, Fn>(std::make_index_sequence<32>{});
    for (size_t index = 0; index < routes.size(); ++index)
    {
        if (routes[index].target) continue;
        auto& route = routes[index];
        route.wrapper = reinterpret_cast<void*>(wrapper);
        const auto status = MH_CreateHook(target, thunks[index], &route.original);
        if (status != MH_OK)
        {
            LogLineNoLock(L"entry_hook unavailable method=" + std::to_wstring(Method) +
                L" status=" + std::to_wstring(status));
            return false;
        }
        route.target = target;
        if (MH_QueueEnableHook(target) != MH_OK)
        { MH_RemoveHook(target); route = EntryRoute{}; return false; }
        LogLineNoLock(L"entry_hook installed method=" + std::to_wstring(Method) +
            L" variant=" + std::to_wstring(index) + L" address=" + std::to_wstring(reinterpret_cast<uintptr_t>(target)));
        return true;
    }
    LogLineNoLock(L"entry_hook variant limit method=" + std::to_wstring(Method));
    return false;
}
template<class Fn, class Object, class... Args>
void ForwardOriginal(Object* object, int method, Fn fallback, Args... args)
{
    struct Scope
    {
        UINT& count;
        explicit Scope(UINT& value) : count(value) { ++count; }
        ~Scope() { --count; }
    } scope(g_forwarding[method]);
    OriginalFor(object, method, fallback)(object, args...);
}
bool ObserveWrites(ID3D11DeviceContext* ctx)
{
    return g_captureActive && !g_internalDiagnosticDraw && GetCurrentThreadId() == g_captureThread &&
        ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
}
void StopWriteEvidence(ID3D11DeviceContext* ctx)
{
    if (ObserveWrites(ctx)) g_writeEvidence.stop(ctx);
}
void ResumeWriteEvidence(ID3D11DeviceContext* ctx) noexcept;
struct EvidencePause
{
    ID3D11DeviceContext* ctx;
    explicit EvidencePause(ID3D11DeviceContext* context) : ctx(context) { StopWriteEvidence(ctx); }
    ~EvidencePause() { ResumeWriteEvidence(ctx); }
};

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
void ResumeWriteEvidence(ID3D11DeviceContext* ctx) noexcept
{
    if (!ObserveWrites(ctx)) return;
    try
    {
        ors::ComPtr<ID3D11DepthStencilView> current;
        ctx->OMGetRenderTargets(0, nullptr, current.put());
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            RetainDsvNoLock(current.get());
        }
        g_writeEvidence.observe(ctx);
        RefreshEntryHooks(ctx);
    }
    catch (...)
    {
        g_writeEvidence.stop(ctx);
        g_captureRecordingFailed = true; g_captureActive = false;
    }
}
void RecordOmSetNoLock(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv)
{
    if (!g_captureActive || g_internalDiagnosticDraw) return;
    ++g_omSetCalls;
    if (!dsv) ++g_omSetNullDsvCalls;
    g_contextDsv[ctx] = reinterpret_cast<uintptr_t>(dsv);
    if (auto* s = RetainDsvNoLock(dsv)) { ++s->bindCount; s->lastBindSerial = ++g_bindSerial; }
}
void RecordDrawNoLock(ID3D11DeviceContext* ctx)
{
    if (!g_captureActive || g_internalDiagnosticDraw) return;
    ++g_drawCalls;
    auto binding = g_contextDsv.find(ctx);
    if (binding == g_contextDsv.end()) return;
    auto found = g_dsvStats.find(binding->second);
    if (found != g_dsvStats.end()) ++found->second.drawCount;
}
void STDMETHODCALLTYPE Hook_OMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    StopWriteEvidence(ctx);
    try { if (g_captureActive && !g_internalDiagnosticDraw) { std::lock_guard<std::mutex> lock(g_mutex); RecordOmSetNoLock(ctx, dsv); } }
    catch (const std::bad_alloc&) { g_captureRecordingFailed = true; g_captureActive = false; }
    ForwardOriginal(ctx, kOMSetRenderTargets, g_OMSetRenderTargets, n, rtvs, dsv);
    ResumeWriteEvidence(ctx);
}
void STDMETHODCALLTYPE Hook_OMSetRenderTargetsAndUAV(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs,
    ID3D11DepthStencilView* dsv, UINT start, UINT count, ID3D11UnorderedAccessView* const* uavs, const UINT* initial)
{
    StopWriteEvidence(ctx);
    try { if (g_captureActive && !g_internalDiagnosticDraw) { std::lock_guard<std::mutex> lock(g_mutex); RecordOmSetNoLock(ctx, dsv); } }
    catch (const std::bad_alloc&) { g_captureRecordingFailed = true; g_captureActive = false; }
    ForwardOriginal(ctx, kOMSetRenderTargetsAndUAV, g_OMSetRenderTargetsAndUAV, n, rtvs, dsv, start, count, uavs, initial);
    ResumeWriteEvidence(ctx);
}
void STDMETHODCALLTYPE Hook_ClearDepthStencilView(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
{
    StopWriteEvidence(ctx);
    DsvStats beforeClear;
    bool diagnose = false;
    try
    {
        if (ObserveWrites(ctx))
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_captureActive && (flags & D3D11_CLEAR_DEPTH))
            {
                ++g_clearDsvCalls;
                if (auto* s = RetainDsvNoLock(dsv))
                {
                    beforeClear = *s;
                    diagnose = true;
                    ++s->clearCount; s->hasDepthClear = true; s->clearDepth = depth;
                }
            }
        }
    }
    catch (const std::bad_alloc&) { g_captureRecordingFailed = true; g_captureActive = false; }
    // Queue a small GPU copy before the destructive clear, never Map inside this hook.
    if (diagnose) QueueStageDiagnostic(ctx, beforeClear, L"before_clear", depth);
    // A clear does not change the context's actual DSV binding.
    StopWriteEvidence(ctx);
    ForwardOriginal(ctx, kClearDepthStencilView, g_ClearDepthStencilView, dsv, flags, depth, stencil);
    if (ObserveWrites(ctx) && (flags & D3D11_CLEAR_DEPTH))
    {
        try
        {
            ors::ComPtr<ID3D11Resource> resource;
            dsv->GetResource(resource.put());
            g_writeEvidence.cleared(resource.get(), depth);
        }
        catch (...) { g_captureRecordingFailed = true; g_captureActive = false; }
    }
    ResumeWriteEvidence(ctx);
}
void RecordDraw(ID3D11DeviceContext* ctx)
{
    if (g_captureActive && !g_internalDiagnosticDraw) { std::lock_guard<std::mutex> lock(g_mutex); RecordDrawNoLock(ctx); }
}
void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* c, UINT a, UINT b, INT d)
{ RecordDraw(c); ForwardOriginal(c, kDrawIndexed, g_DrawIndexed, a, b, d); if (ObserveWrites(c)) RefreshEntryHooks(c); }
void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* c, UINT a, UINT b)
{ RecordDraw(c); ForwardOriginal(c, kDraw, g_Draw, a, b); if (ObserveWrites(c)) RefreshEntryHooks(c); }
void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f)
{ RecordDraw(c); ForwardOriginal(c, kDrawIndexedInstanced, g_DrawIndexedInstanced, a, b, d, e, f); if (ObserveWrites(c)) RefreshEntryHooks(c); }
void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e)
{ RecordDraw(c); ForwardOriginal(c, kDrawInstanced, g_DrawInstanced, a, b, d, e); if (ObserveWrites(c)) RefreshEntryHooks(c); }
void STDMETHODCALLTYPE Hook_DrawAuto(ID3D11DeviceContext* c)
{ RecordDraw(c); ForwardOriginal(c, kDrawAuto, g_DrawAuto); if (ObserveWrites(c)) RefreshEntryHooks(c); }
void STDMETHODCALLTYPE Hook_DrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT offset)
{ RecordDraw(c); ForwardOriginal(c, kDrawIndexedInstancedIndirect, g_DrawIndexedInstancedIndirect, b, offset); if (ObserveWrites(c)) RefreshEntryHooks(c); }
void STDMETHODCALLTYPE Hook_DrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT offset)
{ RecordDraw(c); ForwardOriginal(c, kDrawInstancedIndirect, g_DrawInstancedIndirect, b, offset); if (ObserveWrites(c)) RefreshEntryHooks(c); }
void STDMETHODCALLTYPE Hook_OMSetDepthStencilState(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* state, UINT ref)
{
    ForwardOriginal(ctx, kOMSetDepthStencilState, g_OMSetDepthStencilState, state, ref);
    ResumeWriteEvidence(ctx);
}
void RecordDepthCopy(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src, bool full, bool compatible = true) noexcept
{
    if (!ObserveWrites(ctx)) return;
    try { g_writeEvidence.copied(dst, src, full, compatible); }
    catch (...) { g_captureRecordingFailed = true; g_captureActive = false; }
}
bool CompatibleDepthCopy(ID3D11Resource* dst, ID3D11Resource* src)
{
    if (!dst || !src || dst == src) return false;
    ors::ComPtr<ID3D11Texture2D> a, b;
    if (FAILED(dst->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(a.put()))) ||
        FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(b.put())))) return false;
    D3D11_TEXTURE2D_DESC x{}, y{};
    a->GetDesc(&x); b->GetDesc(&y);
    return x.Width == y.Width && x.Height == y.Height && x.Format == y.Format &&
        x.MipLevels == 1 && y.MipLevels == 1 && x.ArraySize == 1 && y.ArraySize == 1 &&
        x.SampleDesc.Count == y.SampleDesc.Count && x.SampleDesc.Quality == y.SampleDesc.Quality;
}
void STDMETHODCALLTYPE Hook_CopyResource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src)
{
    StopWriteEvidence(ctx);
    ForwardOriginal(ctx, kCopyResource, g_CopyResource, dst, src);
    if (ObserveWrites(ctx)) RecordDepthCopy(ctx, dst, src, true, CompatibleDepthCopy(dst, src));
    ResumeWriteEvidence(ctx);
}
void STDMETHODCALLTYPE Hook_CopySubresourceRegion(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub, UINT x, UINT y, UINT z,
    ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box)
{
    StopWriteEvidence(ctx);
    ForwardOriginal(ctx, kCopySubresourceRegion, g_CopySubresourceRegion, dst, sub, x, y, z, src, srcSub, box);
    if (ObserveWrites(ctx))
    {
        const bool compatible = sub == 0 && srcSub == 0 && CompatibleDepthCopy(dst, src);
        bool full = compatible && x == 0 && y == 0 && z == 0 && !box;
        if (compatible && box)
        {
            ors::ComPtr<ID3D11Texture2D> texture;
            dst->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put()));
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            full = x == 0 && y == 0 && z == 0 && box->left == 0 && box->top == 0 && box->front == 0 &&
                box->right == desc.Width && box->bottom == desc.Height && box->back == 1;
        }
        RecordDepthCopy(ctx, dst, src, full, compatible);
    }
    ResumeWriteEvidence(ctx);
}
void STDMETHODCALLTYPE Hook_ResolveSubresource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dstSub,
    ID3D11Resource* src, UINT srcSub, DXGI_FORMAT format)
{
    StopWriteEvidence(ctx);
    ForwardOriginal(ctx, kResolveSubresource, g_ResolveSubresource, dst, dstSub, src, srcSub, format);
    // Depth resolve support is not assumed. Unverified updates cannot establish freshness.
    RecordDepthCopy(ctx, dst, src, false, false);
    ResumeWriteEvidence(ctx);
}
void STDMETHODCALLTYPE Hook_ExecuteCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore)
{
    StopWriteEvidence(ctx);
    ForwardOriginal(ctx, kExecuteCommandList, g_ExecuteCommandList, list, restore);
    if (ObserveWrites(ctx))
    {
        try { g_writeEvidence.invalidateAll(); }
        catch (...) { g_captureRecordingFailed = true; g_captureActive = false; }
    }
    ResumeWriteEvidence(ctx);
}
void STDMETHODCALLTYPE Hook_ClearState(ID3D11DeviceContext* ctx)
{
    StopWriteEvidence(ctx);
    ForwardOriginal(ctx, kClearState, g_ClearState);
    ResumeWriteEvidence(ctx);
}
HRESULT STDMETHODCALLTYPE Hook_CreateDeferredContext(ID3D11Device* device, UINT flags, ID3D11DeviceContext** context)
{
    const auto hr = OriginalFor(device, kCreateDeferredContext, g_CreateDeferredContext)(device, flags, context);
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
            ++g_hookEpoch;

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
    hook.replacement = replacement;
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
    ++g_hookEpoch;

    return true;
}
template<class T> bool KeepHookOwner(T* object)
{
    ors::ComPtr<IUnknown> identity;
    if (FAILED(object->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(identity.put())))) return false;
    for (const auto& owner : g_hookOwners) if (owner.get() == identity.get()) return true;
    if (g_hookOwners.size() >= 512) return false;
    g_hookOwners.push_back(std::move(identity));
    return true;
}
void* UnwrappedEntry(void** table, int method)
{
    auto* target = table[method];
    for (const auto& hook : g_hooks)
        if (hook.slot == table + method && target == hook.replacement) return hook.original;
    return target;
}
template<int Method, class Fn> void ObserveEntryVariant(void** table, Fn wrapper)
{
    auto* target = UnwrappedEntry(table, Method);
    if (g_sharedEntryTargets.count(target)) return;
    for (size_t other = 0; other < g_entryRoutes.size(); ++other)
        if (other != size_t(Method))
            for (const auto& route : g_entryRoutes[other])
                if (route.target == target)
                {
                    // Keep the trampoline alive until shutdown for any in-flight call.
                    MH_QueueDisableHook(target); MH_ApplyQueued();
                    g_sharedEntryTargets.insert(target);
                    LogLineNoLock(L"entry_hook disabled shared signature address=" + std::to_wstring(reinterpret_cast<uintptr_t>(target)));
                    return;
                }
    for (const auto& route : g_entryRoutes[Method]) if (route.target == target) return;
    if (g_skippedEntries[Method].count(target)) return;
    // Shared no-op stubs cannot be assigned a method-specific calling signature.
    for (int other = 0; other < 115; ++other)
        if (other != Method && UnwrappedEntry(table, other) == target)
        { g_skippedEntries[Method].insert(target); return; }
    if (!InstallEntryHook<Method>(target, wrapper)) g_skippedEntries[Method].insert(target);
}
void ObserveEntryVariantsNoLock(ID3D11DeviceContext* context)
{
    auto** table = *reinterpret_cast<void***>(context);
#define OBSERVE_ENTRY(method, wrapper) ObserveEntryVariant<method>(table, &wrapper)
    OBSERVE_ENTRY(kOMSetRenderTargets, Hook_OMSetRenderTargets);
    OBSERVE_ENTRY(kOMSetRenderTargetsAndUAV, Hook_OMSetRenderTargetsAndUAV);
    OBSERVE_ENTRY(kOMSetDepthStencilState, Hook_OMSetDepthStencilState);
    OBSERVE_ENTRY(kClearDepthStencilView, Hook_ClearDepthStencilView);
    OBSERVE_ENTRY(kCopyResource, Hook_CopyResource);
    OBSERVE_ENTRY(kCopySubresourceRegion, Hook_CopySubresourceRegion);
    OBSERVE_ENTRY(kResolveSubresource, Hook_ResolveSubresource);
    OBSERVE_ENTRY(kExecuteCommandList, Hook_ExecuteCommandList);
    OBSERVE_ENTRY(kClearState, Hook_ClearState);
    OBSERVE_ENTRY(kDraw, Hook_Draw);
    OBSERVE_ENTRY(kDrawIndexed, Hook_DrawIndexed);
    OBSERVE_ENTRY(kDrawInstanced, Hook_DrawInstanced);
    OBSERVE_ENTRY(kDrawIndexedInstanced, Hook_DrawIndexedInstanced);
    OBSERVE_ENTRY(kDrawAuto, Hook_DrawAuto);
    OBSERVE_ENTRY(kDrawInstancedIndirect, Hook_DrawInstancedIndirect);
    OBSERVE_ENTRY(kDrawIndexedInstancedIndirect, Hook_DrawIndexedInstancedIndirect);
#undef OBSERVE_ENTRY
    if (MH_ApplyQueued() != MH_OK) throw std::runtime_error("cannot enable entry hooks");
}
void RefreshEntryHooks(ID3D11DeviceContext* ctx) noexcept
{
    if (!ObserveWrites(ctx)) return;
    try
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ObserveEntryVariantsNoLock(ctx);
    }
    catch (...)
    {
        g_writeEvidence.stop(ctx);
        g_captureRecordingFailed = true; g_captureActive = false;
    }
}

bool InstallHooksForContextNoLock(ID3D11DeviceContext* context, const wchar_t* source)
{
    if (context == nullptr || !KeepHookOwner(context))
        return false;

    ObserveEntryVariantsNoLock(context);

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
    INSTALL_CONTEXT_HOOK(kOMSetDepthStencilState, Hook_OMSetDepthStencilState, OMSetDepthStencilStateFn, g_OMSetDepthStencilState);
    INSTALL_CONTEXT_HOOK(kCopyResource, Hook_CopyResource, CopyResourceFn, g_CopyResource);
    INSTALL_CONTEXT_HOOK(kCopySubresourceRegion, Hook_CopySubresourceRegion, CopySubresourceRegionFn, g_CopySubresourceRegion);
    INSTALL_CONTEXT_HOOK(kResolveSubresource, Hook_ResolveSubresource, ResolveSubresourceFn, g_ResolveSubresource);
    INSTALL_CONTEXT_HOOK(kExecuteCommandList, Hook_ExecuteCommandList, ExecuteCommandListFn, g_ExecuteCommandList);
    INSTALL_CONTEXT_HOOK(kClearState, Hook_ClearState, ClearStateFn, g_ClearState);

#undef INSTALL_CONTEXT_HOOK

    std::wstringstream ss;
    ss << L"hooks " << (ok ? L"installed" : L"failed") << L" for " << source
       << L" context=0x" << std::hex << reinterpret_cast<uintptr_t>(context);
    LogLineNoLock(ss.str());
    return ok;
}

bool InstallHooksForDeviceNoLock(ID3D11Device* device, const wchar_t* source)
{
    if (device == nullptr || !KeepHookOwner(device))
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
    for (auto& family : g_entryRoutes)
        for (auto& route : family) if (route.target) MH_QueueDisableHook(route.target);
    MH_ApplyQueued();
    for (auto& family : g_entryRoutes)
        for (auto& route : family)
            if (route.target) { MH_RemoveHook(route.target); route = EntryRoute{}; }
    for (auto& entries : g_skippedEntries) entries.clear();
    g_sharedEntryTargets.clear();
    for (auto it = g_hooks.rbegin(); it != g_hooks.rend(); ++it)
    {
        DWORD oldProtect = 0;
        if (*it->slot == it->replacement && VirtualProtect(it->slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            *it->slot = it->original;
            VirtualProtect(it->slot, sizeof(void*), oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), it->slot, sizeof(void*));
        }
    }
    g_hooks.clear();
    g_hookOwners.clear();
    ++g_hookEpoch;
    MH_Uninitialize();
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
    bytes += uint64_t(g_stageOutput.width) * g_stageOutput.height * sizeof(float);
    for (const auto& slot : g_stageSlots) bytes += slot->bytes();
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
void LogStageFailure(const wchar_t* stage, const std::wstring& reason) noexcept
{
    try
    {
        LogLineNoLock(L"stage_diag skipped id=" + std::to_wstring(g_stageRequest ? g_stageRequest->id : 0) +
            L" stage=" + stage + L" diagnosticOnly=true reason=" + reason);
    }
    catch (...) {}
}
uint64_t StageBudgetBytes(const CaptureRequest& request)
{
    const uint64_t budget = uint64_t(request.settings.budgetMiB) * kMiB;
    const uint64_t frame = uint64_t(request.width) * request.height * sizeof(float);
    if (budget < frame * 2 + 2 * kMiB) return 0;
    const uint64_t slots = std::min<uint64_t>(kMaxPoolSlots, (budget - frame - 2 * kMiB) / frame);
    return budget - frame * (1 + slots) - 2 * kMiB;
}
uint64_t StagePoolBytes()
{
    uint64_t bytes = uint64_t(g_stageOutput.width) * g_stageOutput.height * sizeof(float);
    for (const auto& slot : g_stageSlots) bytes += slot->bytes();
    return bytes;
}
bool IsDiagnosticCandidate(const DsvStats& source, const CaptureRequest& request)
{
    if (source.width != UINT(request.width) || source.height != UINT(request.height) || source.sampleCount != 1 ||
        !(source.bindFlags & D3D11_BIND_SHADER_RESOURCE) || GetDepthSrvFormat(source.format) == DXGI_FORMAT_UNKNOWN)
        return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC view{};
    source.retainedView->GetDesc(&view);
    return view.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D && view.Texture2D.MipSlice == 0;
}
void QueueStageDiagnostic(ID3D11DeviceContext* ctx, const DsvStats& source, const wchar_t* stage, float nextClear) noexcept
{
    try
    {
        // Stage resources belong exclusively to the capture's immediate-context thread.
        if (GetCurrentThreadId() != g_captureThread || g_internalDiagnosticDraw ||
            ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
        const auto request = g_stageRequest;
        if (!request || !request->settings.stageDiagnostics ||
            !IsDiagnosticCandidate(source, *request)) return;
        if (g_stageSamples.size() >= kMaxStageSamples)
        {
            if (!g_stageLimitLogged) LogStageFailure(stage, L"bounded_sample_limit_64");
            g_stageLimitLogged = true;
            return;
        }
        const UINT width = std::min<UINT>(128, source.width);
        const UINT height = UINT(std::max<uint64_t>(1, uint64_t(width) * request->height / request->width));
        const size_t index = g_stageSamples.size();
        const uint64_t bytes = uint64_t(width) * height * sizeof(float);
        // Diagnostic allocations never evict output/staging resources or exceed the configured budget.
        uint64_t extra = g_stageOutput.width == width && g_stageOutput.height == height ? 0 : bytes;
        if (index >= g_stageSlots.size() || g_stageSlots[index]->width != width || g_stageSlots[index]->height != height)
            extra += bytes;
        if (StagePoolBytes() + extra > StageBudgetBytes(*request) ||
            PoolBytes() + extra > uint64_t(request->settings.budgetMiB) * kMiB)
        { LogStageFailure(stage, L"readback_budget_reserved_for_output"); return; }
        if (index == g_stageSlots.size()) g_stageSlots.push_back(std::make_unique<ors::ReadbackSlot>());
        auto& slot = *g_stageSlots[index];
        StageSample sample;
        sample.slotIndex = index; sample.source = source; sample.stage = stage;
        sample.cameraIndex = g_stageCameraIndex; sample.camera = g_stageCamera;
        sample.observedAt = GetTickCount64();
        sample.clearValue = source.hasDepthClear ? source.clearDepth : (request->settings.reversedZ ? 0.f : 1.f);
        sample.nextClearValue = nextClear;
        sample.writeVersion = g_writeEvidence.versionTag(source.retainedTexture);
        EvidencePause pause(ctx);
        InternalDrawGuard internal(g_internalDiagnosticDraw);
        if (!g_pipeline.initialize(g_renderDevice.get()) ||
            !g_pipeline.ensureTarget(g_renderDevice.get(), width, height, g_stageOutput) ||
            !g_pipeline.ensureSlot(g_renderDevice.get(), width, height, slot) ||
            !g_pipeline.copy(g_renderDevice.get(), ctx, source.retainedTexture,
                GetDepthSrvFormat(source.format), g_stageOutput, slot))
        { LogStageFailure(stage, PipelineFailure()); return; }
        g_stageSamples.push_back(std::move(sample));
    }
    catch (...) { LogStageFailure(stage, L"diagnostic_exception_contained"); }
}
void QueueStageCandidates(ID3D11DeviceContext* ctx, const wchar_t* stage, bool onlyUnobserved = false) noexcept
{
    if (!g_stageRequest || !g_stageRequest->settings.stageDiagnostics) return;
    try
    {
        std::vector<DsvStats> sources;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            sources.reserve(g_dsvStats.size());
            for (const auto& entry : g_dsvStats) sources.push_back(entry.second);
        }
        const auto before = g_stageSamples.size();
        size_t eligible = 0;
        for (const auto& source : sources)
        {
            if (onlyUnobserved && g_writeEvidence.fresh(source.retainedTexture)) continue;
            if (!IsDiagnosticCandidate(source, *g_stageRequest)) continue;
            ++eligible;
            QueueStageDiagnostic(ctx, source, stage);
        }
        LogLineNoLock(L"stage_scan id=" + std::to_wstring(g_stageRequest->id) + L" stage=" + stage +
            L" cameraIndex=" + std::to_wstring(g_stageCameraIndex) + L" observedDSVs=" + std::to_wstring(sources.size()) +
            L" matchingReadable=" + std::to_wstring(eligible) + L" samplesQueued=" + std::to_wstring(g_stageSamples.size() - before) +
            L" diagnosticOnly=true");
    }
    catch (...) { LogStageFailure(stage, L"diagnostic_exception_contained"); }
}
void DrainStageDiagnostics(ID3D11DeviceContext* ctx) noexcept
{
    if (g_stageSamples.empty()) return;
    try
    {
        ctx->Flush();
        const auto deadline = GetTickCount64() + 2000;
        for (const auto& sample : g_stageSamples)
        {
            auto& slot = *g_stageSlots[sample.slotIndex];
            BOOL complete = FALSE;
            HRESULT hr;
            do
            {
                hr = ctx->GetData(slot.complete.get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (hr != S_FALSE && (hr != S_OK || complete)) break;
                if (GetTickCount64() >= deadline) break;
                Sleep(1);
            } while (true);
            if (hr != S_OK || !complete)
            {
                LogStageFailure(sample.stage.c_str(), FAILED(hr) ? HrFailure(g_renderDevice.get(), L"diagnostic_query", hr) :
                    L"diagnostic_readback_deadline_2000ms");
                slot = ors::ReadbackSlot{};
                continue;
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = ctx->Map(slot.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (FAILED(hr))
            {
                LogStageFailure(sample.stage.c_str(), HrFailure(g_renderDevice.get(), L"diagnostic_map", hr));
                slot = ors::ReadbackSlot{};
                continue;
            }
            ContentStats stats;
            stats.hashEnabled = true;
            const auto* base = static_cast<const unsigned char*>(mapped.pData);
            for (UINT y = 0; y < slot.height; ++y)
            {
                const auto* row = reinterpret_cast<const float*>(base + size_t(mapped.RowPitch) * y);
                for (UINT x = 0; x < slot.width; ++x) stats.add(row[x], sample.clearValue);
            }
            ctx->Unmap(slot.staging.get(), 0);
            std::wstringstream ss;
            ss << L"stage_sample id=" << g_stageRequest->id << L" seq=" << sample.slotIndex + 1
               << L" stage=" << sample.stage << L" cameraIndex=" << sample.cameraIndex << L" camera=\"" << sample.camera
               << L"\" dsv=0x" << std::hex << sample.source.dsv << L" resource=0x" << sample.source.resource << std::dec
               << L" observedTick=" << sample.observedAt << L" size=" << sample.source.width << L"x" << sample.source.height
               << L" probe=" << slot.width << L"x" << slot.height << L" draws=" << sample.source.drawCount
               << L" clears=" << sample.source.clearCount << L" binds=" << sample.source.bindCount
               << L" freshEvidence=" << (sample.source.drawCount != 0 || sample.source.clearCount != 0)
               << L" gpuFreshEvidence=" << g_writeEvidence.versionFresh(sample.writeVersion)
               << L" gpuPassedSamples=" << g_writeEvidence.versionSamples(sample.writeVersion)
               << L" clearValue=" << sample.clearValue << L" nextClear=" << sample.nextClearValue
               << L" valid=" << stats.valid << L"/" << stats.pixels << L" nonClear=" << stats.nonClear
               << L" min=" << stats.minValue << L" max=" << stats.maxValue
               << L" probeHash=0x" << std::hex << stats.fingerprint << std::dec << L" diagnosticOnly=true";
            LogLineNoLock(ss.str());
        }
    }
    catch (...) { LogStageFailure(L"drain", L"diagnostic_exception_contained"); }
    g_stageSamples.clear();
}
void HandleCameraStageEvent(const CameraStageEvent& event) noexcept
{
    try
    {
        if (!g_captureActive || !g_stageRequest || g_activeId != event.captureId ||
            !g_stageRequest->settings.stageDiagnostics) return;
        g_stageCameraIndex = event.cameraIndex;
        g_stageCamera = event.label;
        LogLineNoLock(L"camera_stage id=" + std::to_wstring(event.captureId) +
            L" cameraIndex=" + std::to_wstring(event.cameraIndex) + L" stage=" +
            (event.ending ? L"camera_end" : L"camera_begin") + L" camera=\"" + event.label +
            L"\" hookDraws=" + std::to_wstring(g_drawCalls) + L" hookDepthClears=" + std::to_wstring(g_clearDsvCalls));
        if (event.ending)
        {
            ors::ComPtr<ID3D11DeviceContext> ctx;
            g_renderDevice->GetImmediateContext(ctx.put());
            QueueStageCandidates(ctx.get(), L"camera_end");
        }
        // Refresh at explicit camera boundaries, not once per Draw.
        ors::ComPtr<ID3D11DeviceContext> context;
        g_renderDevice->GetImmediateContext(context.put());
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!InstallHooksForContextNoLock(context.get(), L"camera stage boundary"))
                g_captureRecordingFailed = true;
        }
    }
    catch (...) { LogStageFailure(L"camera_marker", L"diagnostic_exception_contained"); }
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
    g_writeEvidence.reset();
    g_slots.clear();
    g_output.reset();
    g_sampleOutput.reset();
    g_sampleSlot = ors::ReadbackSlot{};
    g_stageSamples.clear();
    g_stageSlots.clear();
    g_stageOutput.reset();
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
    stats.hashEnabled = true;
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
    const float clear = g_writeEvidence.clearReference(candidate.retainedTexture,
        candidate.hasDepthClear ? candidate.clearDepth : (request->settings.reversedZ ? 0.0f : 1.0f));
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
    for (auto& kv : g_dsvStats)
    {
        auto& s = kv.second;
        if (s.width != UINT(request->width) || s.height != UINT(request->height)) { ++wrongSize; continue; }
        if (s.sampleCount != 1) { ++msaa; continue; }
        if (!(s.bindFlags & D3D11_BIND_SHADER_RESOURCE) || GetDepthSrvFormat(s.format) == DXGI_FORMAT_UNKNOWN)
        { ++unreadable; continue; }
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        s.retainedView->GetDesc(&view);
        if (view.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice != 0)
        { ++unreadable; continue; }
        s.writeFresh = g_writeEvidence.fresh(s.retainedTexture);
        s.gpuPassedSamples = g_writeEvidence.samples(s.retainedTexture);
        if (!s.writeFresh) { ++noDraw; continue; }
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
           << L" clears=" << s->clearCount << L" evidenceCaptureId=" << g_writeEvidence.captureId
           << L" writeReason=" << g_writeEvidence.reason(s->retainedTexture)
           << L" gpuPassedSamples=" << s->gpuPassedSamples << L" valid=" << stats.valid << L"/" << stats.pixels
           << L" nonClear=" << stats.nonClear << L" min=" << stats.minValue << L" max=" << stats.maxValue;
        ss << L" probeHash=0x" << std::hex << stats.fingerprint << std::dec;
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
           << L" noDraw=" << noDraw << L" noFreshWrite=" << noDraw << L" allClear=" << empty << L" probeFailed=" << failed;
        if (!lastFailure.empty()) ss << L" lastProbeError=" << lastFailure;
        FailCapture(request, ss.str());
        LogMemory(request, L"candidate_selection_failed", true);
        QueueStageCandidates(ctx, L"rejected_no_draw", true);
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
    g_stageRequest = request;
    g_stageSamples.clear();
    if (!request->settings.stageDiagnostics || StagePoolBytes() > StageBudgetBytes(*request))
    { g_stageSlots.clear(); g_stageOutput.reset(); }
    g_stageCamera.clear();
    g_stageCameraIndex = -1;
    g_stageLimitLogged = false;
    g_captureThread = GetCurrentThreadId();
    g_writeEvidence.begin(g_renderDevice.get(), request->id, UINT(request->width), UINT(request->height));
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
        L" renderThread=" + std::to_wstring(GetCurrentThreadId()) +
        L" stageDiagnostics=" + std::to_wstring(request->settings.stageDiagnostics));
    LogMemory(request, L"camera_begin");
    ResumeWriteEvidence(context.get());
}
void EndOnRenderThread(const std::shared_ptr<CaptureRequest>& request, bool cancel)
{
    if (g_activeId == request->id && g_captureRecordingFailed)
    {
        ors::ComPtr<ID3D11DeviceContext> context;
        g_renderDevice->GetImmediateContext(context.put());
        g_writeEvidence.stop(context.get());
        g_writeEvidence.releaseResources();
        g_captureActive = false;
        ClearDsvStatsNoLock();
        g_activeId = 0;
        FailCapture(request, L"cpu_allocation_failed_recording_depth_views");
        LogMemory(request, L"dsv_recording_failed", true);
        g_stageSamples.clear();
        g_stageRequest.reset();
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
    ors::ComPtr<ID3D11DeviceContext> evidenceContext;
    g_renderDevice->GetImmediateContext(evidenceContext.put());
    g_writeEvidence.finish(evidenceContext.get());
    LogLineNoLock(L"write_evidence id=" + std::to_wstring(request->id) +
        L" queries=" + std::to_wstring(g_writeEvidence.queryCount()) + L" fullCopies=" +
        std::to_wstring(g_writeEvidence.fullCopies) + L" unverifiedCopies=" + std::to_wstring(g_writeEvidence.rejectedCopies) +
        L" unverifiedCommandLists=" + std::to_wstring(g_writeEvidence.commandLists) +
        L" error=\"" + g_writeEvidence.error + L"\"");
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
        QueueStageCandidates(context.get(), L"stack_end");
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
                    const float clear = g_writeEvidence.clearReference(candidate->retainedTexture,
                        candidate->hasDepthClear ? candidate->clearDepth : (request->settings.reversedZ ? 0.0f : 1.0f));
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
        DrainStageDiagnostics(context.get());
    }
    g_stageSamples.clear();
    g_stageRequest.reset();
    g_writeEvidence.releaseResources();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_cameraEvents.begin(); it != g_cameraEvents.end();)
            if (it->second.captureId == request->id) it = g_cameraEvents.erase(it); else ++it;
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
    if (eventId >= kCameraEventBase)
    {
        CameraStageEvent event;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto found = g_cameraEvents.find(eventId);
            if (found == g_cameraEvents.end()) return;
            event = std::move(found->second);
            g_cameraEvents.erase(found);
        }
        HandleCameraStageEvent(event);
        return;
    }
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
        g_stageSamples.clear();
        g_stageRequest.reset();
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
    LogLineNoLock(L"initialize build=kk-depth-hotfix-21.1.3.4 api=4");
    if (MH_Initialize() != MH_OK) return kError;
    g_slots.reserve(kMaxPoolSlots);
    g_pendingDepthReadbacks.reserve(kMaxPoolSlots);
    g_hooks.reserve(128);
    g_stageSamples.reserve(kMaxStageSamples);
    g_stageSlots.reserve(kMaxStageSamples);
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
extern "C" __declspec(dllexport) int ORS_SetStageDiagnosticsEnabled(int enabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_settings.stageDiagnostics = enabled != 0;
    return kOk;
}
extern "C" __declspec(dllexport) int ORS_PrepareCameraStageEvent(int captureId, int cameraIndex, int ending, const wchar_t* label) try
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto found = g_requests.find(captureId);
    if (found == g_requests.end() || cameraIndex < 0 ||
        (found->second->phase != CapturePhase::Prepared && found->second->phase != CapturePhase::Capturing)) return kError;
    if (!found->second->settings.stageDiagnostics) return 0;
    if (g_nextCameraEvent >= INT_MAX - kCameraEventBase - 1) return kError;
    CameraStageEvent event;
    event.captureId = captureId; event.cameraIndex = cameraIndex; event.ending = ending != 0;
    event.label = label ? label : L"";
    const int eventId = kCameraEventBase + ++g_nextCameraEvent;
    g_cameraEvents.emplace(eventId, std::move(event));
    return eventId;
}
catch (const std::bad_alloc&) { return kError; }
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
        height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || g_nextId >= (kCameraEventBase - kCaptureEventBase) / 4 - 1)
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
    g_cameraEvents.clear();
    g_stageRequest.reset();
    g_initialized = false;
    LogLineNoLock(L"shutdown");
    g_log.close();
    return kOk;
}
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
