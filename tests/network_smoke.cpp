// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/domain_name.hpp"
#include "rdap/http.hpp"
#include "rdap/rdap_client.hpp"

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
  std::cout << "RDAP network smoke test passed\n";
  return 0;
}
