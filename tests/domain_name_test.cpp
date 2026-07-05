// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/domain_name.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <variant>

using rdap::DomainName;
using rdap::Error;

TEST_CASE("domain names are normalized") {
  auto result = DomainName::parse("  ExAmPlE.COM. \n");
  REQUIRE(std::holds_alternative<DomainName>(result));
  CHECK(std::get<DomainName>(result).ascii() == "example.com");
}

TEST_CASE("a TLD is a valid lookup name") {
  auto result = DomainName::parse("COM");
  REQUIRE(std::holds_alternative<DomainName>(result));
  CHECK(std::get<DomainName>(result).ascii() == "com");
}

TEST_CASE("invalid domain labels are rejected") {
  for (const std::string input : {"", ".com", "example..com", "-example.com", "example-.com",
                                  "example_com", "example.com.."}) {
    CAPTURE(input);
    CHECK(std::holds_alternative<Error>(DomainName::parse(input)));
  }
}

TEST_CASE("unicode input explains the Punycode requirement") {
  auto result = DomainName::parse("café.example");
  REQUIRE(std::holds_alternative<Error>(result));
  CHECK(std::get<Error>(result).message.find("Punycode") != std::string::npos);
}

TEST_CASE("domain length limits are enforced") {
  const std::string long_label(64U, 'a');
  CHECK(std::holds_alternative<Error>(DomainName::parse(long_label + ".com")));

  const std::string maximum_label(63U, 'a');
  CHECK(std::holds_alternative<DomainName>(DomainName::parse(maximum_label + ".com")));
}
