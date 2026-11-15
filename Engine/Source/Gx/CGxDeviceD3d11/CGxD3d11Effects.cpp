#include "Engine/Source/Gx/CGxDeviceD3d11/CGxD3d11Effects.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Engine/Source/Gx/CGxDeviceD3d11/CGxDeviceD3d11.h"
#include "Engine/Source/Gx/CGxDeviceD3d11/SpirvToHlsl.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"

// GxEffects::Texture, Program and Readback are the Vulkan backend's types as far as the compiler is concerned (it defines
// them in its own translation unit); here the same pointers are this file's own D3dTexture, D3dProgram and D3dReadback, only
// ever converted back and forth, never dereferenced as the other.

namespace {

template <typename T>
void Rel(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct D3dTexture {
    ID3D11Texture2D* tex2d = nullptr;
    ID3D11Texture3D* tex3d = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t levels = 1;
    GxEffects::Format format = GxEffects::Format::BGRA8;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
};

struct D3dProgram {
    D3d11SpirvToHlsl::Stage stage = D3d11SpirvToHlsl::Stage::Fragment;
    ID3D11PixelShader* ps = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    uint32_t pushBytes = 0;
    uint32_t volumeMask = 0;
    std::string name;
};

struct D3dReadback {
    ID3D11Texture2D* staging = nullptr;
    ID3D11Query* query = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t texelBytes = 0;
};

D3dTexture* Tex(GxEffects::Texture* t) { return reinterpret_cast<D3dTexture*>(t); }
const D3dTexture* Tex(const GxEffects::Texture* t) { return reinterpret_cast<const D3dTexture*>(t); }
GxEffects::Texture* Out(D3dTexture* t) { return reinterpret_cast<GxEffects::Texture*>(t); }
D3dProgram* Prog(const GxEffects::Program* p) { return const_cast<D3dProgram*>(reinterpret_cast<const D3dProgram*>(p)); }

uint32_t TexelBytes(GxEffects::Format f) {
    switch (f) {
        case GxEffects::Format::BGRA8: return 4;
        case GxEffects::Format::RGBA16F: return 8;
        case GxEffects::Format::RGBA32F: return 16;
        case GxEffects::Format::R32F: return 4;
        case GxEffects::Format::RG16F: return 4;
        case GxEffects::Format::RG32F: return 8;
        case GxEffects::Format::R8: return 1;
        case GxEffects::Format::Depth: return 4;
    }
    return 4;
}

DXGI_FORMAT DxgiOf(GxEffects::Format f) {
    switch (f) {
        case GxEffects::Format::BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case GxEffects::Format::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case GxEffects::Format::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case GxEffects::Format::R32F: return DXGI_FORMAT_R32_FLOAT;
        case GxEffects::Format::RG16F: return DXGI_FORMAT_R16G16_FLOAT;
        case GxEffects::Format::RG32F: return DXGI_FORMAT_R32G32_FLOAT;
        case GxEffects::Format::R8: return DXGI_FORMAT_R8_UNORM;
        case GxEffects::Format::Depth: return DXGI_FORMAT_R32_FLOAT;   // CopyDepth writes it as a float target
    }
    return DXGI_FORMAT_UNKNOWN;
}

// --- built-in shaders ---------------------------------------------------------------------------------------------

// The full-screen triangle; uv 0,0 is the top left of the target (GxEffects.h).
const char kFullScreenVs[] = R"HLSL(
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VsOut main(uint id : SV_VertexID)
{
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}
)HLSL";

// CopyColor: a rectangle of the source (uv rect in c0 = x, y, w, h) onto the whole target.
const char kCopyPs[] = R"HLSL(
Texture2D<float4> t0 : register(t0);
SamplerState s0 : register(s0);
cbuffer Rect : register(b4) { float4 rect; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return t0.SampleLevel(s0, rect.xy + uv * rect.zw, 0);
}
)HLSL";

// CopyDepth: the world's depth, sample 0 if it is multisampled, as a float.
const char kDepthPs[] = R"HLSL(
#if MSAA
Texture2DMS<float> t0 : register(t0);
float main(float4 pos : SV_Position) : SV_Target { return t0.Load(int2(pos.xy), 0); }
#else
Texture2D<float> t0 : register(t0);
float main(float4 pos : SV_Position) : SV_Target { return t0.Load(int3(int2(pos.xy), 0)); }
#endif
)HLSL";

constexpr uint32_t kConstantsRegister = 4;
constexpr uint32_t kPushRegister = 5;
constexpr uint32_t kProgramCacheVersion = 1;

uint64_t Fnv(const void* data, size_t bytes, uint64_t h = 1469598103934665603ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

class D3d11Effects : public GxEffects::Device {
    public:
    CGxDeviceD3d11* m_dev = nullptr;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    ID3D11VertexShader* m_fullScreenVs = nullptr;
    ID3D11PixelShader* m_copyPs = nullptr;
    ID3D11PixelShader* m_depthPs = nullptr;
    ID3D11PixelShader* m_depthMsPs = nullptr;
    ID3D11Buffer* m_constants = nullptr;
    ID3D11Buffer* m_push = nullptr;
    ID3D11RasterizerState* m_raster = nullptr;
    ID3D11DepthStencilState* m_noDepth = nullptr;
    std::map<uint32_t, ID3D11BlendState*> m_blends;
    std::map<uint32_t, ID3D11SamplerState*> m_samplers;
    D3dTexture* m_empty2d = nullptr;
    D3dTexture* m_empty3d = nullptr;
    std::string m_cacheFolder;
    bool m_failed = false;

    bool Attach(CGxDeviceD3d11* dev) {
        if (m_dev == dev && m_device == dev->m_d3dDevice) {
            return !m_failed;
        }

        Reset();
        m_dev = dev;
        m_device = dev->m_d3dDevice;
        m_context = dev->m_d3dContext;
        m_failed = !Init();

        if (m_failed) {
            D3d11Log("GxEffects: initialisation failed, effects off");
        }

        return !m_failed;
    }

    void Reset() {
        Rel(m_fullScreenVs);
        Rel(m_copyPs);
        Rel(m_depthPs);
        Rel(m_depthMsPs);
        Rel(m_constants);
        Rel(m_push);
        Rel(m_raster);
        Rel(m_noDepth);
        for (auto& b : m_blends) Rel(b.second);
        m_blends.clear();
        for (auto& s : m_samplers) Rel(s.second);
        m_samplers.clear();
        DestroyTexture(m_empty2d);
        DestroyTexture(m_empty3d);
        m_dev = nullptr;
        m_device = nullptr;
        m_context = nullptr;
        m_failed = false;
    }

    static ID3DBlob* Compile(const char* source, const char* profile, const char* name, const D3D_SHADER_MACRO* macros = nullptr, UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL2) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr = D3DCompile(source, strlen(source), name, macros, nullptr, "main", profile, flags, 0, &code, &errors);

        if (FAILED(hr) || !code) {
            D3d11Log("GxEffects: %s: compile failed: %.400s", name, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            Rel(code);
        }

        Rel(errors);
        return code;
    }

    bool Init() {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) {
            *slash = 0;
        }
        m_exeFolder = path;
        m_cacheFolder = m_exeFolder + "\\Cache\\D3d11Effects";

        if (ID3DBlob* b = Compile(kFullScreenVs, "vs_5_0", "FullScreenVs")) {
            m_device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &m_fullScreenVs);
            b->Release();
        }

        if (ID3DBlob* b = Compile(kCopyPs, "ps_5_0", "CopyPs")) {
            m_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &m_copyPs);
            b->Release();
        }

        {
            D3D_SHADER_MACRO off[] = { { "MSAA", "0" }, { nullptr, nullptr } };
            D3D_SHADER_MACRO on[] = { { "MSAA", "1" }, { nullptr, nullptr } };

            if (ID3DBlob* b = Compile(kDepthPs, "ps_5_0", "DepthPs", off)) {
                m_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &m_depthPs);
                b->Release();
            }

            if (ID3DBlob* b = Compile(kDepthPs, "ps_5_0", "DepthMsPs", on)) {
                m_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &m_depthMsPs);
                b->Release();
            }
        }

        if (!m_fullScreenVs || !m_copyPs || !m_depthPs || !m_depthMsPs) {
            return false;
        }

        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = GxEffects::kMaxConstantRows * 16;
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(m_device->CreateBuffer(&cb, nullptr, &m_constants))) {
            return false;
        }

        cb.ByteWidth = GxEffects::kMaxPushBytes;

        if (FAILED(m_device->CreateBuffer(&cb, nullptr, &m_push))) {
            return false;
        }

        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        m_device->CreateRasterizerState(&rd, &m_raster);

        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        m_device->CreateDepthStencilState(&dd, &m_noDepth);

        // an empty slot reads (0, 0, 0, 1), as D3D9 reads a stage with no texture
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        const void* level[1] = { black };
        m_empty2d = Tex(CreateFilled(1, 1, 1, GxEffects::Format::RGBA32F, level));
        m_empty3d = Tex(CreateVolume(1, 1, 1, 1, GxEffects::Format::RGBA32F, level));
        return m_raster && m_noDepth && m_empty2d && m_empty3d;
    }

    std::string m_exeFolder;

    // --- textures -----------------------------------------------------------------------------------------------

    D3dTexture* MakeTexture2D(uint32_t width, uint32_t height, uint32_t levels, GxEffects::Format format, bool target, bool storage, const D3D11_SUBRESOURCE_DATA* initial) {
        if (!width || !height) {
            return nullptr;
        }

        D3D11_TEXTURE2D_DESC d = {};
        d.Width = width;
        d.Height = height;
        d.MipLevels = levels;
        d.ArraySize = 1;
        d.Format = DxgiOf(format);
        d.SampleDesc.Count = 1;
        d.Usage = initial && !target && !storage ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (target || storage ? D3D11_BIND_RENDER_TARGET : 0) | (storage ? D3D11_BIND_UNORDERED_ACCESS : 0);

        D3dTexture* t = new D3dTexture();
        t->width = width;
        t->height = height;
        t->levels = levels;
        t->format = format;
        t->dxgi = d.Format;

        bool ok = SUCCEEDED(m_device->CreateTexture2D(&d, initial, &t->tex2d)) && SUCCEEDED(m_device->CreateShaderResourceView(t->tex2d, nullptr, &t->srv));

        if (ok && (target || storage)) {
            ok = SUCCEEDED(m_device->CreateRenderTargetView(t->tex2d, nullptr, &t->rtv));
        }

        if (ok && storage) {
            ok = SUCCEEDED(m_device->CreateUnorderedAccessView(t->tex2d, nullptr, &t->uav));
        }

        if (!ok) {
            D3d11Log("GxEffects: texture %ux%u format %u could not be made", width, height, static_cast<uint32_t>(format));
            DestroyTexture(t);
            return nullptr;
        }

        // contents start as zero
        if (!initial) {
            const float zero[4] = {};

            if (t->rtv) {
                m_context->ClearRenderTargetView(t->rtv, zero);
            }
        }

        return t;
    }

    GxEffects::Texture* CreateTexture(uint32_t width, uint32_t height, GxEffects::Format format, bool target) override {
        return Out(MakeTexture2D(width, height, 1, format, target, false, nullptr));
    }

    GxEffects::Texture* CreateStorage(uint32_t width, uint32_t height, GxEffects::Format format) override {
        if (!CanStore(format)) {
            return nullptr;
        }

        return Out(MakeTexture2D(width, height, 1, format, true, true, nullptr));
    }

    GxEffects::Texture* CreateFilled(uint32_t width, uint32_t height, uint32_t levels, GxEffects::Format format, const void* const* levelTexels) override {
        if (!levels || levels > 16 || !levelTexels) {
            return nullptr;
        }

        D3D11_SUBRESOURCE_DATA data[16] = {};
        const uint32_t bytes = TexelBytes(format);

        for (uint32_t i = 0; i < levels; i++) {
            const uint32_t w = width >> i ? width >> i : 1;
            data[i].pSysMem = levelTexels[i];
            data[i].SysMemPitch = w * bytes;
        }

        return Out(MakeTexture2D(width, height, levels, format, false, false, data));
    }

    GxEffects::Texture* CreateVolume(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, GxEffects::Format format, const void* const* levelTexels) override {
        if (!levels || levels > 16 || !levelTexels || !width || !height || !depth) {
            return nullptr;
        }

        D3D11_SUBRESOURCE_DATA data[16] = {};
        const uint32_t bytes = TexelBytes(format);

        for (uint32_t i = 0; i < levels; i++) {
            const uint32_t w = width >> i ? width >> i : 1;
            const uint32_t h = height >> i ? height >> i : 1;
            data[i].pSysMem = levelTexels[i];
            data[i].SysMemPitch = w * bytes;
            data[i].SysMemSlicePitch = w * h * bytes;
        }

        D3D11_TEXTURE3D_DESC d = {};
        d.Width = width;
        d.Height = height;
        d.Depth = depth;
        d.MipLevels = levels;
        d.Format = DxgiOf(format);
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3dTexture* t = new D3dTexture();
        t->width = width;
        t->height = height;
        t->depth = depth;
        t->levels = levels;
        t->format = format;
        t->dxgi = d.Format;

        if (FAILED(m_device->CreateTexture3D(&d, data, &t->tex3d)) || FAILED(m_device->CreateShaderResourceView(t->tex3d, nullptr, &t->srv))) {
            DestroyTexture(t);
            return nullptr;
        }

        return Out(t);
    }

    static void DestroyTexture(D3dTexture*& t) {
        if (!t) {
            return;
        }

        Rel(t->uav);
        Rel(t->rtv);
        Rel(t->srv);
        Rel(t->tex2d);
        Rel(t->tex3d);
        delete t;
        t = nullptr;
    }

    void Destroy(GxEffects::Texture* texture) override {
        // the runtime keeps a resource alive while the GPU still uses it
        D3dTexture* t = Tex(texture);
        DestroyTexture(t);
    }

    void Size(const GxEffects::Texture* texture, uint32_t& width, uint32_t& height) override {
        const D3dTexture* t = Tex(texture);
        width = t ? t->width : 0;
        height = t ? t->height : 0;
    }

    bool Upload(GxEffects::Texture* texture, const void* texels) override {
        D3dTexture* t = Tex(texture);

        if (!t || !texels) {
            return false;
        }

        const uint32_t bytes = TexelBytes(t->format);

        if (t->tex3d) {
            return false;   // volumes are filled once, when they are made
        }

        m_context->UpdateSubresource(t->tex2d, 0, nullptr, texels, t->width * bytes, 0);
        return true;
    }

    // --- programs -------------------------------------------------------------------------------------------------

    GxEffects::Program* CreateProgram(const uint32_t* spirv, size_t bytes, const char* name) override {
        if (!spirv || bytes < 20) {
            return nullptr;
        }

        const uint64_t key = Fnv(spirv, bytes, Fnv(&kProgramCacheVersion, sizeof(kProgramCacheVersion)));
        char file[MAX_PATH];
        snprintf(file, sizeof(file), "%s\\%016llx.bin", m_cacheFolder.c_str(), static_cast<unsigned long long>(key));

        D3dProgram* program = new D3dProgram();
        program->name = name ? name : "";

        std::vector<uint8_t> code;
        struct Header {
            uint32_t magic;
            uint32_t stage;
            uint32_t pushBytes;
            uint32_t volumeMask;
            uint32_t size;
        } header = {};

        bool cached = false;

        if (FILE* f = fopen(file, "rb")) {
            if (fread(&header, sizeof(header), 1, f) == 1 && header.magic == 0x58454433u && header.size > 0 && header.size < (64u << 20)) {
                code.resize(header.size);
                cached = fread(code.data(), 1, header.size, f) == header.size;
            }

            fclose(f);
        }

        if (!cached) {
            D3d11SpirvToHlsl::Result r = D3d11SpirvToHlsl::Convert(spirv, bytes / 4);

            if (!r.ok) {
                D3d11Log("GxEffects: %s: SPIR-V to HLSL failed: %s", program->name.c_str(), r.error.c_str());
                delete program;
                return nullptr;
            }

            const char* profile = r.stage == D3d11SpirvToHlsl::Stage::Vertex ? "vs_5_0" : (r.stage == D3d11SpirvToHlsl::Stage::Compute ? "cs_5_0" : "ps_5_0");
            ID3DBlob* blob = Compile(r.hlsl.c_str(), profile, program->name.c_str());

            if (!blob) {
                delete program;
                return nullptr;
            }

            code.assign(static_cast<const uint8_t*>(blob->GetBufferPointer()), static_cast<const uint8_t*>(blob->GetBufferPointer()) + blob->GetBufferSize());
            blob->Release();

            header.magic = 0x58454433u;
            header.stage = static_cast<uint32_t>(r.stage);
            header.pushBytes = r.pushBytes;
            header.volumeMask = r.volumeMask;
            header.size = static_cast<uint32_t>(code.size());

            CreateDirectoryA((m_exeFolder + "\\Cache").c_str(), nullptr);
            CreateDirectoryA(m_cacheFolder.c_str(), nullptr);

            if (FILE* f = fopen(file, "wb")) {
                fwrite(&header, sizeof(header), 1, f);
                fwrite(code.data(), 1, code.size(), f);
                fclose(f);
            }
        }

        program->stage = static_cast<D3d11SpirvToHlsl::Stage>(header.stage);
        program->pushBytes = header.pushBytes;
        program->volumeMask = header.volumeMask;

        HRESULT hr = E_FAIL;

        switch (program->stage) {
            case D3d11SpirvToHlsl::Stage::Vertex:
                hr = m_device->CreateVertexShader(code.data(), code.size(), nullptr, &program->vs);
                break;
            case D3d11SpirvToHlsl::Stage::Fragment:
                hr = m_device->CreatePixelShader(code.data(), code.size(), nullptr, &program->ps);
                break;
            case D3d11SpirvToHlsl::Stage::Compute:
                hr = m_device->CreateComputeShader(code.data(), code.size(), nullptr, &program->cs);
                break;
        }

        if (FAILED(hr)) {
            D3d11Log("GxEffects: %s: shader creation failed 0x%08X", program->name.c_str(), static_cast<uint32_t>(hr));
            delete program;
            return nullptr;
        }

        return reinterpret_cast<GxEffects::Program*>(program);
    }

    bool CanBlend(GxEffects::Format format) override {
        UINT support = 0;
        return SUCCEEDED(m_device->CheckFormatSupport(DxgiOf(format), &support)) && (support & D3D11_FORMAT_SUPPORT_BLENDABLE) && (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET);
    }

    bool CanStore(GxEffects::Format format) override {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s = {};
        s.InFormat = DxgiOf(format);
        const UINT need = D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
        return SUCCEEDED(m_device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &s, sizeof(s))) && (s.OutFormatSupport2 & need) == need;
    }

    // --- state ----------------------------------------------------------------------------------------------------

    ID3D11SamplerState* SamplerFor(bool linear, bool wrap, bool mips) {
        const uint32_t key = (linear ? 1u : 0u) | (wrap ? 2u : 0u) | (mips ? 4u : 0u);
        auto found = m_samplers.find(key);

        if (found != m_samplers.end()) {
            return found->second;
        }

        D3D11_SAMPLER_DESC d = {};
        d.Filter = linear ? (mips ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT) : D3D11_FILTER_MIN_MAG_MIP_POINT;
        d.AddressU = d.AddressV = d.AddressW = wrap ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        d.MaxAnisotropy = 1;
        d.ComparisonFunc = D3D11_COMPARISON_NEVER;
        d.MinLOD = 0.0f;
        d.MaxLOD = mips ? D3D11_FLOAT32_MAX : 0.0f;

        ID3D11SamplerState* state = nullptr;
        m_device->CreateSamplerState(&d, &state);
        m_samplers[key] = state;
        return state;
    }

    ID3D11BlendState* BlendFor(GxEffects::Blend blend, uint32_t mask) {
        const uint32_t key = static_cast<uint32_t>(blend) | ((mask & 0xF) << 8);
        auto found = m_blends.find(key);

        if (found != m_blends.end()) {
            return found->second;
        }

        D3D11_BLEND_DESC d = {};
        D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[0];
        rt.RenderTargetWriteMask = mask & 0xF;
        rt.BlendEnable = blend != GxEffects::Blend::Opaque;
        rt.BlendOp = blend == GxEffects::Blend::Max ? D3D11_BLEND_OP_MAX : D3D11_BLEND_OP_ADD;
        rt.BlendOpAlpha = rt.BlendOp;
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ONE;

        switch (blend) {
            case GxEffects::Blend::Alpha:
                rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
                rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
                break;
            case GxEffects::Blend::Premultiplied:
                rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
                rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
                break;
            case GxEffects::Blend::Multiply:
                rt.SrcBlend = D3D11_BLEND_DEST_COLOR;
                rt.SrcBlendAlpha = D3D11_BLEND_DEST_ALPHA;
                rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
                break;
            default:
                break;
        }

        ID3D11BlendState* state = nullptr;
        m_device->CreateBlendState(&d, &state);
        m_blends[key] = state;
        return state;
    }

    void WriteConstants(ID3D11Buffer* buffer, uint32_t capacity, const void* data, uint32_t bytes) {
        D3D11_MAPPED_SUBRESOURCE mapped;

        if (SUCCEEDED(m_context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memset(mapped.pData, 0, capacity);

            if (data && bytes) {
                memcpy(mapped.pData, data, bytes < capacity ? bytes : capacity);
            }

            m_context->Unmap(buffer, 0);
        }
    }

    // The view of what a binding names: our texture, or an engine texture, else the empty one.
    ID3D11ShaderResourceView* ViewFor(const GxEffects::Binding& b, bool volume) {
        if (b.texture) {
            return Tex(b.texture)->srv;
        }

        if (b.engine && b.engine->m_target != GxTex_CubeMap && !volume) {
            CGxDeviceD3d11::TexData* data = m_dev->ITexEnsure(b.engine);

            if (data && data->m_srv) {
                return data->m_srv;
            }
        }

        return volume ? m_empty3d->srv : m_empty2d->srv;
    }

    void BindTextures(bool compute, const GxEffects::Binding* bindings, uint32_t volumeMask, const GxEffects::Texture* avoid) {
        ID3D11ShaderResourceView* srvs[GxEffects::kMaxTextures];
        ID3D11SamplerState* samplers[GxEffects::kMaxTextures];

        for (uint32_t i = 0; i < GxEffects::kMaxTextures; i++) {
            const bool volume = (volumeMask >> i) & 1u;
            srvs[i] = ViewFor(bindings[i], volume);

            // a texture being drawn into cannot be sampled
            if (avoid && bindings[i].texture == avoid) {
                srvs[i] = volume ? m_empty3d->srv : m_empty2d->srv;
            }

            samplers[i] = SamplerFor(bindings[i].linear, bindings[i].wrap, bindings[i].mips);
        }

        if (compute) {
            m_context->CSSetShaderResources(0, GxEffects::kMaxTextures, srvs);
            m_context->CSSetSamplers(0, GxEffects::kMaxTextures, samplers);
        } else {
            m_context->PSSetShaderResources(0, GxEffects::kMaxTextures, srvs);
            m_context->PSSetSamplers(0, GxEffects::kMaxTextures, samplers);
        }
    }

    void UnbindTextures(bool compute) {
        ID3D11ShaderResourceView* none[GxEffects::kMaxTextures] = {};

        if (compute) {
            m_context->CSSetShaderResources(0, GxEffects::kMaxTextures, none);
        } else {
            m_context->PSSetShaderResources(0, GxEffects::kMaxTextures, none);
        }
    }

    // The world's current colour target: a texture the engine renders to, or the back buffer (the multisampled one when
    // there is one).
    bool CurrentTarget(ID3D11RenderTargetView*& rtv, uint32_t& width, uint32_t& height, CGxDeviceD3d11::TexData** texture = nullptr) {
        CGxDevice::TextureTarget& color = m_dev->m_textureTarget[GxBuffers_Color];
        CGxDevice::TextureTarget& depth = m_dev->m_textureTarget[GxBuffers_Depth];
        CGxDeviceD3d11::TexData* colorData = color.m_texture ? static_cast<CGxDeviceD3d11::TexData*>(color.m_apiSpecific) : nullptr;
        rtv = nullptr;

        if (texture) {
            *texture = colorData;
        }

        if (colorData) {
            const uint32_t face = color.m_texture->m_target == GxTex_CubeMap ? color.m_plane : 0;
            rtv = colorData->m_rtv[face < 6 ? face : 0];
            width = colorData->m_width;
            height = colorData->m_height;
        } else if (!depth.m_texture) {
            rtv = m_dev->m_msaaColorRtv ? m_dev->m_msaaColorRtv : m_dev->m_backBufferRtv;
            width = m_dev->m_backBufferWidth;
            height = m_dev->m_backBufferHeight;
        }

        return rtv != nullptr;
    }

    void Finish() {
        // the engine's cached bindings are stale now
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_dev->IInvalidateStateCaches();
    }

    // --- draws ----------------------------------------------------------------------------------------------------

    void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = static_cast<float>(x);
        vp.TopLeftY = static_cast<float>(y);
        vp.Width = static_cast<float>(width);
        vp.Height = static_cast<float>(height);
        vp.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &vp);
        m_context->RSSetScissorRects(0, nullptr);
    }

    void SetFixedState(ID3D11BlendState* blend) {
        const float factor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        m_context->OMSetBlendState(blend, factor, 0xFFFFFFFF);
        m_context->OMSetDepthStencilState(m_noDepth, 0);
        m_context->RSSetState(m_raster);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->GSSetShader(nullptr, nullptr, 0);
    }

    bool Draw(const GxEffects::Pass& pass) override {
        D3dProgram* ps = Prog(pass.program);
        D3dProgram* vs = Prog(pass.vertexProgram);

        if (!ps || ps->stage != D3d11SpirvToHlsl::Stage::Fragment || (pass.points && (!vs || vs->stage != D3d11SpirvToHlsl::Stage::Vertex))) {
            return false;
        }

        ID3D11RenderTargetView* rtv = nullptr;
        uint32_t targetWidth = 0;
        uint32_t targetHeight = 0;

        if (pass.target) {
            D3dTexture* t = Tex(pass.target);
            rtv = t->rtv;
            targetWidth = t->width;
            targetHeight = t->height;
        } else {
            CurrentTarget(rtv, targetWidth, targetHeight);
        }

        if (!rtv) {
            return false;
        }

        // drop the engine's own texture bindings first: one of them may be what is about to be drawn into
        UnbindTextures(false);
        m_context->OMSetRenderTargets(1, &rtv, nullptr);

        if (pass.clear) {
            m_context->ClearRenderTargetView(rtv, pass.clearColour);
        }

        const uint32_t vw = pass.width ? pass.width : targetWidth;
        const uint32_t vh = pass.width ? pass.height : targetHeight;
        SetViewport(static_cast<uint32_t>(pass.x), static_cast<uint32_t>(pass.y), vw, vh);
        SetFixedState(BlendFor(pass.blend, pass.writeMask));

        if (pass.points) {
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            m_context->VSSetShader(vs->vs, nullptr, 0);
        } else {
            m_context->VSSetShader(m_fullScreenVs, nullptr, 0);
        }

        WriteConstants(m_constants, GxEffects::kMaxConstantRows * 16, pass.constants, pass.constantRows * 16);
        m_context->VSSetConstantBuffers(kConstantsRegister, 1, &m_constants);
        m_context->PSSetConstantBuffers(kConstantsRegister, 1, &m_constants);
        m_context->PSSetShader(ps->ps, nullptr, 0);

        // a vertex program of a point pass reads the same textures
        BindTextures(false, pass.textures, ps->volumeMask, pass.target);

        if (pass.points) {
            ID3D11ShaderResourceView* srvs[GxEffects::kMaxTextures];
            ID3D11SamplerState* samplers[GxEffects::kMaxTextures];
            const uint32_t mask = vs->volumeMask | ps->volumeMask;

            for (uint32_t i = 0; i < GxEffects::kMaxTextures; i++) {
                const bool volume = (mask >> i) & 1u;
                srvs[i] = pass.target && pass.textures[i].texture == pass.target ? (volume ? m_empty3d->srv : m_empty2d->srv) : ViewFor(pass.textures[i], volume);
                samplers[i] = SamplerFor(pass.textures[i].linear, pass.textures[i].wrap, pass.textures[i].mips);
            }

            m_context->VSSetShaderResources(0, GxEffects::kMaxTextures, srvs);
            m_context->VSSetSamplers(0, GxEffects::kMaxTextures, samplers);
            m_context->Draw(pass.points, 0);

            ID3D11ShaderResourceView* none[GxEffects::kMaxTextures] = {};
            m_context->VSSetShaderResources(0, GxEffects::kMaxTextures, none);
        } else {
            m_context->Draw(3, 0);
        }

        UnbindTextures(false);
        Finish();
        return true;
    }

    bool Dispatch(const GxEffects::Compute& compute) override {
        D3dProgram* cs = Prog(compute.program);

        if (!cs || cs->stage != D3d11SpirvToHlsl::Stage::Compute) {
            return false;
        }

        // nothing may still be bound for output or as an input of the engine's draws
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        UnbindTextures(false);

        ID3D11UnorderedAccessView* uavs[GxEffects::kMaxImages] = {};

        for (uint32_t i = 0; i < GxEffects::kMaxImages; i++) {
            uavs[i] = compute.images[i] ? Tex(compute.images[i])->uav : nullptr;
        }

        BindTextures(true, compute.textures, cs->volumeMask, nullptr);
        WriteConstants(m_constants, GxEffects::kMaxConstantRows * 16, compute.constants, compute.constantRows * 16);
        WriteConstants(m_push, GxEffects::kMaxPushBytes, compute.push, compute.pushBytes);
        m_context->CSSetConstantBuffers(kConstantsRegister, 1, &m_constants);
        m_context->CSSetConstantBuffers(kPushRegister, 1, &m_push);
        m_context->CSSetShader(cs->cs, nullptr, 0);
        m_context->CSSetUnorderedAccessViews(0, GxEffects::kMaxImages, uavs, nullptr);
        m_context->Dispatch(compute.groupsX, compute.groupsY, compute.groupsZ);

        ID3D11UnorderedAccessView* noUavs[GxEffects::kMaxImages] = {};
        m_context->CSSetUnorderedAccessViews(0, GxEffects::kMaxImages, noUavs, nullptr);
        UnbindTextures(true);
        m_context->CSSetShader(nullptr, nullptr, 0);
        Finish();
        return true;
    }

    // --- the world's target ---------------------------------------------------------------------------------------

    bool TargetSize(uint32_t& width, uint32_t& height) override {
        ID3D11RenderTargetView* rtv = nullptr;
        return CurrentTarget(rtv, width, height);
    }

    bool CopyColor(GxEffects::Texture* destination, int32_t x, int32_t y, uint32_t width, uint32_t height, bool linear) override {
        D3dTexture* dst = Tex(destination);

        if (!dst || !dst->rtv) {
            return false;
        }

        ID3D11RenderTargetView* rtv = nullptr;
        uint32_t sourceWidth = 0;
        uint32_t sourceHeight = 0;
        CGxDeviceD3d11::TexData* colorData = nullptr;

        if (!CurrentTarget(rtv, sourceWidth, sourceHeight, &colorData)) {
            return false;
        }

        ID3D11ShaderResourceView* source = nullptr;

        if (colorData) {
            source = colorData->m_srv;
        } else {
            m_dev->IResolvedBackBuffer();

            if (!m_dev->m_backBufferSrv) {
                m_device->CreateShaderResourceView(m_dev->m_backBuffer, nullptr, &m_dev->m_backBufferSrv);
            }

            source = m_dev->m_backBufferSrv;
        }

        if (!source) {
            return false;
        }

        if (!width) {
            x = 0;
            y = 0;
            width = sourceWidth;
            height = sourceHeight;
        }

        const float rect[4] = {
            static_cast<float>(x) / sourceWidth,
            static_cast<float>(y) / sourceHeight,
            static_cast<float>(width) / sourceWidth,
            static_cast<float>(height) / sourceHeight,
        };

        UnbindTextures(false);
        m_context->OMSetRenderTargets(1, &dst->rtv, nullptr);
        SetViewport(0, 0, dst->width, dst->height);
        SetFixedState(BlendFor(GxEffects::Blend::Opaque, 0xF));
        m_context->VSSetShader(m_fullScreenVs, nullptr, 0);
        m_context->PSSetShader(m_copyPs, nullptr, 0);
        WriteConstants(m_constants, GxEffects::kMaxConstantRows * 16, rect, sizeof(rect));
        m_context->PSSetConstantBuffers(kConstantsRegister, 1, &m_constants);
        ID3D11SamplerState* sampler = SamplerFor(linear, false, false);
        m_context->PSSetShaderResources(0, 1, &source);
        m_context->PSSetSamplers(0, 1, &sampler);
        m_context->Draw(3, 0);
        UnbindTextures(false);
        Finish();
        return true;
    }

    bool CopyDepth(GxEffects::Texture* destination) override {
        D3dTexture* dst = Tex(destination);

        if (!dst || !dst->rtv || dst->format != GxEffects::Format::Depth) {
            return false;
        }

        // only the default depth buffer can be read: a render target texture's depth is not the world's
        if (m_dev->m_textureTarget[GxBuffers_Color].m_texture || m_dev->m_textureTarget[GxBuffers_Depth].m_texture || !m_dev->m_defDepth.m_srv) {
            return false;
        }

        if (dst->width != m_dev->m_defDepth.m_width || dst->height != m_dev->m_defDepth.m_height) {
            return false;
        }

        UnbindTextures(false);
        m_context->OMSetRenderTargets(1, &dst->rtv, nullptr);
        SetViewport(0, 0, dst->width, dst->height);
        SetFixedState(BlendFor(GxEffects::Blend::Opaque, 0xF));
        m_context->VSSetShader(m_fullScreenVs, nullptr, 0);
        m_context->PSSetShader(m_dev->m_sampleCount > 1 ? m_depthMsPs : m_depthPs, nullptr, 0);
        ID3D11ShaderResourceView* source = m_dev->m_defDepth.m_srv;
        m_context->PSSetShaderResources(0, 1, &source);
        m_context->Draw(3, 0);
        UnbindTextures(false);
        Finish();
        return true;
    }

    // --- read-back ------------------------------------------------------------------------------------------------

    D3dReadback* Stage(D3dTexture* t, uint32_t x, uint32_t y, uint32_t width, uint32_t height, bool withQuery) {
        if (!t || !t->tex2d || !width || !height || x + width > t->width || y + height > t->height) {
            return nullptr;
        }

        D3D11_TEXTURE2D_DESC d = {};
        d.Width = width;
        d.Height = height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = t->dxgi;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        D3dReadback* r = new D3dReadback();
        r->width = width;
        r->height = height;
        r->texelBytes = TexelBytes(t->format);

        if (FAILED(m_device->CreateTexture2D(&d, nullptr, &r->staging))) {
            delete r;
            return nullptr;
        }

        const D3D11_BOX box = { x, y, 0, x + width, y + height, 1 };
        m_context->CopySubresourceRegion(r->staging, 0, 0, 0, 0, t->tex2d, 0, &box);

        if (withQuery) {
            D3D11_QUERY_DESC q = {};
            q.Query = D3D11_QUERY_EVENT;
            m_device->CreateQuery(&q, &r->query);

            if (r->query) {
                m_context->End(r->query);
            }
        }

        return r;
    }

    bool Fetch(D3dReadback* r, void* out) {
        D3D11_MAPPED_SUBRESOURCE mapped;

        if (FAILED(m_context->Map(r->staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }

        for (uint32_t row = 0; row < r->height; row++) {
            memcpy(static_cast<uint8_t*>(out) + static_cast<size_t>(row) * r->width * r->texelBytes, static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch, static_cast<size_t>(r->width) * r->texelBytes);
        }

        m_context->Unmap(r->staging, 0);
        return true;
    }

    static void Free(D3dReadback* r) {
        if (r) {
            Rel(r->query);
            Rel(r->staging);
            delete r;
        }
    }

    bool Read(GxEffects::Texture* texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void* out) override {
        D3dReadback* r = Stage(Tex(texture), x, y, width, height, false);

        if (!r) {
            return false;
        }

        const bool ok = Fetch(r, out);
        Free(r);
        return ok;
    }

    GxEffects::Readback* BeginRead(GxEffects::Texture* texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height) override {
        return reinterpret_cast<GxEffects::Readback*>(Stage(Tex(texture), x, y, width, height, true));
    }

    bool PollRead(GxEffects::Readback* read, void* out) override {
        D3dReadback* r = reinterpret_cast<D3dReadback*>(read);

        if (!r) {
            return false;
        }

        BOOL done = FALSE;

        if (r->query && m_context->GetData(r->query, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            return false;
        }

        const bool ok = Fetch(r, out);
        Free(r);
        return ok;
    }

    void CancelRead(GxEffects::Readback* read) override {
        Free(reinterpret_cast<D3dReadback*>(read));
    }

    // --- the engine's shaders -------------------------------------------------------------------------------------

    bool ShaderKnown(const void* code, uint32_t bytes) override {
        // the D3D11 backend compiles any shader model 2 or 3 code the engine holds
        return code && bytes >= 4;
    }

    void ShaderFolder(char* folder, size_t size) override {
        snprintf(folder, size, "%s", m_exeFolder.c_str());
    }
};

D3d11Effects g_effects;

CGxDeviceD3d11* LiveDevice() {
    if (!g_theGxDevicePtr || g_theGxDevicePtr->m_api != GxApi_D3d11) {
        return nullptr;
    }

    CGxDeviceD3d11* dev = static_cast<CGxDeviceD3d11*>(g_theGxDevicePtr);
    return dev->m_context && dev->m_d3dDevice && dev->m_d3dContext ? dev : nullptr;
}

}  // namespace

GxEffects::Device* GxEffects::D3d11Device() {
    CGxDeviceD3d11* dev = LiveDevice();

    if (!dev) {
        if (g_effects.m_dev) {
            g_effects.Reset();
        }

        return nullptr;
    }

    return g_effects.Attach(dev) ? &g_effects : nullptr;
}

bool D3d11EffectsDrawTexture(uint32_t stage, ID3D11ShaderResourceView*& view, ID3D11SamplerState*& sampler) {
    const GxEffects::DrawOverride& o = GxEffects::DrawOverrideFor(stage);

    if (!o.texture || !g_effects.m_dev) {
        return false;
    }

    const D3dTexture* t = Tex(o.texture);

    if (!t->srv) {
        return false;
    }

    view = t->srv;
    sampler = g_effects.SamplerFor(o.linear, o.wrap, o.mips);
    return true;
}
