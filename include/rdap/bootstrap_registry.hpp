// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rdap/domain_name.hpp"

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

} // namespace rdap
