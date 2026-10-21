import SwiftUI
import AppKit
import WebKit

/// Gives ordinary SwiftUI surfaces an explicit arrow cursor region. AppKit's
/// text views otherwise leave the I-beam active when the pointer moves from an
/// editor onto chrome that has no cursor rect of its own. Descendant controls
/// (NSTextField/NSTextView) still provide their more specific cursor regions.
struct YOBROArrowCursorRegion: NSViewRepresentable {
    func makeNSView(context: Context) -> ArrowCursorView { ArrowCursorView() }
    func updateNSView(_ nsView: ArrowCursorView, context: Context) {}

    final class ArrowCursorView: NSView {
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            window?.invalidateCursorRects(for: self)
        }

        override func resetCursorRects() {
            super.resetCursorRects()
            addCursorRect(bounds, cursor: .arrow)
        }

        override func hitTest(_ point: NSPoint) -> NSView? { nil }
    }
}

/// Hit area belongs to the label, so whitespace responds to clicks too.
/// Layout reserves this space: adjacent controls never overlap.
struct YOBROButtonStyle: ButtonStyle {
    var minimumSize: CGFloat = 40
    func makeBody(configuration: Configuration) -> some View {
        Surface(configuration: configuration, minimumSize: minimumSize)
    }
    private struct Surface: View {
        let configuration: ButtonStyle.Configuration
        let minimumSize: CGFloat
        @Environment(\.isEnabled) private var enabled
        @State private var hovering = false
        var body: some View {
            configuration.label
                .frame(minWidth: minimumSize, minHeight: minimumSize)
                .contentShape(Rectangle())
                .background(ink.opacity(enabled && configuration.isPressed ? 0.12 : enabled && hovering ? 0.065 : 0), in: RoundedRectangle(cornerRadius: 9))
                .opacity(enabled ? 1 : 0.4)
                .onHover { hovering = $0 }
        }
    }
}

/// A browser-owned alternative to AppKit/SwiftUI system alerts. Keeping this
/// in the shared chrome layer gives confirmations the same visual language in
/// web pages, Mail, settings, and the library.
struct YOBRODialogOverlay<Fields: View>: View {
    let icon: String
    let title: String
    let message: String
    let confirmTitle: String
    var cancelTitle: String? = nil
    var destructive = false
    var confirmDisabled = false
    let confirm: () -> Void
    var cancel: () -> Void = {}
    /// False for plain notices, which take keyboard focus away from the page
    /// behind them. A dialog with fields focuses its own field instead.
    var hasFields = true
    /// Optional input controls between the message and the buttons. Web dialogs
    /// use this for `window.prompt` text and HTTP authentication credentials.
    @ViewBuilder var fields: () -> Fields
    @State private var focus = DialogFocus()

    var body: some View {
        ZStack {
            Color.black.opacity(0.24).ignoresSafeArea().accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 16) {
                HStack(alignment: .top, spacing: 12) {
                    Image(systemName: icon)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(destructive ? Color.red : moss)
                        .frame(width: 36, height: 36)
                        .background((destructive ? Color.red : moss).opacity(0.11), in: RoundedRectangle(cornerRadius: 10))
                    VStack(alignment: .leading, spacing: 5) {
                        Text(title).font(.system(size: 17, weight: .semibold, design: .rounded))
                        if !message.isEmpty {
                            Text(message).font(.system(size: 12)).foregroundStyle(ink.opacity(0.68)).lineSpacing(3).fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
                fields()
                HStack(spacing: 9) {
                    Spacer()
                    if let cancelTitle {
                        Button(cancelTitle, action: cancel)
                            .buttonStyle(YOBROPopupButtonStyle())
                            .keyboardShortcut(.cancelAction)
                    }
                    Button(confirmTitle, action: confirm)
                        .buttonStyle(YOBROPopupButtonStyle(prominent: true, color: destructive ? .red : moss))
                        .keyboardShortcut(.defaultAction)
                        .disabled(confirmDisabled)
                }
                .background {
                    // Esc also closes a notice that only offers OK.
                    if cancelTitle == nil {
                        Button("", action: confirm)
                            .keyboardShortcut(.cancelAction)
                            .opacity(0)
                            .allowsHitTesting(false)
                            .accessibilityHidden(true)
                    }
                }
            }
            .padding(20)
            .frame(width: 390)
            .background(paper, in: RoundedRectangle(cornerRadius: 17))
            .overlay(RoundedRectangle(cornerRadius: 17).stroke(YOBROTheme.border.opacity(0.55), lineWidth: 1))
            .shadow(color: .black.opacity(0.22), radius: 30, y: 12)
            .accessibilityElement(children: .contain)
            .accessibilityAddTraits(.isModal)
        }
        .foregroundStyle(ink)
        .transition(.opacity.combined(with: .scale(scale: 0.97)))
        .zIndex(1000)
        .onAppear {
            AccessibilityNotification.Announcement(message.isEmpty ? title : "\(title). \(message)").post()
            focus.take(hasFields: hasFields)
        }
        .onDisappear { focus.restore() }
    }
}

/// Remembers what had keyboard focus before an in-window dialog took it and
/// gives it back when the dialog closes, so typed keys never reach the page.
@MainActor
private final class DialogFocus {
    private weak var window: NSWindow?
    private weak var responder: NSResponder?

    func take(hasFields: Bool) {
        guard let window = NSApp.keyWindow, let current = window.firstResponder, current !== window else { return }
        // A focused text field answers through the window's shared field editor.
        let owner = (current as? NSTextView).flatMap { $0.isFieldEditor ? $0.delegate as? NSResponder : nil } ?? current
        // Fields take focus themselves; only a web page must still let go, since
        // page dialogs pass fields for alert() and confirm() too.
        if hasFields {
            guard let view = owner as? NSView, sequence(first: view, next: \.superview).contains(where: { $0 is WKWebView }) else { return }
        }
        self.window = window
        responder = owner
        window.makeFirstResponder(nil)
    }

    func restore() {
        guard let window, let responder else { return }
        self.responder = nil
        if let view = responder as? NSView, view.window !== window { return }
        window.makeFirstResponder(responder)
    }
}

/// Keeps the plain message-and-buttons form of the dialog available without
/// spelling out an empty field list at every call site.
extension YOBRODialogOverlay where Fields == EmptyView {
    init(
        icon: String,
        title: String,
        message: String,
        confirmTitle: String,
        cancelTitle: String? = nil,
        destructive: Bool = false,
        confirmDisabled: Bool = false,
        confirm: @escaping () -> Void,
        cancel: @escaping () -> Void = {}
    ) {
        self.init(
            icon: icon,
            title: title,
            message: message,
            confirmTitle: confirmTitle,
            cancelTitle: cancelTitle,
            destructive: destructive,
            confirmDisabled: confirmDisabled,
            confirm: confirm,
            cancel: cancel,
            hasFields: false,
            fields: { EmptyView() }
        )
    }
}
