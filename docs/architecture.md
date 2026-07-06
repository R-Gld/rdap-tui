# Domain MVP architecture

The application has three layers:

1. `rdap_core` validates domain names, resolves IANA bootstrap data, performs HTTPS requests,
   validates JSON responses, and projects domain objects into a typed model.
2. The TUI owns presentation state and executes the blocking core lookup in a `std::thread` with
   a shared cancellation token. This avoids Apple libc++'s disabled `std::stop_token` while
   retaining deterministic cancellation.
3. Tests replace `HttpClient` with a deterministic fake; the real curl transport is only exercised
   by the opt-in network smoke test.

The domain projection is deliberately tolerant. Missing and `null` optional members remain absent,
known members with incompatible types produce path-specific warnings, and unknown extension
members remain available in the JSON view. An explicit non-domain `objectClassName` disables the
structured views instead of presenting misleading data.

Entity parsing is recursive and bounded to eight levels and 256 cumulative entities. Other
collections are bounded to 1024 items. Parsing happens on the lookup worker so a large response
does not block terminal input.

Presentation formatting is independent from FTXUI. It produces line collections for Overview,
Contacts, DNS, and Notices; the TUI gives each view its own scroll position and preserves both
pretty and original JSON representations.

The bootstrap registry is fetched lazily and retained in memory for the process lifetime. Domain
matching is label-aware and selects all equivalent longest matches. Only HTTPS services are
returned. A lookup tries the next equivalent service after a transport error or HTTP 5xx, but does
not retry 4xx responses or repeat the same endpoint.

`rdap_core` is an internal seam rather than a stable public library. Its API and ABI may change
until version 1.0.
