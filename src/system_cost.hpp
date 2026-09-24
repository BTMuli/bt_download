#pragma once

namespace bt {

struct SystemCost {
    bool metered{false};
    bool low_power{false};
};

SystemCost read_system_cost();

} // namespace bt
