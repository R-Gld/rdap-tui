// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/domain_record_parser.hpp"
#include "rdap/domain_view_formatter.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>
#include <vector>

using namespace rdap;

namespace {

DomainParseResult parsed_fixture(const std::string &name) {
  std::ifstream input(std::string(RDAP_TEST_FIXTURES) + "/" + name);
  REQUIRE(input.good());
  auto parsed = DomainRecordParser::parse(nlohmann::json::parse(input));
  REQUIRE(std::holds_alternative<DomainParseResult>(parsed));
  return std::get<DomainParseResult>(std::move(parsed));
}

bool contains(const std::vector<std::string> &lines, const std::string &fragment) {
  return std::any_of(lines.begin(), lines.end(),
                     [&](const auto &line) { return line.find(fragment) != std::string::npos; });
}

} // namespace

TEST_CASE("domain views expose registration contacts and DNS data") {
  const auto views = DomainViewFormatter::format(parsed_fixture("domain_complete.json"));

  CHECK(contains(views.overview, "Registrar: Example Registrar"));
  CHECK(contains(views.overview, "registration: 1995-08-14T04:00:00Z"));
  CHECK(contains(views.contacts, "help@example.test"));
  CHECK(contains(views.contacts, "1 Registry Way, Paris"));
  CHECK(contains(views.dns, "NS1.EXAMPLE.COM"));
  CHECK(contains(views.dns, "keyTag=12345"));
  CHECK(contains(views.notices, "Use subject to policy."));
}

TEST_CASE("minimal view uses explicit missing-value placeholders") {
  const auto views = DomainViewFormatter::format(parsed_fixture("domain_minimal.json"));

  CHECK(contains(views.overview, "Handle: Not provided"));
  CHECK(contains(views.contacts, "Not provided"));
  CHECK(contains(views.dns, "Not provided"));
}

TEST_CASE("redacted view distinguishes undisclosed contact details") {
  const auto views = DomainViewFormatter::format(parsed_fixture("domain_redacted.json"));

  CHECK(contains(views.contacts, "Contact details: Not disclosed"));
  CHECK(contains(views.overview, "Some registration data was redacted"));
  CHECK(contains(views.notices, "Response truncation was reported"));
}
