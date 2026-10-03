#pragma once

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>

#include "../teleport/runtime.h"
#include "link.h"

namespace sunrise::client::hooks::skycraft {

/**
 * State the game thread publishes for the window, raw-input and render threads. Settings are
 * read once at install, so the hot paths (every polled key read) touch only these atomics.
 */
struct Shared {
    std::atomic_bool enabled{};
    std::atomic_bool mirror{};
    std::atomic_uint32_t handsKey{};

    /** Minecraft is connected and its player is in a world. */
    std::atomic_bool mcInWorld{};
    /** A Minecraft screen (inventory, chat, pause) is open: it takes all input. */
    std::atomic_bool mcScreenOpen{};
    /** Minecraft hands are on: mouse buttons, the wheel and Minecraft's keys are Minecraft's. */
    std::atomic_bool hands{};
    /** Destiny's player and camera were read this frame. */
    std::atomic_bool inGame{};
    /** Minecraft's crosshair is up, at this GUI scale. */
    std::atomic_bool crosshair{};
    std::atomic_int guiScale{};

    /** Virtual Minecraft cursor, in overlay pixels, while a Minecraft screen is open. */
    std::atomic_int cursorX{};
    std::atomic_int cursorY{};
    std::atomic_int viewportW{1920};
    std::atomic_int viewportH{1080};
};

[[nodiscard]] Shared& shared() noexcept;

/** A point in Minecraft space: blocks, Y up, Z south. */
struct McVec {
    double x{};
    double y{};
    double z{};
};

/**
 * One Minecraft block is one Destiny world unit (about a metre: a Guardian and Minecraft's
 * 1.8-block player are near the same height). Destiny is Z up; its X stays Minecraft's X.
 */
[[nodiscard]] inline McVec to_mc(const teleport::Vector& destiny, bool mirror) noexcept {
    return {destiny[0], destiny[2], mirror ? destiny[1] : -destiny[1]};
}

[[nodiscard]] inline teleport::Vector
to_destiny(double x, double y, double z, bool mirror) noexcept {
    return {static_cast<float>(x), static_cast<float>(mirror ? z : -z), static_cast<float>(y)};
}

/** Minecraft's standing eye height, in blocks. */
inline constexpr double kMcEyeHeight = 1.62;

/** @return Whether Minecraft is up and the bridge may take input. Any thread. */
[[nodiscard]] bool takes_input() noexcept;

/** Sends a release of every held key and button to Minecraft. */
void release_all() noexcept;

namespace launcher {

/** Starts Minecraft unless one with the mod already runs. Returns at once. */
void start() noexcept;

/** @return True while a Minecraft with the SkyCraft mod is running. */
[[nodiscard]] bool minecraft_running() noexcept;

} // namespace launcher

namespace overlay {

/** Uploads Minecraft's newest HUD frame and draws it over the back buffer. Present thread. */
void draw(ID3D11Device* device,
          ID3D11DeviceContext* context,
          ID3D11RenderTargetView* target) noexcept;

/** Frees every device object. */
void release() noexcept;

} // namespace overlay

namespace blocks {

/** Drains Minecraft's block meshes and draws them with Destiny's camera. Present thread. */
void draw(ID3D11Device* device,
          ID3D11DeviceContext* context,
          ID3D11RenderTargetView* target,
          const teleport::CameraPose* camera) noexcept;

/** Frees every device object and forgets every section. */
void release() noexcept;

} // namespace blocks

} // namespace sunrise::client::hooks::skycraft
