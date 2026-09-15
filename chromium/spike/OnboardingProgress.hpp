#pragma once

#include <QString>

#include <filesystem>

namespace yobro::spike {

/// Whether the first-run setup was finished, stored as `onboarding.json`.
///
/// Same file and same key as the WebKit build's `OnboardingProgress`. Anything
/// unreadable counts as unfinished: showing the setup once too often is
/// harmless, skipping it on a fresh profile is not.
class OnboardingProgress {
public:
    [[nodiscard]] static bool isComplete(const std::filesystem::path &profileDirectory);

    /// Records the setup as finished. Returns an empty string on success, and a
    /// localised reason otherwise, so the window can say why it will ask again.
    static QString finish(const std::filesystem::path &profileDirectory);

    /// Forgets that the setup ran, so the next start shows it again.
    static QString reset(const std::filesystem::path &profileDirectory);
};

} // namespace yobro::spike
