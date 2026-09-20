#pragma once

class QQuickWindow;

namespace yobro::spike {

/// macOS borderless full-size-content window with transparent titlebar, the
/// same NSWindow treatment the widget shell applies, so the chrome gradient
/// runs under the traffic lights. No-op off macOS and offscreen.
void configureQmlMacWindowFrame(QQuickWindow *window);

} // namespace yobro::spike
