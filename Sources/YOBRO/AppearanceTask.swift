import SwiftUI

/// macOS 26.5 can crash in SwiftUI's Task.immediate executor check when a
/// WindowGroup is recreated after closing its last window. Start ordinary
/// MainActor tasks from appearance callbacks instead, retaining task(id:)'s
/// cancellation and restart behavior.
@MainActor
final class AppearanceTaskLifetime {
    private var task: Task<Void, Never>?

    func start(_ operation: @escaping @MainActor () async -> Void) {
        cancel()
        task = Task { @MainActor in
            guard !Task.isCancelled else { return }
            await operation()
        }
    }

    func cancel() {
        task?.cancel()
        task = nil
    }

    deinit { task?.cancel() }
}

private struct AppearanceTaskModifier<ID: Equatable>: ViewModifier {
    let id: ID
    let operation: @MainActor () async -> Void
    @State private var lifetime = AppearanceTaskLifetime()
    @State private var isVisible = false

    func body(content: Content) -> some View {
        content
            .onAppear {
                isVisible = true
                lifetime.start(operation)
            }
            .onChange(of: id) { _, _ in
                if isVisible { lifetime.start(operation) }
            }
            .onDisappear {
                isVisible = false
                lifetime.cancel()
            }
    }
}

extension View {
    func appearanceTask<ID: Equatable>(id: ID, _ operation: @escaping @MainActor () async -> Void) -> some View {
        modifier(AppearanceTaskModifier(id: id, operation: operation))
    }

    func appearanceTask(_ operation: @escaping @MainActor () async -> Void) -> some View {
        appearanceTask(id: false, operation)
    }
}
