/// Public places to look a property up. Every url is built from the address
/// the county recorded, so a link either resolves to that address or to a
/// search for it - none of them assert a listing exists.

export interface PropertyLinks {
  readonly maps: string;
  readonly zillow: string;
}

const encode = (value: string): string => encodeURIComponent(value.trim().replace(/\s+/g, " "));

const slugAddress = (address: string): string =>
  address.trim().replace(/\s+/g, "-").replace(/[^A-Za-z0-9-]/g, "");

/// "city_of_norfolk_va" -> "Norfolk VA", which is what a map search wants.
export const placeOf = (jurisdiction: string): string =>
  jurisdiction
    .split("_")
    .filter((part) => part !== "city" && part !== "of" && part !== "county")
    .map((part) => (part.length === 2 ? part.toUpperCase() : part.replace(/^./, (c) => c.toUpperCase())))
    .join(" ")
    .trim();

export const linksFor = (address: string, jurisdiction: string): PropertyLinks | undefined => {
  const cleaned = address.trim();
  if (cleaned === "") return undefined;
  const place = placeOf(jurisdiction);
  const full = place === "" ? cleaned : `${cleaned}, ${place}`;
  return {
    maps: `https://www.google.com/maps/search/?api=1&query=${encode(full)}`,
    zillow: `https://www.zillow.com/homes/${slugAddress(full)}_rb/`,
  };
};

export const linkBlock = (address: string, jurisdiction: string): string => {
  const links = linksFor(address, jurisdiction);
  if (links === undefined) return "";
  return `Look it up:\nMap ${links.maps}\nZillow ${links.zillow}`;
};
