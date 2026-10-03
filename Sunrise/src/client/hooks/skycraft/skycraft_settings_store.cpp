/**
 * The SkyCraft bridge configuration, `Sunrise\skycraft.json`. It is read when the client starts
 * and is written back only to create it with its defaults, so the player can edit it by hand.
 */

#include "skycraft_settings_store.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"

namespace sunrise::client::hooks::skycraft::settings {
namespace {

constexpr std::wstring_view kFileSuffix = L"\\skycraft.json";
/** Two paths of UTF-8 plus a few scalars. */
constexpr std::size_t kFileCapacity = 8192;
constexpr std::size_t kScalarCapacity = 32;
constexpr std::uint32_t kMaximumVirtualKey = 254;
constexpr wchar_t kDefaultArguments[] = L"--launch SkyCraft";

SRWLOCK g_lock{SRWLOCK_INIT};
Settings g_settings{};
core::path::Buffer g_path{};
bool g_pathResolved{};

/** @return Settings with the default argument line filled in. */
[[nodiscard]] Settings defaults() noexcept {
    Settings value{};
    (void)wcscpy_s(value.arguments.data(), value.arguments.size(), kDefaultArguments);
    return value;
}

/** Finds the text after one key's colon, with leading blanks skipped. */
[[nodiscard]] bool
value_for(std::string_view text, std::string_view key, std::size_t& at) noexcept {
    const std::size_t found = text.find(key);
    if (found == std::string_view::npos) {
        return false;
    }
    const std::size_t colon = text.find(':', found + key.size());
    if (colon == std::string_view::npos) {
        return false;
    }
    at = colon + 1;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) {
        ++at;
    }
    return at < text.size();
}

/** @return The raw scalar after one key, up to the next separator. */
[[nodiscard]] bool scalar_for(std::string_view text,
                              std::string_view key,
                              std::array<char, kScalarCapacity>& output) noexcept {
    std::size_t at = 0;
    if (!value_for(text, key, at)) {
        return false;
    }
    std::size_t length = 0;
    while (at + length < text.size() && length + 1 < output.size()) {
        const char character = text[at + length];
        if (character == ',' || character == '}' || character == '\n' || character == '\r') {
            break;
        }
        output[length] = character;
        ++length;
    }
    output[length] = '\0';
    return length != 0;
}

/** Reads one JSON string value as UTF-16. Handles the escapes a Windows path needs. */
[[nodiscard]] bool string_for(std::string_view text,
                              std::string_view key,
                              std::array<wchar_t, kTextCapacity>& output) noexcept {
    std::size_t at = 0;
    if (!value_for(text, key, at) || text[at] != '"') {
        return false;
    }
    std::array<char, kTextCapacity * 3> utf8{};
    std::size_t length = 0;
    for (std::size_t index = at + 1; index < text.size(); ++index) {
        char character = text[index];
        if (character == '"') {
            break;
        }
        if (character == '\\' && index + 1 < text.size()) {
            ++index;
            switch (text[index]) {
            case 'n':
                character = '\n';
                break;
            case 't':
                character = '\t';
                break;
            default:
                character = text[index];
                break;
            }
        }
        if (length + 1 >= utf8.size()) {
            return false;
        }
        utf8[length] = character;
        ++length;
    }
    output = {};
    if (length == 0) {
        return true;
    }
    const int written = MultiByteToWideChar(CP_UTF8,
                                            0,
                                            utf8.data(),
                                            static_cast<int>(length),
                                            output.data(),
                                            static_cast<int>(output.size() - 1));
    return written > 0;
}

/** Layers the document over the defaults. A missing or malformed key keeps its default. */
void parse(std::string_view text, Settings& output) noexcept {
    std::array<char, kScalarCapacity> scalar{};
    if (scalar_for(text, "\"enabled\"", scalar)) {
        output.enabled = std::string_view(scalar.data()).starts_with("true");
    }
    if (scalar_for(text, "\"start_minecraft\"", scalar)) {
        output.startMinecraft = std::string_view(scalar.data()).starts_with("true");
    }
    if (scalar_for(text, "\"mirror\"", scalar)) {
        output.mirror = std::string_view(scalar.data()).starts_with("true");
    }
    if (scalar_for(text, "\"hands_key\"", scalar)) {
        const auto key = static_cast<std::uint32_t>(std::strtoul(scalar.data(), nullptr, 0));
        if (key != 0 && key <= kMaximumVirtualKey) {
            output.handsKey = key;
        }
    }
    (void)string_for(text, "\"launcher\"", output.launcher);
    std::array<wchar_t, kTextCapacity> arguments{};
    if (string_for(text, "\"arguments\"", arguments)) {
        output.arguments = arguments;
    }
}

/** Appends one UTF-16 string as an escaped JSON string body. */
[[nodiscard]] bool append_escaped(const wchar_t* value,
                                  char* output,
                                  std::size_t capacity,
                                  std::size_t& length) noexcept {
    std::array<char, kTextCapacity * 3> utf8{};
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    if (bytes <= 0) {
        return false;
    }
    for (int index = 0; index + 1 < bytes; ++index) {
        const char character = utf8[static_cast<std::size_t>(index)];
        const bool escape = character == '"' || character == '\\';
        if (length + (escape ? 2U : 1U) >= capacity) {
            return false;
        }
        if (escape) {
            output[length++] = '\\';
        }
        output[length++] = character;
    }
    output[length] = '\0';
    return true;
}

/** Writes the whole document. */
[[nodiscard]] bool store(const Settings& settings) noexcept {
    std::array<char, kTextCapacity * 6> launcher{};
    std::array<char, kTextCapacity * 6> arguments{};
    std::size_t launcherLength = 0;
    std::size_t argumentsLength = 0;
    if (!append_escaped(settings.launcher.data(), launcher.data(), launcher.size(), launcherLength)
        || !append_escaped(
            settings.arguments.data(), arguments.data(), arguments.size(), argumentsLength)) {
        return false;
    }
    std::array<char, kFileCapacity> document{};
    const int size = std::snprintf(document.data(),
                                   document.size(),
                                   "{\n  \"enabled\": %s,\n"
                                   "  \"start_minecraft\": %s,\n"
                                   "  \"launcher\": \"%s\",\n"
                                   "  \"arguments\": \"%s\",\n"
                                   "  \"hands_key\": %u,\n"
                                   "  \"mirror\": %s\n}\n",
                                   settings.enabled ? "true" : "false",
                                   settings.startMinecraft ? "true" : "false",
                                   launcher.data(),
                                   arguments.data(),
                                   static_cast<unsigned>(settings.handsKey),
                                   settings.mirror ? "true" : "false");
    if (size <= 0 || static_cast<std::size_t>(size) >= document.size()) {
        return false;
    }
    const HANDLE file = CreateFileW(
        g_path.chars.data(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool complete =
        WriteFile(file, document.data(), static_cast<DWORD>(size), &written, nullptr) != FALSE
        && written == static_cast<DWORD>(size);
    complete = CloseHandle(file) != FALSE && complete;
    return complete;
}

/** @return True when the file existed and was read into the settings. */
[[nodiscard]] bool load() noexcept {
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::array<char, kFileCapacity> buffer{};
    DWORD read = 0;
    const bool readOk =
        ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &read, nullptr)
        != FALSE;
    (void)CloseHandle(file);
    if (readOk && read != 0) {
        parse(std::string_view(buffer.data(), read), g_settings);
    }
    return true;
}

} // namespace

void initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_settings = defaults();
    g_pathResolved =
        core::path::artifact_directory(module, g_path) && core::path::append(g_path, kFileSuffix);
    if (!g_pathResolved) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=skycraft stage=settings result=fail reason=path");
    } else if (!load() && !store(g_settings)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=skycraft stage=settings result=fail reason=write_defaults");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_settings = Settings{};
    g_path = core::path::Buffer{};
    g_pathResolved = false;
    ReleaseSRWLockExclusive(&g_lock);
}

Settings get() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Settings snapshot = g_settings;
    ReleaseSRWLockShared(&g_lock);
    return snapshot;
}

} // namespace sunrise::client::hooks::skycraft::settings
