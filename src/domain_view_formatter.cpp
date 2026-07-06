// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/domain_view_formatter.hpp"

#include <algorithm>
#include <sstream>
#include <string_view>

namespace rdap {
namespace {

std::string value_or_missing(const std::optional<std::string> &value) {
  return value.has_value() && !value->empty() ? *value : "Not provided";
}

std::string yes_no_missing(const std::optional<bool> &value) {
  if (!value.has_value()) {
    return "Not provided";
  }
  return *value ? "Yes" : "No";
}

std::string join(const std::vector<std::string> &values, std::string_view separator) {
  std::ostringstream output;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0U) {
      output << separator;
    }
    output << values[index];
  }
  return output.str();
}

std::string strip_scheme(std::string value, std::string_view scheme) {
  if (value.starts_with(scheme)) {
    value.erase(0U, scheme.size());
  }
  return value;
}

std::string labels(const std::vector<std::string> &types) {
  return types.empty() ? std::string{} : " [" + join(types, ", ") + "]";
}

void append_values(std::vector<std::string> &lines, std::string_view label,
                   const std::vector<std::string> &values, std::string_view indent = "  ") {
  if (values.empty()) {
    lines.push_back(std::string(indent) + std::string(label) + ": Not provided");
    return;
  }
  for (std::size_t index = 0; index < values.size(); ++index) {
    lines.push_back(std::string(indent) + (index == 0U ? std::string(label) + ": " : "  ") +
                    values[index]);
  }
}

const RdapEntity *find_registrar(const std::vector<RdapEntity> &entities) {
  for (const auto &entity : entities) {
    if (std::find(entity.roles.begin(), entity.roles.end(), "registrar") != entity.roles.end()) {
      return &entity;
    }
    if (const auto *nested = find_registrar(entity.entities); nested != nullptr) {
      return nested;
    }
  }
  return nullptr;
}

std::string entity_name(const RdapEntity &entity) {
  if (entity.contact.has_value()) {
    if (entity.contact->full_name.has_value() && !entity.contact->full_name->empty()) {
      return *entity.contact->full_name;
    }
    if (!entity.contact->organizations.empty()) {
      return entity.contact->organizations.front();
    }
  }
  return value_or_missing(entity.handle);
}

void append_links(std::vector<std::string> &lines, const std::vector<RdapLink> &links,
                  std::string_view indent) {
  for (const auto &link : links) {
    const auto title = link.title.has_value() ? *link.title : link.relation;
    lines.push_back(std::string(indent) + "- " + title + ": " + link.href);
  }
}

void append_text_blocks(std::vector<std::string> &lines, std::string_view heading,
                        const std::vector<RdapTextBlock> &blocks, std::string_view indent = "") {
  if (blocks.empty()) {
    return;
  }
  lines.push_back(std::string(indent) + std::string(heading));
  for (const auto &block : blocks) {
    auto title = block.title.value_or(block.type.value_or("Notice"));
    lines.push_back(std::string(indent) + "  " + title);
    for (const auto &description : block.description) {
      lines.push_back(std::string(indent) + "    " + description);
    }
    append_links(lines, block.links, std::string(indent) + "    ");
  }
  lines.emplace_back();
}

void append_entity(std::vector<std::string> &lines, const RdapEntity &entity, std::size_t depth,
                   bool redacted) {
  const std::string indent(depth * 2U, ' ');
  const auto role_text = entity.roles.empty() ? "entity" : join(entity.roles, ", ");
  lines.push_back(indent + "[" + role_text + "] " + entity_name(entity));
  lines.push_back(indent + "  Handle: " + value_or_missing(entity.handle));

  if (!entity.public_ids.empty()) {
    for (const auto &id : entity.public_ids) {
      lines.push_back(indent + "  " + id.type + ": " + id.identifier);
    }
  }

  if (!entity.contact.has_value()) {
    lines.push_back(
        indent + "  Contact details: " + std::string(redacted ? "Not disclosed" : "Not provided"));
  } else {
    const auto &contact = *entity.contact;
    append_values(lines, "Organization", contact.organizations, indent + "  ");
    append_values(lines, "Title", contact.titles, indent + "  ");
    append_values(lines, "Contact role", contact.roles, indent + "  ");
    if (contact.emails.empty()) {
      lines.push_back(indent +
                      "  Email: " + std::string(redacted ? "Not disclosed" : "Not provided"));
    }
    for (const auto &email : contact.emails) {
      lines.push_back(indent + "  Email" + labels(email.types) + ": " +
                      strip_scheme(email.value, "mailto:"));
    }
    if (contact.phones.empty()) {
      lines.push_back(indent +
                      "  Phone: " + std::string(redacted ? "Not disclosed" : "Not provided"));
    }
    for (const auto &phone : contact.phones) {
      lines.push_back(indent + "  Phone" + labels(phone.types) + ": " +
                      strip_scheme(phone.value, "tel:"));
    }
    if (contact.addresses.empty()) {
      lines.push_back(indent +
                      "  Address: " + std::string(redacted ? "Not disclosed" : "Not provided"));
    }
    for (const auto &address : contact.addresses) {
      const auto display = address.label.value_or(join(address.components, ", "));
      lines.push_back(indent + "  Address" + labels(address.types) + ": " + display);
    }
  }

  for (const auto &event : entity.events) {
    lines.push_back(indent + "  " + event.action + ": " + event.date);
  }
  lines.emplace_back();
  for (const auto &nested : entity.entities) {
    append_entity(lines, nested, depth + 1U, redacted);
  }
}

void append_scoped_remarks(std::vector<std::string> &lines, const RdapEntity &entity,
                           std::string_view scope) {
  const auto name = entity_name(entity);
  append_text_blocks(lines, std::string(scope) + " " + name + " remarks", entity.remarks);
  for (const auto &nested : entity.entities) {
    append_scoped_remarks(lines, nested, "Entity");
  }
}

} // namespace

DomainViewLines DomainViewFormatter::format(const DomainParseResult &result) {
  DomainViewLines output;
  const auto &domain = result.domain;

  output.overview = {
      "Domain",
      "  LDH name: " + value_or_missing(domain.ldh_name),
      "  Unicode name: " + value_or_missing(domain.unicode_name),
      "  Handle: " + value_or_missing(domain.handle),
      "",
      "Status",
  };
  if (domain.status.empty()) {
    output.overview.push_back("  Not provided");
  } else {
    for (const auto &status : domain.status) {
      output.overview.push_back("  - " + status);
    }
  }

  output.overview.emplace_back();
  output.overview.push_back("Events");
  if (domain.events.empty()) {
    output.overview.push_back("  Not provided");
  } else {
    for (const auto &event : domain.events) {
      auto line = "  " + event.action + ": " + event.date;
      if (event.actor.has_value()) {
        line += " (" + *event.actor + ")";
      }
      output.overview.push_back(std::move(line));
    }
  }

  output.overview.emplace_back();
  output.overview.push_back("Registration");
  const auto *registrar = find_registrar(domain.entities);
  output.overview.push_back("  Registrar: " + std::string(registrar == nullptr
                                                              ? "Not provided"
                                                              : entity_name(*registrar)));
  output.overview.push_back("  WHOIS server: " + value_or_missing(domain.port43));
  output.overview.push_back(
      "  RDAP conformance: " +
      std::string(domain.conformance.empty() ? "Not provided" : join(domain.conformance, ", ")));
  if (result.redacted || result.truncated) {
    output.overview.emplace_back();
    output.overview.push_back("Disclosure warning");
    if (result.redacted) {
      output.overview.push_back("  Some registration data was redacted by the service.");
    }
    if (result.truncated) {
      output.overview.push_back("  The service reported a truncated response.");
    }
  }
  if (!result.warnings.empty()) {
    output.overview.emplace_back();
    output.overview.push_back("Projection warnings: " + std::to_string(result.warnings.size()) +
                              " (see Notices)");
  }

  if (domain.entities.empty()) {
    output.contacts = {"Contacts", "  Not provided"};
  } else {
    output.contacts = {"Contacts", ""};
    for (const auto &entity : domain.entities) {
      append_entity(output.contacts, entity, 0U, result.redacted);
    }
  }

  output.dns = {"Nameservers"};
  if (domain.nameservers.empty()) {
    output.dns.push_back("  Not provided");
  } else {
    for (const auto &nameserver : domain.nameservers) {
      output.dns.push_back("  " + (nameserver.ldh_name.has_value()
                                       ? *nameserver.ldh_name
                                       : value_or_missing(nameserver.unicode_name)));
      for (const auto &address : nameserver.ipv4_addresses) {
        output.dns.push_back("    IPv4: " + address);
      }
      for (const auto &address : nameserver.ipv6_addresses) {
        output.dns.push_back("    IPv6: " + address);
      }
    }
  }

  output.dns.emplace_back();
  output.dns.push_back("DNSSEC");
  if (!domain.secure_dns.has_value()) {
    output.dns.push_back("  Not provided");
  } else {
    const auto &dns = *domain.secure_dns;
    output.dns.push_back("  Zone signed: " + yes_no_missing(dns.zone_signed));
    output.dns.push_back("  Delegation signed: " + yes_no_missing(dns.delegation_signed));
    output.dns.push_back("  Maximum signature life: " +
                         (dns.maximum_signature_life.has_value()
                              ? std::to_string(*dns.maximum_signature_life) + " seconds"
                              : "Not provided"));
    if (dns.ds_data.empty()) {
      output.dns.push_back("  DS records: Not provided");
    } else {
      output.dns.push_back("  DS records");
      for (const auto &record : dns.ds_data) {
        output.dns.push_back(
            "    keyTag=" +
            (record.key_tag.has_value() ? std::to_string(*record.key_tag) : "Not provided") +
            " algorithm=" +
            (record.algorithm.has_value() ? std::to_string(*record.algorithm) : "Not provided") +
            " digestType=" +
            (record.digest_type.has_value() ? std::to_string(*record.digest_type)
                                            : "Not provided"));
        output.dns.push_back("      digest: " + value_or_missing(record.digest));
      }
    }
  }

  output.notices = {"Notices and remarks", ""};
  if (result.redacted) {
    output.notices.push_back("! Data redaction was reported by the RDAP service.");
  }
  if (result.truncated) {
    output.notices.push_back("! Response truncation was reported by the RDAP service.");
  }
  if (result.redacted || result.truncated) {
    output.notices.emplace_back();
  }
  append_text_blocks(output.notices, "Service notices", domain.notices);
  append_text_blocks(output.notices, "Domain remarks", domain.remarks);
  for (const auto &entity : domain.entities) {
    append_scoped_remarks(output.notices, entity, "Entity");
  }
  for (const auto &nameserver : domain.nameservers) {
    append_text_blocks(output.notices,
                       "Nameserver " + value_or_missing(nameserver.ldh_name) + " remarks",
                       nameserver.remarks);
  }
  if (!domain.links.empty()) {
    output.notices.push_back("Domain links");
    append_links(output.notices, domain.links, "  ");
    output.notices.emplace_back();
  }
  if (!result.warnings.empty()) {
    output.notices.push_back("Projection warnings");
    for (const auto &warning : result.warnings) {
      output.notices.push_back("  - " + warning.path + ": " + warning.message);
    }
  }
  if (output.notices.size() == 2U) {
    output.notices.push_back("  None");
  }

  return output;
}

} // namespace rdap
