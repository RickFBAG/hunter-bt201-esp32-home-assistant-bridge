#pragma once

#include <string_view>

#include "esp_err.h"
#include "nvs.h"

#include "bridge_types.h"

namespace bridge {

class StateStore {
   public:
    StateStore() = default;
    ~StateStore();

    esp_err_t init();
    PersistedBridgeState &state();
    const PersistedBridgeState &state() const;
    ZoneStateRecord &zone(ZoneId zone);
    const ZoneStateRecord &zone(ZoneId zone) const;

    void reset_defaults();
    void mark_boot_stale();
    void mark_all_unknown(std::string_view reason, bool error_state);
    void set_bridge_error(std::string_view error);
    void set_zone_error(ZoneId zone, std::string_view error);
    void set_apply_status(ZoneId zone, std::string_view status);
    void clear_zone_error(ZoneId zone);
    void clear_bridge_error();
    esp_err_t save();

   private:
    static constexpr const char *kNamespace = "bridge";
    static constexpr const char *kStateKey = "state";

    nvs_handle_t nvs_handle_{0};
    PersistedBridgeState state_{};
};

}  // namespace bridge

