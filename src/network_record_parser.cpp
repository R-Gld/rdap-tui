// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/network_record_parser.hpp"

#include "rdap/domain_record_parser.hpp"
#include "rdap/resource_query.hpp"

#include <limits>
#include <nlohmann/json.hpp>
#include <string_view>

namespace rdap {
namespace {

Error projection_error(std::string message, std::string detail = {}) {
  return Error{ErrorCode::invalid_json, std::move(message), std::move(detail), std::nullopt,
               std::nullopt};
}

Result<DomainParseResult> common_projection(const nlohmann::json &document,
                                            std::string_view expected_class) {
  if (!document.is_object()) {
    return projection_error("The RDAP response cannot be projected because it is not an object.");
  }
  const auto iterator = document.find("objectClassName");
  const bool missing_class = iterator == document.end() || iterator->is_null();
  bool invalid_class = false;
  if (iterator != document.end() && !iterator->is_null()) {
    if (!iterator->is_string()) {
      invalid_class = true;
    } else {
      const auto actual = iterator->get<std::string>();
      if (actual != expected_class) {
        return projection_error("The RDAP response has an unexpected object class.",
                                "objectClassName is '" + actual + "'");
      }
    }
  }
  auto adapted = document;
  adapted["objectClassName"] = "domain";
  adapted.erase("ldhName");
  adapted.erase("unicodeName");
  adapted.erase("nameservers");
  adapted.erase("secureDNS");
  auto projected = DomainRecordParser::parse(adapted);
  if (missing_class) {
    std::get<DomainParseResult>(projected).warnings.push_back(
        ParseWarning{"$.objectClassName", "Missing network resource object class."});
  } else if (invalid_class) {
    std::get<DomainParseResult>(projected).warnings.push_back(
        ParseWarning{"$.objectClassName", "Expected a network resource object class string."});
  }
  return projected;
}

RegistrationData registration(const nlohmann::json &document, DomainParseResult &common,
                              std::vector<ParseWarning> &warnings) {
  RegistrationData result;
  auto optional_string = [&](std::string_view name) -> std::optional<std::string> {
    const auto iterator = document.find(name);
    if (iterator == document.end() || iterator->is_null()) {
      return std::nullopt;
    }
    if (!iterator->is_string()) {
      warnings.push_back(ParseWarning{"$." + std::string(name), "Expected a string."});
      return std::nullopt;
    }
    return iterator->get<std::string>();
  };

  result.handle = std::move(common.domain.handle);
  result.name = optional_string("name");
  result.type = optional_string("type");
  result.country = optional_string("country");
  result.port43 = std::move(common.domain.port43);
  result.conformance = std::move(common.domain.conformance);
  result.status = std::move(common.domain.status);
  result.events = std::move(common.domain.events);
  result.entities = std::move(common.domain.entities);
  result.notices = std::move(common.domain.notices);
  result.remarks = std::move(common.domain.remarks);
  result.links = std::move(common.domain.links);
  return result;
}

std::optional<std::string> address_field(const nlohmann::json &document, std::string_view name,
                                         std::vector<ParseWarning> &warnings) {
  const auto iterator = document.find(name);
  if (iterator == document.end() || iterator->is_null()) {
    return std::nullopt;
  }
  const auto path = "$." + std::string(name);
  if (!iterator->is_string()) {
    warnings.push_back(ParseWarning{path, "Expected an IP address string."});
    return std::nullopt;
  }
  auto parsed = IpNetworkQuery::parse(iterator->get<std::string>());
  if (const auto *error = std::get_if<Error>(&parsed)) {
    warnings.push_back(ParseWarning{path, error->message});
    return std::nullopt;
  }
  const auto &address = std::get<IpNetworkQuery>(parsed);
  if (address.prefix_length().has_value()) {
    warnings.push_back(ParseWarning{path, "Expected an address without a CIDR prefix."});
    return std::nullopt;
  }
  return std::string(address.canonical());
}

std::optional<std::uint32_t> autnum_field(const nlohmann::json &document, std::string_view name,
                                          std::vector<ParseWarning> &warnings) {
  const auto iterator = document.find(name);
  if (iterator == document.end() || iterator->is_null()) {
    return std::nullopt;
  }
  const auto path = "$." + std::string(name);
  if (iterator->is_number_unsigned()) {
    const auto value = iterator->get<std::uint64_t>();
    if (value <= std::numeric_limits<std::uint32_t>::max()) {
      return static_cast<std::uint32_t>(value);
    }
  } else if (iterator->is_number_integer()) {
    const auto value = iterator->get<std::int64_t>();
    if (value >= 0 &&
        static_cast<std::uint64_t>(value) <= std::numeric_limits<std::uint32_t>::max()) {
      return static_cast<std::uint32_t>(value);
    }
  }
  warnings.push_back(ParseWarning{path, "Expected an unsigned 32-bit integer."});
  return std::nullopt;
}

} // namespace

Result<NetworkParseResult> NetworkRecordParser::parse(const nlohmann::json &document) {
  auto common_result = common_projection(document, "ip network");
  if (const auto *error = std::get_if<Error>(&common_result)) {
    return *error;
  }
  auto common = std::get<DomainParseResult>(std::move(common_result));
  auto warnings = std::move(common.warnings);

  NetworkRecord record;
  record.registration = registration(document, common, warnings);
  record.start_address = address_field(document, "startAddress", warnings);
  record.end_address = address_field(document, "endAddress", warnings);
  const auto version = document.find("ipVersion");
  if (version != document.end() && !version->is_null()) {
    if (version->is_string() &&
        (version->get<std::string>() == "v4" || version->get<std::string>() == "v6")) {
      record.ip_version = version->get<std::string>();
    } else {
      warnings.push_back(ParseWarning{"$.ipVersion", "Expected 'v4' or 'v6'."});
    }
  }
  const auto parent = document.find("parentHandle");
  if (parent != document.end() && !parent->is_null()) {
    if (parent->is_string()) {
      record.parent_handle = parent->get<std::string>();
    } else {
      warnings.push_back(ParseWarning{"$.parentHandle", "Expected a string."});
    }
  }

  if (record.start_address.has_value() && record.end_address.has_value()) {
    const auto start = std::get<IpNetworkQuery>(IpNetworkQuery::parse(*record.start_address));
    const auto end = std::get<IpNetworkQuery>(IpNetworkQuery::parse(*record.end_address));
    if (start.family() != end.family()) {
      warnings.push_back(ParseWarning{"$.endAddress", "The address range mixes IPv4 and IPv6."});
    } else if (start.bytes() > end.bytes()) {
      warnings.push_back(ParseWarning{"$.endAddress", "The address range ends before it starts."});
    }
    const auto expected = start.family() == IpFamily::v4 ? "v4" : "v6";
    if (record.ip_version.has_value() && *record.ip_version != expected) {
      warnings.push_back(
          ParseWarning{"$.ipVersion", "The IP version does not match the address range."});
    }
  } else {
    const auto &address =
        record.start_address.has_value() ? record.start_address : record.end_address;
    if (address.has_value() && record.ip_version.has_value()) {
      const auto parsed = std::get<IpNetworkQuery>(IpNetworkQuery::parse(*address));
      const auto expected = parsed.family() == IpFamily::v4 ? "v4" : "v6";
      if (*record.ip_version != expected) {
        warnings.push_back(
            ParseWarning{"$.ipVersion", "The IP version does not match the address range."});
      }
    }
  }
  return NetworkParseResult{std::move(record), std::move(warnings), common.redacted,
                            common.truncated};
}

Result<AutnumParseResult> AutnumRecordParser::parse(const nlohmann::json &document) {
  auto common_result = common_projection(document, "autnum");
  if (const auto *error = std::get_if<Error>(&common_result)) {
    return *error;
  }
  auto common = std::get<DomainParseResult>(std::move(common_result));
  auto warnings = std::move(common.warnings);

  AutnumRecord record;
  record.registration = registration(document, common, warnings);
  record.start_autnum = autnum_field(document, "startAutnum", warnings);
  record.end_autnum = autnum_field(document, "endAutnum", warnings);
  if (record.start_autnum.has_value() && record.end_autnum.has_value() &&
      *record.start_autnum > *record.end_autnum) {
    warnings.push_back(ParseWarning{"$.endAutnum", "The ASN range ends before it starts."});
  }
  return AutnumParseResult{std::move(record), std::move(warnings), common.redacted,
                           common.truncated};
}

} // namespace rdap
