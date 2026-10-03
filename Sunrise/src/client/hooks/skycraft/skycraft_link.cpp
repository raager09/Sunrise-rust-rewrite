/**
 * The SkyCraft shared-memory link, host side. Ported from SkyCraft's Skyrim plugin (MIT,
 * Copyright (c) 2026 chasmlol); the byte layout is the vendored protocol header.
 */

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <immintrin.h>
#include <sddl.h>

#include "../../../core/logging/log.h"
#include "link.h"

namespace sunrise::client::hooks::skycraft {
namespace {

template <class T> [[nodiscard]] std::atomic_ref<T> atomic(T& value) noexcept {
    return std::atomic_ref<T>(value);
}

/** Minecraft counts as gone once its heartbeat is older than this. */
constexpr std::uint64_t kMcTimeoutMs = 3000;
/** Seqlock reads give up after this many torn attempts and keep the last copy. */
constexpr int kSeqlockAttempts = 64;
/** A collision or render message header is two words. */
constexpr std::uint64_t kMessageHeaderBytes = sizeof(proto::ColMsgHeader);
/** Messages start 8-byte aligned. */
constexpr std::uint64_t kMessageAlignMask = 7;

/**
 * Who may open the mapping: this Windows user, the system and administrators, at normal
 * integrity. A Destiny run as administrator would otherwise make it administrators-only, and a
 * Minecraft started normally could never connect.
 * @return A descriptor to free with LocalFree, or null for the defaults.
 */
[[nodiscard]] PSECURITY_DESCRIPTOR shared_with_this_user() noexcept {
    wchar_t sddl[512] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE) {
        alignas(TOKEN_USER) std::uint8_t buffer[256]{};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenUser, buffer, static_cast<DWORD>(sizeof(buffer)), &size)
            != FALSE) {
            LPWSTR sid = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid)
                != FALSE) {
                (void)wcscat_s(sddl, L"(A;;GA;;;");
                (void)wcscat_s(sddl, sid);
                (void)wcscat_s(sddl, L")");
                LocalFree(sid);
            }
        }
        CloseHandle(token);
    }
    (void)wcscat_s(sddl, L"S:(ML;;NW;;;ME)");
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor, nullptr)
        == FALSE) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=skycraft stage=link reason=security_descriptor error=%lu",
                          GetLastError());
        return nullptr;
    }
    return descriptor;
}

} // namespace

Link& Link::get() noexcept {
    static Link link;
    return link;
}

bool Link::create() noexcept {
    if (base_ != nullptr) {
        return true;
    }
    constexpr std::uint64_t size = proto::kMappingBytes;
    SECURITY_ATTRIBUTES access{sizeof(access), shared_with_this_user(), FALSE};
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE,
                                  access.lpSecurityDescriptor != nullptr ? &access : nullptr,
                                  PAGE_READWRITE,
                                  static_cast<DWORD>(size >> 32U),
                                  static_cast<DWORD>(size & 0xFFFFFFFFU),
                                  proto::kMappingName);
    const DWORD created = GetLastError();
    if (access.lpSecurityDescriptor != nullptr) {
        LocalFree(access.lpSecurityDescriptor);
    }
    if (mapping_ == nullptr) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::error,
                          "ev=skycraft stage=link reason=create_mapping error=%lu",
                          created);
        return false;
    }
    base_ = static_cast<std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (base_ == nullptr) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::error,
                          "ev=skycraft stage=link reason=map_view error=%lu",
                          GetLastError());
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    // A Minecraft still holding the mapping from an earlier run keeps it alive, so every region
    // this side owns starts again from a known state.
    std::memset(base_ + proto::kOffSkyState, 0, sizeof(proto::SkyState));
    std::memset(base_ + proto::kOffWaterGrid, 0, sizeof(proto::WaterGrid));
    std::memset(base_ + proto::kOffOverlayCtl, 0, 0x100);
    std::memset(base_ + proto::kOffInputRing, 0, proto::kInputRingDataOff);
    std::memset(base_ + proto::kOffCollisionRing, 0, proto::kColRingDataOff);
    std::memset(base_ + proto::kOffActorTable, 0, sizeof(proto::ActorTable));
    std::memset(base_ + proto::kOffEventRing, 0, proto::kEventRingDataOff);
    std::memset(base_ + proto::kOffWorldEntities, 0, sizeof(proto::WorldEntities));
    std::memset(base_ + proto::kOffRenderRing, 0, proto::kRenRingDataOff);
    auto* header = at<proto::Header>(proto::kOffHeader);
    header->version = proto::kVersion;
    header->skyrimPid = GetCurrentProcessId();
    header->skyrimHeartbeatMs = GetTickCount64();
    atomic(header->magic).store(proto::kMagic, std::memory_order_release);
    overlayFront_ = 2;

    core::log::writef(core::log::Channel::client,
                      core::log::Level::info,
                      "ev=skycraft stage=link result=ok mapping=%s size_mb=%llu",
                      created == ERROR_ALREADY_EXISTS ? "reused" : "created",
                      static_cast<unsigned long long>(size >> 20U));
    return true;
}

void Link::close() noexcept {
    if (base_ != nullptr) {
        atomic(at<proto::Header>(proto::kOffHeader)->skyrimHeartbeatMs)
            .store(0, std::memory_order_release);
        UnmapViewOfFile(base_);
        base_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

bool Link::mc_alive() const noexcept {
    if (base_ == nullptr) {
        return false;
    }
    const std::uint64_t last =
        atomic(at<proto::Header>(proto::kOffHeader)->mcHeartbeatMs).load(std::memory_order_acquire);
    return last != 0 && GetTickCount64() - last < kMcTimeoutMs;
}

std::uint32_t Link::mc_pid() const noexcept {
    if (base_ == nullptr) {
        return 0;
    }
    return atomic(at<proto::Header>(proto::kOffHeader)->mcPid).load(std::memory_order_acquire);
}

void Link::heartbeat() noexcept {
    if (base_ != nullptr) {
        atomic(at<proto::Header>(proto::kOffHeader)->skyrimHeartbeatMs)
            .store(GetTickCount64(), std::memory_order_release);
    }
}

void Link::write_sky_state(const proto::SkyState& state) noexcept {
    if (base_ == nullptr) {
        return;
    }
    auto* destination = at<proto::SkyState>(proto::kOffSkyState);
    auto sequence = atomic(destination->seq);
    const std::uint32_t before = sequence.load(std::memory_order_relaxed);
    sequence.store(before + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(reinterpret_cast<std::uint8_t*>(destination) + sizeof(std::uint32_t),
                reinterpret_cast<const std::uint8_t*>(&state) + sizeof(std::uint32_t),
                sizeof(proto::SkyState) - sizeof(std::uint32_t));
    sequence.store(before + 2, std::memory_order_release);
}

bool Link::read_mc_state(proto::McState& output) const noexcept {
    if (base_ == nullptr) {
        return false;
    }
    auto* source = at<proto::McState>(proto::kOffMcState);
    auto sequence = atomic(source->seq);
    for (int attempt = 0; attempt < kSeqlockAttempts; ++attempt) {
        const std::uint32_t before = sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            _mm_pause();
            continue;
        }
        std::memcpy(&output, source, sizeof(proto::McState));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sequence.load(std::memory_order_relaxed) == before) {
            return true;
        }
    }
    return false;
}

void Link::push_input(proto::InputType type,
                      std::uint16_t code,
                      std::int32_t a,
                      std::int32_t b,
                      std::int32_t c) noexcept {
    if (base_ == nullptr) {
        return;
    }
    std::uint8_t* const ring = base_ + proto::kOffInputRing;
    auto& headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingHeadOff);
    auto& tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingTailOff);
    AcquireSRWLockExclusive(&inputLock_);
    const std::uint64_t head = atomic(headRef).load(std::memory_order_relaxed);
    const std::uint64_t tail = atomic(tailRef).load(std::memory_order_acquire);
    if (head - tail < proto::kInputRingEntries) {
        auto* entry = reinterpret_cast<proto::InputEvent*>(ring + proto::kInputRingDataOff)
                      + (head & (proto::kInputRingEntries - 1U));
        *entry = proto::InputEvent{static_cast<std::uint16_t>(type), code, a, b, c};
        atomic(headRef).store(head + 1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&inputLock_);
}

bool Link::pop_event(proto::McEvent& output) noexcept {
    if (base_ == nullptr) {
        return false;
    }
    std::uint8_t* const ring = base_ + proto::kOffEventRing;
    auto& headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingHeadOff);
    auto& tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingTailOff);
    const std::uint64_t head = atomic(headRef).load(std::memory_order_acquire);
    std::uint64_t tail = atomic(tailRef).load(std::memory_order_relaxed);
    if (tail >= head) {
        return false;
    }
    if (head - tail > proto::kEventRingEntries) {
        tail = head - proto::kEventRingEntries;
    }
    output = reinterpret_cast<const proto::McEvent*>(
        ring + proto::kEventRingDataOff)[tail & (proto::kEventRingEntries - 1U)];
    atomic(tailRef).store(tail + 1, std::memory_order_release);
    return true;
}

void Link::drain_render(RenderSink sink, void* context, std::uint64_t maximumBytes) noexcept {
    if (base_ == nullptr || sink == nullptr) {
        return;
    }
    std::uint8_t* const ring = base_ + proto::kOffRenderRing;
    auto& headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingHeadOff);
    auto& tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingTailOff);
    const std::uint64_t head = atomic(headRef).load(std::memory_order_acquire);
    std::uint64_t tail = atomic(tailRef).load(std::memory_order_relaxed);
    const std::uint8_t* const data = ring + proto::kRenRingDataOff;
    constexpr std::uint64_t size = proto::kRenRingDataBytes;
    std::uint64_t done = 0;
    while (tail < head && done < maximumBytes) {
        const std::uint64_t position = tail % size;
        const auto* header = reinterpret_cast<const proto::ColMsgHeader*>(data + position);
        if (header->type == proto::kRenPad) {
            tail += size - position;
            continue;
        }
        const std::uint64_t messageBytes =
            (kMessageHeaderBytes + header->payloadBytes + kMessageAlignMask) & ~kMessageAlignMask;
        if (position + messageBytes > size) {
            // A message never wraps; one that would is a torn header, so the rest is dropped.
            tail = head;
            break;
        }
        sink(context, header->type, data + position + kMessageHeaderBytes, header->payloadBytes);
        tail += messageBytes;
        done += messageBytes;
    }
    atomic(tailRef).store(tail, std::memory_order_release);
}

bool Link::acquire_overlay_frame() noexcept {
    if (base_ == nullptr) {
        return false;
    }
    auto& state = at<proto::OverlayCtl>(proto::kOffOverlayCtl)->state;
    if ((atomic(state).load(std::memory_order_acquire) & proto::kOverlayDirty) == 0U) {
        return false;
    }
    const std::uint32_t old = atomic(state).exchange(overlayFront_, std::memory_order_acq_rel);
    overlayFront_ = old & 3U;
    return true;
}

void Link::reset_overlay() noexcept {
    if (base_ == nullptr) {
        return;
    }
    atomic(at<proto::OverlayCtl>(proto::kOffOverlayCtl)->state).store(0, std::memory_order_release);
    overlayFront_ = 2;
}

const std::uint8_t* Link::front_pixels() const noexcept {
    return base_ + proto::kOffOverlayPixels + proto::kOverlaySlotBytes * overlayFront_;
}

const proto::OverlaySlotHdr* Link::front_header() const noexcept {
    return at<proto::OverlaySlotHdr>(proto::kOffOverlaySlotHdr
                                     + sizeof(proto::OverlaySlotHdr) * overlayFront_);
}

} // namespace sunrise::client::hooks::skycraft
