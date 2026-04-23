// SPDX-License-Identifier: MIT
#include "Patcher.hpp"

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include <string>

using namespace RC;

class WindroseIdleCpuFix : public CppUserModBase {
public:
    WindroseIdleCpuFix() : CppUserModBase() {
        ModName = STR("WindroseIdleCpuFix");
        ModVersion = STR("1.0.0");
        ModDescription = STR("Throttle Boost.Asio socket_select_interrupter idle-spin in WindroseServer.");
        ModAuthors = STR("Community");
    }

    ~WindroseIdleCpuFix() override = default;

    auto on_program_start() -> void override {
        auto result = windrose::patcher::apply();
        std::wstring wide(result.message.begin(), result.message.end());
        if (result.ok) {
            Output::send<LogLevel::Default>(STR("[WindroseIdleCpuFix] {}\n"), wide);
        } else {
            Output::send<LogLevel::Warning>(STR("[WindroseIdleCpuFix] patch failed: {}\n"), wide);
        }
    }
};

#define WINDROSE_IDLE_CPU_FIX_API __declspec(dllexport)

extern "C" {

WINDROSE_IDLE_CPU_FIX_API CppUserModBase* start_mod() {
    return new WindroseIdleCpuFix();
}

WINDROSE_IDLE_CPU_FIX_API void uninstall_mod(CppUserModBase* mod) {
    delete mod;
}

} // extern "C"
