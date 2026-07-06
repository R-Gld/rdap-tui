// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rdap/error.hpp"
#include "rdap/network_record.hpp"

#include <nlohmann/json_fwd.hpp>

namespace rdap {

class NetworkRecordParser {
public:
  static Result<NetworkParseResult> parse(const nlohmann::json &document);
};

class AutnumRecordParser {
public:
  static Result<AutnumParseResult> parse(const nlohmann::json &document);
};

} // namespace rdap
