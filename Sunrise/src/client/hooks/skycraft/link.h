#pragma once

#include <Windows.h>

#include <cstdint>

#include "../../../../vendor/skycraft/skycraft_protocol.h"

namespace sunrise::client::hooks::skycraft {

namespace proto = ::skycraft::proto;

/**
 * The shared memory SkyCraft's Minecraft mod opens. Destiny creates it, so this side owns the
 * rings it produces and resets them whenever it maps the region.
 */
class Link final {
public:
    /** @return The one process-wide link. */
    [[nodiscard]] static Link& get() noexcept;

    /** Creates or reopens the mapping. @return True when it is mapped. */
    [[nodiscard]] bool create() noexcept;

    /** Unmaps and closes the mapping. */
    void close() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return base_ != nullptr;
    }

    /** @return True while Minecraft has written its heartbeat recently. */
    [[nodiscard]] bool mc_alive() const noexcept;

    /** @return Process id Minecraft wrote when it opened the mapping. */
    [[nodiscard]] std::uint32_t mc_pid() const noexcept;

    /** Stamps this side's heartbeat. Any thread. */
    void heartbeat() noexcept;

    /** Seqlock write of the host state. One writer thread. */
    void write_sky_state(const proto::SkyState& state) noexcept;

    /** Seqlock read of Minecraft's state. @return False when no consistent copy was read. */
    [[nodiscard]] bool read_mc_state(proto::McState& output) const noexcept;

    /** Queues one input event. Drops it when Minecraft is a whole ring behind. Any thread. */
    void push_input(proto::InputType type,
                    std::uint16_t code,
                    std::int32_t a = 0,
                    std::int32_t b = 0,
                    std::int32_t c = 0) noexcept;

    /** Takes the oldest Minecraft event. @return False when there is none. */
    [[nodiscard]] bool pop_event(proto::McEvent& output) noexcept;

    /** Called for each pending render message with its type, payload and payload size. */
    using RenderSink = void (*)(void* context,
                                std::uint32_t type,
                                const std::uint8_t* payload,
                                std::uint32_t bytes) noexcept;

    /** Hands pending render messages to a sink, up to about a byte budget. Render thread. */
    void drain_render(RenderSink sink, void* context, std::uint64_t maximumBytes) noexcept;

    /** Moves a newer overlay frame into the front slot. @return True when one arrived. */
    [[nodiscard]] bool acquire_overlay_frame() noexcept;

    /** Restarts the overlay slot exchange for a Minecraft that has just connected. */
    void reset_overlay() noexcept;

    [[nodiscard]] const std::uint8_t* front_pixels() const noexcept;
    [[nodiscard]] const proto::OverlaySlotHdr* front_header() const noexcept;

private:
    template <class T> [[nodiscard]] T* at(std::uint64_t offset) const noexcept {
        return reinterpret_cast<T*>(base_ + offset);
    }

    HANDLE mapping_{};
    std::uint8_t* base_{};
    /** The output window and the raw-input window can run on different threads. */
    SRWLOCK inputLock_{SRWLOCK_INIT};
    std::uint32_t overlayFront_{2};
};

} // namespace sunrise::client::hooks::skycraft
