#include "yobro/core/WebKitImport.hpp"

#include "yobro/core/Json.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace yobro::core {
namespace {

constexpr std::size_t maxFileBytes = 16 * 1024 * 1024;
constexpr std::string_view importFiles[] = {"history.json", "bookmarks.json", "session.json"};
std::atomic_uint64_t stagingCounter = 0;

struct SourceFile {
    std::string name;
    std::string bytes;
};

struct SourceSnapshot {
    std::filesystem::path directory;
    std::vector<SourceFile> files;
};

std::string read(const std::filesystem::path &path) {
    if (!std::filesystem::exists(path)) return {};
    if (!std::filesystem::is_regular_file(path))
        throw std::runtime_error("WebKit import source entry is not a regular file.");
    const auto size = std::filesystem::file_size(path);
    if (size > maxFileBytes) throw std::runtime_error("WebKit import file exceeds the size limit.");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read WebKit import file: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::uint64_t fingerprint(const std::string_view bytes) noexcept {
    std::uint64_t value = 14695981039346656037ULL;
    for (const unsigned char byte : bytes) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

SourceSnapshot snapshot(const std::filesystem::path &source) {
    if (!std::filesystem::is_directory(source))
        throw std::runtime_error("WebKit import source is not a directory.");
    SourceSnapshot result{std::filesystem::weakly_canonical(source), {}};
    for (const auto name : importFiles)
        result.files.push_back({std::string(name), read(result.directory / name)});
    return result;
}

void verifyUnchanged(const SourceSnapshot &snapshot) {
    for (const SourceFile &file : snapshot.files) {
        if (read(snapshot.directory / file.name) != file.bytes)
            throw std::runtime_error("WebKit import source changed during import.");
    }
}

const std::string &bytesFor(const SourceSnapshot &snapshot, const std::string_view name) {
    for (const SourceFile &file : snapshot.files)
        if (file.name == name) return file.bytes;
    throw std::logic_error("Missing WebKit import snapshot entry.");
}

Json::Array array(const SourceSnapshot &snapshot, const std::string_view name) {
    const std::string &bytes = bytesFor(snapshot, name);
    if (bytes.empty()) return {};
    const Json value = Json::parse(bytes, {.maxBytes = maxFileBytes, .maxDepth = 48, .rejectDuplicateKeys = true});
    if (!value.isArray()) throw std::runtime_error("WebKit import library file must contain an array.");
    return value.asArray();
}

bool safeUrl(const Json &value) {
    if (!value.isObject()) return false;
    const Json *url = value.find("url");
    if (!url || !url->isString()) return false;
    const std::string &text = url->asString();
    const auto schemeEnd = text.find("://");
    if (schemeEnd == std::string::npos || schemeEnd == 0 || schemeEnd + 3 == text.size()) return false;
    const std::string_view scheme(text.data(), schemeEnd);
    if (scheme != "https" && scheme != "http") return false;
    const std::string_view authority(text.data() + schemeEnd + 3, text.size() - schemeEnd - 3);
    if (authority.empty() || authority.front() == '/' || authority.front() == '?' || authority.front() == '#') return false;
    for (const unsigned char character : text)
        if (character <= 0x20 || character == 0x7f) return false;
    return true;
}

Json::Array safeEntries(const Json::Array &input, std::size_t &rejected) {
    Json::Array result;
    for (const Json &value : input) {
        if (safeUrl(value)) result.push_back(value);
        else ++rejected;
    }
    return result;
}

Json::Array tabs(const SourceSnapshot &snapshot, std::size_t &rejected) {
    const std::string &bytes = bytesFor(snapshot, "session.json");
    if (bytes.empty()) return {};
    const Json root = Json::parse(bytes, {.maxBytes = maxFileBytes, .maxDepth = 48, .rejectDuplicateKeys = true});
    if (!root.isObject()) throw std::runtime_error("WebKit session file must contain an object.");
    const Json *values = root.find("tabs");
    return values && values->isArray() ? safeEntries(values->asArray(), rejected) : Json::Array{};
}

void writeFile(const std::filesystem::path &path, const Json &value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Could not create Chromium import staging file.");
    output << value.serialize();
    output.close();
    if (!output) throw std::runtime_error("Could not write Chromium import staging file.");
#if !defined(_WIN32)
    std::filesystem::permissions(path, std::filesystem::perms::owner_read
        | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace);
#endif
}

std::filesystem::path uniqueSibling(const std::filesystem::path &destination, const std::string_view suffix) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto serial = stagingCounter.fetch_add(1, std::memory_order_relaxed);
    return destination.parent_path() / (destination.filename().string() + "." + std::string(suffix)
        + "." + std::to_string(tick) + "." + std::to_string(serial));
}

void publish(const std::filesystem::path &staging, const std::filesystem::path &destination) {
    const auto backup = uniqueSibling(destination, "webkit-import-backup");
    const bool hadDestination = std::filesystem::exists(destination);
    try {
        if (hadDestination) std::filesystem::rename(destination, backup);
        std::filesystem::rename(staging, destination);
        if (hadDestination) std::filesystem::remove_all(backup);
    } catch (...) {
        std::error_code ignored;
        if (!std::filesystem::exists(destination) && hadDestination)
            std::filesystem::rename(backup, destination, ignored);
        std::filesystem::remove_all(staging, ignored);
        throw std::runtime_error("Could not publish Chromium import transaction.");
    }
}

struct Data {
    Json::Array history;
    Json::Array bookmarks;
    Json::Array tabs;
    std::size_t rejected = 0;
};

Data load(const SourceSnapshot &snapshot) {
    Data data;
    data.history = safeEntries(array(snapshot, "history.json"), data.rejected);
    data.bookmarks = safeEntries(array(snapshot, "bookmarks.json"), data.rejected);
    data.tabs = tabs(snapshot, data.rejected);
    return data;
}

WebKitImportPreview counts(const Data &data) {
    return {data.history.size(), data.bookmarks.size(), data.tabs.size(), data.rejected};
}

Json journal(const SourceSnapshot &snapshot, const Data &data) {
    Json::Array files;
    for (const SourceFile &file : snapshot.files) {
        Json::Object entry;
        entry.emplace("name", Json(file.name));
        entry.emplace("bytes", Json(static_cast<std::int64_t>(file.bytes.size())));
        entry.emplace("fingerprint", Json(std::to_string(fingerprint(file.bytes))));
        files.emplace_back(std::move(entry));
    }
    Json::Object result;
    result.emplace("format", Json("yobro-webkit-import-v1"));
    result.emplace("source", Json(snapshot.directory.string()));
    result.emplace("sourceFiles", Json(std::move(files)));
    result.emplace("history", Json(static_cast<std::int64_t>(data.history.size())));
    result.emplace("bookmarks", Json(static_cast<std::int64_t>(data.bookmarks.size())));
    result.emplace("tabs", Json(static_cast<std::int64_t>(data.tabs.size())));
    result.emplace("rejected", Json(static_cast<std::int64_t>(data.rejected)));
    return Json(std::move(result));
}

} // namespace

WebKitImportPreview WebKitImport::preview(const std::filesystem::path &source) {
    const SourceSnapshot sourceSnapshot = snapshot(source);
    const Data data = load(sourceSnapshot);
    verifyUnchanged(sourceSnapshot);
    return counts(data);
}

WebKitImportPreview WebKitImport::commit(
    const std::filesystem::path &source,
    const std::filesystem::path &destination
) {
    const SourceSnapshot sourceSnapshot = snapshot(source);
    const auto normalizedDestination = std::filesystem::weakly_canonical(destination);
    if (normalizedDestination == sourceSnapshot.directory
        || normalizedDestination.string().starts_with(sourceSnapshot.directory.string() + "/")) {
        throw std::runtime_error("WebKit import destination must not be the source or its child.");
    }
    const Data data = load(sourceSnapshot);
    const auto staging = uniqueSibling(normalizedDestination, "webkit-import-staging");
    try {
        std::filesystem::create_directories(staging);
        writeFile(staging / "history.json", Json(data.history));
        writeFile(staging / "bookmarks.json", Json(data.bookmarks));
        Json::Object session;
        session.emplace("tabs", Json(data.tabs));
        writeFile(staging / "session.json", Json(std::move(session)));
        writeFile(staging / "webkit-import-journal.json", journal(sourceSnapshot, data));
        verifyUnchanged(sourceSnapshot);
        std::filesystem::create_directories(normalizedDestination.parent_path());
        publish(staging, normalizedDestination);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        throw;
    }
    return counts(data);
}

} // namespace yobro::core
