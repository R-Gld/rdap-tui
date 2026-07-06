// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/network_record_parser.hpp"
#include "rdap/network_view_formatter.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>

using namespace rdap;

namespace {

nlohmann::json fixture(const std::string &name) {
  std::ifstream input(std::string(RDAP_TEST_FIXTURES) + "/" + name);
  REQUIRE(input.good());
  return nlohmann::json::parse(input);
}

bool contains(const std::vector<std::string> &lines, const std::string &fragment) {
  return std::any_of(lines.begin(), lines.end(),
                     [&](const auto &line) { return line.find(fragment) != std::string::npos; });
}

} // namespace

TEST_CASE("network views show allocation contacts and notices") {
  auto parsed = NetworkRecordParser::parse(fixture("network_complete.json"));
  REQUIRE(std::holds_alternative<NetworkParseResult>(parsed));
  const auto views = NetworkViewFormatter::format(std::get<NetworkParseResult>(parsed));

  CHECK(contains(views.overview, "Registration country: US"));
  CHECK(contains(views.contacts, "noc@example.test"));
  CHECK(contains(views.dns, "Start address: 192.0.2.0"));
  CHECK(contains(views.dns, "Parent handle: NET-192-0-0-0-0"));
  CHECK(contains(views.notices, "Network lookup terms."));
}

TEST_CASE("autnum views show ASN range and shared contacts") {
  auto parsed = AutnumRecordParser::parse(fixture("autnum_complete.json"));
  REQUIRE(std::holds_alternative<AutnumParseResult>(parsed));
  const auto views = AutnumViewFormatter::format(std::get<AutnumParseResult>(parsed));

  CHECK(contains(views.overview, "Name: CLOUDFLARENET"));
  CHECK(contains(views.contacts, "Example ASN Operator"));
  CHECK(contains(views.dns, "Start ASN: AS13335"));
  CHECK(contains(views.dns, "End ASN: AS13335"));
}

TEST_CASE("redacted network contact is marked undisclosed") {
  auto parsed = NetworkRecordParser::parse(fixture("network_redacted.json"));
  REQUIRE(std::holds_alternative<NetworkParseResult>(parsed));
  const auto views = NetworkViewFormatter::format(std::get<NetworkParseResult>(parsed));

  CHECK(contains(views.contacts, "Contact details: Not disclosed"));
  CHECK(contains(views.notices, "Data redaction was reported"));
}
