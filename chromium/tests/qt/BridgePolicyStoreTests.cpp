#include "spike/BridgePolicyStore.hpp"

#include "yobro/core/Json.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QTemporaryDir>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

void writeRaw(const std::filesystem::path &path, const QByteArray &bytes) {
    check(QDir().mkpath(QString::fromStdString(path.parent_path().string())), "Could not create a policy-test directory.");
    QFile file(QString::fromStdString(path.string()));
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Could not create a raw policy fixture.");
    check(file.write(bytes) == bytes.size(), "Could not write a complete raw policy fixture.");
    file.close();
}

void checkQuarantined(
    const std::filesystem::path &directory,
    std::string_view name,
    const QByteArray &bytes
) {
    const std::filesystem::path path = directory / std::string(name) / "bridge-policy.json";
    writeRaw(path, bytes);
    const yobro::spike::BridgePolicyLoadResult loaded = yobro::spike::BridgePolicyStore(path).load();
    check(!loaded.allowsLibraryAccess, "An invalid policy enabled library access.");
    check(loaded.problem.has_value(), "An invalid policy did not report a problem.");
    check(loaded.quarantinedPath.has_value(), "An invalid policy was not quarantined.");
    check(!std::filesystem::exists(path), "The invalid policy remained at the live path.");
    check(std::filesystem::exists(*loaded.quarantinedPath), "The quarantined policy file is missing.");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary(QStringLiteral("/tmp/yobro-policy-XXXXXX"));
    if (!temporary.isValid()) {
        std::cerr << "BRIDGE POLICY STORE TESTS FAIL: Could not create a temporary directory.\n";
        return 1;
    }

    try {
        const std::filesystem::path root = temporary.path().toStdString();
        const std::filesystem::path primaryPath = root / "primary" / "bridge-policy.json";
        yobro::spike::BridgePolicyStore primary(primaryPath);

        const auto missing = primary.load();
        check(!missing.allowsLibraryAccess, "A missing policy did not fail closed.");
        check(!missing.problem.has_value(), "A missing policy was treated as corrupt.");

        primary.save(true);
        const auto enabled = primary.load();
        check(enabled.allowsLibraryAccess && !enabled.problem, "A persisted true policy did not reload.");
        QFile encoded(QString::fromStdString(primaryPath.string()));
        check(encoded.open(QIODevice::ReadOnly), "Could not read the canonical policy.");
        const yobro::core::Json rootJson = yobro::core::Json::parse(encoded.readAll().toStdString());
        check(rootJson.isObject() && rootJson.asObject().size() == 1, "The policy writer emitted unexpected keys.");
        check(rootJson.find("allowsLibraryAccess") && rootJson.find("allowsLibraryAccess")->asBoolean(),
              "The policy writer emitted the wrong value.");
#if !defined(_WIN32)
        const auto permissions = std::filesystem::status(primaryPath).permissions();
        check((permissions & std::filesystem::perms::owner_read) != std::filesystem::perms::none,
              "The policy is not owner-readable.");
        check((permissions & std::filesystem::perms::owner_write) != std::filesystem::perms::none,
              "The policy is not owner-writable.");
        check((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none,
              "The policy grants group or other permissions.");
#endif

        primary.save(false);
        check(!primary.load().allowsLibraryAccess, "A persisted false policy did not reload.");

        const std::filesystem::path compatiblePath = root / "compatible" / "bridge-policy.json";
        writeRaw(compatiblePath, QByteArrayLiteral("{\"allowsLibraryAccess\":true,\"futureVersion\":1}"));
        check(yobro::spike::BridgePolicyStore(compatiblePath).load().allowsLibraryAccess,
              "Forward-compatible extra policy keys were rejected.");

        checkQuarantined(root, "malformed", QByteArrayLiteral("{"));
        checkQuarantined(root, "missing-key", QByteArrayLiteral("{}"));
        checkQuarantined(root, "wrong-type", QByteArrayLiteral("{\"allowsLibraryAccess\":\"yes\"}"));
        checkQuarantined(root, "duplicate-key", QByteArrayLiteral("{\"allowsLibraryAccess\":true,\"allowsLibraryAccess\":false}"));
        checkQuarantined(root, "array-root", QByteArrayLiteral("[]"));
        checkQuarantined(root, "oversized", QByteArray(64 * 1024 + 1, 'x'));

        const std::filesystem::path secondPath = root / "second" / "bridge-policy.json";
        const yobro::spike::BridgePolicyStore second(secondPath);
        primary.save(true);
        check(primary.load().allowsLibraryAccess, "Primary profile did not retain its policy.");
        check(!second.load().allowsLibraryAccess, "Library access leaked into a second profile.");

        int applied = 0;
        bool appliedValue = false;
        const auto applyError = primary.persistAndApply(false, [&](bool value) {
            ++applied;
            appliedValue = value;
        });
        check(!applyError && applied == 1 && !appliedValue, "A committed policy was not applied exactly once.");

        const std::filesystem::path blockedPath = root / "blocked" / "bridge-policy.json";
        check(QDir().mkpath(QString::fromStdString(blockedPath.string())), "Could not create the blocking policy directory.");
        const yobro::spike::BridgePolicyStore blocked(blockedPath);
        applied = 0;
        const auto blockedError = blocked.persistAndApply(true, [&](bool) { ++applied; });
        check(blockedError.has_value(), "A non-file policy target did not fail persistence.");
        check(applied == 0, "A failed policy commit mutated the running state.");

        std::cout << "BRIDGE POLICY STORE TESTS PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "BRIDGE POLICY STORE TESTS FAIL: " << error.what() << '\n';
        return 1;
    }
}
