#include "hard_link_policy.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace cw::server {

hard_link_failure_action classify_hard_link_failure(
    const std::error_code& error) noexcept {

    if (!error)
        return hard_link_failure_action::fail;

    if (error == std::errc::operation_not_supported ||
        error == std::errc::function_not_supported ||
        error == std::errc::cross_device_link) {
        return hard_link_failure_action::rewrite;
    }

#if defined(_WIN32)
    if (error.category() == std::system_category()) {
        switch (static_cast<DWORD>(error.value())) {
        case ERROR_INVALID_FUNCTION:
        case ERROR_NOT_SUPPORTED:
        case ERROR_NOT_SAME_DEVICE:
            return hard_link_failure_action::rewrite;
        default:
            break;
        }
    }
#endif

    return hard_link_failure_action::fail;
}

}
