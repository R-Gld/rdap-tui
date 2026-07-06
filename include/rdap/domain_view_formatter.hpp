// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rdap/domain_record.hpp"

#include <string>
#include <vector>

namespace rdap {

struct DomainViewLines {
  std::vector<std::string> overview;
  std::vector<std::string> contacts;
  std::vector<std::string> dns;
  std::vector<std::string> notices;
};

class DomainViewFormatter {
public:
  static DomainViewLines format(const DomainParseResult &result);
};

} // namespace rdap
