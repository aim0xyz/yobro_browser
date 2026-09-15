#include "engine/qtwebengine/QtEventLoop.hpp"

#include <QObject>
#include <QTimer>

#include <atomic>
#include <limits>
#include <utility>

namespace yobro::qtwebengine {
namespace {

class QtScheduledTask final : public controller::ScheduledTask {
public:
    explicit QtScheduledTask(std::shared_ptr<std::atomic_bool> cancelled)
        : cancelled_(std::move(cancelled)) {}

    void cancel() noexcept override {
        cancelled_->store(true);
    }

private:
    std::shared_ptr<std::atomic_bool> cancelled_;
};

} // namespace

QtEventLoop::QtEventLoop() : context_(std::make_unique<QObject>()) {}
QtEventLoop::~QtEventLoop() = default;

void QtEventLoop::post(Task task) {
    QTimer::singleShot(0, context_.get(), [task = std::move(task)]() mutable {
        if (task) task();
    });
}

std::shared_ptr<controller::ScheduledTask> QtEventLoop::scheduleAfter(
    std::chrono::milliseconds delay,
    Task task
) {
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    const auto bounded = std::clamp<std::int64_t>(
        delay.count(),
        0,
        std::numeric_limits<int>::max()
    );
    QTimer::singleShot(
        static_cast<int>(bounded),
        context_.get(),
        [cancelled, task = std::move(task)]() mutable {
            if (!cancelled->load() && task) task();
        }
    );
    return std::make_shared<QtScheduledTask>(cancelled);
}

} // namespace yobro::qtwebengine
