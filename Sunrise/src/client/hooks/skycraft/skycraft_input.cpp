/**
 * Who gets the keyboard and mouse while Minecraft is connected.
 *
 * Destiny keeps movement and look. With Minecraft hands on (the hands key, F6 by default), the
 * mouse buttons, the wheel and Minecraft's own keys go to Minecraft only: the hotbar digits, E,
 * Q, T, slash, F3 and O for Minecraft's menu. While a Minecraft screen is open every key and the
 * whole mouse are Minecraft's, the mouse moving its cursor. Destiny reads its actions by polling
 * key state, so a key that is Minecraft's is also reported released to those reads.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#include "../../../core/logging/log.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client::hooks::skycraft {
namespace {

/** Hardware scan code (the DirectInput code, extended keys plus 0x80) to SDL scancode. */
constexpr std::array<std::uint16_t, 256> kScanToSdl = [] {
    std::array<std::uint16_t, 256> t{};
    t[0x01] = 41; // Esc
    for (int i = 0; i < 9; ++i) {
        t[static_cast<std::size_t>(0x02 + i)] = static_cast<std::uint16_t>(30 + i); // 1-9
    }
    t[0x0B] = 39; // 0
    t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;
    t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;
    t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;
    t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;
    t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;
    t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;
    t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;
    t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;
    t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;
    t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;
    for (int i = 0; i < 10; ++i) {
        t[static_cast<std::size_t>(0x3B + i)] = static_cast<std::uint16_t>(58 + i); // F1-F10
    }
    t[0x45] = 83, t[0x46] = 71;
    t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;
    t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;
    t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;
    t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;
    t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;
    t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;
    t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;
    t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;
    return t;
}();

/** Key-message bits: the scan code, the extended flag, and the key's previous state. */
constexpr unsigned kScanShift = 16;
constexpr LPARAM kScanMask = 0xFF;
constexpr LPARAM kExtendedBit = LPARAM{1} << 24U;
constexpr LPARAM kPreviousDownBit = LPARAM{1} << 30U;
constexpr std::uint16_t kExtendedScanOffset = 0x80;
/** Minecraft opens its pause menu on this key; Esc stays Destiny's. */
constexpr int kMinecraftMenuKey = 'O';

/** A UTF-16 high surrogate waiting for its low half. */
std::atomic<std::uint32_t> g_pendingHighSurrogate{0};

/** @return True for the keys Minecraft hands take from Destiny. */
[[nodiscard]] bool is_minecraft_key(int virtualKey) noexcept {
    if (virtualKey >= '0' && virtualKey <= '9') {
        return true;
    }
    switch (virtualKey) {
    case 'E':
    case 'Q':
    case 'T':
    case kMinecraftMenuKey:
    case VK_OEM_2:
    case VK_F3:
    case VK_LBUTTON:
    case VK_RBUTTON:
    case VK_MBUTTON:
    case VK_XBUTTON1:
    case VK_XBUTTON2:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_key_down(UINT message) noexcept {
    return message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
}

[[nodiscard]] bool is_key_message(UINT message) noexcept {
    return is_key_down(message) || message == WM_KEYUP || message == WM_SYSKEYUP;
}

[[nodiscard]] bool is_client_mouse(UINT message) noexcept {
    return message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
}

/** Sends one key message to Minecraft as its SDL scancode. */
void push_key(UINT message, LPARAM value) noexcept {
    auto scan = static_cast<std::uint16_t>((value >> kScanShift) & kScanMask);
    if ((value & kExtendedBit) != 0) {
        scan = static_cast<std::uint16_t>(scan + kExtendedScanOffset);
    }
    const std::uint16_t sdl = kScanToSdl[scan & 0xFFU];
    if (sdl != 0) {
        Link::get().push_input(proto::kInKey, sdl, is_key_down(message) ? 1 : 0);
    }
}

/** Sends one typed UTF-16 unit, joining surrogate pairs into one code point. */
void push_text(WPARAM word) noexcept {
    const auto unit = static_cast<std::uint32_t>(word & 0xFFFFU);
    if (unit >= 0xD800U && unit <= 0xDBFFU) {
        g_pendingHighSurrogate.store(unit, std::memory_order_relaxed);
        return;
    }
    std::uint32_t codePoint = unit;
    if (unit >= 0xDC00U && unit <= 0xDFFFU) {
        const std::uint32_t high = g_pendingHighSurrogate.exchange(0, std::memory_order_relaxed);
        if (high == 0) {
            return;
        }
        codePoint = 0x10000U + ((high - 0xD800U) << 10U) + (unit - 0xDC00U);
    }
    if (codePoint >= 0x20U) {
        Link::get().push_input(proto::kInText, 0, static_cast<std::int32_t>(codePoint));
    }
}

/** Flips Minecraft hands. */
void toggle_hands() noexcept {
    Shared& state = shared();
    const bool on = !state.hands.load(std::memory_order_relaxed);
    state.hands.store(on, std::memory_order_relaxed);
    release_all();
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     on ? "ev=skycraft stage=hands enabled=1"
                        : "ev=skycraft stage=hands enabled=0");
}

/** Raw mouse button flags, each with its SDL button and whether it presses or releases. */
struct ButtonFlag {
    USHORT flag{};
    std::uint16_t sdl{};
    bool down{};
};

constexpr std::array<ButtonFlag, 10> kButtonFlags{{
    {RI_MOUSE_LEFT_BUTTON_DOWN, 1, true},
    {RI_MOUSE_LEFT_BUTTON_UP, 1, false},
    {RI_MOUSE_RIGHT_BUTTON_DOWN, 3, true},
    {RI_MOUSE_RIGHT_BUTTON_UP, 3, false},
    {RI_MOUSE_MIDDLE_BUTTON_DOWN, 2, true},
    {RI_MOUSE_MIDDLE_BUTTON_UP, 2, false},
    {RI_MOUSE_BUTTON_4_DOWN, 4, true},
    {RI_MOUSE_BUTTON_4_UP, 4, false},
    {RI_MOUSE_BUTTON_5_DOWN, 5, true},
    {RI_MOUSE_BUTTON_5_UP, 5, false},
}};

} // namespace

bool route_window_message(UINT message, WPARAM word, LPARAM value) noexcept {
    Shared& state = shared();
    if (!state.enabled.load(std::memory_order_relaxed) || !takes_input()) {
        return false;
    }
    const int virtualKey = static_cast<int>(word);
    const bool screenOpen = state.mcScreenOpen.load(std::memory_order_relaxed);
    if (is_key_message(message)
        && virtualKey == static_cast<int>(state.handsKey.load(std::memory_order_relaxed))) {
        if (is_key_down(message) && (value & kPreviousDownBit) == 0 && !screenOpen) {
            toggle_hands();
        }
        return true;
    }
    if (screenOpen) {
        if (is_key_message(message)) {
            push_key(message, value);
            return true;
        }
        if (message == WM_CHAR) {
            push_text(word);
            return true;
        }
        // The raw-input window carries the mouse; the window's own copies are dropped.
        return is_client_mouse(message) || message == WM_SYSCHAR || message == WM_DEADCHAR;
    }
    if (!state.hands.load(std::memory_order_relaxed)) {
        return false;
    }
    if (is_key_message(message) && is_minecraft_key(virtualKey)) {
        if (virtualKey == kMinecraftMenuKey) {
            if (is_key_down(message) && (value & kPreviousDownBit) == 0) {
                release_all();
                Link::get().push_input(proto::kInOpenMenu, 0);
            }
            return true;
        }
        push_key(message, value);
        return true;
    }
    return is_client_mouse(message) && message != WM_MOUSEMOVE;
}

bool route_raw_input(LPARAM value) noexcept {
    Shared& state = shared();
    if (!state.enabled.load(std::memory_order_relaxed) || !takes_input()) {
        return false;
    }
    const bool screenOpen = state.mcScreenOpen.load(std::memory_order_relaxed);
    const bool hands = state.hands.load(std::memory_order_relaxed);
    if (!screenOpen && !hands) {
        return false;
    }
    RAWINPUT input{};
    UINT size = sizeof(input);
    const UINT read = GetRawInputData(
        reinterpret_cast<HRAWINPUT>(value), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
    if (read == static_cast<UINT>(-1) || read == 0 || input.header.dwType != RIM_TYPEMOUSE) {
        // Raw keyboard input belongs to Minecraft too while one of its screens is open.
        return screenOpen && read != static_cast<UINT>(-1)
               && input.header.dwType == RIM_TYPEKEYBOARD;
    }
    const RAWMOUSE& mouse = input.data.mouse;
    Link& link = Link::get();
    bool buttons = false;
    for (const ButtonFlag& entry : kButtonFlags) {
        if ((mouse.usButtonFlags & entry.flag) != 0) {
            link.push_input(proto::kInMouseButton, entry.sdl, entry.down ? 1 : 0);
            buttons = true;
        }
    }
    if ((mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0) {
        // Already in WHEEL_DELTA units per notch, as the protocol wants.
        const auto delta = static_cast<SHORT>(mouse.usButtonData);
        link.push_input(proto::kInScroll, 0, static_cast<std::int32_t>(delta));
        buttons = true;
    }
    if (screenOpen) {
        if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0
            && (mouse.lLastX != 0 || mouse.lLastY != 0)) {
            const int width = (std::max)(1, state.viewportW.load(std::memory_order_relaxed));
            const int height = (std::max)(1, state.viewportH.load(std::memory_order_relaxed));
            const int x = std::clamp(state.cursorX.load(std::memory_order_relaxed)
                                         + static_cast<int>(mouse.lLastX),
                                     0,
                                     width - 1);
            const int y = std::clamp(state.cursorY.load(std::memory_order_relaxed)
                                         + static_cast<int>(mouse.lLastY),
                                     0,
                                     height - 1);
            state.cursorX.store(x, std::memory_order_relaxed);
            state.cursorY.store(y, std::memory_order_relaxed);
            link.push_input(proto::kInCursor, 0, x, y);
        }
        return true;
    }
    // Hands only: movement still turns Destiny's camera. Button packets rarely carry movement.
    return buttons;
}

bool hides_key(int virtualKey) noexcept {
    Shared& state = shared();
    if (!state.enabled.load(std::memory_order_relaxed)
        || !state.mcInWorld.load(std::memory_order_relaxed)) {
        return false;
    }
    if (state.mcScreenOpen.load(std::memory_order_relaxed)) {
        return true;
    }
    if (virtualKey == static_cast<int>(state.handsKey.load(std::memory_order_relaxed))) {
        return true;
    }
    return state.hands.load(std::memory_order_relaxed) && is_minecraft_key(virtualKey);
}

} // namespace sunrise::client::hooks::skycraft
