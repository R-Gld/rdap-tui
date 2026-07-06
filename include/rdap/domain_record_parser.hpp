// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rdap/domain_record.hpp"
#include "rdap/error.hpp"

#include <nlohmann/json_fwd.hpp>

namespace rdap {

class DomainRecordParser {
public:
  static Result<DomainParseResult> parse(const nlohmann::json &document);
};

} // namespace rdap
