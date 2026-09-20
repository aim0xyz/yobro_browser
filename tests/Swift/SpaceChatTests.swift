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
    func testSystemPromptIncludesFullBrowserAutomationCapabilities() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        var capturedBody = ""
        browser.chat.transport = { request in
            capturedBody = String(decoding: request.httpBody ?? Data(), as: UTF8.self)
            return try self.response(request, message: ["role": "assistant", "content": "Ich kann das Formular für dich ausfüllen und absenden."])
        }
        browser.chat.drafts[browser.space] = "Beantworte diesen Post und fülle das Formular aus"
        browser.chat.send(browser: browser)
        try await finish(browser)
        XCTAssertTrue(capturedBody.contains("full browser automation capabilities"))
        XCTAssertTrue(capturedBody.contains("click_element"))
        XCTAssertTrue(capturedBody.contains("fill_element"))
        XCTAssertTrue(capturedBody.contains("press_key"))
        XCTAssertTrue(capturedBody.contains("scroll_page"))
        XCTAssertEqual(browser.chat.entries.last?.text, "Ich kann das Formular für dich ausfüllen und absenden.")
    }

    @MainActor
    func testClickAndFillElementToolsExecuteOnPage() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        let tab = browser.newAgentTab()
        let html = """
        <html><body>
          <input id="search-input" type="text" value="" />
          <button id="search-btn" onclick="document.body.setAttribute('data-clicked', 'true')">Search</button>
        </body></html>
        """
        tab.webView.loadHTMLString(html, baseURL: URL(string: "https://fixture.invalid"))

        var calls = 0
        var docID = ""
        var inputRef = ""
        var buttonRef = ""

        browser.chat.transport = { request in
            calls += 1
            if calls == 1 {
                // Step 1: LLM calls read_tab
                return try self.response(request, message: [
                    "role": "assistant",
                    "tool_calls": [[
                        "id": "c1",
                        "type": "function",
                        "function": ["name": "read_tab", "arguments": "{\"tab\":\"\(tab.id.uuidString)\"}"]
                    ]]
                ])
            } else if calls == 2 {
                // LLM inspects tool output and calls fill_element
                let reqBody = String(decoding: request.httpBody!, as: UTF8.self)
                XCTAssertTrue(reqBody.contains("Search"))
                return try self.response(request, message: [
                    "role": "assistant",
                    "tool_calls": [[
                        "id": "c2",
                        "type": "function",
                        "function": [
                            "name": "fill_element",
                            "arguments": "{\"tab\":\"\(tab.id.uuidString)\",\"document\":\"\(docID)\",\"ref\":\"\(inputRef)\",\"value\":\"YoBro search text\"}"
                        ]
                    ]]
                ])
            } else if calls == 3 {
                // Step 3: LLM calls click_element
                return try self.response(request, message: [
                    "role": "assistant",
                    "tool_calls": [[
                        "id": "c3",
                        "type": "function",
                        "function": [
                            "name": "click_element",
                            "arguments": "{\"tab\":\"\(tab.id.uuidString)\",\"document\":\"\(docID)\",\"ref\":\"\(buttonRef)\"}"
                        ]
                    ]]
                ])
            }
            return try self.response(request, message: ["role": "assistant", "content": "Suche erfolgreich abgeschickt!"])
        }

        // Pre-fetch the real document ID and refs from page for deterministic test calls
        _ = try await browser.handle(["command": "read", "tab": tab.id.uuidString])
        let prep = try await browser.handle(["command": "read", "tab": tab.id.uuidString])
        if let page = prep["page"] as? [String: Any] {
            docID = page["document"] as? String ?? ""
            if let elements = page["elements"] as? [[String: Any]] {
                inputRef = elements.first(where: { ($0["tag"] as? String) == "input" })?["ref"] as? String ?? ""
                buttonRef = elements.first(where: { ($0["tag"] as? String) == "button" })?["ref"] as? String ?? ""
            }
        }

        browser.chat.drafts[browser.space] = "Suche nach YoBro"
        browser.chat.send(browser: browser)
        try await finish(browser)
        XCTAssertEqual(calls, 4)
        XCTAssertEqual(browser.chat.entries.last?.text, "Suche erfolgreich abgeschickt!")

        // Verify that the element in the webView actually received the value and click!
        let inputValue = try await tab.webView.evaluateJavaScript("document.getElementById('search-input').value") as? String
        let clicked = try await tab.webView.evaluateJavaScript("document.body.getAttribute('data-clicked')") as? String
        XCTAssertEqual(inputValue, "YoBro search text")
        XCTAssertEqual(clicked, "true")
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
    func testLongNoteReadReturnsValidPagedJSON() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        let note = browser.newNote(title: "Long research")
        note.noteContent = String(repeating: "\"\n", count: 20_000)
        var calls = 0
        browser.chat.transport = { request in
            calls += 1
            if calls == 1 {
                let args = try JSONSerialization.data(withJSONObject: ["id": note.id.uuidString])
                return try self.response(request, message: ["role": "assistant", "tool_calls": [["id": "read-note", "type": "function", "function": ["name": "read_note", "arguments": String(decoding: args, as: UTF8.self)]]]])
            }
            let body = try XCTUnwrap(JSONSerialization.jsonObject(with: request.httpBody!) as? [String: Any])
            let messages = try XCTUnwrap(body["messages"] as? [[String: Any]])
            let result = try XCTUnwrap(messages.last?["content"] as? String)
            let page = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(result.utf8)) as? [String: Any])
            XCTAssertEqual((page["content"] as? String)?.count, 20_000)
            XCTAssertEqual(page["hasMore"] as? Bool, true)
            XCTAssertEqual(page["nextOffset"] as? Int, 20_000)
            return try self.response(request, message: ["role": "assistant", "content": "Read page."])
        }
        browser.chat.drafts[browser.space] = "Read the note"
        browser.chat.send(browser: browser)
        try await finish(browser)
        XCTAssertEqual(calls, 2)
    }

    @MainActor
    func testOversizedNoteAppendPreservesExistingContent() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        let note = browser.newNote(title: "Keep me")
        note.noteContent = String(repeating: "a", count: 99_999)
        var calls = 0
        browser.chat.transport = { request in
            calls += 1
            if calls == 1 {
                let args = try JSONSerialization.data(withJSONObject: ["id": note.id.uuidString, "content": "new content", "mode": "append"])
                return try self.response(request, message: ["role": "assistant", "tool_calls": [["id": "write-note", "type": "function", "function": ["name": "write_note", "arguments": String(decoding: args, as: UTF8.self)]]]])
            }
            XCTAssertTrue(String(decoding: request.httpBody!, as: UTF8.self).contains("Existing content was preserved"))
            return try self.response(request, message: ["role": "assistant", "content": "Note full."])
        }
        browser.chat.drafts[browser.space] = "Append findings"
        browser.chat.send(browser: browser)
        try await finish(browser)
        XCTAssertEqual(note.noteContent, String(repeating: "a", count: 99_999))
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
    func testStaleElementActionRecoversWithFreshSnapshot() async throws {
        let browser = fixture(); defer { try? FileManager.default.removeItem(at: browser.home) }
        let tab = browser.newAgentTab()
        tab.webView.loadHTMLString("""
        <html><body>
          <button id="ghost" onclick="void 0">Ghost button</button>
          <button id="fresh" onclick="document.body.setAttribute('data-clicked', 'true')">Fresh button</button>
        </body></html>
        """, baseURL: URL(string: "https://fixture.invalid"))
        // Capture refs like a model would from a read, then re-render the page
        // so the snapshotted element leaves the DOM before the click.
        let prep = try await browser.handle(["command": "read", "tab": tab.id.uuidString])
        var refs: [String: String] = [:]
        if let page = prep["page"] as? [String: Any], let elements = page["elements"] as? [[String: Any]] {
            for element in elements {
                if let label = element["label"] as? String, let ref = element["ref"] as? String { refs[label] = ref }
            }
        }
        let staleRef = try XCTUnwrap(refs["Ghost button"])
        let freshRef = try XCTUnwrap(refs["Fresh button"])
        try await tab.webView.evaluateJavaScript("document.getElementById('ghost').remove(); true")

        var calls = 0
        browser.chat.transport = { request in
            calls += 1
            if calls == 1 {
                return try self.response(request, message: ["role": "assistant", "tool_calls": [["id": "stale", "type": "function", "function": ["name": "click_element", "arguments": "{\"tab\":\"\(tab.id.uuidString)\",\"document\":\"outdated\",\"ref\":\"\(staleRef)\"}"]]]])
            }
            if calls == 2 {
                // The failed click must reach the model as a tool result instead
                // of ending the run.
                XCTAssertTrue(String(decoding: request.httpBody!, as: UTF8.self).contains("The page changed. Read a fresh snapshot before acting."))
                return try self.response(request, message: ["role": "assistant", "tool_calls": [["id": "reread", "type": "function", "function": ["name": "read_tab", "arguments": "{\"tab\":\"\(tab.id.uuidString)\"}"]]]])
            }
            if calls == 3 {
                return try self.response(request, message: ["role": "assistant", "tool_calls": [["id": "retry", "type": "function", "function": ["name": "click_element", "arguments": "{\"tab\":\"\(tab.id.uuidString)\",\"document\":\"current\",\"ref\":\"\(freshRef)\"}"]]]])
            }
            return try self.response(request, message: ["role": "assistant", "content": "Kommentar analysiert."])
        }
        browser.chat.drafts[browser.space] = "Analysiere den Post"
        browser.chat.send(browser: browser)
        try await finish(browser)
        XCTAssertEqual(calls, 4)
        XCTAssertNil(browser.chat.error)
        XCTAssertFalse(browser.chat.entries.contains { $0.kind == "error" })
        XCTAssertEqual(browser.chat.entries.last?.text, "Kommentar analysiert.")
        let clicked = try await tab.webView.evaluateJavaScript("document.body.getAttribute('data-clicked')") as? String
        XCTAssertEqual(clicked, "true")
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
