// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rdap/domain_record.hpp"

#include <cstdint>

namespace rdap {

struct RegistrationData {
  std::optional<std::string> handle;
  std::optional<std::string> name;
  std::optional<std::string> type;
  std::optional<std::string> country;
  std::optional<std::string> port43;
  std::vector<std::string> conformance;
  std::vector<std::string> status;
  std::vector<RdapEvent> events;
  std::vector<RdapEntity> entities;
  std::vector<RdapTextBlock> notices;
  std::vector<RdapTextBlock> remarks;
  std::vector<RdapLink> links;
};

struct NetworkRecord {
  RegistrationData registration;
  std::optional<std::string> start_address;
  std::optional<std::string> end_address;
  std::optional<std::string> ip_version;
  std::optional<std::string> parent_handle;
};

struct AutnumRecord {
  RegistrationData registration;
  std::optional<std::uint32_t> start_autnum;
  std::optional<std::uint32_t> end_autnum;
};

template <typename Record> struct NetworkResourceParseResult {
  Record record;
  std::vector<ParseWarning> warnings;
  bool redacted{false};
  bool truncated{false};
};

using NetworkParseResult = NetworkResourceParseResult<NetworkRecord>;
using AutnumParseResult = NetworkResourceParseResult<AutnumRecord>;

} // namespace rdap
