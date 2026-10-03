#pragma once

#include <Windows.h>

#include <d3d11.h>

namespace sunrise::client::hooks::skycraft {

/**
 * SkyCraft bridge: Destiny hosts SkyCraft's Minecraft mod the way Skyrim does. Destiny moves its
 * own player; Minecraft follows it and supplies its inventory, HUD, hand and block placing.
 */

/** Loads `Sunrise\skycraft.json`. */
void initialize(void* module) noexcept;

/** Forgets the configuration. */
void shutdown() noexcept;

/** Maps the shared memory and starts Minecraft, when the bridge is enabled. */
void install() noexcept;

/** Unmaps the shared memory and frees the renderer. */
void uninstall() noexcept;

/** Once per frame on the thread that owns the camera and the player. */
void frame() noexcept;

/** Present thread, before Sunrise's own interface: Minecraft's blocks, then its HUD. */
void present(ID3D11Device* device,
             ID3D11DeviceContext* context,
             ID3D11RenderTargetView* target) noexcept;

/** Frees device objects ahead of a swap-chain change or device loss. */
void release_device() noexcept;

/**
 * Routes one output-window message.
 * @return True when Minecraft took it and the game must not see it.
 */
[[nodiscard]] bool route_window_message(UINT message, WPARAM word, LPARAM value) noexcept;

/**
 * Routes one WM_INPUT from the game's raw-input window.
 * @return True when Minecraft took it and the game must not see it.
 */
[[nodiscard]] bool route_raw_input(LPARAM value) noexcept;

/**
 * Asked for each key the game polls.
 * @return True when the key belongs to Minecraft right now, so the game reads it released.
 */
[[nodiscard]] bool hides_key(int virtualKey) noexcept;

} // namespace sunrise::client::hooks::skycraft
