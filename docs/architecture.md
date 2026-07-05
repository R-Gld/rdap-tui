# Milestone 0 architecture

The application has three layers:

1. `rdap_core` validates domain names, resolves IANA bootstrap data, performs HTTPS requests, and
   validates JSON responses.
2. The TUI owns presentation state and executes the blocking core lookup in a `std::thread` with
   a shared cancellation token. This avoids Apple libc++'s disabled `std::stop_token` while
   retaining deterministic cancellation.
3. Tests replace `HttpClient` with a deterministic fake; the real curl transport is only exercised
   by the opt-in network smoke test.

The bootstrap registry is fetched lazily and retained in memory for the process lifetime. Domain
matching is label-aware and selects all equivalent longest matches. Only HTTPS services are
returned. A lookup tries the next equivalent service after a transport error or HTTP 5xx, but does
not retry 4xx responses or repeat the same endpoint.

`rdap_core` is an internal seam rather than a stable public library. Its API and ABI may change
until version 1.0.
