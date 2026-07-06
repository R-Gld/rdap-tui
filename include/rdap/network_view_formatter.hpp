// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rdap/domain_view_formatter.hpp"
#include "rdap/network_record.hpp"

namespace rdap {

class NetworkViewFormatter {
public:
  static DomainViewLines format(const NetworkParseResult &result);
};

class AutnumViewFormatter {
public:
  static DomainViewLines format(const AutnumParseResult &result);
};

} // namespace rdap
