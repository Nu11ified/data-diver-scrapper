#pragma once

#include <string>
#include <string_view>

namespace dd::entity {

// Canonical forms used to decide that two records describe one property.
std::string normalize_parcel(std::string_view parcel);
std::string normalize_address(std::string_view address);

// A street address broken into the parts that decide identity. Offices
// disagree about which parts they print: a treasurer writes
// "0555 LIBERTY ST E", an assessor writes "555 Liberty ST" and never
// publishes the directional at all. Comparing whole strings therefore fails
// on records that describe one building, so the parts are compared instead.
struct Address {
    std::string number;       // house number, leading zeros stripped
    std::string street;       // street name with no directional or suffix
    std::string suffix;       // ST, AVE, RD, canonical form, may be empty
    std::string directional;  // N, S, E, W, may be empty
    std::string unit;         // apartment or condo designator, may be empty
    bool locatable = false;   // false for block references and placeholders
};

Address parse_address(std::string_view address);

// The key two records must share to be the same building: number, street and
// unit. Suffix and directional are excluded because sources omit them, and
// are checked separately by compatible().
std::string address_join_key(const Address& address);

// False when the parts both records do publish contradict each other, as in
// EAST versus WEST or STREET versus AVENUE. A part only one side publishes
// is not a contradiction.
bool compatible(const Address& a, const Address& b);

// Stable key for a property within a jurisdiction. Parcel wins when present
// because it is the assessor's own identity; the address form is the
// fallback for sources that never print a parcel. Empty when the record
// carries neither, meaning it cannot be resolved.
std::string property_key(std::string_view jurisdiction, std::string_view parcel,
                         std::string_view address);

// True when two owner name strings plausibly refer to the same party
// ("Smith, Jane" vs "Jane Smith", case and punctuation aside).
bool same_owner(std::string_view a, std::string_view b);

} // namespace dd::entity
