import AppKit
import AVFoundation
import Security
import Speech

enum VoiceWake {
    static let window: TimeInterval = 60
    static let pattern = #"^\W*(?:(?:yo|jo|yoo|yoah)\W+bro|yobro|jobro)\b\W*"#

    static func command(in text: String) -> String? {
        guard let range = text.range(of: pattern, options: [.regularExpression, .caseInsensitive]) else { return nil }
        return String(text[range.upperBound...]).trimmingCharacters(in: .punctuationCharacters.union(.whitespacesAndNewlines))
    }

    static func isStopCommand(_ text: String) -> Bool {
        guard let command = command(in: text)?.lowercased() else { return false }
        return command == "stopp" || command == "stop"
    }

    static func acceptsFollowUp(until deadline: Date?, at now: Date = Date()) -> Bool {
        guard let deadline else { return false }
        return now < deadline
    }

    static func silenceTimeout(for transcript: String, wakeRecognized: Bool) -> TimeInterval {
        // Keep the same recognition session open after a wake word on its own.
        // Restarting it at the ordinary pause boundary loses the next sentence.
        if wakeRecognized && command(in: transcript)?.isEmpty == true { return 4 }
        return 2.5
    }
}

enum VoiceDictation {
    static func finalizationTimeout(hasTranscript: Bool) -> TimeInterval {
        // A partial result is a usable fallback, but leave a short window for
        // Apple's final result to include the last spoken word.
        hasTranscript ? 1.5 : 3
    }

    static func combine(segments: [String], final: String) -> String {
        (segments + [final])
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
            .joined(separator: " ")
    }
}

/// Keeps the microphone tap attached while recognition requests change.
/// The tap never appends to a request after it has been ended or cancelled.
private final class VoiceAudioRouter: @unchecked Sendable {
    private let lock = NSLock()
    private var request: SFSpeechAudioBufferRecognitionRequest?

    func use(_ request: SFSpeechAudioBufferRecognitionRequest?) {
        lock.lock()
        self.request = request
        lock.unlock()
    }

    func append(_ buffer: AVAudioPCMBuffer) -> SFSpeechAudioBufferRecognitionRequest? {
        lock.lock()
        defer { lock.unlock() }
        request?.append(buffer)
        return request
    }
}

private final class VoicePlaybackCompletion: @unchecked Sendable {
    private let lock = NSLock()
    private var continuation: CheckedContinuation<Void, Error>?
    private var result: Result<Void, Error>?

    func wait() async throws {
        try await withCheckedThrowingContinuation { continuation in
            lock.lock()
            if let result {
                lock.unlock()
                continuation.resume(with: result)
            } else {
                self.continuation = continuation
                lock.unlock()
            }
        }
    }

    func finish(_ result: Result<Void, Error>) {
        lock.lock()
        guard self.result == nil else { lock.unlock(); return }
        self.result = result
        let continuation = self.continuation
        self.continuation = nil
        lock.unlock()
        continuation?.resume(with: result)
    }
}

enum VoiceActivationMode: String, CaseIterable, Identifiable {
    case wakeWord, rightOption
    var id: String { rawValue }
    var title: String {
        switch self {
        case .wakeWord: L("„Yo Bro“", "“Yo Bro”")
        case .rightOption: L("Rechte Option + „Yo Bro“", "Right Option + “Yo Bro”")
        }
    }
}

enum FishVoiceKey {
    private static func query(service: String, account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service, kSecAttrAccount as String: account]
    }

    private static func read(service: String, account: String) -> String? {
        var item = query(service: service, account: account)
        item[kSecReturnData as String] = true
        item[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        guard SecItemCopyMatching(item as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func load() -> String {
        read(service: "local.yobro.voice", account: "fish-audio") ?? ""
    }

    static func save(_ key: String) throws {
        let value = key.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty else { throw YOBROError.message(L("Fish-Audio-Schlüssel fehlt.", "Fish Audio key is missing.")) }
        let item = query(service: "local.yobro.voice", account: "fish-audio")
        let data = Data(value.utf8)
        let status = SecItemUpdate(item as CFDictionary, [kSecValueData as String: data] as CFDictionary)
        if status == errSecItemNotFound {
            var added = item
            added[kSecValueData as String] = data
            added[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
            guard SecItemAdd(added as CFDictionary, nil) == errSecSuccess else {
                throw YOBROError.message(L("Fish-Audio-Schlüssel konnte nicht gespeichert werden.", "Could not save Fish Audio key."))
            }
        } else if status != errSecSuccess {
            throw YOBROError.message(L("Fish-Audio-Schlüssel konnte nicht gespeichert werden.", "Could not save Fish Audio key."))
        }
    }
}

/// Turns spoken requests into ordinary YoBro Space chat turns. Existing agent
/// permissions and approval prompts still govern mail and browser actions.
@MainActor
final class YoBroVoice: NSObject, ObservableObject {
    @Published private(set) var enabled = false
    @Published private(set) var isListening = false
    @Published private(set) var state = L("Sprachmodus aus", "Voice mode off")
    @Published private(set) var activeUntil: Date?
    @Published private(set) var lastHeard = ""
    @Published private(set) var lastTranscriptionSeconds: TimeInterval?
    @Published private(set) var lastAgentSeconds: TimeInterval?
    @Published var activationMode: VoiceActivationMode = VoiceActivationMode(rawValue: UserDefaults.standard.string(forKey: "YOBRO.voiceActivationMode") ?? "") ?? .wakeWord {
        didSet {
            UserDefaults.standard.set(activationMode.rawValue, forKey: "YOBRO.voiceActivationMode")
            guard activationMode != oldValue else { return }
            activeUntil = nil
            activeWindowTask?.cancel(); activeWindowTask = nil
            dictationSegments.removeAll()
            timer?.invalidate(); timer = nil
            finishRecognition(cancel: true)
            removeOptionMonitors()
            if enabled { activateInput() }
        }
    }
    @Published var fishKey = FishVoiceKey.load()

    private let recognizer = SFSpeechRecognizer(locale: Locale(identifier: YOBROLanguage.speechLanguage))
    private let engine = AVAudioEngine()
    private let audioRouter = VoiceAudioRouter()
    private var request: SFSpeechAudioBufferRecognitionRequest?
    private var recognition: SFSpeechRecognitionTask?
    private var tapInstalled = false
    private var timer: Timer?
    private var heardSpeech = false
    private var lastSound = Date()
    private var started = Date()
    private var endingPhrase = false
    private var endingStarted: Date?
    private var speechEndedAt: Date?
    private var agentStartedAt: Date?
    private var lastTranscript = ""
    private var wakeRecognizedInPhrase = false
    private var recognitionInterruptOnly = false
    private var recognitionWasOption = false
    private var activeWindowTask: Task<Void, Never>?
    private var localOptionMonitor: Any?
    private var globalOptionMonitor: Any?
    private var optionDown = false
    private var dictationSegments: [String] = []
    private var isSpeaking = false
    private var awaitingSpace: String?
    private var speechTask: Task<Void, Never>?
    private weak var browser: BrowserModel?
    private var generation = UUID()
    private var starting = false
    private let voiceID = "691e1028fd0444a785c58948993d1123"
    private var configurationObserver: NSObjectProtocol?
    // Voice is opt-in: listening starts only after the user turned it on once.
    private var shouldResume = UserDefaults.standard.object(forKey: "YOBRO.voiceEnabled") as? Bool ?? false

    /// Without a wake word or push-to-talk, audio stays on this Mac when it can
    /// recognize speech locally. Only explicit requests may use Apple's server.
    var recognizesWakeWordOnDevice: Bool { recognizer?.supportsOnDeviceRecognition == true }

    var hasCurrentActivity: Bool {
        isSpeaking || awaitingSpace != nil || VoiceWake.acceptsFollowUp(until: activeUntil)
            || optionRecordingActive
    }

    var optionRecordingActive: Bool { activationMode == .rightOption && optionDown && isListening }

    var latencySummary: String? {
        guard lastTranscriptionSeconds != nil || lastAgentSeconds != nil else { return nil }
        let transcription = lastTranscriptionSeconds.map { String(format: "%.1f s", $0) } ?? "–"
        let agent = lastAgentSeconds.map { String(format: "%.1f s", $0) } ?? "–"
        return L("Zuletzt: Sprache → Text \(transcription) · Agent \(agent)",
                 "Last: speech → text \(transcription) · agent \(agent)")
    }

    func resumeIfEnabled(browser: BrowserModel) async {
        guard shouldResume, !enabled, !starting, browser.agentEnabled, browser.isProfileActive else { return }
        await toggle(browser: browser)
    }

    @discardableResult
    func saveKey() -> Bool {
        do { try FishVoiceKey.save(fishKey); state = L("Fish Audio ist bereit.", "Fish Audio is ready."); return true }
        catch { state = error.localizedDescription; return false }
    }

    func toggle(browser: BrowserModel) async {
        if enabled || starting {
            shouldResume = false
            UserDefaults.standard.set(false, forKey: "YOBRO.voiceEnabled")
            stop(); return
        }
        starting = true
        let startGeneration = UUID()
        generation = startGeneration
        defer { starting = false }
        guard browser.isProfileActive, browser.agentEnabled else {
            state = L("Agentenzugriff zuerst aktivieren.", "Enable agent access first."); return
        }
        guard !browser.chat.connection.endpoint.isEmpty, !browser.chat.connection.model.isEmpty else {
            state = L("Zuerst ein Modell mit YoBro verbinden.", "Connect a model to YoBro first."); return
        }
        let permission = await withCheckedContinuation { continuation in
            SFSpeechRecognizer.requestAuthorization { continuation.resume(returning: $0) }
        }
        guard generation == startGeneration else { return }
        guard permission == .authorized else { state = L("Spracherkennung nicht erlaubt.", "Speech recognition not allowed."); return }
        let microphone = await AVCaptureDevice.requestAccess(for: .audio)
        guard generation == startGeneration else { return }
        guard microphone else { state = L("Mikrofon nicht erlaubt.", "Microphone not allowed."); return }
        guard recognizer?.isAvailable == true else { state = L("Spracherkennung ist derzeit nicht verfügbar.", "Speech recognition is currently unavailable."); return }
        self.browser = browser
        enabled = true
        shouldResume = true
        UserDefaults.standard.set(true, forKey: "YOBRO.voiceEnabled")
        generation = UUID()
        browser.showAgent = true
        if configurationObserver == nil {
            configurationObserver = NotificationCenter.default.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.audioConfigurationChanged() }
            }
        }
        browser.chat.voiceResponse = { [weak self] space, answer in
            self?.speechTask?.cancel()
            self?.speechTask = Task { await self?.receivedAnswer(space: space, answer: answer) }
        }
        activateInput()
    }

    private func activateInput() {
        if activationMode == .rightOption { installOptionMonitors() }
        listen()
    }

    private func installOptionMonitors() {
        guard localOptionMonitor == nil, globalOptionMonitor == nil else { return }
        if !CGPreflightListenEventAccess() { _ = CGRequestListenEventAccess() }
        localOptionMonitor = NSEvent.addLocalMonitorForEvents(matching: .flagsChanged) { [weak self] event in
            Task { @MainActor in self?.optionChanged(event) }
            return event
        }
        globalOptionMonitor = NSEvent.addGlobalMonitorForEvents(matching: .flagsChanged) { [weak self] event in
            Task { @MainActor in self?.optionChanged(event) }
        }
    }

    private func removeOptionMonitors() {
        if let localOptionMonitor { NSEvent.removeMonitor(localOptionMonitor) }
        if let globalOptionMonitor { NSEvent.removeMonitor(globalOptionMonitor) }
        localOptionMonitor = nil
        globalOptionMonitor = nil
        optionDown = false
    }

    private func optionChanged(_ event: NSEvent) {
        guard enabled, activationMode == .rightOption, event.keyCode == 61 else { return }
        let down = event.modifierFlags.contains(.option)
        guard down != optionDown else { return }
        optionDown = down
        if down {
            guard !endingPhrase, awaitingSpace == nil, !isSpeaking else {
                optionDown = false
                return
            }
            dictationSegments.removeAll()
            lastTranscriptionSeconds = nil
            listen()
        } else {
            releaseOptionRecording()
        }
    }

    private func releaseOptionRecording() {
        guard activationMode == .rightOption,
              recognitionWasOption || !dictationSegments.isEmpty else { return }
        if request != nil && !endingPhrase {
            endPhrase()
        } else if request == nil {
            completeOptionRecording(with: "")
        }
    }

    private func completeOptionRecording(with final: String) {
        let text = VoiceDictation.combine(segments: dictationSegments, final: final)
        dictationSegments.removeAll()
        if let speechEndedAt { lastTranscriptionSeconds = Date().timeIntervalSince(speechEndedAt) }
        speechEndedAt = nil
        if text.isEmpty {
            listen()
            state = L("Keine Sprache erkannt · erneut aufnehmen", "No speech recognized · record again")
            return
        }
        process(text, fromOption: true)
    }

    func stop() {
        enabled = false
        generation = UUID()
        activeUntil = nil
        activeWindowTask?.cancel(); activeWindowTask = nil
        lastHeard = ""
        dictationSegments.removeAll()
        awaitingSpace = nil
        speechEndedAt = nil; agentStartedAt = nil
        optionDown = false
        speechTask?.cancel(); speechTask = nil
        isSpeaking = false
        timer?.invalidate(); timer = nil
        finishRecognition(cancel: true)
        removeOptionMonitors()
        if let configurationObserver { NotificationCenter.default.removeObserver(configurationObserver) }
        configurationObserver = nil
        browser?.chat.voiceResponse = nil
        state = L("Sprachmodus aus", "Voice mode off")
    }

    /// A new input device (for example AirPods) stops the engine and changes
    /// the input format. Reinstall the tap instead of silently going deaf.
    private func audioConfigurationChanged() {
        guard enabled else { return }
        timer?.invalidate(); timer = nil
        finishRecognition(cancel: true)
        if !isSpeaking { listen() }
    }

    func stopCurrentAction(browser: BrowserModel) {
        browser.chat.cancel()
        speechTask?.cancel(); speechTask = nil
        generation = UUID()
        awaitingSpace = nil
        dictationSegments.removeAll()
        optionDown = false
        isSpeaking = false
        speechEndedAt = nil; agentStartedAt = nil
        activeUntil = nil
        activeWindowTask?.cancel(); activeWindowTask = nil
        timer?.invalidate(); timer = nil
        finishRecognition(cancel: true)
        for tab in browser.tabs where browser.agentTabIDs.contains(tab.id) {
            tab.webView.stopLoading()
        }
        if enabled {
            listen()
            state = L("Gestoppt · Yo Bro", "Stopped · Yo Bro")
        }
    }

    private func finishRecognition(cancel: Bool) {
        isListening = false
        audioRouter.use(nil)
        if engine.isRunning { engine.stop() }
        if tapInstalled { engine.inputNode.removeTap(onBus: 0); tapInstalled = false }
        if cancel { recognition?.cancel() } else { request?.endAudio() }
        if cancel { recognition = nil; request = nil }
    }

    private func changeRecognitionKeepingMicrophone() {
        audioRouter.use(nil)
        recognition?.cancel()
        recognition = nil
        request = nil
    }

    private func listen() {
        guard enabled else { return }
        let interruptOnly = awaitingSpace != nil || isSpeaking
        if engine.isRunning {
            changeRecognitionKeepingMicrophone()
        } else {
            finishRecognition(cancel: true)
            // Macs without an input device report a zero format; installing a
            // tap with it raises an Objective-C exception and ends the app.
            let format = engine.inputNode.outputFormat(forBus: 0)
            guard format.sampleRate > 0, format.channelCount > 0 else {
                stop()
                state = L("Kein Mikrofon gefunden.", "No microphone found.")
                return
            }
        }
        let optionRecording = activationMode == .rightOption && optionDown && !interruptOnly
        let explicitRequest = optionRecording || !interruptOnly && VoiceWake.acceptsFollowUp(until: activeUntil)
        let speechRequest = SFSpeechAudioBufferRecognitionRequest()
        speechRequest.shouldReportPartialResults = true
        // Ambient listening for the wake word stays on this Mac where possible.
        // After "Yo Bro" or push-to-talk, Apple's more accurate server may help.
        speechRequest.requiresOnDeviceRecognition = recognizesWakeWordOnDevice && !explicitRequest
        speechRequest.contextualStrings = ["Yo Bro", "YoBro", "ChatGPT", "Spotify"]
        request = speechRequest
        audioRouter.use(speechRequest)
        heardSpeech = false; endingPhrase = false; endingStarted = nil; lastTranscript = ""; wakeRecognizedInPhrase = false; recognitionInterruptOnly = interruptOnly; recognitionWasOption = optionRecording; started = Date()
        state = interruptOnly ? L("YoBro arbeitet …", "YoBro is working…") : recognitionWasOption ? L("Sprich jetzt · zum Senden loslassen", "Speak now · release to send") : VoiceWake.acceptsFollowUp(until: activeUntil)
            ? L("Ich höre zu · ohne Aktivierungswort", "Listening · no wake word needed")
            : L("Sag „Yo Bro“", "Say “Yo Bro”")
        recognition = recognizer?.recognitionTask(with: speechRequest) { [weak self] result, error in
            Task { @MainActor in self?.receive(result: result, error: error, for: speechRequest) }
        }
        if !engine.isRunning {
            let input = engine.inputNode
            let format = input.outputFormat(forBus: 0)
            input.installTap(onBus: 0, bufferSize: 1024, format: format) { [weak self] buffer, _ in
                guard let self, let source = self.audioRouter.append(buffer),
                      let channel = buffer.floatChannelData?[0] else { return }
                let count = Int(buffer.frameLength)
                guard count > 0 else { return }
                var power: Float = 0
                for i in 0..<count { power += channel[i] * channel[i] }
                if power / Float(count) > 0.000025 {
                    DispatchQueue.main.async {
                        guard self.request === source else { return }
                        self.heardSpeech = true
                        self.lastSound = Date()
                    }
                }
            }
            tapInstalled = true
            do { engine.prepare(); try engine.start() }
            catch { finishRecognition(cancel: true); removeOptionMonitors(); enabled = false; state = error.localizedDescription; return }
        }
        isListening = true
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
    }

    private func tick() {
        guard enabled else { return }
        if recognitionWasOption && optionDown && !endingPhrase {
            if Date().timeIntervalSince(started) > 40 {
                let segment = lastTranscript.trimmingCharacters(in: .whitespacesAndNewlines)
                if !segment.isEmpty { dictationSegments.append(segment) }
                listen()
            }
        }
        if let activeUntil, Date() >= activeUntil {
            self.activeUntil = nil
            if !recognitionWasOption && isListening {
                state = L("Sag „Yo Bro“", "Say “Yo Bro”")
            }
        }
        let silenceTimeout = VoiceWake.silenceTimeout(for: lastTranscript, wakeRecognized: wakeRecognizedInPhrase)
        if !recognitionWasOption && !endingPhrase,
           (heardSpeech && Date().timeIntervalSince(lastSound) > silenceTimeout
            || Date().timeIntervalSince(started) > 45 && Date().timeIntervalSince(lastSound) > 0.5) {
            let transcript = lastTranscript
            changeRecognitionKeepingMicrophone()
            if transcript.isEmpty { listen() } else { process(transcript) }
        }
        if let endingStarted,
           Date().timeIntervalSince(endingStarted) > VoiceDictation.finalizationTimeout(hasTranscript: !lastTranscript.isEmpty || !dictationSegments.isEmpty) {
            timer?.invalidate(); timer = nil
            let fallback = lastTranscript
            finishRecognition(cancel: true)
            if recognitionWasOption { completeOptionRecording(with: fallback) }
            else if fallback.isEmpty { listen() }
            else { process(fallback) }
        }
    }

    private func endPhrase() {
        guard !endingPhrase else { return }
        endingPhrase = true
        endingStarted = Date()
        if recognitionWasOption { speechEndedAt = endingStarted }
        finishRecognition(cancel: false)
        state = L("Transkribiere …", "Transcribing…")
    }

    private func receive(result: SFSpeechRecognitionResult?, error: Error?, for source: SFSpeechAudioBufferRecognitionRequest) {
        guard enabled, request === source else { return }
        if let result, result.isFinal {
            timer?.invalidate(); timer = nil
            let text = result.bestTranscription.formattedString
            if recognitionWasOption {
                if optionDown && !endingPhrase {
                    let segment = text.trimmingCharacters(in: .whitespacesAndNewlines)
                    if !segment.isEmpty { dictationSegments.append(segment) }
                    changeRecognitionKeepingMicrophone()
                    listen()
                } else {
                    finishRecognition(cancel: true)
                    completeOptionRecording(with: text.isEmpty ? lastTranscript : text)
                }
            } else {
                changeRecognitionKeepingMicrophone()
                process(text)
            }
        } else if let result {
            let text = result.bestTranscription.formattedString
            if text != lastTranscript && !text.isEmpty {
                lastTranscript = text
                heardSpeech = true
                lastSound = Date()
                if recognitionWasOption && optionDown && !recognitionInterruptOnly {
                    let words = text.split(whereSeparator: \.isWhitespace).count
                    state = L("Aufnahme · \(words) Wörter erkannt", "Recording · \(words) words recognized")
                }
                if VoiceWake.isStopCommand(text), let browser {
                    stopCurrentAction(browser: browser)
                    return
                }
                if !recognitionWasOption, !recognitionInterruptOnly, !wakeRecognizedInPhrase,
                   VoiceWake.command(in: text) != nil {
                    wakeRecognizedInPhrase = true
                    activateWakeWindow()
                    state = L("Yo Bro erkannt · sprich weiter", "Yo Bro recognized · keep speaking")
                    NSSound(named: NSSound.Name("Tink"))?.play()
                }
            }
        } else if error != nil {
            timer?.invalidate(); timer = nil
            let fallback = lastTranscript
            if recognitionWasOption {
                if optionDown && !endingPhrase {
                    let segment = fallback.trimmingCharacters(in: .whitespacesAndNewlines)
                    if !segment.isEmpty { dictationSegments.append(segment) }
                    changeRecognitionKeepingMicrophone()
                    listen()
                } else {
                    finishRecognition(cancel: true)
                    completeOptionRecording(with: fallback)
                }
                return
            }
            changeRecognitionKeepingMicrophone()
            if !fallback.isEmpty { process(fallback); return }
            if engine.isRunning {
                listen()
            } else {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                    guard let self, self.request == nil else { return }
                    self.listen()
                }
            }
        }
    }

    private func process(_ raw: String, fromOption: Bool = false) {
        guard enabled, let browser, browser.isProfileActive, browser.agentEnabled else { stop(); return }
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if !fromOption && !text.isEmpty {
            lastTranscriptionSeconds = Date().timeIntervalSince(lastSound)
        }
        if !text.isEmpty { lastHeard = String(text.prefix(160)) }
        if VoiceWake.isStopCommand(text) {
            stopCurrentAction(browser: browser)
            return
        }
        if recognitionInterruptOnly || browser.chat.running || awaitingSpace != nil || isSpeaking {
            listen()
            return
        }
        let acceptsFollowUp = VoiceWake.acceptsFollowUp(until: activeUntil)
        if fromOption {
            if !text.isEmpty { submit(text, browser: browser) }
            else { listen() }
        } else if let command = VoiceWake.command(in: text) {
            if !wakeRecognizedInPhrase { activateWakeWindow() }
            if command.isEmpty {
                listen()
                return
            }
            submit(command, browser: browser)
        } else if acceptsFollowUp && !text.isEmpty {
            submit(text, browser: browser)
        } else {
            listen()
        }
    }

    private func activateWakeWindow() {
        activeUntil = Date().addingTimeInterval(VoiceWake.window)
        activeWindowTask?.cancel()
        activeWindowTask = Task { @MainActor [weak self] in
            do { try await Task.sleep(for: .seconds(VoiceWake.window)) }
            catch { return }
            guard let self else { return }
            self.activeUntil = nil
            self.activeWindowTask = nil
            if self.isListening && !self.recognitionWasOption {
                self.state = L("Sag „Yo Bro“", "Say “Yo Bro”")
            }
        }
    }

    private func submit(_ text: String, browser: BrowserModel) {
        guard !browser.chat.running else { listen(); return }
        agentStartedAt = Date()
        lastAgentSeconds = nil
        awaitingSpace = browser.space
        browser.showAgent = true
        browser.chat.send(text, browser: browser)
        if !browser.chat.running {
            awaitingSpace = nil
            agentStartedAt = nil
            state = browser.chat.error ?? L("Agent nicht bereit.", "Agent not ready.")
            listen()
        } else {
            state = L("YoBro antwortet …", "YoBro is answering…")
            listen()
        }
    }

    private func receivedAnswer(space: String, answer: String?) async {
        guard enabled, awaitingSpace == space else { return }
        awaitingSpace = nil
        if let agentStartedAt { lastAgentSeconds = Date().timeIntervalSince(agentStartedAt) }
        agentStartedAt = nil
        // The input recognizer and microphone must be off during playback.
        // Keeping them running competes with audio output and makes the blue
        // listening frame misleading while YoBro is speaking.
        activeUntil = nil
        activeWindowTask?.cancel(); activeWindowTask = nil
        timer?.invalidate(); timer = nil
        finishRecognition(cancel: true)
        guard let answer, !answer.isEmpty else {
            state = browser?.chat.error ?? L("Keine Antwort erhalten.", "No answer received.")
            listen()
            return
        }
        await speak(answer)
    }

    private func speak(_ text: String) async {
        guard enabled else { return }
        isSpeaking = true
        state = L("YoBro spricht …", "YoBro is speaking…")
        let current = generation
        let privacy = browser.map(BroPrivacySession.forBrowser) ?? BroPrivacySession()
        let segments = BroSpeechText.utterances(in: BroSpeechText.prepare(text))
            .flatMap { privacy.segments(in: $0) }
        do {
            for segment in segments where !segment.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                try Task.checkCancellation()
                if fishKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || segment.sensitive || segment.text.trimmingCharacters(in: .whitespacesAndNewlines).count < 5 {
                    try await speakLocally(segment.text)
                } else {
                    do {
                        try await streamFish(
                            segment.text,
                            maxAudioDuration: segment.text == "Haha." ? BroSpeechText.laughAudioLimit : nil
                        )
                    }
                    catch { try await speakLocally(segment.text) }
                }
            }
        }
        catch is CancellationError {}
        catch { state = error.localizedDescription }
        isSpeaking = false
        guard enabled, generation == current else { return }
        listen()
    }

    private func speakLocally(_ text: String) async throws {
        let synthesizer = AVSpeechSynthesizer()
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = AVSpeechSynthesisVoice(language: YOBROLanguage.speechLanguage)
        synthesizer.speak(utterance)
        defer { synthesizer.stopSpeaking(at: .immediate) }
        while synthesizer.isSpeaking {
            try Task.checkCancellation()
            try await Task.sleep(for: .milliseconds(80))
        }
    }

    private func streamFish(_ text: String, maxAudioDuration: TimeInterval? = nil) async throws {
        var request = URLRequest(url: URL(string: "https://api.fish.audio/v1/tts")!)
        request.httpMethod = "POST"
        request.setValue("Bearer \(fishKey)", forHTTPHeaderField: "Authorization")
        request.setValue("s2.1-pro-free", forHTTPHeaderField: "model")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: ["text": text, "reference_id": voiceID,
                                                                      "format": "pcm", "sample_rate": 44100, "latency": "balanced"])
        let (bytes, response) = try await URLSession.shared.bytes(for: request)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            throw YOBROError.message(L("Fish Audio konnte nicht sprechen.", "Fish Audio could not speak."))
        }
        let output = AVAudioEngine()
        let player = AVAudioPlayerNode()
        let format = AVAudioFormat(standardFormatWithSampleRate: 44100, channels: 1)!
        output.attach(player)
        output.connect(player, to: output.mainMixerNode, format: format)
        try output.start()
        player.play()
        defer { player.stop(); output.stop() }
        var raw = Data()
        var lastBuffer: AVAudioPCMBuffer?
        let maxAudioBytes = maxAudioDuration.map { Int(44100 * 2 * $0) }
        var receivedAudioBytes = 0
        for try await byte in bytes {
            try Task.checkCancellation()
            raw.append(byte)
            receivedAudioBytes += 1
            if raw.count >= 8192 {
                if let lastBuffer { player.scheduleBuffer(lastBuffer, completionHandler: nil) }
                let size = raw.count & ~1
                lastBuffer = Self.pcmBuffer(raw.prefix(size), format: format)
                raw.removeFirst(size)
                await Task.yield()
            }
            if let maxAudioBytes, receivedAudioBytes >= maxAudioBytes { break }
        }
        guard raw.count % 2 == 0 else { throw YOBROError.message("Fish Audio hat unvollständige Audiodaten geliefert.") }
        if raw.count >= 2 {
            if let lastBuffer { player.scheduleBuffer(lastBuffer, completionHandler: nil) }
            lastBuffer = Self.pcmBuffer(raw.prefix(raw.count & ~1), format: format)
        }
        guard let finalBuffer = lastBuffer else { throw YOBROError.message("Fish Audio hat keine Audiodaten geliefert.") }
        try Task.checkCancellation()
        let completion = VoicePlaybackCompletion()
        player.scheduleBuffer(finalBuffer, completionCallbackType: .dataPlayedBack) { _ in
            completion.finish(.success(()))
        }
        try await withTaskCancellationHandler {
            try await completion.wait()
        } onCancel: {
            player.stop()
            completion.finish(.failure(CancellationError()))
        }
    }

    private static func pcmBuffer(_ data: Data, format: AVAudioFormat) -> AVAudioPCMBuffer {
        let frames = data.count / 2
        let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(frames))!
        buffer.frameLength = AVAudioFrameCount(frames)
        let channel = buffer.floatChannelData![0]
        data.withUnsafeBytes { bytes in
            for i in 0..<frames {
                let sample = Int16(bitPattern: UInt16(bytes[i * 2]) | UInt16(bytes[i * 2 + 1]) << 8)
                channel[i] = Float(sample) / 32768
            }
        }
        return buffer
    }
}
