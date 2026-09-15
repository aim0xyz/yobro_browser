#include "spike/BridgePolicyStore.hpp"

#include "yobro/core/Json.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace yobro::spike {
namespace {

constexpr qint64 maximumPolicyBytes = 64 * 1024;

QString nativePath(const std::filesystem::path &path) {
    return QString::fromStdString(path.string());
}

std::runtime_error fileError(std::string_view action, const QFileDevice &file) {
    return std::runtime_error(
        std::string(action) + ": " + file.errorString().toStdString()
    );
}

} // namespace

BridgePolicyStore::BridgePolicyStore(std::filesystem::path path)
    : path_(std::filesystem::absolute(std::move(path)).lexically_normal()) {
    if (path_.filename().empty())
        throw std::invalid_argument("A bridge-policy file path is required.");
}

const std::filesystem::path &BridgePolicyStore::path() const noexcept {
    return path_;
}

BridgePolicyLoadResult BridgePolicyStore::load() const {
    const QString source = nativePath(path_);
    const QFileInfo info(source);
    if (!info.exists() && !info.isSymLink())
        return {};
    if (info.isSymLink())
        return quarantine("symbolic links are not accepted");
    if (!info.isFile())
        return quarantine("the policy path is not a regular file");

    QFile file(source);
    if (!file.open(QIODevice::ReadOnly))
        return quarantine("the policy could not be opened: " + file.errorString().toStdString());
    if (file.size() > maximumPolicyBytes) {
        file.close();
        return quarantine("the policy exceeds 64 KiB");
    }
    const QByteArray bytes = file.read(maximumPolicyBytes + 1);
    if (bytes.size() > maximumPolicyBytes || !file.atEnd()) {
        file.close();
        return quarantine("the policy exceeds 64 KiB");
    }
    if (file.error() != QFileDevice::NoError) {
        const std::string reason = file.errorString().toStdString();
        file.close();
        return quarantine("the policy could not be read: " + reason);
    }
    file.close();

    try {
        const core::Json root = core::Json::parse(
            std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
            {
                .maxBytes = static_cast<std::size_t>(maximumPolicyBytes),
                .maxDepth = 8,
                .rejectDuplicateKeys = true,
            }
        );
        if (!root.isObject())
            return quarantine("the JSON root is not an object");
        const core::Json *allows = root.find("allowsLibraryAccess");
        if (!allows || !allows->isBoolean())
            return quarantine("allowsLibraryAccess is missing or is not a boolean");
        return {.allowsLibraryAccess = allows->asBoolean()};
    } catch (const std::exception &error) {
        return quarantine("the JSON is invalid: " + std::string(error.what()));
    }
}

void BridgePolicyStore::save(bool allowsLibraryAccess) const {
    const QString target = nativePath(path_);
    const QFileInfo existing(target);
    if (existing.isSymLink())
        throw std::runtime_error("Refusing to replace a symbolic-link bridge policy.");
    if (existing.exists() && !existing.isFile())
        throw std::runtime_error("The bridge-policy path is not a regular file.");

    const QString parent = nativePath(path_.parent_path());
    if (!QDir().mkpath(parent) || !QFileInfo(parent).isDir())
        throw std::runtime_error("Could not create the bridge-policy directory.");

    core::Json::Object object;
    object.emplace("allowsLibraryAccess", core::Json(allowsLibraryAccess));
    const std::string encoded = core::Json(std::move(object)).serialize();

    QSaveFile file(target);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        throw fileError("Could not open the bridge policy for an atomic write", file);
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        throw fileError("Could not secure the bridge-policy temporary file", file);
    }
    if (file.write(encoded.data(), static_cast<qint64>(encoded.size())) != static_cast<qint64>(encoded.size())) {
        file.cancelWriting();
        throw fileError("Could not write the complete bridge policy", file);
    }
    if (!file.commit())
        throw fileError("Could not commit the bridge policy", file);

#if !defined(_WIN32)
    std::error_code permissionError;
    const std::filesystem::perms actual = std::filesystem::status(path_, permissionError).permissions();
    const std::filesystem::perms expected = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    const std::filesystem::perms forbidden = std::filesystem::perms::owner_exec
        | std::filesystem::perms::group_all
        | std::filesystem::perms::others_all;
    if (permissionError || (actual & expected) != expected || (actual & forbidden) != std::filesystem::perms::none) {
        QFile::remove(target);
        throw std::runtime_error("The committed bridge policy was not owner-only and was removed.");
    }
#endif
}

std::optional<std::string> BridgePolicyStore::persistAndApply(
    bool allowsLibraryAccess,
    const Apply &apply
) const {
    try {
        save(allowsLibraryAccess);
    } catch (const std::exception &error) {
        return error.what();
    }
    apply(allowsLibraryAccess);
    return std::nullopt;
}

BridgePolicyLoadResult BridgePolicyStore::quarantine(std::string reason) const {
    BridgePolicyLoadResult result;
    result.problem = "Bridge policy disabled: " + std::move(reason) + ".";

    const QString source = nativePath(path_);
    for (int suffix = 0; suffix < 10'000; ++suffix) {
        std::filesystem::path candidate = path_;
        candidate += suffix == 0
            ? ".corrupt"
            : ".corrupt-" + std::to_string(suffix);
        const QString destination = nativePath(candidate);
        const QFileInfo destinationInfo(destination);
        if (destinationInfo.exists() || destinationInfo.isSymLink())
            continue;
        if (QFile::rename(source, destination)) {
            result.quarantinedPath = candidate;
            *result.problem += " Quarantined as " + candidate.filename().string() + ".";
        } else {
            *result.problem += " The invalid file could not be quarantined.";
        }
        return result;
    }
    *result.problem += " No quarantine filename was available.";
    return result;
}

} // namespace yobro::spike
