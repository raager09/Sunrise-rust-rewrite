/**
 * Minecraft's HUD, hand and screens over Destiny's picture. Minecraft renders them into the
 * shared overlay slots (premultiplied alpha); this uploads the newest one and draws it full
 * screen, with Minecraft's crosshair in an inverting pass of its own and the screen cursor on
 * top. Ported from SkyCraft's Skyrim compositor (MIT, Copyright (c) 2026 chasmlol).
 */

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include "../../../core/logging/log.h"
#include "internal.h"

namespace sunrise::client::hooks::skycraft::overlay {
namespace {

constexpr char kShader[] = R"(
cbuffer Params : register(b0) { float2 cursor; float2 viewport; float cursorOn; float flipY; float2 pad; float4 invertRect; };
Texture2D overlay : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
bool InInvertRect(float2 p) { return all(p >= invertRect.xy) && all(p < invertRect.zw); }
float4 Overlay(float2 uv) {
    if (flipY > 0.5) uv.y = 1 - uv.y;
    return overlay.Sample(samp, uv);
}
float4 PSInvert(VSOut i) : SV_Target {
    if (!InInvertRect(i.pos.xy)) discard;
    return float4(Overlay(i.uv).rgb, 0);
}
float4 PSMain(VSOut i) : SV_Target {
    if (InInvertRect(i.pos.xy)) return 0;
    float4 c = Overlay(i.uv);
    if (cursorOn > 0.5) {
        float2 p = i.pos.xy - cursor;
        if (p.x >= 0 && p.y >= 0 && p.y < 18 && p.x <= p.y * 0.6) {
            bool edge = p.x < 1.5 || p.x > p.y * 0.6 - 1.5 || p.y > 16.5;
            c = float4(edge ? float3(0, 0, 0) : float3(1, 1, 1), 1);
        }
    }
    return c;
}
)";

struct alignas(16) Params {
    float cursor[2];
    float viewport[2];
    float cursorOn;
    float flipY;
    float pad[2];
    /** Back-buffer pixels x0, y0, x1, y1 of Minecraft's crosshair and attack indicator. */
    float invertRect[4];
};
static_assert(sizeof(Params) % 16 == 0);

/** Minecraft's crosshair box around the centre, and its attack indicator below, in GUI pixels. */
constexpr float kCrosshairHalf = 12.0F;
constexpr float kIndicatorBottom = 28.0F;

struct Resources {
    ID3D11Device* owner{};
    ID3D11VertexShader* vertexShader{};
    ID3D11PixelShader* pixelShader{};
    ID3D11PixelShader* invertShader{};
    ID3D11BlendState* blend{};
    ID3D11BlendState* invertBlend{};
    ID3D11SamplerState* sampler{};
    ID3D11RasterizerState* rasterizer{};
    ID3D11DepthStencilState* depth{};
    ID3D11Buffer* params{};
    ID3DDeviceContextState* contextState{};
    ID3D11Texture2D* texture{};
    ID3D11ShaderResourceView* view{};
    UINT width{};
    UINT height{};
    bool haveFrame{};
    bool flipY{true};
    bool failed{};
};

Resources g_resources{};

template <typename Interface> void release_com(Interface*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

[[nodiscard]] bool compile(const char* entry, const char* target, ID3DBlob** output) noexcept {
    ID3DBlob* errors = nullptr;
    const HRESULT result = D3DCompile(kShader,
                                      sizeof(kShader) - 1,
                                      "skycraft_overlay",
                                      nullptr,
                                      nullptr,
                                      entry,
                                      target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                      0,
                                      output,
                                      &errors);
    if (FAILED(result)) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::error,
                          "ev=skycraft stage=overlay_shader entry=%s error=%s",
                          entry,
                          errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer())
                                            : "unknown");
    }
    release_com(errors);
    return SUCCEEDED(result);
}

[[nodiscard]] bool create_context_state(ID3D11Device* device) noexcept {
    ID3D11Device1* device1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1)))
        || device1 == nullptr) {
        release_com(device1);
        return false;
    }
    const D3D_FEATURE_LEVEL requested = device->GetFeatureLevel();
    D3D_FEATURE_LEVEL selected{};
    const HRESULT result = device1->CreateDeviceContextState(0,
                                                             &requested,
                                                             1,
                                                             D3D11_SDK_VERSION,
                                                             __uuidof(ID3D11Device),
                                                             &selected,
                                                             &g_resources.contextState);
    release_com(device1);
    return SUCCEEDED(result) && g_resources.contextState != nullptr;
}

[[nodiscard]] bool initialize(ID3D11Device* device) noexcept {
    if (g_resources.owner == device) {
        return !g_resources.failed;
    }
    release();
    g_resources.owner = device;
    g_resources.failed = true;

    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob = nullptr;
    ID3DBlob* invertBlob = nullptr;
    const bool compiled = compile("VSMain", "vs_5_0", &vertexBlob)
                          && compile("PSMain", "ps_5_0", &pixelBlob)
                          && compile("PSInvert", "ps_5_0", &invertBlob);
    if (compiled) {
        (void)device->CreateVertexShader(vertexBlob->GetBufferPointer(),
                                         vertexBlob->GetBufferSize(),
                                         nullptr,
                                         &g_resources.vertexShader);
        (void)device->CreatePixelShader(pixelBlob->GetBufferPointer(),
                                        pixelBlob->GetBufferSize(),
                                        nullptr,
                                        &g_resources.pixelShader);
        (void)device->CreatePixelShader(invertBlob->GetBufferPointer(),
                                        invertBlob->GetBufferSize(),
                                        nullptr,
                                        &g_resources.invertShader);
    }
    release_com(vertexBlob);
    release_com(pixelBlob);
    release_com(invertBlob);
    if (!compiled) {
        return false;
    }

    D3D11_BLEND_DESC blend{};
    D3D11_RENDER_TARGET_BLEND_DESC& target = blend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_ONE;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    (void)device->CreateBlendState(&blend, &g_resources.blend);
    // Minecraft's inverting blend for its crosshair; the back buffer's alpha is left alone.
    target.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    target.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    target.SrcBlendAlpha = D3D11_BLEND_ZERO;
    target.DestBlendAlpha = D3D11_BLEND_ONE;
    (void)device->CreateBlendState(&blend, &g_resources.invertBlend);

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    (void)device->CreateSamplerState(&sampler, &g_resources.sampler);

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    (void)device->CreateRasterizerState(&rasterizer, &g_resources.rasterizer);

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    (void)device->CreateDepthStencilState(&depth, &g_resources.depth);

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Params);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    (void)device->CreateBuffer(&buffer, nullptr, &g_resources.params);

    g_resources.failed = g_resources.vertexShader == nullptr || g_resources.pixelShader == nullptr
                         || g_resources.invertShader == nullptr || g_resources.blend == nullptr
                         || g_resources.invertBlend == nullptr || g_resources.sampler == nullptr
                         || g_resources.rasterizer == nullptr || g_resources.depth == nullptr
                         || g_resources.params == nullptr || !create_context_state(device);
    core::log::write(core::log::Channel::client,
                     g_resources.failed ? core::log::Level::error : core::log::Level::info,
                     g_resources.failed ? "ev=skycraft stage=overlay_init result=fail"
                                        : "ev=skycraft stage=overlay_init result=ok");
    return !g_resources.failed;
}

[[nodiscard]] bool ensure_texture(ID3D11Device* device, UINT width, UINT height) noexcept {
    if (g_resources.texture != nullptr && g_resources.width == width
        && g_resources.height == height) {
        return true;
    }
    release_com(g_resources.view);
    release_com(g_resources.texture);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &g_resources.texture))
        || FAILED(
            device->CreateShaderResourceView(g_resources.texture, nullptr, &g_resources.view))) {
        release_com(g_resources.texture);
        return false;
    }
    g_resources.width = width;
    g_resources.height = height;
    return true;
}

void upload_latest(ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
    Link& link = Link::get();
    if (!link.acquire_overlay_frame()) {
        return;
    }
    const proto::OverlaySlotHdr* header = link.front_header();
    if (header->width == 0 || header->height == 0 || header->width > proto::kMaxOverlayW
        || header->height > proto::kMaxOverlayH) {
        return;
    }
    if (!ensure_texture(device, header->width, header->height)) {
        return;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g_resources.texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    const std::uint8_t* source = link.front_pixels();
    const std::size_t rowBytes = static_cast<std::size_t>(header->width) * 4U;
    auto* destination = static_cast<std::uint8_t*>(mapped.pData);
    if (mapped.RowPitch == rowBytes) {
        std::memcpy(destination, source, rowBytes * header->height);
    } else {
        for (UINT row = 0; row < header->height; ++row) {
            std::memcpy(destination + static_cast<std::size_t>(row) * mapped.RowPitch,
                        source + static_cast<std::size_t>(row) * rowBytes,
                        rowBytes);
        }
    }
    context->Unmap(g_resources.texture, 0);
    g_resources.flipY = (header->flags & 1U) != 0U;
    g_resources.haveFrame = true;
}

[[nodiscard]] bool target_size(ID3D11RenderTargetView* target, UINT& width, UINT& height) noexcept {
    ID3D11Resource* resource = nullptr;
    target->GetResource(&resource);
    ID3D11Texture2D* texture = nullptr;
    if (resource == nullptr
        || FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                           reinterpret_cast<void**>(&texture)))) {
        release_com(resource);
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    release_com(texture);
    release_com(resource);
    width = description.Width;
    height = description.Height;
    return width != 0 && height != 0;
}

void render(ID3D11DeviceContext* context,
            ID3D11RenderTargetView* target,
            UINT width,
            UINT height,
            bool invert,
            const Params& params) noexcept {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g_resources.params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    std::memcpy(mapped.pData, &params, sizeof(params));
    context->Unmap(g_resources.params, 0);

    const D3D11_VIEWPORT viewport{
        0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F};
    constexpr float blendFactor[4]{};
    ID3D11Buffer* constants = g_resources.params;
    ID3D11ShaderResourceView* view = g_resources.view;
    ID3D11SamplerState* sampler = g_resources.sampler;
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(g_resources.blend, blendFactor, 0xFFFFFFFFU);
    context->OMSetDepthStencilState(g_resources.depth, 0);
    context->RSSetState(g_resources.rasterizer);
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->VSSetShader(g_resources.vertexShader, nullptr, 0);
    context->PSSetShader(g_resources.pixelShader, nullptr, 0);
    context->PSSetShaderResources(0, 1, &view);
    context->PSSetSamplers(0, 1, &sampler);
    context->PSSetConstantBuffers(0, 1, &constants);
    context->Draw(3, 0);
    if (invert) {
        context->OMSetBlendState(g_resources.invertBlend, blendFactor, 0xFFFFFFFFU);
        context->PSSetShader(g_resources.invertShader, nullptr, 0);
        context->Draw(3, 0);
    }
}

} // namespace

void draw(ID3D11Device* device,
          ID3D11DeviceContext* context,
          ID3D11RenderTargetView* target) noexcept {
    Shared& state = shared();
    UINT width = 0;
    UINT height = 0;
    if (!target_size(target, width, height)) {
        return;
    }
    state.viewportW.store(static_cast<int>(width), std::memory_order_relaxed);
    state.viewportH.store(static_cast<int>(height), std::memory_order_relaxed);
    if (!initialize(device)) {
        return;
    }
    upload_latest(device, context);
    if (!g_resources.haveFrame || g_resources.width == 0 || g_resources.height == 0) {
        return;
    }

    const float scaleX = static_cast<float>(width) / static_cast<float>(g_resources.width);
    const float scaleY = static_cast<float>(height) / static_cast<float>(g_resources.height);
    const bool screenOpen = state.mcScreenOpen.load(std::memory_order_relaxed);
    const int guiScale = state.guiScale.load(std::memory_order_relaxed);
    const bool invert = state.crosshair.load(std::memory_order_relaxed) && guiScale > 0;
    Params params{};
    params.cursor[0] = static_cast<float>(state.cursorX.load(std::memory_order_relaxed)) * scaleX;
    params.cursor[1] = static_cast<float>(state.cursorY.load(std::memory_order_relaxed)) * scaleY;
    params.viewport[0] = static_cast<float>(width);
    params.viewport[1] = static_cast<float>(height);
    params.cursorOn = screenOpen ? 1.0F : 0.0F;
    params.flipY = g_resources.flipY ? 1.0F : 0.0F;
    if (invert) {
        const float gui = static_cast<float>(guiScale) * scaleX;
        const float centreX = static_cast<float>(width) * 0.5F;
        const float centreY = static_cast<float>(height) * 0.5F;
        params.invertRect[0] = centreX - kCrosshairHalf * gui;
        params.invertRect[1] = centreY - kCrosshairHalf * gui;
        params.invertRect[2] = centreX + kCrosshairHalf * gui;
        params.invertRect[3] = centreY + kIndicatorBottom * gui;
    }

    ID3D11DeviceContext1* context1 = nullptr;
    if (FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                       reinterpret_cast<void**>(&context1)))
        || context1 == nullptr) {
        release_com(context1);
        return;
    }
    ID3DDeviceContextState* previous = nullptr;
    context1->SwapDeviceContextState(g_resources.contextState, &previous);
    if (previous != nullptr) {
        render(context, target, width, height, invert, params);
    }
    ID3DDeviceContextState* privateState = nullptr;
    context1->SwapDeviceContextState(previous, &privateState);
    release_com(privateState);
    release_com(previous);
    release_com(context1);
}

void release() noexcept {
    release_com(g_resources.view);
    release_com(g_resources.texture);
    release_com(g_resources.contextState);
    release_com(g_resources.params);
    release_com(g_resources.depth);
    release_com(g_resources.rasterizer);
    release_com(g_resources.sampler);
    release_com(g_resources.invertBlend);
    release_com(g_resources.blend);
    release_com(g_resources.invertShader);
    release_com(g_resources.pixelShader);
    release_com(g_resources.vertexShader);
    g_resources = Resources{};
}

} // namespace sunrise::client::hooks::skycraft::overlay
