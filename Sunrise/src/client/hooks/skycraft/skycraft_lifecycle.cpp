/**
 * The SkyCraft bridge's lifetime and its once-a-frame exchange with Minecraft.
 *
 * Destiny moves its own player, so SkyState carries kSkyHostDrives: Minecraft puts its player at
 * Destiny's eye every tick instead of running its own movement against collision this side does
 * not export yet. Look comes from Destiny's camera; Minecraft aims, reaches and places from it.
 */

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../input/window_focus.h"
#include "../../player/player_position.h"
#include "../teleport/runtime.h"
#include "internal.h"
#include "runtime.h"
#include "skycraft_settings_store.h"

namespace sunrise::client::hooks::skycraft {
namespace {

/** A camera farther than this from the body is not the first-person eye (sparrow, emotes). */
constexpr float kThirdPersonDistance = 4.0F;
/** One Minecraft world stands for every Destiny destination until collision is exported. */
constexpr std::uint32_t kWorldId = 1;
constexpr std::uint32_t kCollisionEpoch = 1;
/** Minecraft's sun stays at noon; Destiny lights the picture. */
constexpr float kGameHour = 12.0F;
/** Events taken per frame, so a burst cannot stall the game thread. */
constexpr int kEventsPerFrame = 64;
constexpr double kDegreesPerRadian = 57.29577951308232;

Shared g_shared{};
std::atomic_bool g_installed{false};

/** Game-thread state of the exchange. */
struct Exchange {
    std::uint32_t teleportSeq{1};
    std::uint32_t lastMcPid{};
    bool mcWasAlive{};
    std::uint64_t eventsSeen{};
};
Exchange g_exchange{};

[[nodiscard]] float length_of(const teleport::Vector& value) noexcept {
    return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}

/** Minecraft's yaw and pitch, in degrees, for a Destiny direction. */
void look_from(const teleport::Vector& forward, bool mirror, float& yaw, float& pitch) noexcept {
    const float length = length_of(forward);
    if (!(length > 0.0F)) {
        return;
    }
    const teleport::Vector unit{forward[0] / length, forward[1] / length, forward[2] / length};
    const McVec direction = to_mc(unit, mirror);
    yaw = static_cast<float>(std::atan2(-direction.x, direction.z) * kDegreesPerRadian);
    pitch = static_cast<float>(-std::asin(std::clamp(direction.y, -1.0, 1.0)) * kDegreesPerRadian);
}

/** Takes Minecraft's events. Hits on Destiny's combatants need an actor table first. */
void drain_events(Link& link) noexcept {
    proto::McEvent event{};
    for (int taken = 0; taken < kEventsPerFrame && link.pop_event(event); ++taken) {
        ++g_exchange.eventsSeen;
        core::log::writef(core::log::Channel::client,
                          core::log::Level::debug,
                          "ev=skycraft stage=event type=%u total=%llu",
                          event.type,
                          static_cast<unsigned long long>(g_exchange.eventsSeen));
    }
}

/** Notices Minecraft arriving, restarting or leaving. */
void track_connection(Link& link, bool alive) noexcept {
    const std::uint32_t pid = link.mc_pid();
    const bool restarted = alive && pid != 0 && pid != g_exchange.lastMcPid;
    if (alive && (!g_exchange.mcWasAlive || restarted)) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::info,
                          "ev=skycraft stage=connect result=ok pid=%u",
                          pid);
        link.reset_overlay();
        ++g_exchange.teleportSeq;
    } else if (!alive && g_exchange.mcWasAlive) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=skycraft stage=connect result=lost");
        g_shared.hands.store(false, std::memory_order_relaxed);
    }
    if (alive) {
        g_exchange.lastMcPid = pid;
    }
    g_exchange.mcWasAlive = alive;
}

} // namespace

Shared& shared() noexcept {
    return g_shared;
}

bool takes_input() noexcept {
    if (!g_installed.load(std::memory_order_acquire)
        || !g_shared.enabled.load(std::memory_order_relaxed)
        || !g_shared.mcInWorld.load(std::memory_order_relaxed)) {
        return false;
    }
    const Link& link = Link::get();
    return link.valid() && link.mc_alive() && !core::ui::runtime::snapshot().visible;
}

void release_all() noexcept {
    Link::get().push_input(proto::kInReleaseAll, 0);
}

void initialize(void* module) noexcept {
    settings::initialize(module);
}

void shutdown() noexcept {
    settings::shutdown();
}

void install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return;
    }
    const settings::Settings configured = settings::get();
    g_shared.mirror.store(configured.mirror, std::memory_order_relaxed);
    g_shared.handsKey.store(configured.handsKey, std::memory_order_relaxed);
    if (!configured.enabled) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=skycraft stage=install result=skip reason=disabled");
        return;
    }
    if (!Link::get().create()) {
        return;
    }
    g_exchange = Exchange{};
    g_shared.enabled.store(true, std::memory_order_relaxed);
    g_installed.store(true, std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=skycraft stage=install result=ok");
    launcher::start();
}

void uninstall() noexcept {
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    g_shared.enabled.store(false, std::memory_order_relaxed);
    g_shared.mcInWorld.store(false, std::memory_order_relaxed);
    g_shared.hands.store(false, std::memory_order_relaxed);
    release_all();
    Link::get().close();
}

void frame() noexcept {
    if (!g_installed.load(std::memory_order_acquire)) {
        return;
    }
    Link& link = Link::get();
    link.heartbeat();
    proto::McState mc{};
    const bool alive = link.mc_alive();
    const bool haveMc = alive && link.read_mc_state(mc);
    track_connection(link, alive);

    const bool inWorld = haveMc && (mc.flags & proto::kMcInWorld) != 0U;
    const bool screenOpen = haveMc && (mc.flags & proto::kMcScreenOpen) != 0U;
    if (screenOpen && !g_shared.mcScreenOpen.load(std::memory_order_relaxed)) {
        g_shared.cursorX.store(g_shared.viewportW.load(std::memory_order_relaxed) / 2,
                               std::memory_order_relaxed);
        g_shared.cursorY.store(g_shared.viewportH.load(std::memory_order_relaxed) / 2,
                               std::memory_order_relaxed);
    }
    g_shared.mcInWorld.store(inWorld, std::memory_order_relaxed);
    g_shared.mcScreenOpen.store(screenOpen, std::memory_order_relaxed);
    g_shared.guiScale.store(haveMc ? static_cast<int>(mc.guiScale) : 0, std::memory_order_relaxed);

    teleport::CameraPose camera{};
    const bool haveCamera = teleport::camera_pose(camera);
    const player::position::Snapshot body = player::position::snapshot();
    const bool inGame = haveCamera && body.present;
    g_shared.inGame.store(inGame, std::memory_order_relaxed);

    proto::SkyState state{};
    state.flags = proto::kSkyHostDrives | (inGame ? proto::kSkyInGame : proto::kSkyLoading);
    if (core::ui::runtime::snapshot().visible || !client::input::game_focused()) {
        state.flags |= proto::kSkyMenuOpen;
    }
    if (inGame) {
        const bool mirror = g_shared.mirror.load(std::memory_order_relaxed);
        teleport::Vector feet = body.position;
        const teleport::Vector apart{camera.position[0] - body.position[0],
                                     camera.position[1] - body.position[1],
                                     camera.position[2] - body.position[2]};
        if (length_of(apart) <= kThirdPersonDistance) {
            // First person: Minecraft's eye goes exactly where Destiny's camera is, so its
            // crosshair and Destiny's point at the same block.
            feet = camera.position;
            feet[teleport::kVerticalLane] -= static_cast<float>(kMcEyeHeight);
        }
        const McVec position = to_mc(feet, mirror);
        state.posX = position.x;
        state.posY = position.y;
        state.posZ = position.z;
        look_from(camera.forward, mirror, state.yaw, state.pitch);
    }
    state.worldId = kWorldId;
    state.collisionEpoch = kCollisionEpoch;
    state.teleportSeq = g_exchange.teleportSeq;
    state.viewportW =
        static_cast<std::uint32_t>(g_shared.viewportW.load(std::memory_order_relaxed));
    state.viewportH =
        static_cast<std::uint32_t>(g_shared.viewportH.load(std::memory_order_relaxed));
    state.gameHour = kGameHour;
    link.write_sky_state(state);

    g_shared.crosshair.store(inWorld && inGame && !screenOpen && mc.cameraMode == 0,
                             std::memory_order_relaxed);
    drain_events(link);
}

void present(ID3D11Device* device,
             ID3D11DeviceContext* context,
             ID3D11RenderTargetView* target) noexcept {
    if (!g_installed.load(std::memory_order_acquire) || device == nullptr || context == nullptr
        || target == nullptr) {
        return;
    }
    Link& link = Link::get();
    // Present runs while the game is paused too, so the heartbeat is kept here as well.
    link.heartbeat();
    if (!link.mc_alive()) {
        return;
    }
    teleport::CameraPose camera{};
    const bool haveCamera =
        g_shared.inGame.load(std::memory_order_relaxed) && teleport::camera_pose(camera);
    blocks::draw(device, context, target, haveCamera ? &camera : nullptr);
    if (g_shared.mcInWorld.load(std::memory_order_relaxed)) {
        overlay::draw(device, context, target);
    }
}

void release_device() noexcept {
    blocks::release();
    overlay::release();
}

} // namespace sunrise::client::hooks::skycraft
