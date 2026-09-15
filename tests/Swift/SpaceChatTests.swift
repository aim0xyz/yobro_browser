import XCTest
import SwiftUI
@testable import YOBRO

final class SpaceChatTests: XCTestCase {
    func testLegacyChatEntriesLoadWithoutOptionalMetadata() throws {
        let data = Data(#"[{"space":"Work","kind":"assistant","text":"Restored context"}]"#.utf8)
        let entries = try JSONDecoder().decode([SpaceChatEntry].self, from: data)

        XCTAssertEqual(entries.first?.space, "Work")
        XCTAssertEqual(entries.first?.text, "Restored context")
        XCTAssertNotNil(entries.first?.id)
        XCTAssertNotNil(entries.first?.date)
        XCTAssertEqual(entries.first?.saved, false)
    }

    @MainActor
    func testProviderErrorIncludesUsefulMessage() {
        let data = Data(#"{"error":{"message":"Model is not available"}}"#.utf8)
        XCTAssertTrue(SpaceChatStore.providerError(data: data, status: 404).contains("Model is not available"))
        XCTAssertTrue(SpaceChatStore.providerError(data: Data(), status: 429).contains("429"))
    }

    func testProviderPresetsFillEndpointAndModel() {
        XCTAssertEqual(ChatProviderPreset.openRouter.endpoint, "https://openrouter.ai/api/v1/chat/completions")
        XCTAssertEqual(ChatProviderPreset.anthropic.endpoint, "https://api.anthropic.com/v1/messages")
        XCTAssertEqual(ChatProviderPreset.anthropic.defaultModel, "claude-sonnet-4-6")
        XCTAssertEqual(ChatProviderPreset.openAI.endpoint, "https://api.openai.com/v1/chat/completions")
        XCTAssertEqual(ChatProviderPreset.matching(ChatProviderPreset.openRouter.endpoint), .openRouter)
        XCTAssertEqual(ChatProviderPreset.matching("https://localhost.example/v1/chat/completions"), .custom)
    }

    @MainActor
    func testAnthropicRequestAndToolResponseConversion() throws {
        let messages: [[String: Any]] = [["role": "system", "content": "System"], ["role": "user", "content": "Hello"]]
        let body = SpaceChatStore.anthropicBody(model: "claude-sonnet-4-6", messages: messages)
        XCTAssertEqual(body["system"] as? String, "System")
        XCTAssertEqual(body["max_tokens"] as? Int, 4096)
        XCTAssertEqual((body["tools"] as? [[String: Any]])?.first?["input_schema"] as? [String: Any] != nil, true)
        let response: [String: Any] = ["content": [["type": "text", "text": "Checking"], ["type": "tool_use", "id": "tool-1", "name": "list_notes", "input": [:]]]]
        let converted = try XCTUnwrap(SpaceChatStore.providerMessage(response, provider: .anthropic))
        XCTAssertEqual(converted["content"] as? String, "Checking")
        XCTAssertEqual((converted["tool_calls"] as? [[String: Any]])?.first?["id"] as? String, "tool-1")
    }

    func testOpenRouterPKCEAndAttributionConstants() throws {
        let verifier = try OpenRouterOAuth.randomToken()
        XCTAssertFalse(verifier.contains("+"))
        XCTAssertFalse(verifier.contains("/"))
        XCTAssertFalse(verifier.contains("="))
        XCTAssertEqual(OpenRouterOAuth.referer, "https://yobro.lol")
        XCTAssertEqual(OpenRouterOAuth.title, "YoBro")
        let url = try OpenRouterOAuth.authorizationURL(callback: URL(string: "http://127.0.0.1:56178/openrouter/callback")!, challenge: "challenge")
        let items = Dictionary(uniqueKeysWithValues: URLComponents(url: url, resolvingAgainstBaseURL: false)!.queryItems!.map { ($0.name, $0.value ?? "") })
        XCTAssertEqual(items["key_label"], "YoBro")
        XCTAssertEqual(items["callback_url"], "http://127.0.0.1:56178/openrouter/callback")
    }

    func testOpenRouterModelsCanSortFreeModelsFirst() throws {
        let paidJSON = #"{"id":"paid/model","name":"Alpha","pricing":{"prompt":"0.000001","completion":"0.000002"}}"#
        let freeJSON = #"{"id":"free/model:free","name":"Zulu","pricing":{"prompt":"0","completion":"0"}}"#
        let paid = try JSONDecoder().decode(OpenRouterModel.self, from: Data(paidJSON.utf8))
        let free = try JSONDecoder().decode(OpenRouterModel.self, from: Data(freeJSON.utf8))
        XCTAssertFalse(paid.isFree)
        XCTAssertTrue(free.isFree)
        XCTAssertEqual(OpenRouterOAuth.sortedModels([free, paid], freeFirst: false).map(\.id), [paid.id, free.id])
        XCTAssertEqual(OpenRouterOAuth.sortedModels([paid, free], freeFirst: true).map(\.id), [free.id, paid.id])
    }

    @MainActor
    func testProviderSelectionNeverPersistsAPIKey() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let chat = SpaceChatStore(home: home)
        chat.apiKey = "old-session-key"
        chat.selectProvider(.openRouter)
        XCTAssertEqual(chat.connection.endpoint, ChatProviderPreset.openRouter.endpoint)
        XCTAssertEqual(chat.connection.model, "openrouter/auto")
        XCTAssertTrue(chat.apiKey.isEmpty)
        chat.apiKey = "never-write-this"
        chat.persist()
        XCTAssertFalse(try String(contentsOf: home.appendingPathComponent("chat-connection.json")).contains("never-write-this"))
    }

    @MainActor
    func testAPIKeyPersistsOnlyInKeychain() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer {
            try? AgentAPIKeySecrets.remove(endpoint: ChatProviderPreset.openRouter.endpoint, home: home)
            try? FileManager.default.removeItem(at: home)
        }
        let secret = "keychain-only-secret"
        let chat = SpaceChatStore(home: home)
        chat.selectProvider(.openRouter)
        chat.apiKey = secret
        try chat.saveConnection()

        let connectionJSON = try String(contentsOf: home.appendingPathComponent("chat-connection.json"))
        XCTAssertFalse(connectionJSON.contains(secret))
        let historyJSON = try String(contentsOf: home.appendingPathComponent("space-chat.json"))
        XCTAssertFalse(historyJSON.contains(secret))

        let restored = SpaceChatStore(home: home)
        XCTAssertEqual(restored.apiKey, secret)
    }

    @MainActor
    func testDisconnectOpenRouterRemovesOnlyItsStoredKey() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let chat = SpaceChatStore(home: home)
        chat.selectProvider(.openRouter)
        chat.apiKey = "temporary-openrouter-key"
        try chat.saveConnection()
        chat.disconnectOpenRouter()
        XCTAssertTrue(chat.apiKey.isEmpty)
        XCTAssertTrue(chat.openRouterModels.isEmpty)
        XCTAssertTrue(try AgentAPIKeySecrets.read(endpoint: ChatProviderPreset.openRouter.endpoint, home: home).isEmpty)
        XCTAssertEqual(chat.connection.endpoint, ChatProviderPreset.openRouter.endpoint)
    }

    func testEndpointValidation() throws {
        XCTAssertThrowsError(try ChatConnection(endpoint: "http://remote.example/v1/chat/completions", model: "test").url())
        XCTAssertThrowsError(try ChatConnection(endpoint: "https://user:secret@example.com/api", model: "test").url())
        XCTAssertThrowsError(try ChatConnection(endpoint: "https://example.com/api?key=secret", model: "test").url())
        XCTAssertNoThrow(try ChatConnection(endpoint: "http://127.0.0.1:11434/v1/chat/completions", model: "test").url())
        XCTAssertNoThrow(try ChatConnection(endpoint: "https://example.com/v1/chat/completions", model: "test").url())
    }
    @MainActor
    func testPersistenceRenameAndSecretExclusion() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let chat = SpaceChatStore(home: home)
        chat.apiKey = "never-persist-me"
        chat.append("assistant", "A result", space: "A")
        chat.append("user", "Private B", space: "B")
        chat.keep(chat.entries[0].id); chat.rename("A", to: "Client")
        let restored = SpaceChatStore(home: home)
        XCTAssertEqual(restored.entries[0].space, "Client")
        XCTAssertTrue(restored.entries[0].saved)
        XCTAssertEqual(restored.entries[1].space, "B")
        XCTAssertTrue(restored.apiKey.isEmpty)
        XCTAssertFalse(try String(contentsOf: home.appendingPathComponent("space-chat.json")).contains("never-persist-me"))
    }
    @MainActor
    func testNewChatClearsOnlyCurrentSpace() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let chat = SpaceChatStore(home: home)
        chat.append("user", "Old A", space: "A")
        chat.append("assistant", "Keep B", space: "B")
        chat.drafts["A"] = "draft"
        chat.contexts["A"] = "context"
        chat.newChat(space: "A")
        XCTAssertFalse(chat.entries.contains { $0.space == "A" })
        XCTAssertTrue(chat.entries.contains { $0.space == "B" })
        XCTAssertNil(chat.drafts["A"])
        XCTAssertNil(chat.contexts["A"])
    }
    @MainActor
    func testCorruptHistoryPreserved() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: home) }
        let file = home.appendingPathComponent("space-chat.json")
        try Data("broken".utf8).write(to: file)
        let chat = SpaceChatStore(home: home); chat.append("user", "new", space: "A")
        XCTAssertNotNil(chat.error)
        XCTAssertEqual(try String(contentsOf: file), "broken")
    }
    @MainActor
    func fixture() -> BrowserModel {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let browser = BrowserModel(root: home)
        browser.newTab()
        browser.chat.connection = ChatConnection(endpoint: "http://localhost:1234/v1/chat/completions", model: "fixture")
        return browser
    }
    func response(_ request: URLRequest, message: [String: Any]) throws -> (Data, URLResponse) {
        (try JSONSerialization.data(withJSONObject: ["choices": [["message": message]]]), HTTPURLResponse(url: request.url!, statusCode: 200, httpVersion: nil, headerFields: nil)!)
    }
    @MainActor
    func finish(_ browser: BrowserModel) async throws {
        for _ in 0..<500 {
            if !browser.chat.running { return }
            try await Task.sleep(nanoseconds: 10_000_000)
        }
        XCTFail("Chat did not finish"); browser.chat.cancel()
    }
    @MainActor
    func testMailContextAndSpaceIsolation() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        let chat = browser.chat
        chat.append("user", "OTHER_SPACE_SECRET", space: "Other")
        chat.contexts[browser.space] = "Mail fixture"
        chat.drafts[browser.space] = "Summarize"
        chat.transport = { request in
            let body = String(decoding: request.httpBody!, as: UTF8.self)
            XCTAssertFalse(body.contains("OTHER_SPACE_SECRET")); XCTAssertTrue(body.contains("Mail fixture"))
            return try self.response(request, message: ["role": "assistant", "content": "Summary"])
        }
        chat.send(browser: browser); try await finish(browser)
        XCTAssertEqual(chat.entries.last?.text, "Summary")
        XCTAssertNil(chat.contexts[browser.space])
    }

    @MainActor
    func testTwoConsecutiveMessagesKeepTheCompleteConversation() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        var responseNumber = 0
        browser.chat.transport = { request in
            responseNumber += 1
            return try self.response(request, message: ["role": "assistant", "content": "Answer \(responseNumber)"])
        }

        browser.chat.drafts[browser.space] = "First question"
        browser.chat.send(browser: browser)
        try await finish(browser)
        browser.chat.drafts[browser.space] = "Second question"
        browser.chat.send(browser: browser)
        try await finish(browser)

        let visible = browser.chat.entries.filter { $0.space == browser.space }
        XCTAssertEqual(visible.map(\.kind), ["user", "assistant", "user", "assistant"])
        XCTAssertEqual(visible.map(\.text), ["First question", "Answer 1", "Second question", "Answer 2"])
        let restored = SpaceChatStore(home: browser.home)
        XCTAssertEqual(restored.entries.filter { $0.space == browser.space }.map(\.text), visible.map(\.text))
    }
    @MainActor
    func testMailToolUsesBuiltInMailScreen() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        var calls = 0
        browser.chat.transport = { request in
            calls += 1
            if calls == 1 {
                return try self.response(request, message: ["role": "assistant", "tool_calls": [["id": "mail", "type": "function", "function": ["name": "read_mail", "arguments": "{\"limit\":10}"]]]])
            }
            let body = String(decoding: request.httpBody!, as: UTF8.self)
            XCTAssertTrue(body.contains("built-in YoBro mail screen"))
            return try self.response(request, message: ["role": "assistant", "content": "No account configured."])
        }
        browser.chat.drafts[browser.space] = "Schau in meine E-Mails"
        browser.chat.send(browser: browser)
        try await finish(browser)
        XCTAssertTrue(browser.showMail)
        XCTAssertEqual(calls, 2)
        XCTAssertNil(browser.agentTab)
    }
    @MainActor
    func testAgentCanCreateNoteInCurrentSpace() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        var calls = 0
        browser.chat.transport = { request in
            calls += 1
            if calls == 1 {
                return try self.response(request, message: ["role": "assistant", "tool_calls": [["id": "note", "type": "function", "function": ["name": "create_note", "arguments": "{\"title\":\"Ideas\",\"content\":\"First idea\"}"]]]])
            }
            XCTAssertTrue(String(decoding: request.httpBody!, as: UTF8.self).contains("Created note"))
            return try self.response(request, message: ["role": "assistant", "content": "Done."])
        }
        browser.chat.drafts[browser.space] = "Create a note"
        browser.chat.send(browser: browser)
        try await finish(browser)
        let note = try XCTUnwrap(browser.tabs.first { $0.isNote && $0.title == "Ideas" })
        XCTAssertEqual(note.noteContent, "First idea")
        XCTAssertEqual(note.space, browser.space)
        XCTAssertNil(browser.agentTab)
    }
    @MainActor
    func testApprovalDeclineAndPause() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        let chat = browser.chat, count = browser.tabs.count
        var calls = 0
        chat.transport = { request in
            calls += 1
            if calls == 1 {
                return try self.response(request, message: ["role": "assistant", "content": "Open the reference page.", "tool_calls": [["id": "call1", "type": "function", "function": ["name": "open_url", "arguments": "{\"url\":\"https://example.invalid\"}"]]]])
            }
            return try self.response(request, message: ["role": "assistant", "content": "Declined."])
        }
        chat.drafts[browser.space] = "Open reference"; chat.send(browser: browser)
        for _ in 0..<100 { if chat.pendingURL != nil { break }; try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertNotNil(chat.pendingURL); XCTAssertEqual(browser.tabs.count, count)
        chat.resolve(false); try await finish(browser)
        XCTAssertEqual(browser.tabs.count, count); XCTAssertEqual(calls, 2)
        calls = 0; chat.drafts[browser.space] = "Again"; chat.send(browser: browser)
        for _ in 0..<100 { if chat.pendingURL != nil { break }; try await Task.sleep(nanoseconds: 10_000_000) }
        browser.agentEnabled = false
        XCTAssertFalse(chat.running); XCTAssertNil(chat.pendingURL)
        chat.resolve(true)
        try await Task.sleep(nanoseconds: 30_000_000)
        XCTAssertEqual(browser.tabs.count, count); XCTAssertEqual(calls, 1)
    }
    @MainActor
    func testApprovedOpenAndCrossSpaceReadRejection() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        let original = browser.space
        let other = browser.newTab(space: "Studio")
        browser.switchSpace(original)
        let count = browser.tabs.count
        var calls = 0
        browser.chat.transport = { request in
            calls += 1
            if calls == 1 {
                return try self.response(request, message: ["role": "assistant", "tool_calls": [["id": "outside", "type": "function", "function": ["name": "read_tab", "arguments": "{\"tab\":\"\(other.id.uuidString)\"}"]]]])
            }
            if calls == 2 {
                XCTAssertTrue(String(decoding: request.httpBody!, as: UTF8.self).contains("outside this Space"))
                return try self.response(request, message: ["role": "assistant", "tool_calls": [["id": "open", "type": "function", "function": ["name": "open_url", "arguments": "{\"url\":\"http://127.0.0.1:1/fixture\"}"]]]])
            }
            return try self.response(request, message: ["role": "assistant", "content": "Opened."])
        }
        browser.chat.drafts[original] = "Open reference"; browser.chat.send(browser: browser)
        for _ in 0..<100 { if browser.chat.pendingURL != nil { break }; try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertNotNil(browser.chat.pendingURL); XCTAssertEqual(browser.tabs.count, count)
        browser.chat.resolve(true); try await finish(browser)
        XCTAssertEqual(browser.tabs.count, count + 1)
        XCTAssertEqual(browser.active?.space, original)
        XCTAssertEqual(browser.agentTab?.url, "http://127.0.0.1:1/fixture")
        XCTAssertNotEqual(browser.activeID, browser.agentTabID)
        XCTAssertEqual(calls, 3)
    }
    @MainActor
    func testSpaceSwitchCancelsLateResponse() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        browser.chat.transport = { request in
            try? await Task.sleep(nanoseconds: 100_000_000)
            return try self.response(request, message: ["role": "assistant", "content": "LATE_RESPONSE"])
        }
        browser.chat.drafts[browser.space] = "Hello"; browser.chat.send(browser: browser)
        await Task.yield(); browser.switchSpace("Studio")
        try await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertFalse(browser.chat.running)
        XCTAssertFalse(browser.chat.entries.contains { $0.text == "LATE_RESPONSE" })
    }
    @MainActor
    func testReadToolUsesAgentWebKitTab() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        let userID = browser.activeID
        let tab = browser.newAgentTab()
        tab.webView.loadHTMLString("<html><body><h1>Price fixture</h1><p>verified-price-42</p></body></html>", baseURL: URL(string: "https://fixture.invalid"))
        var calls = 0
        browser.chat.transport = { request in
            calls += 1
            if calls == 1 {
                return try self.response(request, message: ["role": "assistant", "tool_calls": [["id": "read", "type": "function", "function": ["name": "read_tab", "arguments": "{\"tab\":\"\(tab.id.uuidString)\"}"]]]])
            }
            XCTAssertTrue(String(decoding: request.httpBody!, as: UTF8.self).contains("verified-price-42"))
            return try self.response(request, message: ["role": "assistant", "content": "Price: 42"])
        }
        browser.chat.drafts[browser.space] = "Read price"; browser.chat.send(browser: browser)
        try await finish(browser)
        XCTAssertEqual(calls, 2); XCTAssertEqual(browser.activeID, userID)
        XCTAssertTrue(browser.chat.entries.contains { $0.kind == "action" && $0.text.hasPrefix("read:") })
        XCTAssertEqual(browser.chat.entries.last?.text, "Price: 42")
    }
    @MainActor
    func testPanelLayout() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        browser.showAgent = true
        browser.chat.append("user", "Fasse die Kundenanfrage zusammen.", space: browser.space)
        browser.chat.append("context", "Mail: Angebot für Kunde X\nBitte prüfe die Preise für die nächste Projektphase.", space: browser.space)
        browser.chat.append("assistant", "Der Kunde bittet um ein aktualisiertes Angebot. Als nächster Schritt können wir die genannten Preise auf den zugehörigen Seiten prüfen.", space: browser.space)
        let view = NSHostingView(rootView: BrowserShell(model: browser))
        view.frame = NSRect(x: 0, y: 0, width: 1360, height: 880)
        let window = NSWindow(contentRect: view.frame, styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = view; window.setContentSize(NSSize(width: 1360, height: 880)); window.orderFront(nil)
        defer { window.close() }
        try await Task.sleep(nanoseconds: 300_000_000)
        view.layoutSubtreeIfNeeded()
        let bitmap = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in: view.bounds))
        view.cacheDisplay(in: view.bounds, to: bitmap)
        try XCTUnwrap(bitmap.representation(using: .png, properties: [:])).write(to: URL(fileURLWithPath: "/tmp/YOBRO-space-chat.png"))
    }
}
