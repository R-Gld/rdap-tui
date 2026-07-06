// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rdap {

struct ParseWarning {
  std::string path;
  std::string message;
};

struct RdapLink {
  std::string value;
  std::string relation;
  std::string href;
  std::optional<std::string> title;
  std::optional<std::string> media_type;
};

struct RdapEvent {
  std::string action;
  std::string date;
  std::optional<std::string> actor;
  std::vector<RdapLink> links;
};

struct RdapTextBlock {
  std::optional<std::string> title;
  std::optional<std::string> type;
  std::vector<std::string> description;
  std::vector<RdapLink> links;
};

struct PublicId {
  std::string type;
  std::string identifier;
};

struct LabeledValue {
  std::string value;
  std::vector<std::string> types;
  std::optional<unsigned int> preference;
};

struct PostalAddress {
  std::vector<std::string> components;
  std::vector<std::string> types;
  std::optional<std::string> label;
  std::optional<unsigned int> preference;
};

struct JCardContact {
  std::optional<std::string> full_name;
  std::vector<std::string> organizations;
  std::vector<std::string> titles;
  std::vector<std::string> roles;
  std::vector<LabeledValue> emails;
  std::vector<LabeledValue> phones;
  std::vector<PostalAddress> addresses;
};

struct RdapEntity {
  std::optional<std::string> handle;
  std::vector<std::string> roles;
  std::vector<PublicId> public_ids;
  std::optional<JCardContact> contact;
  std::vector<std::string> status;
  std::vector<RdapEvent> events;
  std::vector<RdapTextBlock> remarks;
  std::vector<RdapLink> links;
  std::vector<RdapEntity> entities;
};

struct RdapNameserver {
  std::optional<std::string> handle;
  std::optional<std::string> ldh_name;
  std::optional<std::string> unicode_name;
  std::vector<std::string> ipv4_addresses;
  std::vector<std::string> ipv6_addresses;
  std::vector<std::string> status;
  std::vector<RdapEvent> events;
  std::vector<RdapTextBlock> remarks;
  std::vector<RdapLink> links;
};

struct DsData {
  std::optional<std::uint64_t> key_tag;
  std::optional<std::uint64_t> algorithm;
  std::optional<std::uint64_t> digest_type;
  std::optional<std::string> digest;
};

struct SecureDns {
  std::optional<bool> zone_signed;
  std::optional<bool> delegation_signed;
  std::optional<std::uint64_t> maximum_signature_life;
  std::vector<DsData> ds_data;
};

struct DomainRecord {
  std::optional<std::string> handle;
  std::optional<std::string> ldh_name;
  std::optional<std::string> unicode_name;
  std::vector<std::string> status;
  std::optional<std::string> port43;
  std::vector<std::string> conformance;
  std::vector<RdapEvent> events;
  std::vector<RdapNameserver> nameservers;
  std::vector<RdapEntity> entities;
  std::optional<SecureDns> secure_dns;
  std::vector<RdapTextBlock> notices;
  std::vector<RdapTextBlock> remarks;
  std::vector<RdapLink> links;
};

struct DomainParseResult {
  DomainRecord domain;
  std::vector<ParseWarning> warnings;
  bool redacted{false};
  bool truncated{false};
};

} // namespace rdap
