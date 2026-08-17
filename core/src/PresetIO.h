#pragma once
#include "sl/Instrument.h"
#include <nlohmann/json_fwd.hpp>

namespace sl {

// Reads zyn's instrument JSON unchanged -- the format the demo page emits from
// "Copy JSON" and stores in its preset files.
Instrument instrumentFromJson(const nlohmann::json& j);

// Emits zyn's exact field names and its false/null/absent conventions, so a
// round-tripped file still loads in the demo page.
nlohmann::json instrumentToJson(const Instrument& inst);

} // namespace sl
