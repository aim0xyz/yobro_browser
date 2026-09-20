#include "spike/qml/QmlShellSupport.hpp"

#include <QGuiApplication>
#include <QQuickWindow>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace yobro::spike {

void configureQmlMacWindowFrame(QQuickWindow *window) {
#if defined(__APPLE__)
    if (!window) return;
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) return;
    if (!window->isVisible()) window->create();
    id nsView = reinterpret_cast<id>(window->winId());
    if (!nsView) return;
    id nsWindow = reinterpret_cast<id (*)(id, SEL)>(&objc_msgSend)(nsView, sel_registerName("window"));
    if (!nsWindow) return;

    reinterpret_cast<void (*)(id, SEL, bool)>(&objc_msgSend)(
        nsWindow, sel_registerName("setTitlebarAppearsTransparent:"), true
    );
    reinterpret_cast<void (*)(id, SEL, long)>(&objc_msgSend)(
        nsWindow, sel_registerName("setTitleVisibility:"), 1
    );
    long mask = reinterpret_cast<long (*)(id, SEL)>(&objc_msgSend)(
        nsWindow, sel_registerName("styleMask")
    );
    mask |= (1L << 15); // NSWindowStyleMaskFullSizeContentView
    reinterpret_cast<void (*)(id, SEL, long)>(&objc_msgSend)(
        nsWindow, sel_registerName("setStyleMask:"), mask
    );
    reinterpret_cast<void (*)(id, SEL, bool)>(&objc_msgSend)(
        nsWindow, sel_registerName("setMovableByWindowBackground:"), true
    );
#else
    Q_UNUSED(window);
#endif
}

} // namespace yobro::spike
