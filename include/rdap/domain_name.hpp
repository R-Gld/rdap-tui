// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rdap/error.hpp"

#include <string>
#include <string_view>

namespace rdap {

class DomainName {
public:
  static Result<DomainName> parse(std::string_view input);

  [[nodiscard]] std::string_view ascii() const noexcept { return ascii_; }

private:
  explicit DomainName(std::string ascii) : ascii_(std::move(ascii)) {}

  std::string ascii_;
};

} // namespace rdap
