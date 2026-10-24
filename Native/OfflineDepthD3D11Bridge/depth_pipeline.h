#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <array>
#include <memory>
#include <string>
#include <utility>

namespace ors
{
#ifdef ORS_TESTING
inline bool testMemoryPressure = false;
inline bool testFailTextureAllocations = false;
#endif
template<class T> class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : value(other.value) { other.value = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) { reset(); value = other.value; other.value = nullptr; }
        return *this;
    }
    T* get() const { return value; }
    T* operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
    T** put() { reset(); return &value; }
    void reset() { if (value) { value->Release(); value = nullptr; } }
private:
    T* value = nullptr;
};

template<class T> struct ShaderBinding
{
    ComPtr<T> shader;
    ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES]{};
    UINT count = D3D11_SHADER_MAX_INTERFACES;
    ~ShaderBinding() { for (UINT i = 0; i < count; ++i) if (instances[i]) instances[i]->Release(); }
};

// Unity caches its own state. Restore every binding changed by the bridge.
class PipelineStateGuard
{
public:
    explicit PipelineStateGuard(ID3D11DeviceContext* c) : ctx(c)
    {
        ctx->OMGetRenderTargets(8, rtvs, dsv.put());
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, uavs);
        ctx->OMGetBlendState(blend.put(), blendFactor, &sampleMask);
        ctx->OMGetDepthStencilState(depth.put(), &stencilRef);
        ctx->RSGetState(raster.put());
        ctx->RSGetViewports(&viewportCount, viewports);
        ctx->RSGetScissorRects(&scissorCount, scissors);
        ctx->IAGetInputLayout(layout.put());
        ctx->IAGetPrimitiveTopology(&topology);
        ctx->VSGetShader(vs.shader.put(), vs.instances, &vs.count);
        ctx->PSGetShader(ps.shader.put(), ps.instances, &ps.count);
        ctx->GSGetShader(gs.shader.put(), gs.instances, &gs.count);
        ctx->HSGetShader(hs.shader.put(), hs.instances, &hs.count);
        ctx->DSGetShader(ds.shader.put(), ds.instances, &ds.count);
        ctx->PSGetShaderResources(0, 1, srv.put());
        ctx->PSGetSamplers(0, 1, sampler.put());
        ctx->GetPredication(predicate.put(), &predicateValue);
    }
    ~PipelineStateGuard()
    {
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);
        UINT rtvCount = 0;
        for (UINT i = 0; i < 8; ++i) if (rtvs[i]) rtvCount = i + 1;
        UINT counts[8];
        for (auto& count : counts) count = UINT(-1);
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount, rtvs, dsv.get(), rtvCount,
            8 - rtvCount, uavs + rtvCount, counts);
        ctx->OMSetBlendState(blend.get(), blendFactor, sampleMask);
        ctx->OMSetDepthStencilState(depth.get(), stencilRef);
        ctx->RSSetState(raster.get());
        ctx->RSSetViewports(viewportCount, viewports);
        ctx->RSSetScissorRects(scissorCount, scissors);
        ctx->IASetInputLayout(layout.get());
        ctx->IASetPrimitiveTopology(topology);
        ctx->VSSetShader(vs.shader.get(), vs.instances, vs.count);
        ctx->PSSetShader(ps.shader.get(), ps.instances, ps.count);
        ctx->GSSetShader(gs.shader.get(), gs.instances, gs.count);
        ctx->HSSetShader(hs.shader.get(), hs.instances, hs.count);
        ctx->DSSetShader(ds.shader.get(), ds.instances, ds.count);
        auto* oldSrv = srv.get();
        auto* oldSampler = sampler.get();
        ctx->PSSetShaderResources(0, 1, &oldSrv);
        ctx->PSSetSamplers(0, 1, &oldSampler);
        ctx->SetPredication(predicate.get(), predicateValue);
        for (auto* rtv : rtvs) if (rtv) rtv->Release();
        for (auto* uav : uavs) if (uav) uav->Release();
    }
    PipelineStateGuard(const PipelineStateGuard&) = delete;
    PipelineStateGuard& operator=(const PipelineStateGuard&) = delete;
private:
    ID3D11DeviceContext* ctx;
    ID3D11RenderTargetView* rtvs[8]{};
    ID3D11UnorderedAccessView* uavs[8]{};
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicateValue = FALSE;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0, stencilRef = 0;
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ShaderBinding<ID3D11VertexShader> vs;
    ShaderBinding<ID3D11PixelShader> ps;
    ShaderBinding<ID3D11GeometryShader> gs;
    ShaderBinding<ID3D11HullShader> hs;
    ShaderBinding<ID3D11DomainShader> ds;
};

struct MemorySnapshot
{
    bool systemValid = false, gpuValid = false;
    uint64_t availableRam = 0, availableCommit = 0, privateBytes = 0;
    uint64_t gpuBudget = 0, gpuUsage = 0, sharedBudget = 0, sharedUsage = 0;
    bool pressure() const
    {
#ifdef ORS_TESTING
        if (testMemoryPressure) return true;
#endif
        constexpr uint64_t threshold = 512ull * 1024 * 1024;
        return (systemValid && (availableRam < threshold || availableCommit < threshold)) ||
            (gpuValid && gpuBudget > 0 && gpuUsage > gpuBudget - gpuBudget / 10);
    }
};

inline MemorySnapshot ReadMemory(ID3D11Device* device)
{
    MemorySnapshot result;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        result.systemValid = true;
        result.availableRam = memory.ullAvailPhys;
        result.availableCommit = memory.ullAvailPageFile;
    }
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof(process);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)))
        result.privateBytes = process.PrivateUsage;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter3> adapter3;
    if (device && SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.put()))) &&
        SUCCEEDED(dxgiDevice->GetAdapter(adapter.put())) &&
        SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(adapter3.put()))))
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
        {
            result.gpuValid = true;
            result.gpuBudget = local.Budget;
            result.gpuUsage = local.CurrentUsage;
        }
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)))
        {
            result.sharedBudget = shared.Budget;
            result.sharedUsage = shared.CurrentUsage;
        }
    }
    return result;
}

struct FloatTarget
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT width = 0, height = 0;
    void reset() { rtv.reset(); texture.reset(); width = height = 0; }
};

struct ReadbackSlot
{
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Query> complete;
    UINT width = 0, height = 0;
    bool busy = false;
    uint64_t bytes() const { return uint64_t(width) * height * sizeof(float); }
};

class DepthPipeline
{
public:
    HRESULT lastHr = S_OK;
    std::wstring lastOperation;
    uint64_t textureAllocations = 0;

    bool initialize(ID3D11Device* device)
    {
        if (owner == device && vs && ps && sampler && raster && depth && blend) return true;
        reset();
        owner = device;
        const char* source =
            "Texture2D<float> DepthTex : register(t0);\n"
            "SamplerState PointSampler : register(s0);\n"
            "struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
            "V vs(uint id : SV_VertexID) { V o; float2 uv=float2((id<<1)&2,id&2);"
            "o.uv=uv; o.pos=float4(uv*float2(2,-2)+float2(-1,1),0,1); return o; }\n"
            "float ps(V i) : SV_Target { return DepthTex.SampleLevel(PointSampler,i.uv,0); }\n";
        ComPtr<ID3DBlob> vb, pb, errors;
        if (!check(D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "vs", "vs_4_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vb.put(), errors.put()), L"compile_depth_vs") ||
            !check(D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "ps", "ps_4_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, pb.put(), errors.put()), L"compile_depth_ps") ||
            !check(device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, vs.put()), L"create_depth_vs") ||
            !check(device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, ps.put()), L"create_depth_ps"))
            return false;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        return check(device->CreateSamplerState(&sd, sampler.put()), L"create_point_sampler") &&
            check(device->CreateRasterizerState(&rd, raster.put()), L"create_rasterizer") &&
            check(device->CreateDepthStencilState(&dd, depth.put()), L"create_depth_state") &&
            check(device->CreateBlendState(&bd, blend.put()), L"create_blend_state");
    }

    bool ensureTarget(ID3D11Device* device, UINT width, UINT height, FloatTarget& target)
    {
        if (target.width == width && target.height == height && target.texture) return true;
        target.reset();
#ifdef ORS_TESTING
        if (testFailTextureAllocations) return check(E_OUTOFMEMORY, L"create_r32_output");
#endif
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (!check(device->CreateTexture2D(&desc, nullptr, target.texture.put()), L"create_r32_output") ||
            !check(device->CreateRenderTargetView(target.texture.get(), nullptr, target.rtv.put()), L"create_r32_rtv"))
        { target.reset(); return false; }
        target.width = width; target.height = height;
        ++textureAllocations;
        return true;
    }

    bool ensureSlot(ID3D11Device* device, UINT width, UINT height, ReadbackSlot& slot)
    {
        if (slot.width == width && slot.height == height && slot.staging && slot.complete) return true;
        if (slot.busy) return check(E_UNEXPECTED, L"reuse_busy_staging_slot");
        slot.staging.reset(); slot.complete.reset();
        slot.width = slot.height = 0;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC query{};
        query.Query = D3D11_QUERY_EVENT;
        if (!check(device->CreateTexture2D(&desc, nullptr, slot.staging.put()), L"create_r32_staging") ||
            !check(device->CreateQuery(&query, slot.complete.put()), L"create_completion_query"))
        { slot.staging.reset(); slot.complete.reset(); return false; }
        slot.width = width; slot.height = height;
        ++textureAllocations;
        return true;
    }

    bool copy(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* source,
        DXGI_FORMAT format, FloatTarget& target, ReadbackSlot& slot)
    {
        ComPtr<ID3D11ShaderResourceView> srv;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels = 1;
        if (!check(device->CreateShaderResourceView(source, &desc, srv.put()), L"create_depth_srv")) return false;
        PipelineStateGuard saved(ctx);
        ctx->SetPredication(nullptr, FALSE);
        auto* rtv = target.rtv.get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        const FLOAT sentinel[4] = {-1, -1, -1, -1};
        ctx->ClearRenderTargetView(rtv, sentinel);
        ctx->OMSetBlendState(blend.get(), nullptr, UINT(-1));
        ctx->OMSetDepthStencilState(depth.get(), 0);
        ctx->RSSetState(raster.get());
        D3D11_VIEWPORT viewport{};
        viewport.Width = float(target.width); viewport.Height = float(target.height); viewport.MaxDepth = 1;
        ctx->RSSetViewports(1, &viewport);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.get(), nullptr, 0);
        ctx->PSSetShader(ps.get(), nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        auto* input = srv.get();
        auto* point = sampler.get();
        ctx->PSSetShaderResources(0, 1, &input);
        ctx->PSSetSamplers(0, 1, &point);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);
        ctx->CopyResource(slot.staging.get(), target.texture.get());
        ctx->End(slot.complete.get());
        return true;
    }

    void reset()
    {
        vs.reset(); ps.reset(); sampler.reset(); raster.reset(); depth.reset(); blend.reset(); owner = nullptr;
    }
private:
    bool check(HRESULT hr, const wchar_t* operation)
    {
        lastHr = hr;
        lastOperation = operation;
        return SUCCEEDED(hr);
    }
    ID3D11Device* owner = nullptr;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11BlendState> blend;
};
}
