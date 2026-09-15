#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace yobro::controller {

class ScheduledTask {
public:
    virtual ~ScheduledTask() = default;
    virtual void cancel() noexcept = 0;
};

class EventLoop {
public:
    using Task = std::function<void()>;

    virtual ~EventLoop() = default;
    virtual void post(Task task) = 0;
    [[nodiscard]] virtual std::shared_ptr<ScheduledTask> scheduleAfter(
        std::chrono::milliseconds delay,
        Task task
    ) = 0;
};

} // namespace yobro::controller
