#pragma once

#include "yobro/controller/EventLoop.hpp"

#include <memory>

class QObject;

namespace yobro::qtwebengine {

class QtEventLoop final : public controller::EventLoop {
public:
    QtEventLoop();
    ~QtEventLoop() override;

    void post(Task task) override;
    [[nodiscard]] std::shared_ptr<controller::ScheduledTask> scheduleAfter(
        std::chrono::milliseconds delay,
        Task task
    ) override;

private:
    std::unique_ptr<QObject> context_;
};

} // namespace yobro::qtwebengine
