/**
 * Minecraft's placed blocks in Destiny's world. Minecraft builds each 16-block section's mesh
 * with its own block renderer and ships it, with its block atlas, through the render ring. The
 * meshes are moved into Destiny space once when they arrive and drawn every frame with Destiny's
 * camera, so the blocks stay where they were placed.
 *
 * They are drawn at present, after Destiny's frame, against a depth buffer of their own: blocks
 * hide each other, but Destiny's own geometry does not hide them yet.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <unordered_map>
#include <vector>

#include "../../../core/logging/log.h"
#include "internal.h"

namespace sunrise::client::hooks::skycraft::blocks {
namespace {

constexpr char kShader[] = R"(
cbuffer Camera : register(b0)
{
    float4 camera_position;
    float4 camera_right;
    float4 camera_up;
    float4 camera_forward;
    float4 projection;
    float4 depth_and_pass;
};
Texture2D atlas : register(t0);
SamplerState samp : register(s0);
struct VSIn
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    uint light : TEXCOORD1;
    uint flags : TEXCOORD2;
};
struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint light : TEXCOORD1;
    nointerpolation uint flags : TEXCOORD2;
};
VSOut VSMain(VSIn input)
{
    VSOut output;
    const float3 relative = input.position - camera_position.xyz;
    const float depth = dot(relative, camera_forward.xyz);
    const float horizontal = dot(relative, camera_right.xyz) * projection.x;
    const float vertical = dot(relative, camera_up.xyz) * projection.y;
    output.position = float4(horizontal, vertical, depth * depth_and_pass.x - depth_and_pass.y, depth);
    output.uv = input.uv;
    output.color = input.color;
    output.light = input.light;
    output.flags = input.flags;
    return output;
}
float4 PSMain(VSOut input) : SV_Target
{
    const bool translucent = (input.flags & 2u) != 0;
    const bool translucentPass = depth_and_pass.z > 0.5;
    if (translucent != translucentPass) discard;
    float4 texel = atlas.Sample(samp, input.uv) * input.color;
    if (!translucent && texel.a < 0.5) discard;
    // Minecraft's fixed face shading: down, up, north, south, west, east.
    const float shades[7] = { 1.0, 0.5, 1.0, 0.8, 0.8, 0.6, 0.6 };
    const uint face = min((input.flags >> 4) & 7u, 6u);
    const float level = max(float(input.light & 15u), float((input.light >> 8) & 15u)) / 15.0;
    const float brightness = lerp(0.18, 1.0, level / (4.0 - 3.0 * level));
    const float3 lit = texel.rgb * shades[face] * brightness;
    return float4(lit, translucent ? texel.a : 1.0);
}
)";

/** Camera basis, projection, depth mapping and pass, as the shader reads them. */
struct alignas(16) CameraConstants {
    std::array<float, 4> position{};
    std::array<float, 4> right{};
    std::array<float, 4> up{};
    std::array<float, 4> forward{};
    std::array<float, 4> projection{};
    std::array<float, 4> depthAndPass{};
};
static_assert(sizeof(CameraConstants) % 16 == 0);

/** The protocol's vertex, with the position moved into Destiny space. */
struct Vertex {
    float x, y, z;
    float u, v;
    std::uint32_t color;
    std::uint32_t light;
    std::uint32_t flags;
};
static_assert(sizeof(Vertex) == sizeof(proto::RenVertex));

constexpr float kNearPlane = 0.05F;
constexpr float kFarPlane = 2000.0F;
constexpr float kPi = 3.14159265358979323846F;
constexpr int kSectionSize = 16;
/** Render-ring bytes taken per frame, so a world arriving all at once spreads over frames. */
constexpr std::uint64_t kDrainBytesPerFrame = 16ULL << 20U;
/** Largest atlas side accepted from the ring. */
constexpr std::uint32_t kMaximumAtlasSide = 16384;

struct Section {
    ID3D11Buffer* buffer{};
    UINT vertices{};
};

struct Resources {
    ID3D11Device* owner{};
    ID3D11VertexShader* vertexShader{};
    ID3D11PixelShader* pixelShader{};
    ID3D11InputLayout* layout{};
    ID3D11Buffer* constants{};
    ID3D11SamplerState* sampler{};
    ID3D11RasterizerState* rasterizer{};
    ID3D11BlendState* opaqueBlend{};
    ID3D11BlendState* translucentBlend{};
    ID3D11DepthStencilState* opaqueDepth{};
    ID3D11DepthStencilState* translucentDepth{};
    ID3DDeviceContextState* contextState{};
    ID3D11Texture2D* atlas{};
    ID3D11ShaderResourceView* atlasView{};
    std::uint32_t atlasWidth{};
    std::uint32_t atlasHeight{};
    ID3D11Texture2D* depth{};
    ID3D11DepthStencilView* depthView{};
    UINT depthWidth{};
    UINT depthHeight{};
    bool failed{};
};

Resources g_resources{};
std::unordered_map<std::uint64_t, Section> g_sections{};
std::vector<Vertex> g_scratch{};

template <typename Interface> void release_com(Interface*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

[[nodiscard]] std::uint64_t section_key(std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
    constexpr std::uint64_t mask = (1ULL << 21U) - 1U;
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) & mask)
           | ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) & mask) << 21U)
           | ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) & mask) << 42U);
}

void clear_sections() noexcept {
    for (auto& entry : g_sections) {
        release_com(entry.second.buffer);
    }
    g_sections.clear();
}

[[nodiscard]] bool compile(const char* entry, const char* target, ID3DBlob** output) noexcept {
    ID3DBlob* errors = nullptr;
    const HRESULT result = D3DCompile(kShader,
                                      sizeof(kShader) - 1,
                                      "skycraft_blocks",
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
                          "ev=skycraft stage=blocks_shader entry=%s error=%s",
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

/** Creates shaders and fixed state for one device. */
[[nodiscard]] bool initialize(ID3D11Device* device) noexcept {
    if (g_resources.owner == device) {
        return !g_resources.failed;
    }
    release();
    g_resources.owner = device;
    g_resources.failed = true;

    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob = nullptr;
    const bool compiled =
        compile("VSMain", "vs_5_0", &vertexBlob) && compile("PSMain", "ps_5_0", &pixelBlob);
    if (compiled) {
        (void)device->CreateVertexShader(vertexBlob->GetBufferPointer(),
                                         vertexBlob->GetBufferSize(),
                                         nullptr,
                                         &g_resources.vertexShader);
        (void)device->CreatePixelShader(pixelBlob->GetBufferPointer(),
                                        pixelBlob->GetBufferSize(),
                                        nullptr,
                                        &g_resources.pixelShader);
        const std::array<D3D11_INPUT_ELEMENT_DESC, 5> elements{{
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
        }};
        (void)device->CreateInputLayout(elements.data(),
                                        static_cast<UINT>(elements.size()),
                                        vertexBlob->GetBufferPointer(),
                                        vertexBlob->GetBufferSize(),
                                        &g_resources.layout);
    }
    release_com(vertexBlob);
    release_com(pixelBlob);
    if (!compiled) {
        return false;
    }

    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(CameraConstants);
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    (void)device->CreateBuffer(&constants, nullptr, &g_resources.constants);

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

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED
                                                  | D3D11_COLOR_WRITE_ENABLE_GREEN
                                                  | D3D11_COLOR_WRITE_ENABLE_BLUE;
    (void)device->CreateBlendState(&blend, &g_resources.opaqueBlend);
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    (void)device->CreateBlendState(&blend, &g_resources.translucentBlend);

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS;
    (void)device->CreateDepthStencilState(&depth, &g_resources.opaqueDepth);
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    (void)device->CreateDepthStencilState(&depth, &g_resources.translucentDepth);

    g_resources.failed =
        g_resources.vertexShader == nullptr || g_resources.pixelShader == nullptr
        || g_resources.layout == nullptr || g_resources.constants == nullptr
        || g_resources.sampler == nullptr || g_resources.rasterizer == nullptr
        || g_resources.opaqueBlend == nullptr || g_resources.translucentBlend == nullptr
        || g_resources.opaqueDepth == nullptr || g_resources.translucentDepth == nullptr
        || !create_context_state(device);
    core::log::write(core::log::Channel::client,
                     g_resources.failed ? core::log::Level::error : core::log::Level::info,
                     g_resources.failed ? "ev=skycraft stage=blocks_init result=fail"
                                        : "ev=skycraft stage=blocks_init result=ok");
    return !g_resources.failed;
}

void receive_atlas(ID3D11Device* device,
                   const std::uint8_t* payload,
                   std::uint32_t bytes) noexcept {
    if (bytes < sizeof(proto::RenAtlas)) {
        return;
    }
    proto::RenAtlas header{};
    std::memcpy(&header, payload, sizeof(header));
    const std::uint64_t pixels = std::uint64_t{header.width} * header.height * 4U;
    if (header.width == 0 || header.height == 0 || header.width > kMaximumAtlasSide
        || header.height > kMaximumAtlasSide || bytes < sizeof(header) + pixels) {
        return;
    }
    release_com(g_resources.atlasView);
    release_com(g_resources.atlas);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = header.width;
    description.Height = header.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{payload + sizeof(header), header.width * 4U, 0};
    if (FAILED(device->CreateTexture2D(&description, &data, &g_resources.atlas))
        || FAILED(
            device->CreateShaderResourceView(g_resources.atlas, nullptr, &g_resources.atlasView))) {
        release_com(g_resources.atlas);
        return;
    }
    g_resources.atlasWidth = header.width;
    g_resources.atlasHeight = header.height;
    core::log::writef(core::log::Channel::client,
                      core::log::Level::info,
                      "ev=skycraft stage=atlas width=%u height=%u",
                      header.width,
                      header.height);
}

void receive_atlas_region(ID3D11DeviceContext* context,
                          const std::uint8_t* payload,
                          std::uint32_t bytes) noexcept {
    if (g_resources.atlas == nullptr || bytes < sizeof(proto::RenAtlasRegion)) {
        return;
    }
    proto::RenAtlasRegion region{};
    std::memcpy(&region, payload, sizeof(region));
    const std::uint64_t pixels = std::uint64_t{region.width} * region.height * 4U;
    if (region.width == 0 || region.height == 0 || region.x + region.width > g_resources.atlasWidth
        || region.y + region.height > g_resources.atlasHeight || bytes < sizeof(region) + pixels) {
        return;
    }
    const D3D11_BOX box{
        region.x, region.y, 0, region.x + region.width, region.y + region.height, 1};
    context->UpdateSubresource(
        g_resources.atlas, 0, &box, payload + sizeof(region), region.width * 4U, 0);
}

void receive_section(ID3D11Device* device,
                     const std::uint8_t* payload,
                     std::uint32_t bytes) noexcept {
    if (bytes < sizeof(proto::RenSection)) {
        return;
    }
    proto::RenSection header{};
    std::memcpy(&header, payload, sizeof(header));
    const std::uint64_t key = section_key(header.sx, header.sy, header.sz);
    if (const auto found = g_sections.find(key); found != g_sections.end()) {
        release_com(found->second.buffer);
        g_sections.erase(found);
    }
    const std::uint64_t vertexBytes = std::uint64_t{header.vertexCount} * sizeof(proto::RenVertex);
    if (header.vertexCount == 0 || bytes < sizeof(header) + vertexBytes) {
        return;
    }
    const bool mirror = shared().mirror.load(std::memory_order_relaxed);
    const double originX = static_cast<double>(header.sx) * kSectionSize;
    const double originY = static_cast<double>(header.sy) * kSectionSize;
    const double originZ = static_cast<double>(header.sz) * kSectionSize;
    g_scratch.resize(header.vertexCount);
    const std::uint8_t* source = payload + sizeof(header);
    for (std::uint32_t index = 0; index < header.vertexCount; ++index) {
        proto::RenVertex vertex{};
        std::memcpy(&vertex, source + std::size_t{index} * sizeof(vertex), sizeof(vertex));
        const teleport::Vector at =
            to_destiny(originX + vertex.x, originY + vertex.y, originZ + vertex.z, mirror);
        g_scratch[index] = Vertex{
            at[0], at[1], at[2], vertex.u, vertex.v, vertex.color, vertex.light, vertex.flags};
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = static_cast<UINT>(vertexBytes);
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA data{g_scratch.data(), 0, 0};
    Section section{};
    if (FAILED(device->CreateBuffer(&description, &data, &section.buffer))) {
        return;
    }
    section.vertices = header.vertexCount;
    g_sections.emplace(key, section);
}

/** Context handed through the render-ring drain. */
struct DrainContext {
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
};

void receive(void* raw,
             std::uint32_t type,
             const std::uint8_t* payload,
             std::uint32_t bytes) noexcept {
    auto* drain = static_cast<DrainContext*>(raw);
    switch (type) {
    case proto::kRenAtlas:
        receive_atlas(drain->device, payload, bytes);
        break;
    case proto::kRenAtlasRegion:
        receive_atlas_region(drain->context, payload, bytes);
        break;
    case proto::kRenSection:
        receive_section(drain->device, payload, bytes);
        break;
    case proto::kRenClearAll:
        clear_sections();
        break;
    default:
        // The player's body, entities, lights and dug blocks come with later milestones.
        break;
    }
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
    return width != 0 && height != 0 && description.SampleDesc.Count == 1;
}

[[nodiscard]] bool ensure_depth(ID3D11Device* device, UINT width, UINT height) noexcept {
    if (g_resources.depthView != nullptr && g_resources.depthWidth == width
        && g_resources.depthHeight == height) {
        return true;
    }
    release_com(g_resources.depthView);
    release_com(g_resources.depth);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &g_resources.depth))
        || FAILED(
            device->CreateDepthStencilView(g_resources.depth, nullptr, &g_resources.depthView))) {
        release_com(g_resources.depth);
        return false;
    }
    g_resources.depthWidth = width;
    g_resources.depthHeight = height;
    return true;
}

[[nodiscard]] float dot(const teleport::Vector& left, const teleport::Vector& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

[[nodiscard]] bool normalize(teleport::Vector& value) noexcept {
    const float length = std::sqrt(dot(value, value));
    if (!(length > 0.000001F) || !std::isfinite(length)) {
        return false;
    }
    for (float& lane : value) {
        lane /= length;
    }
    return true;
}

/** The same camera basis and projection Sunrise's world markers use. */
[[nodiscard]] bool camera_constants(const teleport::CameraPose& camera,
                                    CameraConstants& output) noexcept {
    teleport::Vector forward = camera.forward;
    teleport::Vector up = camera.up;
    if (!normalize(forward) || !(camera.horizontalFov > 0.0F) || !(camera.horizontalFov < kPi)
        || !(camera.aspect > 0.0F)) {
        return false;
    }
    const float along = dot(up, forward);
    for (std::size_t lane = 0; lane < up.size(); ++lane) {
        up[lane] -= forward[lane] * along;
    }
    if (!normalize(up)) {
        return false;
    }
    teleport::Vector right{forward[1] * up[2] - forward[2] * up[1],
                           forward[2] * up[0] - forward[0] * up[2],
                           forward[0] * up[1] - forward[1] * up[0]};
    if (!normalize(right)) {
        return false;
    }
    const float tangent = std::tan(camera.horizontalFov * 0.5F);
    if (!(tangent > 0.0F) || !std::isfinite(tangent)) {
        return false;
    }
    const float depthScale = kFarPlane / (kFarPlane - kNearPlane);
    output.position = {camera.position[0], camera.position[1], camera.position[2], 0.0F};
    output.right = {right[0], right[1], right[2], 0.0F};
    output.up = {up[0], up[1], up[2], 0.0F};
    output.forward = {forward[0], forward[1], forward[2], 0.0F};
    output.projection = {1.0F / tangent, camera.aspect / tangent, 0.0F, 0.0F};
    output.depthAndPass = {depthScale, kNearPlane * depthScale, 0.0F, 0.0F};
    return true;
}

[[nodiscard]] bool write_constants(ID3D11DeviceContext* context,
                                   const CameraConstants& constants) noexcept {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g_resources.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(g_resources.constants, 0);
    return true;
}

void render(ID3D11DeviceContext* context,
            ID3D11RenderTargetView* target,
            UINT width,
            UINT height,
            CameraConstants constants) noexcept {
    context->ClearDepthStencilView(g_resources.depthView, D3D11_CLEAR_DEPTH, 1.0F, 0);
    const D3D11_VIEWPORT viewport{
        0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F};
    constexpr float blendFactor[4]{};
    constexpr UINT stride = sizeof(Vertex);
    constexpr UINT offset = 0;
    ID3D11Buffer* constantBuffer = g_resources.constants;
    ID3D11ShaderResourceView* atlas = g_resources.atlasView;
    ID3D11SamplerState* sampler = g_resources.sampler;
    context->OMSetRenderTargets(1, &target, g_resources.depthView);
    context->RSSetViewports(1, &viewport);
    context->RSSetState(g_resources.rasterizer);
    context->IASetInputLayout(g_resources.layout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_resources.vertexShader, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &constantBuffer);
    context->PSSetShader(g_resources.pixelShader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &constantBuffer);
    context->PSSetShaderResources(0, 1, &atlas);
    context->PSSetSamplers(0, 1, &sampler);

    for (int pass = 0; pass < 2; ++pass) {
        const bool translucent = pass == 1;
        constants.depthAndPass[2] = translucent ? 1.0F : 0.0F;
        if (!write_constants(context, constants)) {
            return;
        }
        context->OMSetBlendState(translucent ? g_resources.translucentBlend
                                             : g_resources.opaqueBlend,
                                 blendFactor,
                                 0xFFFFFFFFU);
        context->OMSetDepthStencilState(
            translucent ? g_resources.translucentDepth : g_resources.opaqueDepth, 0);
        for (const auto& entry : g_sections) {
            ID3D11Buffer* buffer = entry.second.buffer;
            context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
            context->Draw(entry.second.vertices, 0);
        }
    }
}

} // namespace

void draw(ID3D11Device* device,
          ID3D11DeviceContext* context,
          ID3D11RenderTargetView* target,
          const teleport::CameraPose* camera) noexcept {
    if (!initialize(device)) {
        return;
    }
    DrainContext drain{device, context};
    Link::get().drain_render(&receive, &drain, kDrainBytesPerFrame);
    if (camera == nullptr || g_sections.empty() || g_resources.atlasView == nullptr) {
        return;
    }
    CameraConstants constants{};
    UINT width = 0;
    UINT height = 0;
    if (!camera_constants(*camera, constants) || !target_size(target, width, height)
        || !ensure_depth(device, width, height)) {
        return;
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
        render(context, target, width, height, constants);
    }
    ID3DDeviceContextState* privateState = nullptr;
    context1->SwapDeviceContextState(previous, &privateState);
    release_com(privateState);
    release_com(previous);
    release_com(context1);
}

void release() noexcept {
    clear_sections();
    g_scratch = {};
    release_com(g_resources.depthView);
    release_com(g_resources.depth);
    release_com(g_resources.atlasView);
    release_com(g_resources.atlas);
    release_com(g_resources.contextState);
    release_com(g_resources.translucentDepth);
    release_com(g_resources.opaqueDepth);
    release_com(g_resources.translucentBlend);
    release_com(g_resources.opaqueBlend);
    release_com(g_resources.rasterizer);
    release_com(g_resources.sampler);
    release_com(g_resources.constants);
    release_com(g_resources.layout);
    release_com(g_resources.pixelShader);
    release_com(g_resources.vertexShader);
    g_resources = Resources{};
}

} // namespace sunrise::client::hooks::skycraft::blocks
