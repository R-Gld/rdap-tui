// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rdap/domain_name.hpp"
#include "rdap/resource_query.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rdap {

class BootstrapRegistry {
public:
  static Result<BootstrapRegistry> parse(std::string_view document);
  [[nodiscard]] Result<std::vector<std::string>> resolve(const DomainName &domain) const;

private:
  struct Service {
    std::vector<std::string> entries;
    std::vector<std::string> urls;
  };

  std::vector<Service> services_;
};

class IpBootstrapRegistry {
public:
  static Result<IpBootstrapRegistry> parse(std::string_view document, IpFamily family);
  [[nodiscard]] Result<std::vector<std::string>> resolve(const IpNetworkQuery &query) const;

private:
  struct Prefix {
    std::array<std::uint8_t, 16> bytes{};
    std::uint8_t length{};
  };
  struct Service {
    std::vector<Prefix> prefixes;
    std::vector<std::string> urls;
  };

  IpFamily family_{IpFamily::v4};
  std::vector<Service> services_;
};

class AsnBootstrapRegistry {
public:
  static Result<AsnBootstrapRegistry> parse(std::string_view document);
  [[nodiscard]] Result<std::vector<std::string>> resolve(const AutnumQuery &query) const;

private:
  struct Range {
    std::uint32_t start{};
    std::uint32_t end{};
  };
  struct Service {
    std::vector<Range> ranges;
    std::vector<std::string> urls;
  };

  std::vector<Service> services_;
};

} // namespace rdap
