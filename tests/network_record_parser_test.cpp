// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/network_record_parser.hpp"

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

} // namespace

TEST_CASE("complete IP network response is projected with shared entities") {
  auto parsed = NetworkRecordParser::parse(fixture("network_complete.json"));
  REQUIRE(std::holds_alternative<NetworkParseResult>(parsed));
  const auto &result = std::get<NetworkParseResult>(parsed);

  CHECK(result.record.start_address == "192.0.2.0");
  CHECK(result.record.end_address == "192.0.2.255");
  CHECK(result.record.ip_version == "v4");
  CHECK(result.record.registration.country == "US");
  REQUIRE(result.record.registration.entities.size() == 1U);
  REQUIRE(result.record.registration.entities.front().entities.size() == 1U);
  CHECK(result.warnings.empty());
}

TEST_CASE("IPv6 response addresses are canonicalized") {
  auto parsed = NetworkRecordParser::parse(fixture("network_minimal.json"));
  REQUIRE(std::holds_alternative<NetworkParseResult>(parsed));
  const auto &record = std::get<NetworkParseResult>(parsed).record;

  CHECK(record.start_address == "2001:db8::");
  CHECK(record.end_address == "2001:db8::ffff");
}

TEST_CASE("malformed network fields produce warnings") {
  auto parsed = NetworkRecordParser::parse(fixture("network_malformed.json"));
  REQUIRE(std::holds_alternative<NetworkParseResult>(parsed));
  const auto &result = std::get<NetworkParseResult>(parsed);

  CHECK_FALSE(result.record.start_address.has_value());
  CHECK_FALSE(result.record.end_address.has_value());
  CHECK_FALSE(result.record.ip_version.has_value());
  CHECK(result.warnings.size() >= 8U);
}

TEST_CASE("network disclosures and object classes are handled") {
  auto parsed = NetworkRecordParser::parse(fixture("network_redacted.json"));
  REQUIRE(std::holds_alternative<NetworkParseResult>(parsed));
  CHECK(std::get<NetworkParseResult>(parsed).redacted);

  const auto wrong =
      NetworkRecordParser::parse(nlohmann::json::parse(R"({"objectClassName":"autnum"})"));
  CHECK(std::holds_alternative<Error>(wrong));
}

TEST_CASE("autnum response projects unsigned 32-bit ranges") {
  auto parsed = AutnumRecordParser::parse(fixture("autnum_complete.json"));
  REQUIRE(std::holds_alternative<AutnumParseResult>(parsed));
  const auto &result = std::get<AutnumParseResult>(parsed);

  CHECK(result.record.start_autnum == 13335U);
  CHECK(result.record.end_autnum == 13335U);
  CHECK(result.record.registration.name == "CLOUDFLARENET");
  CHECK(result.warnings.empty());
}

TEST_CASE("malformed autnum fields are omitted with warnings") {
  auto parsed = AutnumRecordParser::parse(fixture("autnum_malformed.json"));
  REQUIRE(std::holds_alternative<AutnumParseResult>(parsed));
  const auto &result = std::get<AutnumParseResult>(parsed);

  CHECK_FALSE(result.record.start_autnum.has_value());
  CHECK_FALSE(result.record.end_autnum.has_value());
  CHECK(result.truncated);
  CHECK(result.warnings.size() >= 4U);
}
