// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace windrose::patcher {

struct Result {
    bool ok;
    std::string message;
};

// Applies the Boost.Asio socket_select_interrupter idle-spin patch to the
// currently loaded main module. Safe to call twice: a second call detects
// the trampoline prologue at the jump target and no-ops.
Result apply();

} // namespace windrose::patcher
