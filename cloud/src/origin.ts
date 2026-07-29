export const requestOrigin = (
  requestUrl: string,
  headers: Readonly<Record<string, string | undefined>>,
  configuredOrigin: string,
): string => {
  const parsed = new URL(requestUrl, "http://worker");
  const host = headers.host;
  if (host !== undefined && host !== "") {
    const protocol =
      parsed.hostname !== "worker"
        ? parsed.protocol.replace(":", "")
        : /^localhost(?::|$)|^127\.0\.0\.1(?::|$)/.test(host)
          ? "http"
          : "https";
    return `${protocol}://${host}`;
  }

  if (parsed.hostname !== "worker") return parsed.origin;
  return configuredOrigin.replace(/\/+$/, "");
};
