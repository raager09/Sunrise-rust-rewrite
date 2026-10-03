#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>

namespace sunrise::client::hooks::skycraft::settings {

/** Longest launcher path or argument line kept, in UTF-16 units. */
inline constexpr std::size_t kTextCapacity = 520;
/** F6, until another key is picked. */
inline constexpr std::uint32_t kDefaultHandsKey = VK_F6;

/** What `Sunrise\skycraft.json` holds. Written with defaults the first time it is missing. */
struct Settings {
    /** The whole bridge. Off until the player turns it on. */
    bool enabled{false};
    /** Start Minecraft once the game's hooks are in, unless one with the mod already runs. */
    bool startMinecraft{true};
    /** Launcher to start. Empty: SkyCraft's own Prism install, then an installed Prism. */
    std::array<wchar_t, kTextCapacity> launcher{};
    /** Arguments for the launcher. */
    std::array<wchar_t, kTextCapacity> arguments{};
    /** Toggles Minecraft hands: mouse buttons, wheel and Minecraft's keys go to Minecraft. */
    std::uint32_t handsKey{kDefaultHandsKey};
    /** Flips the world's handedness when Destiny's axes turn out mirrored against Minecraft's. */
    bool mirror{false};
};

/** Resolves the file, loads it, and writes it with defaults when it does not exist yet. */
void initialize(void* module) noexcept;

/** Drops the loaded configuration. */
void shutdown() noexcept;

/** @return One lock-consistent copy of the configuration. */
[[nodiscard]] Settings get() noexcept;

} // namespace sunrise::client::hooks::skycraft::settings
