#include "spike/BrowserSync.hpp"
#include "spike/ChaCha20Poly1305.hpp"
#include "spike/SyncAccount.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using yobro::spike::ChaCha20Poly1305;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

QByteArray hex(const char *text) {
    return QByteArray::fromHex(QByteArray(text));
}

/// 0x00 … 0x1f, the key used by several RFC 8439 vectors.
QByteArray sequentialKey(int first) {
    QByteArray key(32, Qt::Uninitialized);
    for (int index = 0; index < 32; ++index) key[index] = static_cast<char>(first + index);
    return key;
}

// MARK: - RFC 8439

/// RFC 8439 §2.3.2.
void checkBlockVector() {
    const QByteArray output = ChaCha20Poly1305::block(sequentialKey(0), 1, hex("000000090000004a00000000"));
    const QByteArray expected = hex(
        "10f1e7e4d13b5915500fdd1fa32071c4"
        "c7d1f4c733c068030422aa9ac3d46c4e"
        "d2826446079faa0914c2d705d98b02a2"
        "b5129cd1de164eb9cbd083e8a2503c4e"
    );
    check(output == expected,
          "The ChaCha20 block function does not match RFC 8439: "
              + QString::fromLatin1(output.toHex()).toStdString());
}

/// RFC 8439 §2.4.2.
void checkStreamVector() {
    const QByteArray plaintext = QByteArrayLiteral(
        "Ladies and Gentlemen of the class of '99: If I could offer you only one "
        "tip for the future, sunscreen would be it.");
    const QByteArray ciphertext =
        ChaCha20Poly1305::chacha20(sequentialKey(0), 1, hex("000000000000004a00000000"), plaintext);
    const QByteArray expected = hex(
        "6e2e359a2568f98041ba0728dd0d6981"
        "e97e7aec1d4360c20a27afccfd9fae0b"
        "f91b65c5524733ab8f593dabcd62b357"
        "1639d624e65152ab8f530c359f0861d8"
        "07ca0dbf500d6a6156a38e088a22b65e"
        "52bc514d16ccf806818ce91ab7793736"
        "5af90bbf74a35be6b40b8eedf2785e42"
        "874d"
    );
    check(ciphertext == expected,
          "The ChaCha20 stream does not match RFC 8439: "
              + QString::fromLatin1(ciphertext.toHex()).toStdString());
    // Decrypting is the same operation, so a round trip has to come back.
    check(ChaCha20Poly1305::chacha20(sequentialKey(0), 1, hex("000000000000004a00000000"), ciphertext)
              == plaintext,
          "The stream cipher is not its own inverse.");
}

/// RFC 8439 §2.5.2.
void checkPoly1305Vector() {
    const QByteArray tag = ChaCha20Poly1305::poly1305(
        hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"),
        QByteArrayLiteral("Cryptographic Forum Research Group")
    );
    check(tag == hex("a8061dc1305136c6c22b8baf0c0127a9"),
          "Poly1305 does not match RFC 8439: " + QString::fromLatin1(tag.toHex()).toStdString());

    // A message that is an exact multiple of the block size takes the other
    // branch through the final block, so it is worth its own check.
    const QByteArray sixteen = ChaCha20Poly1305::poly1305(
        hex("0100000000000000000000000000000000000000000000000000000000000000"),
        QByteArray(16, '\0')
    );
    check(sixteen.size() == 16, "Poly1305 returned a tag of the wrong length.");
}

/// RFC 8439 §2.8.2, the complete authenticated construction.
void checkAeadVector() {
    const QByteArray key = sequentialKey(0x80);
    const QByteArray nonce = hex("070000004041424344454647");
    const QByteArray aad = hex("50515253c0c1c2c3c4c5c6c7");
    const QByteArray plaintext = QByteArrayLiteral(
        "Ladies and Gentlemen of the class of '99: If I could offer you only one "
        "tip for the future, sunscreen would be it.");

    const QByteArray sealed = ChaCha20Poly1305::seal(key, nonce, plaintext, aad);
    check(sealed.left(12) == nonce, "The sealed box does not start with the nonce.");
    const QByteArray ciphertext = sealed.mid(12, sealed.size() - 12 - 16);
    const QByteArray tag = sealed.right(16);
    check(ciphertext == hex(
              "d31a8d34648e60db7b86afbc53ef7ec2"
              "a4aded51296e08fea9e2b5a736ee62d6"
              "3dbea45e8ca9671282fafb69da92728b"
              "1a71de0a9e060b2905d6a5b67ecd3b36"
              "92ddbd7f2d778b8c9803aee328091b58"
              "fab324e4fad675945585808b4831d7bc"
              "3ff4def08e4b7a9de576d26586cec64b"
              "6116"
          ),
          "The AEAD ciphertext does not match RFC 8439: "
              + QString::fromLatin1(ciphertext.toHex()).toStdString());
    check(tag == hex("1ae10b594f09e26a7e902ecbd0600691"),
          "The AEAD tag does not match RFC 8439: " + QString::fromLatin1(tag.toHex()).toStdString());

    const auto opened = ChaCha20Poly1305::open(key, sealed, aad);
    check(opened.has_value() && *opened == plaintext, "The sealed box did not open again.");
}

void checkAeadRefusals() {
    const QByteArray key = ChaCha20Poly1305::randomKey();
    const QByteArray nonce = ChaCha20Poly1305::randomNonce();
    const QByteArray plaintext = QByteArrayLiteral("Etwas Vertrauliches");
    const QByteArray sealed = ChaCha20Poly1305::seal(key, nonce, plaintext);
    check(ChaCha20Poly1305::open(key, sealed).value_or(QByteArray()) == plaintext,
          "A freshly sealed box did not open.");

    // Every kind of tampering has to end in nothing, never in plaintext.
    QByteArray flippedTag = sealed;
    flippedTag[flippedTag.size() - 1] = static_cast<char>(flippedTag.back() ^ 0x01);
    check(!ChaCha20Poly1305::open(key, flippedTag).has_value(), "A changed tag was accepted.");

    QByteArray flippedCipher = sealed;
    flippedCipher[20] = static_cast<char>(flippedCipher.at(20) ^ 0x01);
    check(!ChaCha20Poly1305::open(key, flippedCipher).has_value(),
          "A changed ciphertext was accepted.");

    QByteArray flippedNonce = sealed;
    flippedNonce[0] = static_cast<char>(flippedNonce.at(0) ^ 0x01);
    check(!ChaCha20Poly1305::open(key, flippedNonce).has_value(), "A changed nonce was accepted.");

    check(!ChaCha20Poly1305::open(ChaCha20Poly1305::randomKey(), sealed).has_value(),
          "A box opened with the wrong key.");
    check(!ChaCha20Poly1305::open(key, sealed.left(sealed.size() - 1)).has_value(),
          "A truncated box was accepted.");
    check(!ChaCha20Poly1305::open(key, {}).has_value(), "An empty box was accepted.");
    check(!ChaCha20Poly1305::open(key, QByteArray(20, 'x')).has_value(),
          "A box too short to hold a tag was accepted.");
    // Additional data is authenticated, so changing it must break the box.
    const QByteArray withAad = ChaCha20Poly1305::seal(key, nonce, plaintext, QByteArrayLiteral("v1"));
    check(!ChaCha20Poly1305::open(key, withAad, QByteArrayLiteral("v2")).has_value(),
          "Additional data is not authenticated.");
    check(ChaCha20Poly1305::open(key, withAad, QByteArrayLiteral("v1")).has_value(),
          "Matching additional data was refused.");

    // Wrong key or nonce sizes must not silently produce something.
    check(ChaCha20Poly1305::seal(QByteArray(31, 'k'), nonce, plaintext).isEmpty(),
          "A short key was accepted for sealing.");
    check(ChaCha20Poly1305::seal(key, QByteArray(11, 'n'), plaintext).isEmpty(),
          "A short nonce was accepted for sealing.");
    check(!ChaCha20Poly1305::open(QByteArray(31, 'k'), sealed).has_value(),
          "A short key was accepted for opening.");
}

void checkRandomness() {
    check(ChaCha20Poly1305::randomKey().size() == 32, "A generated key has the wrong length.");
    check(ChaCha20Poly1305::randomNonce().size() == 12, "A generated nonce has the wrong length.");
    // Two keys in a row being equal would mean the generator is not working.
    check(ChaCha20Poly1305::randomKey() != ChaCha20Poly1305::randomKey(),
          "Two generated keys were identical.");
    check(ChaCha20Poly1305::randomNonce() != ChaCha20Poly1305::randomNonce(),
          "Two generated nonces were identical.");
}

/// An empty message still has to be authenticated, and long messages have to
/// cross the 64-byte block boundary correctly.
void checkSizes() {
    const QByteArray key = ChaCha20Poly1305::randomKey();
    const QByteArray nonce = ChaCha20Poly1305::randomNonce();
    for (const int size : {0, 1, 15, 16, 17, 63, 64, 65, 127, 128, 129, 1000, 4096}) {
        QByteArray plaintext(size, Qt::Uninitialized);
        for (int index = 0; index < size; ++index) plaintext[index] = static_cast<char>(index % 251);
        const QByteArray sealed = ChaCha20Poly1305::seal(key, nonce, plaintext);
        check(sealed.size() == size + 12 + 16,
              "A sealed box has the wrong length for " + std::to_string(size) + " bytes.");
        const auto opened = ChaCha20Poly1305::open(key, sealed);
        check(opened.has_value() && *opened == plaintext,
              "A round trip failed for " + std::to_string(size) + " bytes.");
    }
}

// MARK: - Snapshot

using yobro::spike::AuthClient;
using yobro::spike::EncryptedSyncPayload;
using yobro::spike::SupabaseAuthSession;
using yobro::spike::SupabaseClient;
using yobro::spike::SupabaseProject;
using yobro::spike::SyncBookmark;
using yobro::spike::SyncCipher;
using yobro::spike::SyncController;
using yobro::spike::SyncFolder;
using yobro::spike::SyncHistoryEntry;
using yobro::spike::SyncMerge;
using yobro::spike::SyncRecord;
using yobro::spike::SyncSecrets;
using yobro::spike::SyncService;
using yobro::spike::SyncSnapshot;
using yobro::spike::SyncTab;
using yobro::spike::SyncTabResolution;

SyncTab makeTab(const QString &id, const QString &url, const QString &space = QStringLiteral("Personal")) {
    SyncTab tab;
    tab.id = id;
    tab.url = url;
    tab.title = QStringLiteral("Titel ") + id;
    tab.space = space;
    return tab;
}

SyncSnapshot baseSnapshot() {
    SyncSnapshot snapshot;
    snapshot.profileId = QStringLiteral("11111111111111111111111111111111");
    snapshot.modifiedAt = 1000;
    snapshot.spaces = {QStringLiteral("Personal")};
    snapshot.currentSpace = QStringLiteral("Personal");
    return snapshot;
}

void checkHistorySanitisation() {
    // A fragment is never worth syncing and can carry a token of its own.
    check(SyncSnapshot::sanitizedHistoryUrl(QStringLiteral("https://example.com/a#secret"))
              == QStringLiteral("https://example.com/a"),
          "The fragment was not removed.");
    check(SyncSnapshot::sanitizedHistoryUrl(QStringLiteral("https://example.com/a?q=1"))
              == QStringLiteral("https://example.com/a?q=1"),
          "A harmless query was removed.");

    // Google sign-in addresses carry one-time tokens in path and query.
    const auto google = SyncSnapshot::sanitizedHistoryUrl(
        QStringLiteral("https://accounts.google.com/o/oauth2/auth?client_id=1&code=secret"));
    check(google.has_value(), "A Google sign-in URL was dropped entirely.");
    check(!google->contains(QStringLiteral("secret")) && !google->contains(QStringLiteral("oauth2")),
          "A Google sign-in URL kept its token: " + google->toStdString());
    const auto prompt = SyncSnapshot::sanitizedHistoryUrl(
        QStringLiteral("https://login.example.com/signin_prompt/step?token=abc"));
    check(prompt.has_value() && !prompt->contains(QStringLiteral("abc")),
          "A sign-in prompt URL kept its token.");

    for (const QString &raw : {
             QString(),
             QStringLiteral("file:///etc/passwd"),
             QStringLiteral("about:blank"),
             QStringLiteral("javascript:alert(1)"),
             QStringLiteral("chrome://settings"),
             QStringLiteral("https:///nohost"),
         }) {
        check(!SyncSnapshot::sanitizedHistoryUrl(raw).has_value(),
              "A URL that must not travel was accepted: " + raw.toStdString());
    }

    // The snapshot's own pass has to apply all of that and drop stale icons.
    SyncSnapshot snapshot = baseSnapshot();
    snapshot.history.push_back({QStringLiteral("file:///etc/passwd"), {}, 5, 1});
    snapshot.history.push_back({QStringLiteral("https://example.com/a#x"), QStringLiteral("A"), 6, 2});
    snapshot.spaceIcons.insert(QStringLiteral("Personal"), QStringLiteral("circle"));
    snapshot.spaceIcons.insert(QStringLiteral("Gelöscht"), QStringLiteral("square"));
    snapshot.sanitize();
    check(snapshot.history.size() == 1, "Sanitising did not drop the local file URL.");
    check(snapshot.history.front().url == QStringLiteral("https://example.com/a"),
          "Sanitising did not clean the surviving URL.");
    check(snapshot.spaceIcons.size() == 1, "An icon of a space that does not exist was kept.");
}

void checkValidation() {
    check(baseSnapshot().validationProblem().isEmpty(), "A usable snapshot was refused.");

    const auto refused = [](const std::function<void(SyncSnapshot &)> &change, const char *what) {
        SyncSnapshot snapshot = baseSnapshot();
        change(snapshot);
        check(!snapshot.validationProblem().isEmpty(),
              std::string("An unusable snapshot was accepted: ") + what);
    };
    refused([](SyncSnapshot &s) { s.spaces.clear(); }, "no spaces");
    refused([](SyncSnapshot &s) { s.currentSpace = QStringLiteral("Weg"); }, "current space unknown");
    refused([](SyncSnapshot &s) {
        s.tabs.push_back(makeTab(QStringLiteral("t1"), QStringLiteral("https://a.de/"),
                                 QStringLiteral("Fremd")));
    }, "tab in an unknown space");
    refused([](SyncSnapshot &s) { s.version = SyncSnapshot::currentVersion + 1; }, "newer schema");
    refused([](SyncSnapshot &s) {
        s.tabs.resize(SyncSnapshot::maximumTabs + 1, makeTab(QStringLiteral("t"), QStringLiteral("https://a.de/")));
    }, "too many tabs");
    refused([](SyncSnapshot &s) {
        s.bookmarks.resize(SyncSnapshot::maximumBookmarks + 1,
                           SyncBookmark{QStringLiteral("A"), QStringLiteral("https://a.de/"), {}});
    }, "too many bookmarks");

    SyncSnapshot empty = baseSnapshot();
    check(empty.isInitialEmpty(), "A fresh snapshot does not count as empty.");
    empty.bookmarks.push_back({QStringLiteral("A"), QStringLiteral("https://a.de/"), {}});
    check(!empty.isInitialEmpty(), "A snapshot with a bookmark still counts as empty.");
}

void checkEnvelope() {
    const QByteArray key = SyncCipher::makeKey();
    check(key.size() == 32, "A generated sync key has the wrong length.");

    // The recovery code has to survive being written down and typed again.
    const QString code = SyncCipher::encodeKey(key);
    check(code.count(QLatin1Char('-')) == 7, "The recovery code is not grouped.");
    check(SyncCipher::decodeKey(code).value_or(QByteArray()) == key,
          "A recovery code did not decode back to its key.");
    check(SyncCipher::decodeKey(code.toLower()).value_or(QByteArray()) == key,
          "A lower-case recovery code was refused.");
    QString undashed = code;
    undashed.remove(QLatin1Char('-'));
    check(SyncCipher::decodeKey(undashed).value_or(QByteArray()) == key,
          "A recovery code without dashes was refused.");
    for (const QString &bad : {
             QString(),
             QStringLiteral("zu-kurz"),
             SyncCipher::encodeKey(key) + QStringLiteral("00"),
             SyncCipher::encodeKey(key).replace(0, 1, QLatin1Char('Z')),
         }) {
        check(!SyncCipher::decodeKey(bad).has_value(),
              "An invalid recovery code was accepted: " + bad.toStdString());
    }

    SyncSnapshot snapshot = baseSnapshot();
    snapshot.tabs.push_back(makeTab(QStringLiteral("t1"), QStringLiteral("https://example.com/")));
    snapshot.bookmarks.push_back({QStringLiteral("A"), QStringLiteral("https://a.de/"), QStringLiteral("F")});
    const auto payload = SyncCipher::seal(snapshot, key);
    check(payload.has_value(), "A snapshot could not be sealed.");
    check(payload->algorithm == QStringLiteral("ChaChaPoly"), "The envelope names the wrong algorithm.");
    check(payload->formatVersion == SyncSnapshot::currentVersion,
          "The envelope names the wrong schema version.");
    // Nothing readable may be left in the envelope.
    check(!payload->ciphertext.contains(QByteArrayLiteral("example.com")),
          "The sealed payload still contains a URL.");

    const auto opened = SyncCipher::open(*payload, key);
    check(opened.has_value(), "A sealed snapshot did not open.");
    check(opened->tabs.size() == 1 && opened->tabs.front().url == QStringLiteral("https://example.com/"),
          "The reopened snapshot lost its tab.");
    check(opened->bookmarks.size() == 1 && opened->bookmarks.front().folder == QStringLiteral("F"),
          "The reopened snapshot lost its bookmark.");

    check(!SyncCipher::open(*payload, SyncCipher::makeKey()).has_value(),
          "A payload opened with the wrong key.");
    // The envelope round trips through JSON on the way to the backend.
    const EncryptedSyncPayload restored = EncryptedSyncPayload::fromJson(payload->toJson());
    check(SyncCipher::open(restored, key).has_value(), "The envelope did not survive JSON.");

    // A payload from a schema this build does not know is refused, not guessed.
    EncryptedSyncPayload future = *payload;
    future.formatVersion = SyncSnapshot::currentVersion + 1;
    check(!SyncCipher::open(future, key).has_value(), "A newer schema version was accepted.");
    EncryptedSyncPayload other = *payload;
    other.algorithm = QStringLiteral("AES-GCM");
    check(!SyncCipher::open(other, key).has_value(), "Another algorithm was accepted.");
}

void checkMerge() {
    SyncSnapshot local = baseSnapshot();
    local.modifiedAt = 2000;
    local.spaces = {QStringLiteral("Personal"), QStringLiteral("Arbeit")};
    local.tabs.push_back(makeTab(QStringLiteral("local-1"), QStringLiteral("https://local.example/")));
    local.bookmarks.push_back({QStringLiteral("Lokal"), QStringLiteral("https://l.de/"), QStringLiteral("F")});
    local.history.push_back({QStringLiteral("https://shared.example/"), QStringLiteral("Alt"), 100, 3});
    local.folders.push_back({QStringLiteral("f-local"), QStringLiteral("Lokal"), QStringLiteral("Personal"), {}});
    local.closedTabs.push_back(makeTab(QStringLiteral("closed-local"), QStringLiteral("https://cl.de/")));
    local.spaceIcons.insert(QStringLiteral("Personal"), QStringLiteral("lokal"));

    SyncSnapshot remote = baseSnapshot();
    remote.modifiedAt = 3000;
    remote.spaces = {QStringLiteral("Personal"), QStringLiteral("Freizeit")};
    remote.tabs.push_back(makeTab(QStringLiteral("remote-1"), QStringLiteral("https://remote.example/")));
    remote.activeId = QStringLiteral("remote-1");
    remote.bookmarks.push_back({QStringLiteral("Fremd"), QStringLiteral("https://r.de/"), QStringLiteral("F")});
    remote.history.push_back({QStringLiteral("https://shared.example/"), QStringLiteral("Neu"), 200, 1});
    remote.folders.push_back({QStringLiteral("f-remote"), QStringLiteral("Fremd"), QStringLiteral("Freizeit"), {}});
    remote.folders.push_back({QStringLiteral("f-nowhere"), QStringLiteral("Nirgends"),
                              QStringLiteral("Unbekannt"), {}});
    remote.closedTabs.push_back(makeTab(QStringLiteral("closed-remote"), QStringLiteral("https://cr.de/")));
    remote.spaceIcons.insert(QStringLiteral("Personal"), QStringLiteral("fremd"));
    remote.spaceIcons.insert(QStringLiteral("Freizeit"), QStringLiteral("neu"));

    const SyncMerge::Result adopted = SyncMerge::merge(local, remote, SyncTabResolution::adoptRemote);
    check(adopted.problem.isEmpty(), "A valid merge was refused: " + adopted.problem.toStdString());
    const SyncSnapshot &merged = adopted.snapshot;

    // Collections that only grow are unioned, so no side loses anything.
    check(merged.spaces.size() == 3, "The spaces were not unioned.");
    check(merged.bookmarks.size() == 2, "A bookmark was lost in the merge.");
    check(merged.closedTabs.size() == 2, "A closed tab was lost in the merge.");
    // A folder whose space nobody knows is not adopted.
    check(merged.folders.size() == 2, "A folder without a space was adopted.");
    // Local icons win; the remote only fills gaps.
    check(merged.spaceIcons.value(QStringLiteral("Personal")) == QStringLiteral("lokal"),
          "The remote overwrote a local space icon.");
    check(merged.spaceIcons.value(QStringLiteral("Freizeit")) == QStringLiteral("neu"),
          "The remote icon did not fill the gap.");
    // History is merged by URL, taking the higher visit count and newer title.
    check(merged.history.size() == 1, "The shared history entry was duplicated.");
    check(merged.history.front().visits == 3, "The visit count was not the maximum.");
    check(merged.history.front().title == QStringLiteral("Neu"), "The newer title did not win.");
    // The tabs follow the caller's decision.
    check(merged.tabs.size() == 1 && merged.tabs.front().id == QStringLiteral("remote-1"),
          "Adopting the remote tabs did not work.");
    check(merged.activeId == QStringLiteral("remote-1"), "The adopted active tab was lost.");

    const SyncMerge::Result kept = SyncMerge::merge(local, remote, SyncTabResolution::keepLocal);
    check(kept.snapshot.tabs.size() == 1 && kept.snapshot.tabs.front().id == QStringLiteral("local-1"),
          "Keeping the local tabs did not work.");
    check(kept.snapshot.bookmarks.size() == 2, "Keeping the tabs also skipped the archives.");

    // Merging twice must change nothing, which is what lets the result be
    // pushed straight back.
    const SyncMerge::Result again = SyncMerge::merge(merged, remote, SyncTabResolution::adoptRemote);
    check(again.problem.isEmpty(), "The second merge was refused.");
    check(again.snapshot.history.size() == merged.history.size()
              && again.snapshot.history.front().visits == merged.history.front().visits,
          "Merging twice inflated the history.");
    check(again.snapshot.bookmarks.size() == merged.bookmarks.size(),
          "Merging twice duplicated bookmarks.");
    check(again.snapshot.spaces.size() == merged.spaces.size(), "Merging twice duplicated spaces.");

    // An active tab the remote no longer has must not survive.
    SyncSnapshot strayActive = remote;
    strayActive.activeId = QStringLiteral("does-not-exist");
    check(SyncMerge::merge(local, strayActive, SyncTabResolution::adoptRemote).snapshot.activeId.isEmpty(),
          "An active tab that does not exist was adopted.");

    // Invalid remote data is refused rather than merged.
    SyncSnapshot broken = remote;
    broken.spaces.clear();
    check(!SyncMerge::merge(local, broken, SyncTabResolution::adoptRemote).problem.isEmpty(),
          "Invalid remote data was merged.");

    // A closed-tab archive is capped, keeping the newest.
    SyncSnapshot manyClosed = remote;
    for (int index = 0; index < 40; ++index) {
        manyClosed.closedTabs.push_back(
            makeTab(QStringLiteral("c") + QString::number(index), QStringLiteral("https://c.de/")));
    }
    check(SyncMerge::merge(local, manyClosed, SyncTabResolution::keepLocal).snapshot.closedTabs.size()
              == static_cast<std::size_t>(SyncSnapshot::maximumClosedTabs),
          "The closed-tab archive was not capped.");
}

// MARK: - Project configuration

void checkProjectResolution(const QString &root) {
    const std::filesystem::path directory = (root + QStringLiteral("/project")).toStdString();
    check(QDir().mkpath(root + QStringLiteral("/project")), "Could not create the project directory.");

    // Without an override the built-in project is used.
    const SupabaseProject builtIn = SupabaseProject::resolve(directory);
    check(builtIn.url.scheme() == QStringLiteral("https"), "The built-in project is not HTTPS.");
    check(!builtIn.publishableKey.isEmpty(), "The built-in project has no key.");

    // A file next to the profiles overrides it.
    {
        QFile file(QString::fromStdString((directory / "supabase.json").string()));
        check(file.open(QIODevice::WriteOnly), "Could not write the project file.");
        file.write(QByteArrayLiteral(
            R"({"url":"https://own.example.org","publishableKey":"sb_publishable_own"})"));
    }
    const SupabaseProject overridden = SupabaseProject::resolve(directory);
    check(overridden.url.host() == QStringLiteral("own.example.org"),
          "A stored project override was ignored.");
    check(overridden.publishableKey == QStringLiteral("sb_publishable_own"),
          "A stored key override was ignored.");

    // An override that could leak the token falls back to the built-in project.
    for (const QByteArray &content : {
             QByteArrayLiteral(R"({"url":"http://own.example.org","publishableKey":"k"})"),
             QByteArrayLiteral(R"({"url":"https://","publishableKey":"k"})"),
             QByteArrayLiteral(R"({"url":"https://own.example.org","publishableKey":""})"),
             QByteArrayLiteral(R"({"url":"https://user:pw@own.example.org","publishableKey":"k"})"),
             QByteArrayLiteral("not json"),
         }) {
        QFile file(QString::fromStdString((directory / "supabase.json").string()));
        check(file.open(QIODevice::WriteOnly), "Could not write the project file.");
        file.write(content);
        file.close();
        check(SupabaseProject::resolve(directory).url == builtIn.url,
              "An unusable override was accepted: " + std::string(content.constData()));
    }

    check(!SupabaseProject::isUsableUrl(QUrl(QStringLiteral("http://a.example"))),
          "A plain HTTP project was accepted.");
    check(SupabaseProject::isUsableUrl(QUrl(QStringLiteral("https://a.example"))),
          "An HTTPS project was refused.");
}

void checkEndpointHostLock() {
    SupabaseProject project;
    project.url = QUrl(QStringLiteral("https://project.example.org"));
    project.publishableKey = QStringLiteral("key");
    QObject context;
    SupabaseClient client(project, &context);

    const QUrl good = client.endpoint(QStringLiteral("rest/v1/browser_sync"));
    check(good.host() == QStringLiteral("project.example.org") && good.scheme() == QStringLiteral("https"),
          "A normal endpoint was refused.");
    check(good.path().contains(QStringLiteral("rest/v1/browser_sync")), "The endpoint path is wrong.");

    // A path that tries to leave the project's host gets no URL at all.
    for (const QString &path : {
             QStringLiteral("//evil.example/steal"),
             QStringLiteral("../../evil"),
         }) {
        const QUrl url = client.endpoint(path);
        check(url.isEmpty() || url.host() == QStringLiteral("project.example.org"),
              "An endpoint left the project host: " + url.toString().toStdString());
    }
}

// MARK: - Controller

/// A backend in memory, so the whole cycle runs without a server.
class FakeBackend final : public SyncService {
public:
    std::optional<SyncRecord> stored;
    int fetches = 0;
    int pushes = 0;
    QString fetchProblem;
    QString pushProblem;
    QList<qint64> expectedRevisions;

    void fetch(
        const QString &,
        std::function<void(QString, std::optional<SyncRecord>)> done
    ) override {
        ++fetches;
        done(fetchProblem, fetchProblem.isEmpty() ? stored : std::nullopt);
    }

    void push(
        const QString &profileId,
        qint64 expectedRevision,
        const EncryptedSyncPayload &payload,
        std::function<void(QString, std::optional<SyncRecord>)> done
    ) override {
        ++pushes;
        expectedRevisions.append(expectedRevision);
        if (!pushProblem.isEmpty()) {
            done(pushProblem, std::nullopt);
            return;
        }
        // The same optimistic check the backend function performs.
        const qint64 current = stored ? stored->revision : -1;
        if (expectedRevision != current) {
            done(QStringLiteral("revision conflict"), std::nullopt);
            return;
        }
        SyncRecord record;
        record.profileId = profileId;
        record.revision = current + 1;
        record.modifiedAt = 0;
        record.payload = payload;
        stored = record;
        done({}, record);
    }
};

class FakeAuth final : public AuthClient {
public:
    bool accept = true;
    bool confirmationNeeded = false;
    int signOuts = 0;
    QString problem;

    void signIn(const QString &email, const QString &, Answer done) override {
        answer(email, done);
    }
    void signUp(const QString &email, const QString &, Answer done) override {
        answer(email, done);
    }
    void signOut(const QString &) override { ++signOuts; }

private:
    void answer(const QString &email, const Answer &done) {
        if (!problem.isEmpty()) {
            done(problem, std::nullopt);
            return;
        }
        if (confirmationNeeded) {
            done({}, std::nullopt);
            return;
        }
        if (!accept) {
            done(QStringLiteral("Anmeldung fehlgeschlagen."), std::nullopt);
            return;
        }
        SupabaseAuthSession session;
        session.accessToken = QStringLiteral("token");
        session.refreshToken = QStringLiteral("refresh");
        session.userId = QStringLiteral("user-1");
        session.email = email;
        done({}, session);
    }
};

/// A browser stand-in: the controller reads and writes this snapshot.
struct FakeBrowser {
    SyncSnapshot state = baseSnapshot();
    int writes = 0;
    SyncTabResolution lastResolution = SyncTabResolution::keepLocal;
    QString writeProblem;

    SyncController::SnapshotReader reader() {
        return [this] { return state; };
    }
    SyncController::SnapshotWriter writer() {
        return [this](const SyncSnapshot &snapshot, SyncTabResolution resolution) {
            ++writes;
            lastResolution = resolution;
            if (!writeProblem.isEmpty()) return writeProblem;
            state = snapshot;
            return QString();
        };
    }
};

void checkControllerIdentity(const QString &root) {
    const QString directory = root + QStringLiteral("/identity");
    check(QDir().mkpath(directory), "Could not create the identity directory.");
    FakeBrowser browser;
    QString first;
    {
        SyncController controller(directory.toStdString(), browser.reader(), browser.writer());
        first = controller.profileId();
        check(!first.isEmpty(), "The controller made no profile id.");
        check(!controller.signedIn(), "A fresh controller looks signed in.");
    }
    // The backend keys its rows by this id, so it has to survive a restart.
    SyncController again(directory.toStdString(), browser.reader(), browser.writer());
    check(again.profileId() == first, "The profile id changed after a restart.");

    QFile file(directory + QStringLiteral("/sync.json"));
    check(file.open(QIODevice::ReadOnly), "The sync settings were not written.");
    const QString stored = QString::fromUtf8(file.readAll());
    // Neither the tokens nor the key may end up in a file.
    check(!stored.contains(QStringLiteral("token")), "A token was written into sync.json.");
    check(!stored.contains(QStringLiteral("key")), "A key was written into sync.json.");
}

void checkControllerRefusals(const QString &root) {
    const QString directory = root + QStringLiteral("/refusals");
    check(QDir().mkpath(directory), "Could not create the refusal directory.");
    FakeBrowser browser;
    FakeBackend backend;
    FakeAuth auth;
    SyncController controller(directory.toStdString(), browser.reader(), browser.writer());
    controller.setService(&backend);
    controller.setAuthClient(&auth);

    QString reported = QStringLiteral("not called");
    controller.syncNow([&reported](QString problem) { reported = problem; });
    check(!reported.isEmpty(), "Syncing without an account was reported as success.");
    check(backend.fetches == 0, "Syncing without an account reached the backend.");

    controller.signIn({}, QStringLiteral("pw"), {}, [&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "Signing in without an address was accepted.");
    controller.signIn(QStringLiteral("a@b.de"), {}, {}, [&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "Signing in without a password was accepted.");
    // A recovery code is either a key or a typo, never something in between.
    controller.signIn(QStringLiteral("a@b.de"), QStringLiteral("pw"), QStringLiteral("nope"),
                      [&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "An invalid recovery code was accepted.");
    check(!controller.signedIn(), "A refused sign-in left the controller signed in.");

    auth.problem = QStringLiteral("Falsches Passwort.");
    controller.signIn(QStringLiteral("a@b.de"), QStringLiteral("pw"), {},
                      [&reported](QString p) { reported = p; });
    check(reported == QStringLiteral("Falsches Passwort."), "A rejected sign-in was not reported.");
    check(!controller.signedIn(), "A rejected sign-in left the controller signed in.");
    auth.problem.clear();

    // A project that wants an e-mail confirmation must not look signed in.
    auth.confirmationNeeded = true;
    controller.signIn(QStringLiteral("a@b.de"), QStringLiteral("pw"), {},
                      [&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "A pending confirmation was reported as success.");
    check(!controller.signedIn(), "A pending confirmation left the controller signed in.");
    auth.confirmationNeeded = false;

    controller.signUp(QStringLiteral("a@b.de"), QStringLiteral("kurz"),
                      [&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "A password of four characters was accepted for a new account.");
}

void checkFullCycle(const QString &root) {
    const QString directory = root + QStringLiteral("/cycle");
    check(QDir().mkpath(directory), "Could not create the cycle directory.");
    FakeBrowser browser;
    browser.state.tabs.push_back(makeTab(QStringLiteral("local-1"), QStringLiteral("https://local.example/")));
    browser.state.bookmarks.push_back({QStringLiteral("Lokal"), QStringLiteral("https://l.de/"), {}});
    browser.state.history.push_back({QStringLiteral("https://l.de/"), QStringLiteral("Lokal"), 100, 2});
    browser.state.modifiedAt = 5000;

    FakeBackend backend;
    FakeAuth auth;
    SyncController controller(directory.toStdString(), browser.reader(), browser.writer());
    controller.setService(&backend);
    controller.setAuthClient(&auth);

    QString reported = QStringLiteral("not called");
    controller.signIn(QStringLiteral("a@b.de"), QStringLiteral("password"), {},
                      [&reported](QString p) { reported = p; });
    check(reported.isEmpty(), "Signing in failed: " + reported.toStdString());
    check(controller.signedIn(), "Signing in did not take.");
    check(controller.hasKey(), "Signing in did not settle a sync key.");
    check(!controller.recoveryCode().isEmpty(), "There is no recovery code to write down.");
    const QString code = controller.recoveryCode();

    // First sync: nothing is stored yet, so the local state becomes revision 0.
    controller.syncNow([&reported](QString p) { reported = p; });
    check(reported.isEmpty(), "The first sync failed: " + reported.toStdString());
    check(backend.pushes == 1 && backend.stored.has_value(), "The first sync stored nothing.");
    check(backend.expectedRevisions.first() == -1,
          "The first push did not announce that there was no row.");
    check(backend.stored->revision == 0, "The first revision is not zero.");

    // A second sync sees its own data and stays consistent.
    controller.syncNow([&reported](QString p) { reported = p; });
    check(reported.isEmpty(), "The second sync failed: " + reported.toStdString());
    check(backend.stored->revision == 1, "The second sync did not raise the revision.");
    check(backend.expectedRevisions.at(1) == 0, "The second push announced the wrong revision.");

    // Another device with the same key adds something; the merge keeps both.
    const auto keyFor = SyncCipher::decodeKey(code);
    check(keyFor.has_value(), "The recovery code did not decode.");
    SyncSnapshot fromOther = SyncCipher::open(backend.stored->payload, *keyFor).value();
    fromOther.bookmarks.push_back({QStringLiteral("Fremd"), QStringLiteral("https://r.de/"), {}});
    fromOther.tabs.push_back(makeTab(QStringLiteral("remote-1"), QStringLiteral("https://remote.example/")));
    fromOther.modifiedAt = QDateTime::currentMSecsSinceEpoch() + 60'000;
    backend.stored->payload = SyncCipher::seal(fromOther, *keyFor).value();

    const int writesBefore = browser.writes;
    controller.syncNow([&reported](QString p) { reported = p; });
    check(reported.isEmpty(), "The merging sync failed: " + reported.toStdString());
    check(browser.writes == writesBefore + 1, "The merged state was not written back.");
    check(browser.lastResolution == SyncTabResolution::adoptRemote,
          "The newer side's tabs were not adopted.");
    check(browser.state.bookmarks.size() == 2, "The merge lost a bookmark.");
    bool sawRemoteTab = false;
    for (const SyncTab &tab : browser.state.tabs)
        if (tab.id == QStringLiteral("remote-1")) sawRemoteTab = true;
    check(sawRemoteTab, "The other device's tab did not arrive.");

    // Data written with a different key cannot be opened, and that is said
    // plainly instead of the local state being overwritten.
    SyncSnapshot foreign = baseSnapshot();
    foreign.bookmarks.push_back({QStringLiteral("X"), QStringLiteral("https://x.de/"), {}});
    backend.stored->payload = SyncCipher::seal(foreign, SyncCipher::makeKey()).value();
    const std::size_t bookmarksBefore = browser.state.bookmarks.size();
    controller.syncNow([&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "Unreadable cloud data was reported as success.");
    check(browser.state.bookmarks.size() == bookmarksBefore,
          "Unreadable cloud data changed the local state.");

    // A backend failure is reported and changes nothing.
    backend.stored.reset();
    backend.fetchProblem = QStringLiteral("Server nicht erreichbar.");
    controller.syncNow([&reported](QString p) { reported = p; });
    check(reported == QStringLiteral("Server nicht erreichbar."), "A backend failure was not reported.");
    backend.fetchProblem.clear();

    // Signing out keeps the key, because the recovery code is its only twin and
    // the data on the server would otherwise be unreadable.
    controller.signOut();
    check(!controller.signedIn(), "Signing out did not take.");
    check(auth.signOuts == 1, "The account service was not told about the sign-out.");
    check(controller.hasKey(), "Signing out threw away the sync key.");
    controller.syncNow([&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "Syncing after signing out was reported as success.");
}

void checkSecrets(const QString &root) {
    const QString directory = root + QStringLiteral("/secrets");
    SyncSecrets secrets(directory.toStdString());
    if (!secrets.available()) return;
    // A separate service name from the WebKit build's, so neither shell can read
    // the other's tokens.
    check(secrets.service().find("xyz.aimo.yobro.sync") == std::string::npos,
          "The sync Keychain service is the WebKit build's.");
    check(secrets.service().find(directory.toStdString()) != std::string::npos,
          "The sync Keychain service is not profile specific.");

    secrets.removeSession();
    secrets.removeKey();
    check(!secrets.readSession().has_value(), "A session existed before it was stored.");
    SupabaseAuthSession session;
    session.accessToken = QStringLiteral("access");
    session.refreshToken = QStringLiteral("refresh");
    session.userId = QStringLiteral("user-1");
    session.email = QStringLiteral("a@b.de");
    check(secrets.storeSession(session), "A session could not be stored.");
    const auto read = secrets.readSession();
    check(read.has_value() && read->accessToken == QStringLiteral("access")
              && read->email == QStringLiteral("a@b.de"),
          "A stored session did not come back.");

    const QByteArray key = SyncCipher::makeKey();
    check(secrets.storeKey(key), "A key could not be stored.");
    check(secrets.readKey() == key, "A stored key did not come back.");
    // A key of the wrong length is refused rather than stored short.
    check(!secrets.storeKey(QByteArray(16, 'k')), "A short key was stored.");
    check(secrets.readKey() == key, "A refused key replaced the stored one.");

    check(secrets.removeSession(), "A session could not be removed.");
    check(!secrets.readSession().has_value(), "A removed session came back.");
    check(secrets.removeKey(), "A key could not be removed.");
    check(secrets.readKey().isEmpty(), "A removed key came back.");
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir work;
    if (!work.isValid()) return 1;

    try {
        checkBlockVector();
        checkStreamVector();
        checkPoly1305Vector();
        checkAeadVector();
        checkAeadRefusals();
        checkRandomness();
        checkSizes();
        checkHistorySanitisation();
        checkValidation();
        checkEnvelope();
        checkMerge();
        checkProjectResolution(work.path());
        checkEndpointHostLock();
        checkControllerIdentity(work.path());
        checkControllerRefusals(work.path());
        checkFullCycle(work.path());
        checkSecrets(work.path());
    } catch (const std::exception &error) {
        std::cerr << "SYNC FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "SYNC PASS\n";
    return 0;
}
