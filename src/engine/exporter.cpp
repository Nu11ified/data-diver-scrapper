#include "dd/engine/exporter.hpp"

#include "dd/engine/compile.hpp"

namespace dd::exporter {
std::string county_json(store::Store& store, const schema::Registry& registry,
                        const std::string& county) {
    return compile::render_county_json(county, compile::county(store, registry, county));
}
} // namespace dd::exporter
