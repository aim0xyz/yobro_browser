import XCTest
@testable import YOBRO

final class MailDiscoveryTests: XCTestCase {
    func testKnownProviders() async throws {
        let id = UUID()
        let gmail = try await MailDiscovery.discover(address: "test@gmail.com", id: id)
        XCTAssertEqual(gmail.account.id, id)
        XCTAssertEqual(gmail.account.imapHost, "imap.gmail.com")
        XCTAssertTrue(gmail.explanation.contains(L("App-Passwort", "app password")))
        let apple = try await MailDiscovery.discover(address: "test@icloud.com", id: id)
        XCTAssertEqual(apple.account.smtpSecurity, "starttls")
        XCTAssertEqual(apple.account.smtpPort, 587)
        let space = MailDiscovery.preset("Spacemail", address: "test@custom-domain.org", id: id)
        XCTAssertEqual(space.account.imapHost, "mail.spacemail.com")
        XCTAssertEqual(space.account.username, "test@custom-domain.org")
    }
    func testOAuthOnlyIsNotPresentedAsPasswordLogin() async throws {
        let result = try await MailDiscovery.discover(address: "test@outlook.com", id: UUID())
        XCTAssertTrue(result.oauthOnly)
        XCTAssertTrue(result.explanation.contains(L("nicht verbunden", "cannot currently be connected")))
    }
    func testBadAddressNeverStartsDiscovery() async {
        for value in ["no-address", "a@x/evil.org", "a@x.org?query=leak", "a@x.org\n"] {
            do { _ = try await MailDiscovery.discover(address: value, id: UUID()); XCTFail(value) }
            catch { }
        }
    }
    func testAutoconfigPreservesMultipleAuthenticationOptions() {
        let data = Data("""
        <clientConfig><emailProvider id="test"><incomingServer type="imap"><hostname>imap.example.org</hostname><port>993</port><socketType>SSL</socketType><username>%EMAILADDRESS%</username><authentication>OAuth2</authentication><authentication>password-cleartext</authentication></incomingServer><outgoingServer type="smtp"><hostname>smtp.example.org</hostname><port>587</port><socketType>STARTTLS</socketType></outgoingServer></emailProvider></clientConfig>
        """.utf8)
        let delegate = ProviderXML(), parser = XMLParser(data: data)
        parser.delegate = delegate; parser.shouldResolveExternalEntities = false
        XCTAssertTrue(parser.parse())
        XCTAssertEqual(delegate.servers.count, 2)
        XCTAssertEqual(delegate.servers[0]["auth"], "OAuth2,password-cleartext,")
        XCTAssertEqual(delegate.servers[1]["socketType"], "STARTTLS")
    }
    func testAccountConfigurationContainsNoPassword() throws {
        let account = MailDiscovery.preset("Gmail", address: "test@gmail.com", id: UUID()).account
        let data = try JSONEncoder().encode(account)
        XCTAssertFalse(String(decoding: data, as: UTF8.self).contains("password"))
        XCTAssertEqual(try JSONDecoder().decode(MailAccount.self, from: data), account)
    }
    func testWorkerProcessReportsFailureWithoutEchoingSecret() async {
        var account = MailAccount(address: "test@example.org", username: "test@example.org", imapHost: "127.0.0.1", imapPort: 1, smtpHost: "127.0.0.1", smtpPort: 1)
        account.label = "Transport test"
        do {
            _ = try await MailTransport.run(action: "test", account: account, password: "SYNTHETIC-SECRET-DO-NOT-ECHO")
            XCTFail("Closed test port should fail")
        } catch {
            XCTAssertFalse(error.localizedDescription.contains("SYNTHETIC-SECRET"))
            XCTAssertTrue(error.localizedDescription.contains(L("fehlgeschlagen", "failed")), error.localizedDescription)
        }
    }
}
