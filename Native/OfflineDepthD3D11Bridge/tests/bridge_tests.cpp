#include "../bridge.cpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void Hr(HRESULT result, const char* message) { Require(SUCCEEDED(result), message); }

struct Scene
{
    ors::ComPtr<ID3D11Texture2D> texture;
    ors::ComPtr<ID3D11DepthStencilView> dsv;
    ors::ComPtr<ID3D11VertexShader> vs;
    ors::ComPtr<ID3D11PixelShader> ps;
    ors::ComPtr<ID3D11Buffer> constants;
    ors::ComPtr<ID3D11DepthStencilState> depth;
    ors::ComPtr<ID3D11RasterizerState> raster;
    UINT width, height;
    Scene(ID3D11Device* device, UINT w, UINT h, bool readable = true, UINT samples = 1,
        DXGI_FORMAT format = DXGI_FORMAT_R32_TYPELESS) : width(w), height(h)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = samples;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | (readable ? D3D11_BIND_SHADER_RESOURCE : 0);
        Hr(device->CreateTexture2D(&desc, nullptr, texture.put()), "create scene depth texture");
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = format == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT :
            (format == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D32_FLOAT);
        view.ViewDimension = samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        Hr(device->CreateDepthStencilView(texture.get(), &view, dsv.put()), "create scene dsv");
        const char* source =
            "cbuffer Frame : register(b0) { float4 parameters; };"
            "struct V { float4 p:SV_POSITION; float2 uv:TEXCOORD0; };"
            "V vs(uint id:SV_VertexID) { V o; float2 uv=float2((id<<1)&2,id&2);"
            "o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1); o.uv=uv; return o; }"
            "float ps(V i):SV_Depth { return parameters.y > 0 ? parameters.z : parameters.x + i.uv.x*.3 + i.uv.y*.2; }";
        ors::ComPtr<ID3DBlob> vb, pb, errors;
        Hr(D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, vb.put(), errors.put()), "compile scene vs");
        Hr(D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, pb.put(), errors.put()), "compile scene ps");
        Hr(device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, vs.put()), "create scene vs");
        Hr(device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, ps.put()), "create scene ps");
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = 16; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Hr(device->CreateBuffer(&buffer, nullptr, constants.put()), "create frame constants");
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        Hr(device->CreateDepthStencilState(&dd, depth.put()), "create scene depth state");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        Hr(device->CreateRasterizerState(&rd, raster.put()), "create scene raster state");
    }
    void draw(ID3D11DeviceContext* ctx, float offset, float clear, bool empty = false)
    {
        ctx->SetPredication(nullptr, FALSE);
        ctx->ClearDepthStencilView(dsv.get(), D3D11_CLEAR_DEPTH, clear, 0);
        ctx->OMSetRenderTargets(0, nullptr, dsv.get());
        ctx->OMSetBlendState(nullptr, nullptr, UINT(-1));
        ctx->OMSetDepthStencilState(depth.get(), 0);
        ctx->RSSetState(raster.get());
        D3D11_VIEWPORT vp{};
        vp.Width = float(width); vp.Height = float(height); vp.MaxDepth = 1;
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.get(), nullptr, 0);
        ctx->PSSetShader(ps.get(), nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        float values[4] = {offset, empty ? 1.f : 0.f, clear, 0};
        ctx->UpdateSubresource(constants.get(), 0, nullptr, values, 0, 0);
        auto* cb = constants.get();
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->Draw(3, 0);
    }
};

struct PoisonState
{
    ors::ComPtr<ID3D11BlendState> blend;
    ors::ComPtr<ID3D11RasterizerState> raster;
    ors::ComPtr<ID3D11GeometryShader> gs;
    explicit PoisonState(ID3D11Device* device)
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].RenderTargetWriteMask = 0;
        Hr(device->CreateBlendState(&bd, blend.put()), "create poison blend");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_FRONT; rd.ScissorEnable = TRUE;
        Hr(device->CreateRasterizerState(&rd, raster.put()), "create poison raster");
        const char* source = "struct V { float4 p:SV_POSITION; float2 uv:TEXCOORD0; };"
            "[maxvertexcount(3)] void gs(triangle V v[3], inout TriangleStream<V> stream) {}";
        ors::ComPtr<ID3DBlob> blob, errors;
        Hr(D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "gs", "gs_4_0", 0, 0, blob.put(), errors.put()), "compile poison gs");
        Hr(device->CreateGeometryShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, gs.put()), "create poison gs");
    }
    void apply(ID3D11DeviceContext* ctx)
    {
        FLOAT factor[4] = {.1f,.2f,.3f,.4f};
        ctx->OMSetBlendState(blend.get(), factor, 0);
        ctx->RSSetState(raster.get());
        D3D11_RECT rect{0,0,0,0};
        ctx->RSSetScissorRects(1, &rect);
        ctx->GSSetShader(gs.get(), nullptr, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    }
    void verify(ID3D11DeviceContext* ctx)
    {
        ors::ComPtr<ID3D11BlendState> actualBlend;
        ors::ComPtr<ID3D11RasterizerState> actualRaster;
        ors::ComPtr<ID3D11GeometryShader> actualGs;
        FLOAT factor[4]; UINT mask = 1;
        ctx->OMGetBlendState(actualBlend.put(), factor, &mask);
        ctx->RSGetState(actualRaster.put());
        ctx->GSGetShader(actualGs.put(), nullptr, nullptr);
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        ctx->IAGetPrimitiveTopology(&topology);
        Require(actualBlend.get() == blend.get() && mask == 0 && std::fabs(factor[2] - .3f) < .00001f, "blend state not restored");
        Require(actualRaster.get() == raster.get(), "raster state not restored");
        Require(actualGs.get() == gs.get(), "geometry shader not restored");
        Require(topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST, "topology not restored");
    }
};

int Prepare(int width, int height, const std::filesystem::path& path)
{
    const int id = ORS_BeginCapture(width, height, L"WARP integration test");
    Require(id > 0, "prepare capture");
    Require(ORS_SetDepthRFloatOutputPath(id, path.c_str()) == 0, "set capture path");
    return id;
}
void Begin(int id) { OnRenderEvent(kCaptureEventBase + id * 4); }
void End(int id) { OnRenderEvent(kCaptureEventBase + id * 4 + 1); }
void VerifyImage(const std::filesystem::path& path, UINT w, UINT h, float offset)
{
    Require(std::filesystem::exists(path), "depth file missing");
    Require(std::filesystem::file_size(path) == uint64_t(w)*h*4, "depth file size mismatch");
    std::vector<float> values(size_t(w)*h);
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(values.data()), std::streamsize(values.size()*4));
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            const float expected = offset + (float(x)+.5f)/w*.3f + (float(y)+.5f)/h*.2f;
            Require(std::fabs(values[size_t(y)*w+x] - expected) < .00001f, "depth value, frame identity or row orientation mismatch");
        }
}
}

int main()
{
    try
    {
        const auto folder = std::filesystem::temp_directory_path() /
            (L"ORSDepthPressureTests-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(folder);
        std::wcout << L"Artifacts: " << folder << std::endl;
        Require(ORS_Initialize((folder/L"probe.log").c_str()) == 0, "initialize bridge");
        ors::ComPtr<ID3D11Device> device;
        ors::ComPtr<ID3D11DeviceContext> ctx;
        D3D_FEATURE_LEVEL selected;
        Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, device.put(), &selected, ctx.put()), "create WARP device");
        Scene scene(device.get(), 67, 41);
        Scene unrelated(device.get(), 32, 24);
        PoisonState poison(device.get());
        Require(ORS_SetUnityD3D11Texture(scene.texture.get()) == 0, "set native texture device");
        ORS_ConfigureReadback(64, 1, 1);
        ORS_SetCandidateDiagnosticsEnabled(0);

        // CPU End must not terminate collection ahead of the ordered render event.
        const auto firstPath = folder/L"first.rfloat";
        int id = Prepare(67, 41, firstPath);
        Begin(id);
        ORS_EndCapture();
        Require(g_captureActive, "CPU End prematurely closed render capture");
        scene.draw(ctx.get(), .15f, 0);
        const auto bound = g_contextDsv[ctx.get()];
        ctx->ClearDepthStencilView(unrelated.dsv.get(), D3D11_CLEAR_DEPTH, 0, 0);
        Require(g_contextDsv[ctx.get()] == bound, "ClearDepthStencilView changed tracked OM binding");
        poison.apply(ctx.get());
        End(id);
        poison.verify(ctx.get());
        OnRenderEvent(2003);
        Require(ORS_GetDepthCaptureStatus(id) == 2, "first capture not saved");
        VerifyImage(firstPath, 67, 41, .15f);
        std::cout << "PASS render-thread end, draw attribution, pipeline isolation and exact float/row readback\n";

        // Independent request IDs must keep queued frame paths and content paired.
        std::vector<int> ids;
        g_testDeferNonBlockingReadbacks = true;
        for (int frame = 0; frame < 24; ++frame)
        {
            const auto path = folder/(L"frame-" + std::to_wstring(frame) + L".rfloat");
            id = Prepare(67, 41, path);
            ids.push_back(id);
            Begin(id);
            scene.draw(ctx.get(), .1f + frame*.005f, 0);
            poison.apply(ctx.get());
            End(id);
            poison.verify(ctx.get());
            Require(g_pendingDepthReadbacks.size() <= kMaxPoolSlots, "readback queue exceeded capacity");
        }
        OnRenderEvent(2003);
        for (int frame = 0; frame < 24; ++frame)
        {
            Require(ORS_GetDepthCaptureStatus(ids[frame]) == 2, "queued frame not saved");
            VerifyImage(folder/(L"frame-" + std::to_wstring(frame) + L".rfloat"), 67, 41, .1f+frame*.005f);
        }
        Require(g_pipeline.textureAllocations <= 7, "texture allocations grew with frame count");
        std::cout << "PASS 24 queued frames, immutable pairing, final flush and bounded texture reuse\n";

        ors::testMemoryPressure = true;
        for (int frame = 0; frame < 4; ++frame)
        {
            const auto path = folder/(L"pressure-" + std::to_wstring(frame) + L".rfloat");
            id = Prepare(67, 41, path);
            Begin(id); scene.draw(ctx.get(), .3f+frame*.01f, 0); End(id);
            Require(g_slots.size() == 1 && g_pendingDepthReadbacks.size() <= 1, "memory pressure did not reduce pool/in-flight work");
        }
        OnRenderEvent(2003);
        for (int frame = 0; frame < 4; ++frame)
            VerifyImage(folder/(L"pressure-"+std::to_wstring(frame)+L".rfloat"), 67, 41, .3f+frame*.01f);
        ors::testMemoryPressure = false;
        g_testDeferNonBlockingReadbacks = false;
        std::cout << "PASS simulated memory pressure: single-slot backpressure without lost frames\n";

        const int preparedA = Prepare(67, 41, folder/L"prepared-a.rfloat");
        Begin(preparedA);
        const int preparedB = Prepare(67, 41, folder/L"prepared-b.rfloat");
        Require(g_captureActive && g_activeId == preparedA, "preparing next request altered the active capture");
        scene.draw(ctx.get(), .11f, 0); End(preparedA);
        Begin(preparedB); scene.draw(ctx.get(), .33f, 0); End(preparedB);
        OnRenderEvent(2003);
        VerifyImage(folder/L"prepared-a.rfloat", 67, 41, .11f);
        VerifyImage(folder/L"prepared-b.rfloat", 67, 41, .33f);
        std::cout << "PASS preparing another request cannot overwrite active frame identity/path\n";

        // A failure on the same realtime output path must not retain the previous depth.
        id = Prepare(67, 41, firstPath);
        Begin(id); scene.draw(ctx.get(), 0, 0, true); End(id);
        Require(ORS_GetDepthCaptureStatus(id) < 0 && !std::filesystem::exists(firstPath), "all-clear frame accepted or stale depth retained");
        id = Prepare(67, 41, folder/L"wrong-size.rfloat");
        Begin(id); unrelated.draw(ctx.get(), .2f, 0); End(id);
        Require(ORS_GetDepthCaptureStatus(id) < 0 && !std::filesystem::exists(folder/L"wrong-size.rfloat"), "wrong resolution accepted");
        Scene unreadable(device.get(), 67, 41, false);
        id = Prepare(67, 41, folder/L"unreadable.rfloat");
        Begin(id); unreadable.draw(ctx.get(), .2f, 0); End(id);
        Require(ORS_GetDepthCaptureStatus(id) < 0, "unreadable depth accepted");
        Scene msaa(device.get(), 67, 41, true, 4);
        id = Prepare(67, 41, folder/L"msaa.rfloat");
        Begin(id); msaa.draw(ctx.get(), .2f, 0); End(id);
        Require(ORS_GetDepthCaptureStatus(id) < 0, "MSAA-only candidate accepted");
        std::cout << "PASS all-clear, stale-file, wrong-size and unreadable-candidate rejection\n";

        g_sampleOutput.reset();
        ors::testFailTextureAllocations = true;
        id = Prepare(67, 41, folder/L"allocation-failed.rfloat");
        Begin(id); scene.draw(ctx.get(), .2f, 0); End(id);
        Require(ORS_GetDepthCaptureStatus(id) < 0 && !std::filesystem::exists(folder/L"allocation-failed.rfloat"), "unverified candidate used after allocation failure");
        Require(g_requests.at(id)->failure.find(L"8007000e") != std::wstring::npos, "allocation HRESULT missing");
        ors::testFailTextureAllocations = false;
        std::cout << "PASS forced allocation failure: HRESULT logged, no score-only fallback or fake depth\n";

        id = Prepare(67, 41, folder/L"recording-failed.rfloat");
        Begin(id);
        g_testFailDsvRecording = true;
        scene.draw(ctx.get(), .2f, 0);
        g_testFailDsvRecording = false;
        End(id);
        Require(ORS_GetDepthCaptureStatus(id) < 0 &&
            g_requests.at(id)->failure == L"cpu_allocation_failed_recording_depth_views", "recording allocation exception escaped or was not classified");
        std::cout << "PASS native hook contains CPU allocation failure and lets original D3D calls continue\n";

        for (auto format : {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS})
        {
            Scene formatted(device.get(), 67, 41, true, 1, format);
            const auto path = folder/(L"format-" + std::to_wstring(UINT(format)) + L".rfloat");
            id = Prepare(67, 41, path);
            Begin(id); formatted.draw(ctx.get(), .17f, 0); End(id); OnRenderEvent(2003);
            VerifyImage(path, 67, 41, .17f);
        }
        id = Prepare(67, 41, folder/L"custom-clear.rfloat");
        Begin(id); scene.draw(ctx.get(), 0, .37f, true); End(id);
        Require(ORS_GetDepthCaptureStatus(id) < 0, "actual DSV clear value was ignored");
        std::cout << "PASS D32+stencil/D24 formats and actual per-DSV clear values\n";

        // Resize and non-reversed depth retain the same float contract.
        ORS_ConfigureReadback(64, 1, 0);
        id = Prepare(32, 24, folder/L"resize.rfloat");
        Begin(id); unrelated.draw(ctx.get(), .25f, 1); End(id); OnRenderEvent(2003);
        VerifyImage(folder/L"resize.rfloat", 32, 24, .25f);
        id = Prepare(32, 24, folder/L"forward-clear.rfloat");
        Begin(id); unrelated.draw(ctx.get(), 0, 1, true); End(id);
        Require(ORS_GetDepthCaptureStatus(id) < 0, "non-reversed all-one clear accepted");
        ORS_ConfigureReadback(64, 1, 1);
        id = Prepare(4096, 4096, folder/L"budget.rfloat");
        Require(PoolLimit(g_requests.at(id)) == 0, "insufficient memory budget accepted");
        Begin(id); OnRenderEvent(kCaptureEventBase + id*4 + 2);
        Require(ORS_GetDepthCaptureStatus(id) < 0, "cancel not reported");
        std::cout << "PASS resize, forward/reversed clear conventions, budget limit and cancellation\n";

        Require(g_pendingDepthReadbacks.empty(), "final readbacks still pending");
        ctx->ClearState();
        Require(ORS_Shutdown() == 0, "shutdown failed");
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
