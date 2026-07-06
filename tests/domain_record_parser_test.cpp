// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/domain_record_parser.hpp"

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

DomainParseResult parsed_fixture(const std::string &name) {
  auto result = DomainRecordParser::parse(fixture(name));
  REQUIRE(std::holds_alternative<DomainParseResult>(result));
  return std::get<DomainParseResult>(std::move(result));
}

} // namespace

TEST_CASE("complete domain response is projected") {
  const auto result = parsed_fixture("domain_complete.json");

  CHECK(result.domain.ldh_name == "EXAMPLE.COM");
  REQUIRE(result.domain.entities.size() == 1U);
  const auto &registrar = result.domain.entities.front();
  CHECK(registrar.roles == std::vector<std::string>{"registrar"});
  REQUIRE(registrar.contact.has_value());
  CHECK(registrar.contact->full_name == "Example Registrar");
  REQUIRE(registrar.contact->emails.size() == 1U);
  CHECK(registrar.contact->emails.front().types == std::vector<std::string>{"work"});
  CHECK(registrar.contact->emails.front().preference == 1U);

  REQUIRE(result.domain.nameservers.size() == 1U);
  CHECK(result.domain.nameservers.front().ipv4_addresses == std::vector<std::string>{"192.0.2.1"});
  REQUIRE(result.domain.secure_dns.has_value());
  CHECK(result.domain.secure_dns->zone_signed == true);
  REQUIRE(result.domain.secure_dns->ds_data.size() == 1U);
  CHECK(result.domain.secure_dns->ds_data.front().key_tag == 12345U);
  CHECK(result.warnings.empty());
}

TEST_CASE("minimal domain response keeps absent fields optional") {
  const auto result = parsed_fixture("domain_minimal.json");

  CHECK(result.domain.ldh_name == "EXAMPLE.COM");
  CHECK_FALSE(result.domain.handle.has_value());
  CHECK(result.domain.entities.empty());
  CHECK(result.warnings.empty());
}

TEST_CASE("known malformed fields produce warnings without rejecting valid fields") {
  const auto result = parsed_fixture("domain_malformed.json");

  CHECK_FALSE(result.domain.ldh_name.has_value());
  CHECK(result.domain.status.empty());
  CHECK_FALSE(result.domain.secure_dns->zone_signed.has_value());
  CHECK_FALSE(result.domain.secure_dns->maximum_signature_life.has_value());
  CHECK(result.warnings.size() >= 8U);
}

TEST_CASE("redaction and truncation disclosures are detected") {
  const auto result = parsed_fixture("domain_redacted.json");

  CHECK(result.redacted);
  CHECK(result.truncated);
}

TEST_CASE("nested entities are retained") {
  const auto result = parsed_fixture("domain_nested.json");

  REQUIRE(result.domain.entities.size() == 1U);
  REQUIRE(result.domain.entities.front().entities.size() == 1U);
  CHECK(result.domain.entities.front().entities.front().contact->full_name == "Nested Contact");
}

TEST_CASE("explicit non-domain object falls back to JSON") {
  const auto document =
      nlohmann::json::parse(R"({"objectClassName":"entity","handle":"ENTITY-1"})");

  const auto result = DomainRecordParser::parse(document);

  REQUIRE(std::holds_alternative<Error>(result));
  CHECK(std::get<Error>(result).code == ErrorCode::invalid_json);
}

TEST_CASE("null fields are treated as absent") {
  const auto document = nlohmann::json::parse(
      R"({"objectClassName":"domain","rdapConformance":null,"ldhName":null})");

  const auto result = DomainRecordParser::parse(document);

  REQUIRE(std::holds_alternative<DomainParseResult>(result));
  const auto &projection = std::get<DomainParseResult>(result);
  CHECK_FALSE(projection.domain.ldh_name.has_value());
  REQUIRE(projection.warnings.size() == 1U);
  CHECK(projection.warnings.front().path == "$.rdapConformance");
}

TEST_CASE("unknown extension members are ignored without warnings") {
  const auto document = nlohmann::json::parse(
      R"({"objectClassName":"domain","rdapConformance":["rdap_level_0"],"extension":{"arbitrary":[1,2,3]}})");

  const auto result = DomainRecordParser::parse(document);

  REQUIRE(std::holds_alternative<DomainParseResult>(result));
  CHECK(std::get<DomainParseResult>(result).warnings.empty());
}

TEST_CASE("collections and entity recursion are bounded") {
  nlohmann::json document = {
      {"objectClassName", "domain"},
      {"rdapConformance", {"rdap_level_0"}},
      {"status", nlohmann::json::array()},
      {"entities", nlohmann::json::array()},
  };
  for (std::size_t index = 0; index < 1025U; ++index) {
    document["status"].push_back("active");
  }

  nlohmann::json nested = {
      {"objectClassName", "entity"},
      {"handle", "leaf"},
  };
  for (std::size_t depth = 0; depth < 10U; ++depth) {
    nested = {
        {"objectClassName", "entity"},
        {"handle", "level-" + std::to_string(depth)},
        {"entities", nlohmann::json::array({std::move(nested)})},
    };
  }
  document["entities"].push_back(std::move(nested));

  const auto result = DomainRecordParser::parse(document);

  REQUIRE(std::holds_alternative<DomainParseResult>(result));
  const auto &projection = std::get<DomainParseResult>(result);
  CHECK(projection.domain.status.size() == 1024U);
  CHECK(projection.warnings.size() == 2U);
}
