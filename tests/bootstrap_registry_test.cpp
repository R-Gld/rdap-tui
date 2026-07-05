// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/bootstrap_registry.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <variant>
#include <vector>

using rdap::BootstrapRegistry;
using rdap::DomainName;
using rdap::Error;

namespace {

DomainName domain(std::string_view value) {
  auto result = DomainName::parse(value);
  REQUIRE(std::holds_alternative<DomainName>(result));
  return std::get<DomainName>(std::move(result));
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
