// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/domain_name.hpp"
#include "rdap/domain_record_parser.hpp"
#include "rdap/domain_view_formatter.hpp"
#include "rdap/http.hpp"
#include "rdap/network_record_parser.hpp"
#include "rdap/network_view_formatter.hpp"
#include "rdap/rdap_client.hpp"
#include "rdap/resource_query.hpp"

#include <iostream>
#include <variant>

int main() {
  auto domain = rdap::DomainName::parse("example.com");
  if (const auto *error = std::get_if<rdap::Error>(&domain)) {
    std::cerr << error->message << '\n';
    return 1;
  }

  rdap::CurlHttpClient http;
  rdap::RdapClient client(http);
  auto result = client.lookup_domain(std::get<rdap::DomainName>(domain), {});
  if (const auto *error = std::get_if<rdap::Error>(&result)) {
    std::cerr << error->message << ": " << error->detail << '\n';
    return 1;
  }

  const auto &document = std::get<rdap::RdapResponse>(result).document;
  if (document.value("objectClassName", "") != "domain") {
    std::cerr << "Unexpected RDAP object class\n";
    return 1;
  }
  auto projection = rdap::DomainRecordParser::parse(document);
  if (const auto *error = std::get_if<rdap::Error>(&projection)) {
    std::cerr << error->message << ": " << error->detail << '\n';
    return 1;
  }
  const auto views =
      rdap::DomainViewFormatter::format(std::get<rdap::DomainParseResult>(projection));
  if (views.overview.empty()) {
    std::cerr << "Structured RDAP view is empty\n";
    return 1;
  }

  auto ip_query = rdap::ResourceQueryParser::parse("1.1.1.1");
  if (const auto *error = std::get_if<rdap::Error>(&ip_query)) {
    std::cerr << error->message << '\n';
    return 1;
  }
  auto ip_result = client.lookup(std::get<rdap::ResourceQuery>(ip_query), {});
  if (const auto *error = std::get_if<rdap::Error>(&ip_result)) {
    std::cerr << error->message << ": " << error->detail << '\n';
    return 1;
  }
  auto network = rdap::NetworkRecordParser::parse(std::get<rdap::RdapResponse>(ip_result).document);
  if (!std::holds_alternative<rdap::NetworkParseResult>(network) ||
      rdap::NetworkViewFormatter::format(std::get<rdap::NetworkParseResult>(network))
          .overview.empty()) {
    std::cerr << "IP network projection failed\n";
    return 1;
  }

  auto asn_query = rdap::ResourceQueryParser::parse("AS13335");
  if (const auto *error = std::get_if<rdap::Error>(&asn_query)) {
    std::cerr << error->message << '\n';
    return 1;
  }
  auto asn_result = client.lookup(std::get<rdap::ResourceQuery>(asn_query), {});
  if (const auto *error = std::get_if<rdap::Error>(&asn_result)) {
    std::cerr << error->message << ": " << error->detail << '\n';
    return 1;
  }
  auto autnum = rdap::AutnumRecordParser::parse(std::get<rdap::RdapResponse>(asn_result).document);
  if (!std::holds_alternative<rdap::AutnumParseResult>(autnum) ||
      rdap::AutnumViewFormatter::format(std::get<rdap::AutnumParseResult>(autnum))
          .overview.empty()) {
    std::cerr << "ASN projection failed\n";
    return 1;
  }
  std::cout << "RDAP network smoke test passed\n";
  return 0;
}
