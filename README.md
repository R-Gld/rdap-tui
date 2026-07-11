# rdap-tui

`rdap-tui` is an open-source terminal client for the Registration Data Access Protocol (RDAP).
The Network Resources MVP supports interactive domain, IPv4, IPv6, CIDR, and ASN lookups with
structured registration, contact, resource, notice, and JSON views. Discovery uses the IANA
bootstrap registries and requests use verified HTTPS.

![rdap-tui domain lookup](docs/rdap-tui.svg)

## Requirements

- C++20 compiler: GCC 13+, Clang 18+, or AppleClang 16+
- CMake 3.25+
- Conan 2.26+

## Build

```sh
# One-time setup only, if no Conan default profile exists:
# conan profile detect
conan lock create . --lockfile-out=conan.lock -s compiler.cppstd=20
conan install . --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release
cmake --build --preset conan-release
ctest --preset conan-release
```

Run the executable generated under `build/Release/`:

```sh
./build/Release/rdap-tui
```

Conan also generates a `conan-debug` preset when installed with
`-s build_type=Debug`.

## Downloads

Tagged releases provide directly downloadable executables for Linux and macOS, plus a
`SHA256SUMS` file. Users may need to restore the executable bit after downloading:

```sh
chmod +x rdap-tui-v*-linux-* # or rdap-tui-v*-macos-*
```

## Usage

Enter an ASCII domain, Punycode A-label, IPv4 or IPv6 address, CIDR prefix, or ASN and press Enter.
ASN input accepts both `AS13335` and `13335`; numeric-only input is therefore treated as an ASN.
CIDR input is normalized to its network prefix before lookup.

The result is split into five views: Overview, Contacts, a resource-specific DNS/Network/Autnum
view, Notices, and JSON. Press 1–5 to switch views while the query field is not focused. The JSON
view retains both a pretty-printed document and the byte-preserving response.

Use Tab to move between controls, arrow keys or Page Up/Page Down to scroll, and Ctrl+C or Escape
to quit.

```text
rdap-tui [--help] [--version]
```

Malformed optional fields do not hide valid registration data: the structured projection reports
warnings in the Notices view, while the complete response remains available under JSON.

The country shown for IP and ASN results is the registration country reported by the registry; it
is not IP geolocation.

Unicode-to-IDNA conversion, BGP/RPKI data, geolocation, and binary packages remain outside this
milestone.

## Bootstrap cache

The IANA bootstrap registries (used to find the authoritative RDAP service for a query) are
cached to disk, honoring the `Cache-Control`/`ETag` headers IANA returns, so repeated runs avoid
re-fetching them. The cache lives at:

- Linux: `$XDG_CACHE_HOME/rdap-tui/bootstrap` when `XDG_CACHE_HOME` is an absolute path
  (or `~/.cache/rdap-tui/bootstrap`)
- macOS: `~/Library/Caches/rdap-tui/bootstrap`

Deleting this directory clears the cache; a fresh copy is fetched on the next lookup.

## Optional network smoke test

Normal tests never access the Internet. To build the explicit smoke test:

```sh
conan install . --build=missing -s build_type=Debug -s compiler.cppstd=20 \
  -o '&:network_tests=True'
cmake --preset conan-debug -DRDAP_ENABLE_NETWORK_TESTS=ON
cmake --build --preset conan-debug
ctest --preset conan-debug -L network --output-on-failure
```

The smoke test contacts IANA and authoritative services for `example.com`, `1.1.1.1`, and
`AS13335`; external outages or rate limiting can therefore make it fail.

## Standards and security

Discovery follows [RFC 9224](https://www.rfc-editor.org/rfc/rfc9224.html), query URLs follow
[RFC 9082](https://www.rfc-editor.org/rfc/rfc9082.html), and responses follow
[RFC 9083](https://www.rfc-editor.org/rfc/rfc9083.html). TLS verification is mandatory and
redirects are restricted to HTTPS.

See [CONTRIBUTING.md](CONTRIBUTING.md) for development checks and [SECURITY.md](SECURITY.md) for
reporting vulnerabilities.

## License

Copyright © 2026 rdap-tui contributors.

Licensed under GPL-3.0-or-later. See [LICENSE](LICENSE).
