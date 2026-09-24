#include "system_cost.hpp"

#ifdef _WIN32
#include <netlistmgr.h>
#include <objbase.h>
#include <windows.h>
#endif

namespace bt {

SystemCost read_system_cost() {
    SystemCost result;
#ifdef _WIN32
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) result.low_power = power.SystemStatusFlag == 1;

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(initialized)) {
        INetworkCostManager* cost_manager = nullptr;
        const HRESULT created = CoCreateInstance(CLSID_NetworkListManager, nullptr,
            CLSCTX_ALL, IID_INetworkCostManager, reinterpret_cast<void**>(&cost_manager));
        if (SUCCEEDED(created)) {
            DWORD cost = 0;
            if (SUCCEEDED(cost_manager->GetCost(&cost, nullptr))) {
                constexpr DWORD metered_flags = NLM_CONNECTION_COST_FIXED
                    | NLM_CONNECTION_COST_VARIABLE | NLM_CONNECTION_COST_OVERDATALIMIT
                    | NLM_CONNECTION_COST_ROAMING | NLM_CONNECTION_COST_APPROACHINGDATALIMIT;
                result.metered = (cost & metered_flags) != 0;
            }
            cost_manager->Release();
        }
        CoUninitialize();
    }
#endif
    return result;
}

} // namespace bt
