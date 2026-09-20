import SwiftUI
import AppKit

/// Native multiline editing keeps selection, undo, and input-method composition intact.
struct ChatMessageInput: NSViewRepresentable {
    @Binding var text: String
    let onSend: () -> Void

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSScrollView()
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        let editor = ChatMessageTextView(frame: NSRect(x: 0, y: 0, width: 300, height: 40))
        editor.minSize = NSSize(width: 0, height: 40)
        editor.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        editor.isRichText = false
        editor.isAutomaticQuoteSubstitutionEnabled = false
        editor.isAutomaticDashSubstitutionEnabled = false
        editor.allowsUndo = true
        editor.drawsBackground = false
        editor.font = .systemFont(ofSize: 12)
        editor.textColor = .labelColor
        editor.insertionPointColor = .labelColor
        editor.textContainerInset = NSSize(width: 0, height: 4)
        editor.isVerticallyResizable = true
        editor.isHorizontallyResizable = false
        editor.autoresizingMask = [.width]
        editor.textContainer?.widthTracksTextView = true
        editor.textContainer?.containerSize = NSSize(width: 300, height: CGFloat.greatestFiniteMagnitude)
        editor.setAccessibilityLabel(L("Nachricht an diesen Space …", "Message this Space …"))
        editor.delegate = context.coordinator
        editor.onSend = onSend
        scroll.documentView = editor
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        context.coordinator.parent = self
        guard let editor = scroll.documentView as? ChatMessageTextView else { return }
        editor.onSend = onSend
        if editor.string != text {
            editor.string = text
            editor.setSelectedRange(NSRange(location: (text as NSString).length, length: 0))
        }
    }

    func sizeThatFits(_ proposal: ProposedViewSize, nsView scroll: NSScrollView, context: Context) -> CGSize? {
        guard let width = proposal.width, width > 0,
              let editor = scroll.documentView as? NSTextView,
              let container = editor.textContainer, let layout = editor.layoutManager else { return nil }
        container.containerSize = NSSize(width: width, height: CGFloat.greatestFiniteMagnitude)
        layout.ensureLayout(for: container)
        let height = layout.usedRect(for: container).height + editor.textContainerInset.height * 2
        return CGSize(width: width, height: min(96, max(40, ceil(height))))
    }

    final class Coordinator: NSObject, NSTextViewDelegate {
        var parent: ChatMessageInput
        init(_ parent: ChatMessageInput) { self.parent = parent }
        func textDidChange(_ notification: Notification) {
            guard let editor = notification.object as? NSTextView else { return }
            parent.text = editor.string
        }
    }
}

final class ChatMessageTextView: NSTextView {
    var onSend: (() -> Void)?

    override func keyDown(with event: NSEvent) {
        if [36, 76].contains(event.keyCode), !hasMarkedText() {
            if event.modifierFlags.contains(.shift) {
                // Bypass submit commands: insert at the selection using native editing.
                super.insertNewline(nil)
            } else if !event.isARepeat {
                onSend?()
            }
            return
        }
        super.keyDown(with: event)
    }
}
