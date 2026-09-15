import SwiftUI
import AppKit

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
    /// Optional input controls between the message and the buttons. Web dialogs
    /// use this for `window.prompt` text and HTTP authentication credentials.
    @ViewBuilder var fields: () -> Fields

    var body: some View {
        ZStack {
            Color.black.opacity(0.24).ignoresSafeArea()
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
            }
            .padding(20)
            .frame(width: 390)
            .background(paper, in: RoundedRectangle(cornerRadius: 17))
            .overlay(RoundedRectangle(cornerRadius: 17).stroke(YOBROTheme.border.opacity(0.55), lineWidth: 1))
            .shadow(color: .black.opacity(0.22), radius: 30, y: 12)
        }
        .foregroundStyle(ink)
        .transition(.opacity.combined(with: .scale(scale: 0.97)))
        .zIndex(1000)
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
            fields: { EmptyView() }
        )
    }
}
