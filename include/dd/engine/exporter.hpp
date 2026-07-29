#pragma once

#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"

#include <string>

namespace dd::exporter {
std::string county_json(store::Store& store, const schema::Registry& registry,
                        const std::string& county);
} // namespace dd::exporter
