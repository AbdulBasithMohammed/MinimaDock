#pragma once
#include <optional>
#include <string>

namespace minima {
struct HttpOptions {
    const wchar_t* agent=nullptr; // nullptr: a browser-like agent (masjid sites block bare clients).
    std::wstring language;        // Accept-Language, e.g. "en-GB" (empty: not sent).
    int timeout_ms=15000;         // Resolve/connect and send/receive limits.
};
struct HttpResult {
    int status=0;     // HTTP status; 0 when the request never got an answer (offline, timeout).
    std::string body; // Only read for 200.
};
// One blocking HTTP(S) GET with a 4 MB cap (worker threads only). Redirects are
// followed and the system proxy (including PAC scripts) is used.
HttpResult http_request(const std::wstring& url,const HttpOptions& options={});
// The body of a 200 answer.
inline std::optional<std::string> http_get(const std::wstring& url) {
    auto result=http_request(url);
    if(result.status!=200)return std::nullopt;
    return std::move(result.body);
}
}
