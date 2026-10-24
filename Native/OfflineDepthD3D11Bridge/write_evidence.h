#pragma once

#include "depth_pipeline.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace ors
{
#ifdef ORS_TESTING
inline bool testWriteQueryUnavailable = false;
#endif
// Resource versions form a time-ordered graph. A copy inherits the source's
// version at submission time, never a write that happens later in the capture.
class WriteEvidence
{
    static constexpr size_t none = std::numeric_limits<size_t>::max();
    struct Version
    {
        size_t source = none, previous = none;
        bool cleared = false, partial = false, fresh = false, hasClearValue = false, continuation = false;
        float clearValue = 0;
        uint64_t passedSamples = 0;
    };
    struct Resource
    {
        ComPtr<IUnknown> identity;
        size_t latest = none;
    };
    struct Segment { size_t query = 0, version = 0; };
    ID3D11Device* device = nullptr;
    std::unordered_map<uintptr_t, Resource> resources;
    std::vector<Version> versions;
    std::vector<ComPtr<ID3D11Query>> queries;
    std::vector<Segment> segments;
    size_t active = none;
    uintptr_t activeResource = 0;
    UINT width = 0, height = 0;
    bool incomplete = false;
    void queryError(const wchar_t* operation, HRESULT hr)
    {
        std::wstringstream text;
        text << operation << L" hr=0x" << std::hex << unsigned(hr);
        if (device) text << L" deviceRemovedReason=0x" << unsigned(device->GetDeviceRemovedReason());
        error = text.str();
    }

    Resource* resource(ID3D11Resource* input, bool create)
    {
        if (!input) return nullptr;
        D3D11_RESOURCE_DIMENSION dimension{};
        input->GetType(&dimension);
        if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return nullptr;
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(input->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put())))) return nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width != width || desc.Height != height || desc.ArraySize != 1 || desc.MipLevels != 1 ||
            (desc.Format != DXGI_FORMAT_R32_TYPELESS && desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS &&
             desc.Format != DXGI_FORMAT_R24G8_TYPELESS)) return nullptr;
        ComPtr<IUnknown> identity;
        if (FAILED(input->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(identity.put())))) return nullptr;
        const auto key = reinterpret_cast<uintptr_t>(identity.get());
        auto found = resources.find(key);
        if (found != resources.end()) return &found->second;
        if (!create) return nullptr;
        if (resources.size() >= 256 || versions.size() >= 4096)
        { incomplete = true; error = L"write_evidence_graph_limit"; return nullptr; }
        Resource item;
        item.identity = std::move(identity);
        item.latest = versions.size();
        versions.emplace_back();
        return &resources.emplace(key, std::move(item)).first->second;
    }
public:
    int captureId = 0;
    std::wstring error;
    uint64_t fullCopies = 0, rejectedCopies = 0, commandLists = 0;

    void begin(ID3D11Device* owner, int id, UINT w, UINT h)
    {
        if (device != owner) queries.clear();
        device = owner; captureId = id; width = w; height = h;
        resources.clear(); versions.clear(); segments.clear();
        active = none; activeResource = 0; error.clear(); fullCopies = rejectedCopies = commandLists = 0; incomplete = false;
    }
    void stop(ID3D11DeviceContext* ctx)
    {
        if (active != none) ctx->End(queries[active].get());
        active = none; activeResource = 0;
    }
    void observe(ID3D11DeviceContext* ctx)
    {
        ComPtr<ID3D11DepthStencilView> dsv;
        ctx->OMGetRenderTargets(0, nullptr, dsv.put());
        ComPtr<ID3D11Resource> texture;
        ComPtr<IUnknown> identity;
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        bool writable = false;
        if (dsv)
        {
            dsv->GetDesc(&view);
            dsv->GetResource(texture.put());
            texture->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(identity.put()));
            ComPtr<ID3D11DepthStencilState> state;
            UINT ref = 0;
            ctx->OMGetDepthStencilState(state.put(), &ref);
            D3D11_DEPTH_STENCIL_DESC desc{};
            desc.DepthEnable = TRUE; desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            if (state) state->GetDesc(&desc);
            writable = desc.DepthEnable && desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL &&
                !(view.Flags & D3D11_DSV_READ_ONLY_DEPTH) && view.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D;
        }
        const auto key = reinterpret_cast<uintptr_t>(identity.get());
        if (active != none && writable && key == activeResource) return;
        stop(ctx);
        if (!writable) return;
#ifdef ORS_TESTING
        if (testWriteQueryUnavailable) { error = L"test_gpu_write_query_unavailable"; return; }
#endif
        auto* target = resource(texture.get(), true);
        if (!target) return;
        if (segments.size() >= 512) { error = L"gpu_write_query_limit_512"; return; }
        const size_t index = segments.size();
        if (index == queries.size())
        {
            D3D11_QUERY_DESC desc{}; desc.Query = D3D11_QUERY_OCCLUSION;
            ComPtr<ID3D11Query> query;
            const auto hr = device->CreateQuery(&desc, query.put());
            if (FAILED(hr)) { queryError(L"create_gpu_write_query_failed", hr); return; }
            queries.push_back(std::move(query));
        }
        if (versions.size() >= 4096) { incomplete = true; error = L"write_evidence_graph_limit"; return; }
        Version version;
        version.source = target->latest; version.continuation = true;
        version.hasClearValue = versions[target->latest].hasClearValue;
        version.clearValue = versions[target->latest].clearValue;
        target->latest = versions.size();
        versions.push_back(version);
        segments.push_back({index, target->latest});
        ctx->Begin(queries[index].get());
        active = index; activeResource = key;
    }
    void cleared(ID3D11Resource* texture, float value)
    {
        auto* target = resource(texture, true);
        if (!target) return;
        if (versions.size() >= 4096) { incomplete = true; error = L"write_evidence_graph_limit"; return; }
        Version version;
        version.cleared = version.hasClearValue = true; version.clearValue = value;
        target->latest = versions.size();
        versions.push_back(version);
    }
    void copied(ID3D11Resource* destination, ID3D11Resource* source, bool full, bool compatible = true)
    {
        auto* dst = resource(destination, true);
        if (!dst) return;
        if (versions.size() >= 4096) { incomplete = true; error = L"write_evidence_graph_limit"; return; }
        const auto previous = dst->latest;
        // Inserting a source may rehash resources; keep only indices across that call.
        auto* src = compatible ? resource(source, true) : nullptr;
        const auto sourceVersion = src ? src->latest : none;
        dst = resource(destination, false);
        if (sourceVersion == previous) return;
        Version version;
        version.source = sourceVersion; version.previous = previous; version.partial = !full;
        if (sourceVersion != none)
        {
            version.hasClearValue = versions[sourceVersion].hasClearValue;
            version.clearValue = versions[sourceVersion].clearValue;
        }
        dst->latest = versions.size();
        versions.push_back(version);
        if (full && compatible) ++fullCopies; else ++rejectedCopies;
    }
    void invalidateAll()
    {
        ++commandLists;
        for (auto& item : resources)
        {
            if (versions.size() >= 4096) { incomplete = true; error = L"write_evidence_graph_limit"; break; }
            item.second.latest = versions.size();
            versions.emplace_back();
        }
    }
    void finish(ID3D11DeviceContext* ctx)
    {
        stop(ctx);
        ctx->Flush();
        const auto deadline = GetTickCount64() + 2000;
        for (const auto& segment : segments)
        {
            UINT64 passed = 0;
            HRESULT hr;
            do
            {
                hr = ctx->GetData(queries[segment.query].get(), &passed, sizeof(passed), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (hr != S_FALSE || GetTickCount64() >= deadline) break;
                Sleep(1);
            } while (true);
            if (hr == S_OK) versions[segment.version].passedSamples += passed;
            else if (FAILED(hr)) queryError(L"gpu_write_query_failed", hr);
            else error = L"gpu_write_query_timeout_2000ms";
        }
        if (!error.empty()) queries.clear();
        for (auto& version : versions)
        {
            version.fresh = version.cleared || version.passedSamples > 0;
            if (version.source != none && versions[version.source].fresh &&
                (!version.partial || versions[version.previous].fresh)) version.fresh = true;
        }
    }
    bool fresh(ID3D11Resource* texture)
    {
        auto* item = resource(texture, false);
        return !incomplete && item && versions[item->latest].fresh;
    }
    uint64_t samples(ID3D11Resource* texture)
    {
        auto* item = resource(texture, false);
        return item ? versionSamples(item->latest) : 0;
    }
    float clearReference(ID3D11Resource* texture, float fallback)
    {
        auto* item = resource(texture, false);
        if (!item) return fallback;
        const auto& version = versions[item->latest];
        return version.hasClearValue ? version.clearValue : fallback;
    }
    const wchar_t* reason(ID3D11Resource* texture)
    {
        auto* item = resource(texture, false);
        if (incomplete) return L"write_evidence_graph_incomplete";
        if (!item) return L"unobserved_resource";
        auto index = item->latest;
        while (versions[index].continuation && versions[index].passedSamples == 0) index = versions[index].source;
        const auto& version = versions[index];
        if (!version.fresh) return L"current_capture_write_unproven";
        if (version.passedSamples > 0) return L"gpu_depth_samples";
        if (version.source != none) return L"copy_from_current_capture";
        return version.cleared ? L"current_capture_clear" : L"gpu_depth_samples";
    }
    size_t queryCount() const { return segments.size(); }
    size_t versionTag(ID3D11Resource* texture)
    {
        auto* item = resource(texture, false);
        return item ? item->latest : none;
    }
    bool versionFresh(size_t index) const { return !incomplete && index < versions.size() && versions[index].fresh; }
    uint64_t versionSamples(size_t index) const
    {
        uint64_t total = 0;
        while (index < versions.size())
        {
            total += versions[index].passedSamples;
            index = versions[index].source;
        }
        return total;
    }
    void releaseResources() { for (auto& entry : resources) entry.second.identity.reset(); }
    void reset()
    {
        resources.clear(); versions.clear(); segments.clear(); queries.clear();
        active = none; activeResource = 0; device = nullptr; captureId = 0;
    }
};
}
