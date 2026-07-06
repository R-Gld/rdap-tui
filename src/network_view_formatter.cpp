// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/network_view_formatter.hpp"

#include <string>

namespace rdap {
namespace {

std::string value_or_missing(const std::optional<std::string> &value) {
  return value.has_value() && !value->empty() ? *value : "Not provided";
}

std::string number_or_missing(const std::optional<std::uint32_t> &value) {
  return value.has_value() ? std::to_string(*value) : "Not provided";
}

template <typename ParseResult>
DomainViewLines common_views(const RegistrationData &registration, const ParseResult &result) {
  DomainParseResult adapted;
  adapted.domain.handle = registration.handle;
  adapted.domain.port43 = registration.port43;
  adapted.domain.conformance = registration.conformance;
  adapted.domain.status = registration.status;
  adapted.domain.events = registration.events;
  adapted.domain.entities = registration.entities;
  adapted.domain.notices = registration.notices;
  adapted.domain.remarks = registration.remarks;
  adapted.domain.links = registration.links;
  adapted.warnings = result.warnings;
  adapted.redacted = result.redacted;
  adapted.truncated = result.truncated;
  return DomainViewFormatter::format(adapted);
}

void append_status_and_events(std::vector<std::string> &lines,
                              const RegistrationData &registration) {
  lines.emplace_back();
  lines.push_back("Status");
  if (registration.status.empty()) {
    lines.push_back("  Not provided");
  } else {
    for (const auto &status : registration.status) {
      lines.push_back("  - " + status);
    }
  }
  lines.emplace_back();
  lines.push_back("Events");
  if (registration.events.empty()) {
    lines.push_back("  Not provided");
  } else {
    for (const auto &event : registration.events) {
      auto line = "  " + event.action + ": " + event.date;
      if (event.actor.has_value()) {
        line += " (" + *event.actor + ")";
      }
      lines.push_back(std::move(line));
    }
  }
}

template <typename ParseResult>
void append_disclosures(std::vector<std::string> &lines, const ParseResult &result) {
  if (result.redacted || result.truncated) {
    lines.emplace_back();
    lines.push_back("Disclosure warning");
    if (result.redacted) {
      lines.push_back("  Some registration data was redacted by the service.");
    }
    if (result.truncated) {
      lines.push_back("  The service reported a truncated response.");
    }
  }
  if (!result.warnings.empty()) {
    lines.emplace_back();
    lines.push_back("Projection warnings: " + std::to_string(result.warnings.size()) +
                    " (see Notices)");
  }
}

void append_registration(std::vector<std::string> &lines, const RegistrationData &registration) {
  lines.emplace_back();
  lines.push_back("Registration");
  lines.push_back("  Type: " + value_or_missing(registration.type));
  lines.push_back("  Registration country: " + value_or_missing(registration.country));
  lines.push_back("  WHOIS server: " + value_or_missing(registration.port43));
}

} // namespace

DomainViewLines NetworkViewFormatter::format(const NetworkParseResult &result) {
  const auto &record = result.record;
  auto output = common_views(record.registration, result);
  output.overview = {
      "IP network",
      "  Name: " + value_or_missing(record.registration.name),
      "  Handle: " + value_or_missing(record.registration.handle),
  };
  append_status_and_events(output.overview, record.registration);
  append_registration(output.overview, record.registration);
  append_disclosures(output.overview, result);

  output.dns = {
      "Network",
      "  Start address: " + value_or_missing(record.start_address),
      "  End address: " + value_or_missing(record.end_address),
      "  IP version: " + value_or_missing(record.ip_version),
      "  Parent handle: " + value_or_missing(record.parent_handle),
  };
  return output;
}

DomainViewLines AutnumViewFormatter::format(const AutnumParseResult &result) {
  const auto &record = result.record;
  auto output = common_views(record.registration, result);
  output.overview = {
      "Autonomous system",
      "  Name: " + value_or_missing(record.registration.name),
      "  Handle: " + value_or_missing(record.registration.handle),
  };
  append_status_and_events(output.overview, record.registration);
  append_registration(output.overview, record.registration);
  append_disclosures(output.overview, result);

  output.dns = {
      "Autnum",
      "  Start ASN: AS" + number_or_missing(record.start_autnum),
      "  End ASN: AS" + number_or_missing(record.end_autnum),
  };
  if (!record.start_autnum.has_value()) {
    output.dns[1] = "  Start ASN: Not provided";
  }
  if (!record.end_autnum.has_value()) {
    output.dns[2] = "  End ASN: Not provided";
  }
  return output;
}

} // namespace rdap
