// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/rdap_client.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace rdap;

namespace {

class TempDirectory {
public:
  TempDirectory() {
    // Each Catch2 TEST_CASE runs as its own process under ctest (which runs
    // several in parallel), so uniqueness must hold across processes, not
    // just within one -- a per-process counter alone is not enough.
    std::random_device random_device;
    const auto unique = (static_cast<std::uint64_t>(random_device()) << 32U) |
                        static_cast<std::uint64_t>(random_device());
    path_ = std::filesystem::temp_directory_path() / ("rdap-tui-test-" + std::to_string(unique));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TempDirectory(const TempDirectory &) = delete;
  TempDirectory &operator=(const TempDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

class FakeHttpClient final : public HttpClient {
public:
  Result<HttpResponse> get(const HttpRequest &request, const CancellationToken &) override {
    requests.push_back(request);
    REQUIRE_FALSE(responses.empty());
    auto result = std::move(responses.front());
    responses.pop_front();
    return result;
  }

  std::deque<Result<HttpResponse>> responses;
  std::vector<HttpRequest> requests;
};

HttpResponse response(long status, std::string body,
                      std::string effective_url = "https://registry.example/domain/example.com") {
  return HttpResponse{
      status, std::move(effective_url), "application/rdap+json", std::move(body), {}};
}

std::string bootstrap(std::string urls = R"("https://registry.example/")") {
  return R"({"version":"1.0","services":[[["com"],[)" + urls + R"(]]]})";
}

DomainName example_domain() {
  auto parsed = DomainName::parse("Example.COM");
  REQUIRE(std::holds_alternative<DomainName>(parsed));
  return std::get<DomainName>(std::move(parsed));
}

ResourceQuery query(std::string_view value) {
  auto parsed = ResourceQueryParser::parse(value);
  REQUIRE(std::holds_alternative<ResourceQuery>(parsed));
  return std::get<ResourceQuery>(std::move(parsed));
}

} // namespace

TEST_CASE("client performs bootstrap and domain lookup") {
  FakeHttpClient http;
  http.responses.emplace_back(response(200L, bootstrap(), "https://data.iana.org/rdap/dns.json"));
  http.responses.emplace_back(
      response(200L, R"({"objectClassName":"domain","ldhName":"EXAMPLE.COM"})"));
  RdapClient client(http);
  std::vector<LookupStage> stages;

  auto result = client.lookup_domain(example_domain(), {},
                                     [&](LookupStage stage) { stages.push_back(stage); });

  REQUIRE(std::holds_alternative<RdapResponse>(result));
  const auto &rdap_response = std::get<RdapResponse>(result);
  CHECK(rdap_response.request_url == "https://registry.example/domain/example.com");
  CHECK(rdap_response.document["objectClassName"] == "domain");
  REQUIRE(http.requests.size() == 2U);
  CHECK(http.requests[0].maximum_body_size == 2U * 1024U * 1024U);
  CHECK(http.requests[1].headers.front().find("application/rdap+json") != std::string::npos);
  CHECK(stages ==
        std::vector<LookupStage>{LookupStage::loading_bootstrap, LookupStage::querying_registry});
}

TEST_CASE("bootstrap is cached for subsequent lookups") {
  FakeHttpClient http;
  http.responses.emplace_back(response(200L, bootstrap()));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"domain"})"));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"domain"})"));
  RdapClient client(http);

  REQUIRE(std::holds_alternative<RdapResponse>(client.lookup_domain(example_domain(), {})));
  REQUIRE(std::holds_alternative<RdapResponse>(client.lookup_domain(example_domain(), {})));
  CHECK(http.requests.size() == 3U);
}

TEST_CASE("client falls back after a retryable registry failure") {
  FakeHttpClient http;
  http.responses.emplace_back(
      response(200L, bootstrap(R"("https://one.example/","https://two.example/")")));
  http.responses.emplace_back(response(503L, R"({"errorCode":503})"));
  http.responses.emplace_back(
      response(200L, R"({"objectClassName":"domain"})", "https://two.example/domain/example.com"));
  RdapClient client(http);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<RdapResponse>(result));
  CHECK(std::get<RdapResponse>(result).http.effective_url ==
        "https://two.example/domain/example.com");
}

TEST_CASE("429 exposes retry information without trying another service") {
  FakeHttpClient http;
  http.responses.emplace_back(
      response(200L, bootstrap(R"("https://one.example/","https://two.example/")")));
  auto limited = response(429L, R"({"errorCode":429})");
  limited.headers["retry-after"] = "60";
  http.responses.emplace_back(std::move(limited));
  RdapClient client(http);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<Error>(result));
  const auto &error = std::get<Error>(result);
  CHECK(error.http_status == 429L);
  CHECK(error.retry_after == "60");
  CHECK(http.requests.size() == 2U);
}

TEST_CASE("invalid RDAP JSON is reported") {
  FakeHttpClient http;
  http.responses.emplace_back(response(200L, bootstrap()));
  http.responses.emplace_back(response(200L, "not json"));
  RdapClient client(http);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<Error>(result));
  CHECK(std::get<Error>(result).code == ErrorCode::invalid_json);
}

TEST_CASE("transport cancellation is not retried") {
  FakeHttpClient http;
  http.responses.emplace_back(
      response(200L, bootstrap(R"("https://one.example/","https://two.example/")")));
  http.responses.emplace_back(
      Error{ErrorCode::cancelled, "cancelled", {}, std::nullopt, std::nullopt});
  RdapClient client(http);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<Error>(result));
  CHECK(std::get<Error>(result).code == ErrorCode::cancelled);
  CHECK(http.requests.size() == 2U);
}

TEST_CASE("transport safety errors are propagated") {
  for (const auto code : {ErrorCode::timeout, ErrorCode::response_too_large, ErrorCode::tls}) {
    CAPTURE(code);
    FakeHttpClient http;
    http.responses.emplace_back(response(200L, bootstrap()));
    http.responses.emplace_back(Error{code, "request failed", {}, std::nullopt, std::nullopt});
    RdapClient client(http);

    auto result = client.lookup_domain(example_domain(), {});
    REQUIRE(std::holds_alternative<Error>(result));
    CHECK(std::get<Error>(result).code == code);
  }
}

TEST_CASE("client performs IPv4 lookup through the address bootstrap") {
  FakeHttpClient http;
  http.responses.emplace_back(
      response(200L, R"({"version":"1.0","services":[[["0.0.0.0/0"],["https://rir.example/"]]]})",
               "https://data.iana.org/rdap/ipv4.json"));
  http.responses.emplace_back(response(
      200L,
      R"({"objectClassName":"ip network","startAddress":"192.0.2.0","endAddress":"192.0.2.255"})",
      "https://rir.example/ip/192.0.2.1"));
  RdapClient client(http);

  auto result = client.lookup(query("192.0.2.1"), {});

  REQUIRE(std::holds_alternative<RdapResponse>(result));
  CHECK(std::get<RdapResponse>(result).request_url == "https://rir.example/ip/192.0.2.1");
  REQUIRE(http.requests.size() == 2U);
  CHECK(http.requests.front().url == "https://data.iana.org/rdap/ipv4.json");
}

TEST_CASE("client preserves normalized CIDR path segments") {
  FakeHttpClient http;
  http.responses.emplace_back(
      response(200L, R"({"version":"1.0","services":[[["0.0.0.0/0"],["https://rir.example/"]]]})"));
  http.responses.emplace_back(
      response(200L, R"({"objectClassName":"ip network"})", "https://rir.example/ip/192.0.2.0/24"));
  RdapClient client(http);

  auto result = client.lookup(query("192.0.2.42/24"), {});

  REQUIRE(std::holds_alternative<RdapResponse>(result));
  CHECK(std::get<RdapResponse>(result).request_url == "https://rir.example/ip/192.0.2.0/24");
}

TEST_CASE("client performs ASN lookup through the ASN bootstrap") {
  FakeHttpClient http;
  http.responses.emplace_back(response(
      200L, R"({"version":"1.0","services":[[["1-4294967295"],["https://rir.example/"]]]})",
      "https://data.iana.org/rdap/asn.json"));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"autnum","startAutnum":13335})",
                                       "https://rir.example/autnum/13335"));
  RdapClient client(http);

  auto result = client.lookup(query("AS13335"), {});

  REQUIRE(std::holds_alternative<RdapResponse>(result));
  CHECK(std::get<RdapResponse>(result).request_url == "https://rir.example/autnum/13335");
  CHECK(http.requests.front().url == "https://data.iana.org/rdap/asn.json");
}

TEST_CASE("number resource bootstraps are cached independently") {
  FakeHttpClient http;
  http.responses.emplace_back(
      response(200L, R"({"version":"1.0","services":[[["0.0.0.0/0"],["https://v4.example/"]]]})"));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"ip network"})"));
  http.responses.emplace_back(
      response(200L, R"({"version":"1.0","services":[[["1-100"],["https://asn.example/"]]]})"));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"autnum"})"));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"ip network"})"));
  RdapClient client(http);

  REQUIRE(std::holds_alternative<RdapResponse>(client.lookup(query("192.0.2.1"), {})));
  REQUIRE(std::holds_alternative<RdapResponse>(client.lookup(query("AS42"), {})));
  REQUIRE(std::holds_alternative<RdapResponse>(client.lookup(query("198.51.100.1"), {})));
  CHECK(http.requests.size() == 5U);
}

TEST_CASE("cold disk cache performs one fetch and writes the cache") {
  TempDirectory directory;
  BootstrapCache cache(directory.path());
  FakeHttpClient http;
  http.responses.emplace_back(response(200L, bootstrap()));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"domain"})"));
  RdapClient client(http, &cache);

  REQUIRE(std::holds_alternative<RdapResponse>(client.lookup_domain(example_domain(), {})));
  CHECK(http.requests.size() == 2U);

  const auto cached = cache.read(BootstrapKind::domain);
  REQUIRE(cached.has_value());
  CHECK(cached->body == bootstrap());
}

TEST_CASE("a fresh disk cache is served without any bootstrap HTTP request") {
  TempDirectory directory;
  BootstrapCache cache(directory.path());
  CachedBootstrap entry;
  entry.body = bootstrap();
  entry.cached_at = std::chrono::system_clock::now();
  entry.max_age = std::chrono::hours(1);
  REQUIRE(cache.write(BootstrapKind::domain, entry));

  FakeHttpClient http;
  http.responses.emplace_back(response(200L, R"({"objectClassName":"domain"})"));
  RdapClient client(http, &cache);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<RdapResponse>(result));
  CHECK(http.requests.size() == 1U);
}

TEST_CASE("a stale disk cache issues a conditional request and honors 304") {
  TempDirectory directory;
  BootstrapCache cache(directory.path());
  CachedBootstrap entry;
  entry.body = bootstrap();
  entry.etag = "\"old-etag\"";
  entry.cached_at = std::chrono::system_clock::now() - std::chrono::hours(2);
  entry.max_age = std::chrono::hours(1);
  REQUIRE(cache.write(BootstrapKind::domain, entry));

  FakeHttpClient http;
  http.responses.emplace_back(response(304L, ""));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"domain"})"));
  RdapClient client(http, &cache);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<RdapResponse>(result));
  REQUIRE(http.requests.size() == 2U);
  const auto &conditional_headers = http.requests.front().headers;
  CHECK(std::find(conditional_headers.begin(), conditional_headers.end(),
                  "If-None-Match: \"old-etag\"") != conditional_headers.end());

  const auto refreshed = cache.read(BootstrapKind::domain);
  REQUIRE(refreshed.has_value());
  CHECK(refreshed->cached_at > entry.cached_at);
  CHECK(refreshed->etag == entry.etag);
}

TEST_CASE("a stale disk cache is overwritten on 200") {
  TempDirectory directory;
  BootstrapCache cache(directory.path());
  CachedBootstrap entry;
  entry.body = bootstrap(R"("https://old.example/")");
  entry.etag = "\"old-etag\"";
  entry.cached_at = std::chrono::system_clock::now() - std::chrono::hours(2);
  entry.max_age = std::chrono::hours(1);
  REQUIRE(cache.write(BootstrapKind::domain, entry));

  FakeHttpClient http;
  auto fresh_bootstrap = response(200L, bootstrap(R"("https://new.example/")"));
  fresh_bootstrap.headers["etag"] = "\"new-etag\"";
  http.responses.emplace_back(std::move(fresh_bootstrap));
  http.responses.emplace_back(
      response(200L, R"({"objectClassName":"domain"})", "https://new.example/domain/example.com"));
  RdapClient client(http, &cache);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<RdapResponse>(result));
  CHECK(std::get<RdapResponse>(result).request_url == "https://new.example/domain/example.com");

  const auto updated = cache.read(BootstrapKind::domain);
  REQUIRE(updated.has_value());
  CHECK(updated->etag == "\"new-etag\"");
}

TEST_CASE("a stale disk cache serves the stale body after a transport error") {
  TempDirectory directory;
  BootstrapCache cache(directory.path());
  CachedBootstrap entry;
  entry.body = bootstrap();
  entry.cached_at = std::chrono::system_clock::now() - std::chrono::hours(2);
  entry.max_age = std::chrono::hours(1);
  REQUIRE(cache.write(BootstrapKind::domain, entry));

  FakeHttpClient http;
  http.responses.emplace_back(
      Error{ErrorCode::transport, "network unreachable", {}, std::nullopt, std::nullopt});
  http.responses.emplace_back(response(200L, R"({"objectClassName":"domain"})"));
  RdapClient client(http, &cache);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<RdapResponse>(result));
  CHECK(http.requests.size() == 2U);
}

TEST_CASE("a stale disk cache serves the stale body after a server error") {
  TempDirectory directory;
  BootstrapCache cache(directory.path());
  CachedBootstrap entry;
  entry.body = bootstrap();
  entry.cached_at = std::chrono::system_clock::now() - std::chrono::hours(2);
  entry.max_age = std::chrono::hours(1);
  REQUIRE(cache.write(BootstrapKind::domain, entry));

  FakeHttpClient http;
  http.responses.emplace_back(response(503L, R"({"errorCode":503})"));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"domain"})"));
  RdapClient client(http, &cache);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<RdapResponse>(result));
  CHECK(http.requests.size() == 2U);
}

TEST_CASE("a stale disk cache still fails on a 4xx bootstrap response") {
  TempDirectory directory;
  BootstrapCache cache(directory.path());
  CachedBootstrap entry;
  entry.body = bootstrap();
  entry.cached_at = std::chrono::system_clock::now() - std::chrono::hours(2);
  entry.max_age = std::chrono::hours(1);
  REQUIRE(cache.write(BootstrapKind::domain, entry));

  FakeHttpClient http;
  http.responses.emplace_back(response(404L, R"({"errorCode":404})"));
  RdapClient client(http, &cache);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<Error>(result));
  CHECK(std::get<Error>(result).http_status == 404L);
}

TEST_CASE("disk cache errors do not fail a lookup") {
  TempDirectory directory;
  const auto blocked_file = directory.path() / "blocked";
  std::ofstream(blocked_file) << "not a directory";
  BootstrapCache cache(blocked_file / "bootstrap");

  FakeHttpClient http;
  http.responses.emplace_back(response(200L, bootstrap()));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"domain"})"));
  RdapClient client(http, &cache);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<RdapResponse>(result));
}
