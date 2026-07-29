#pragma once

#include <string>
#include <string_view>

namespace dd::entity {
std::string normalize_parcel(std::string_view parcel);
std::string normalize_address(std::string_view address);

struct Address {
    std::string number;       // house number, leading zeros stripped
    std::string street;       // street name with no directional or suffix
    std::string suffix;       // ST, AVE, RD, canonical form, may be empty
    std::string directional;  // N, S, E, W, may be empty
    std::string unit;         // apartment or condo designator, may be empty
    bool locatable = false;   // false for block references and placeholders
};

Address parse_address(std::string_view address);

std::string address_join_key(const Address& address);

bool compatible(const Address& a, const Address& b);

std::string property_key(std::string_view jurisdiction, std::string_view parcel,
                         std::string_view address);

bool same_owner(std::string_view a, std::string_view b);
} // namespace dd::entity
