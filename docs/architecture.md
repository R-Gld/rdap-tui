# Network Resources MVP architecture

The application has three layers:

1. `rdap_core` detects and validates domain, IP/CIDR, and ASN queries, resolves the corresponding
   IANA bootstrap data, performs HTTPS requests, and projects responses into typed models.
2. The TUI owns presentation state and executes the blocking core lookup in a `std::thread` with
   a shared cancellation token. This avoids Apple libc++'s disabled `std::stop_token` while
   retaining deterministic cancellation.
3. Tests replace `HttpClient` with a deterministic fake; the real curl transport is only exercised
   by the opt-in network smoke test.

The projections are deliberately tolerant. Missing and `null` optional members remain absent,
known members with incompatible types produce path-specific warnings, and unknown extension
members remain available in the JSON view. An explicit mismatched `objectClassName` disables the
structured views instead of presenting misleading data.

Entity parsing is recursive and bounded to eight levels and 256 cumulative entities. Other
collections are bounded to 1024 items. Parsing happens on the lookup worker so a large response
does not block terminal input.

Presentation formatting is independent from FTXUI. It produces line collections for Overview,
Contacts, DNS, and Notices; the TUI gives each view its own scroll position and preserves both
pretty and original JSON representations.

The four bootstrap registries are fetched lazily and cached independently for the process
lifetime. Domain matching is label-aware, IP matching uses the longest binary prefix, and ASN
matching uses inclusive ranges. Only HTTPS services are returned. A lookup tries the next
equivalent service after a transport error or HTTP 5xx, but does not retry 4xx responses or repeat
the same endpoint.

Each bootstrap registry is additionally persisted to an on-disk `BootstrapCache`, so a fresh
process reuses the previous fetch instead of always querying IANA, per RFC 9224's guidance to
honor HTTP cache signaling rather than a fixed re-fetch interval. A fresh entry (within the
`Cache-Control: max-age` IANA returned) is served with zero network I/O; a stale entry is
revalidated with a conditional GET (`If-None-Match`/`If-Modified-Since`), and a `304` response
simply extends the freshness window. A transport error or HTTP 5xx while revalidating falls back
to the stale cached body rather than failing the lookup, since bootstrap data changes rarely and
availability matters more than perfect freshness; a 4xx is still a hard error, as it more likely
signals a permanent problem with the URL itself. Disk I/O failures (an unwritable cache directory,
a corrupt cache file) are absorbed internally and never the reason a lookup fails.

`rdap_core` is an internal seam rather than a stable public library. Its API and ABI may change
until version 1.0.
