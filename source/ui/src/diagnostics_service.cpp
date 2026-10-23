#include "diagnostics_service.h"

#include "diagnostics.h" // utils::diagnostics（首异常计数 / 内存采样）

namespace aiwrite::ui {

void Diagnostics::install_exception_counter()
{
    utils::diagnostics::installExceptionCounter();
}

void Diagnostics::log_exception_summary()
{
    utils::diagnostics::logExceptionSummary();
}

void Diagnostics::log_memory_sample(const std::string& tag)
{
    utils::diagnostics::logMemorySample(tag);
}

void Diagnostics::post_status(const std::string& text)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_  = text;
    pending_ = true;
}

bool Diagnostics::take_status(std::string* out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_) {
        return false;
    }
    if (out != nullptr) {
        *out = status_;
    }
    pending_ = false;
    return true;
}

} // namespace aiwrite::ui
