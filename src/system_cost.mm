#include "system_cost.hpp"

#import <Foundation/Foundation.h>
#import <Network/Network.h>

#include <atomic>

namespace bt {
namespace {

std::atomic<bool> expensive_path{false};

void start_path_monitor() {
    static nw_path_monitor_t monitor = nw_path_monitor_create();
    nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
        expensive_path.store(nw_path_is_expensive(path) || nw_path_is_constrained(path),
            std::memory_order_relaxed);
    });
    nw_path_monitor_set_queue(monitor, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    nw_path_monitor_start(monitor);
}

} // namespace

SystemCost read_system_cost() {
    static const bool started = (start_path_monitor(), true);
    (void)started;
    bool low_power = false;
    if (@available(macOS 12.0, *)) {
        low_power = [[NSProcessInfo processInfo] isLowPowerModeEnabled];
    }
    return {expensive_path.load(std::memory_order_relaxed), low_power};
}

} // namespace bt
