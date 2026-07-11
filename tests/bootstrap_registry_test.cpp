// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/bootstrap_registry.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <variant>
#include <vector>

using rdap::AsnBootstrapRegistry;
using rdap::AutnumQuery;
using rdap::BootstrapRegistry;
using rdap::DomainName;
using rdap::Error;
using rdap::IpBootstrapRegistry;
using rdap::IpFamily;
using rdap::IpNetworkQuery;

namespace {

DomainName domain(std::string_view value) {
  auto result = DomainName::parse(value);
  REQUIRE(std::holds_alternative<DomainName>(result));
  return std::get<DomainName>(std::move(result));
}

IpNetworkQuery ip(std::string_view value) {
  auto result = IpNetworkQuery::parse(value);
  REQUIRE(std::holds_alternative<IpNetworkQuery>(result));
  return std::get<IpNetworkQuery>(std::move(result));
}

AutnumQuery autnum(std::string_view value) {
  auto result = AutnumQuery::parse(value);
  REQUIRE(std::holds_alternative<AutnumQuery>(result));
  return std::get<AutnumQuery>(std::move(result));
}

} // namespace

TEST_CASE("bootstrap uses the longest label-wise match") {
  constexpr std::string_view document = R"({
    "version": "1.0",
    "publication": "2026-01-01T00:00:00Z",
    "unknown": true,
    "services": [
      [["com"], ["https://com.example/rdap/", "http://com.example/rdap/"]],
      [["example.com"], ["https://specific.example/rdap/"]],
      [["goodexample.com"], ["https://wrong.example/rdap/"]]
    ]
  })";

  auto parsed = BootstrapRegistry::parse(document);
  REQUIRE(std::holds_alternative<BootstrapRegistry>(parsed));
  auto result = std::get<BootstrapRegistry>(parsed).resolve(domain("a.example.com"));
  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  CHECK(std::get<std::vector<std::string>>(result) ==
        std::vector<std::string>{"https://specific.example/rdap/"});
}

TEST_CASE("equivalent bootstrap entries merge and deduplicate HTTPS services") {
  constexpr std::string_view document = R"({
    "version": "1.0",
    "services": [
      [["com"], ["https://one.example/", "https://shared.example/"]],
      [["com"], ["https://shared.example/", "https://two.example/"]]
    ]
  })";

  auto parsed = BootstrapRegistry::parse(document);
  REQUIRE(std::holds_alternative<BootstrapRegistry>(parsed));
  auto result = std::get<BootstrapRegistry>(parsed).resolve(domain("example.com"));
  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  CHECK(std::get<std::vector<std::string>>(result) ==
        std::vector<std::string>{"https://one.example/", "https://shared.example/",
                                 "https://two.example/"});
}

TEST_CASE("HTTP-only bootstrap services are rejected") {
  constexpr std::string_view document = R"({
    "version": "1.0",
    "services": [[["com"], ["http://insecure.example/"]]]
  })";
  auto parsed = BootstrapRegistry::parse(document);
  REQUIRE(std::holds_alternative<BootstrapRegistry>(parsed));
  CHECK(std::holds_alternative<Error>(
      std::get<BootstrapRegistry>(parsed).resolve(domain("example.com"))));
}

TEST_CASE("malformed bootstrap documents are rejected") {
  for (const std::string document :
       {"not json", R"({"version":"2.0","services":[]})",
        R"({"version":"1.0","services":[["com","https://example/"]]})",
        R"({"version":"1.0","services":[[["com"],["https://example"]]]})"}) {
    CAPTURE(document);
    CHECK(std::holds_alternative<Error>(BootstrapRegistry::parse(document)));
  }
}

TEST_CASE("IPv4 bootstrap selects and merges the longest prefix") {
  constexpr std::string_view document = R"({
    "version":"1.0",
    "services":[
      [["0.0.0.0/0"],["https://default.example/"]],
      [["192.0.2.0/24"],["https://specific.example/","http://insecure.example/"]],
      [["192.0.2.0/24"],["https://specific.example/","https://second.example/"]]
    ]
  })";

  auto parsed = IpBootstrapRegistry::parse(document, IpFamily::v4);
  REQUIRE(std::holds_alternative<IpBootstrapRegistry>(parsed));
  auto result = std::get<IpBootstrapRegistry>(parsed).resolve(ip("192.0.2.42"));
  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  CHECK(std::get<std::vector<std::string>>(result) ==
        std::vector<std::string>{"https://specific.example/", "https://second.example/"});

  auto covering = std::get<IpBootstrapRegistry>(parsed).resolve(ip("192.0.0.0/8"));
  REQUIRE(std::holds_alternative<std::vector<std::string>>(covering));
  CHECK(std::get<std::vector<std::string>>(covering) ==
        std::vector<std::string>{"https://default.example/"});
}

TEST_CASE("IPv6 bootstrap performs binary prefix matching") {
  constexpr std::string_view document = R"({
    "version":"1.0",
    "services":[
      [["2001:db8::/32"],["https://wide.example/"]],
      [["2001:db8:1000::/36"],["https://specific.example/"]]
    ]
  })";

  auto parsed = IpBootstrapRegistry::parse(document, IpFamily::v6);
  REQUIRE(std::holds_alternative<IpBootstrapRegistry>(parsed));
  auto result = std::get<IpBootstrapRegistry>(parsed).resolve(ip("2001:db8:1fff::1"));
  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  CHECK(std::get<std::vector<std::string>>(result) ==
        std::vector<std::string>{"https://specific.example/"});
}

TEST_CASE("ASN bootstrap resolves inclusive ranges") {
  constexpr std::string_view document = R"({
    "version":"1.0",
    "services":[
      [["1-100"],["https://one.example/"]],
      [["101-200"],["http://insecure.example/","https://two.example/"]]
    ]
  })";

  auto parsed = AsnBootstrapRegistry::parse(document);
  REQUIRE(std::holds_alternative<AsnBootstrapRegistry>(parsed));
  auto result = std::get<AsnBootstrapRegistry>(parsed).resolve(autnum("AS200"));
  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  CHECK(std::get<std::vector<std::string>>(result) ==
        std::vector<std::string>{"https://two.example/"});
}

TEST_CASE("ASN bootstrap resolves singleton ranges") {
  constexpr std::string_view document = R"({
    "version":"1.0",
    "services":[
      [["2043"],["https://single.example/"]],
      [["2044-2050"],["https://range.example/"]]
    ]
  })";

  auto parsed = AsnBootstrapRegistry::parse(document);
  REQUIRE(std::holds_alternative<AsnBootstrapRegistry>(parsed));
  auto result = std::get<AsnBootstrapRegistry>(parsed).resolve(autnum("AS2043"));
  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  CHECK(std::get<std::vector<std::string>>(result) ==
        std::vector<std::string>{"https://single.example/"});
}

TEST_CASE("number bootstrap entries validate families ranges and overlaps") {
  CHECK(std::holds_alternative<Error>(IpBootstrapRegistry::parse(
      R"({"version":"1.0","services":[[["2001:db8::/32"],["https://example/"]]]})", IpFamily::v4)));
  CHECK(std::holds_alternative<Error>(AsnBootstrapRegistry::parse(
      R"({"version":"1.0","services":[[["200-100"],["https://example/"]]]})")));
  CHECK(std::holds_alternative<Error>(AsnBootstrapRegistry::parse(
      R"({"version":"1.0","services":[[["1-10"],["https://one/"]],[["10-20"],["https://two/"]]]})")));
}
