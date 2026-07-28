#pragma once

#include <string>
#include <string_view>

namespace dd::entity {

// Canonical forms used to decide that two records describe one property.
std::string normalize_parcel(std::string_view parcel);
std::string normalize_address(std::string_view address);

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
