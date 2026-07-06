// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/resource_query.hpp"

#include <catch2/catch_test_macros.hpp>
#include <variant>

using namespace rdap;

TEST_CASE("resource query detects domains IP addresses CIDRs and ASNs") {
  CHECK(std::holds_alternative<DomainName>(
      std::get<ResourceQuery>(ResourceQueryParser::parse("example.com"))));
  CHECK(std::holds_alternative<DomainName>(
      std::get<ResourceQuery>(ResourceQueryParser::parse("aside.example"))));
  CHECK(std::holds_alternative<IpNetworkQuery>(
      std::get<ResourceQuery>(ResourceQueryParser::parse("192.0.2.1"))));
  CHECK(std::holds_alternative<IpNetworkQuery>(
      std::get<ResourceQuery>(ResourceQueryParser::parse("2001:db8::1"))));
  CHECK(std::holds_alternative<AutnumQuery>(
      std::get<ResourceQuery>(ResourceQueryParser::parse("13335"))));
  CHECK(std::holds_alternative<AutnumQuery>(
      std::get<ResourceQuery>(ResourceQueryParser::parse("as13335"))));
}

TEST_CASE("IP queries are canonicalized") {
  auto ipv4 = IpNetworkQuery::parse(" 192.0.2.42/24 ");
  REQUIRE(std::holds_alternative<IpNetworkQuery>(ipv4));
  CHECK(std::get<IpNetworkQuery>(ipv4).canonical() == "192.0.2.0/24");

  auto ipv6 = IpNetworkQuery::parse("2001:0DB8:0:0:0:0:0:1/64");
  REQUIRE(std::holds_alternative<IpNetworkQuery>(ipv6));
  CHECK(std::get<IpNetworkQuery>(ipv6).canonical() == "2001:db8::/64");

  auto all = IpNetworkQuery::parse("0:0:0:0:0:0:0:0/0");
  REQUIRE(std::holds_alternative<IpNetworkQuery>(all));
  CHECK(std::get<IpNetworkQuery>(all).canonical() == "::/0");
}

TEST_CASE("invalid IP-shaped input is not treated as a domain") {
  for (const auto input : {"999.1.1.1", "192.168.001.1", "2001:db8::1%en0", "192.0.2.1/33",
                           "2001:db8::1/129", "192.0.2.1//24"}) {
    CAPTURE(input);
    CHECK(std::holds_alternative<Error>(ResourceQueryParser::parse(input)));
  }
}

TEST_CASE("ASN parsing uses asplain unsigned 32-bit values") {
  auto parsed = AutnumQuery::parse("AS001335");
  REQUIRE(std::holds_alternative<AutnumQuery>(parsed));
  CHECK(std::get<AutnumQuery>(parsed).number() == 1335U);
  CHECK(std::get<AutnumQuery>(parsed).canonical() == "AS1335");

  CHECK(std::holds_alternative<AutnumQuery>(AutnumQuery::parse("4294967295")));
  CHECK(std::holds_alternative<Error>(AutnumQuery::parse("4294967296")));
  CHECK(std::holds_alternative<Error>(AutnumQuery::parse("AS1.10")));
}
