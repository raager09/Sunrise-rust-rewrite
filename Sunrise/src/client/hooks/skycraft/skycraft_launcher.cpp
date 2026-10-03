/**
 * Starts SkyCraft's Minecraft next to Destiny. The Fabric mod then waits for the link, hides its
 * window, opens its world by itself and quits again once this process is gone. By default this
 * is the Prism Launcher instance SkyCraft unpacks to %LOCALAPPDATA%\SkyCraft.
 */

#include <Windows.h>

#include <array>
#include <cwchar>

#include "../../../core/logging/log.h"
#include "internal.h"
#include "skycraft_settings_store.h"

namespace sunrise::client::hooks::skycraft::launcher {
namespace {

/** A Minecraft with the SkyCraft mod holds this mutex while it runs. */
constexpr wchar_t kRunningMutex[] = L"Local\\SkyCraft_v1_minecraft";

/** Where a launcher is looked for when none is configured, in order. */
constexpr std::array<const wchar_t*, 3> kPrismCandidates{
    L"%LOCALAPPDATA%\\SkyCraft\\Prism\\prismlauncher.exe",
    L"%LOCALAPPDATA%\\Programs\\PrismLauncher\\prismlauncher.exe",
    L"%ProgramFiles%\\PrismLauncher\\prismlauncher.exe",
};

using PathText = std::array<wchar_t, MAX_PATH * 2>;

[[nodiscard]] bool expand(const wchar_t* text, PathText& output) noexcept {
    const DWORD length =
        ExpandEnvironmentStringsW(text, output.data(), static_cast<DWORD>(output.size()));
    return length != 0 && length <= output.size();
}

[[nodiscard]] bool exists(const PathText& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.data());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/** @return True when the path ends in a batch-file extension. */
[[nodiscard]] bool is_script(const PathText& path) noexcept {
    const wchar_t* const dot = std::wcsrchr(path.data(), L'.');
    return dot != nullptr && (_wcsicmp(dot, L".bat") == 0 || _wcsicmp(dot, L".cmd") == 0);
}

/** Picks the configured launcher, or the first Prism found. */
[[nodiscard]] bool choose(const settings::Settings& configured, PathText& output) noexcept {
    if (configured.launcher[0] != L'\0') {
        return expand(configured.launcher.data(), output) && exists(output);
    }
    for (const wchar_t* candidate : kPrismCandidates) {
        if (expand(candidate, output) && exists(output)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool minecraft_running() noexcept {
    const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kRunningMutex);
    if (mutex == nullptr) {
        return false;
    }
    CloseHandle(mutex);
    return true;
}

void start() noexcept {
    const settings::Settings configured = settings::get();
    if (!configured.startMinecraft) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=skycraft stage=launch result=skip reason=start_minecraft_off");
        return;
    }
    if (minecraft_running()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=skycraft stage=launch result=skip reason=already_running");
        return;
    }
    PathText program{};
    if (!choose(configured, program)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=skycraft stage=launch result=fail reason=no_launcher "
                         "hint=install_skycraft_or_set_launcher");
        return;
    }
    PathText directory = program;
    if (wchar_t* const slash = std::wcsrchr(directory.data(), L'\\'); slash != nullptr) {
        *slash = L'\0';
    }
    const bool script = is_script(program);
    std::array<wchar_t, MAX_PATH * 4> command{};
    const int written = script ? swprintf_s(command.data(),
                                            command.size(),
                                            L"cmd.exe /c \"\"%s\" %s\"",
                                            program.data(),
                                            configured.arguments.data())
                               : swprintf_s(command.data(),
                                            command.size(),
                                            L"\"%s\" %s",
                                            program.data(),
                                            configured.arguments.data());
    if (written <= 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=skycraft stage=launch result=fail reason=command_length");
        return;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr,
                       command.data(),
                       nullptr,
                       nullptr,
                       FALSE,
                       script ? CREATE_NO_WINDOW : 0,
                       nullptr,
                       directory.data(),
                       &startup,
                       &process)
        == FALSE) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=skycraft stage=launch result=fail reason=create_process error=%lu",
                          GetLastError());
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=skycraft stage=launch result=ok");
}

} // namespace sunrise::client::hooks::skycraft::launcher
