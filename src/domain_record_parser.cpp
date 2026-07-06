// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/domain_record_parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <nlohmann/json.hpp>
#include <string_view>

namespace rdap {
namespace {

constexpr std::size_t maximum_entity_depth = 8U;
constexpr std::size_t maximum_entities = 256U;
constexpr std::size_t maximum_collection_items = 1024U;

std::string child_path(std::string_view base, std::string_view child) {
  return std::string(base) + "." + std::string(child);
}

std::string item_path(std::string_view base, std::size_t index) {
  return std::string(base) + "[" + std::to_string(index) + "]";
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

class Parser {
public:
  Result<DomainParseResult> parse(const nlohmann::json &document) {
    if (!document.is_object()) {
      return Error{ErrorCode::invalid_json,
                   "The RDAP response cannot be projected because it is not an object.",
                   {},
                   std::nullopt,
                   std::nullopt};
    }

    const auto class_name = string_field(document, "objectClassName", "$");
    if (class_name.has_value() && *class_name != "domain") {
      return Error{ErrorCode::invalid_json, "The RDAP response is not a domain object.",
                   "objectClassName is '" + *class_name + "'", std::nullopt, std::nullopt};
    }
    if (!class_name.has_value()) {
      warn("$.objectClassName", "Missing or invalid domain object class.");
    }

    DomainRecord domain;
    domain.handle = string_field(document, "handle", "$");
    domain.ldh_name = string_field(document, "ldhName", "$");
    domain.unicode_name = string_field(document, "unicodeName", "$");
    domain.port43 = string_field(document, "port43", "$");
    domain.status = string_array(document, "status", "$");
    domain.conformance = string_array(document, "rdapConformance", "$");
    if (!document.contains("rdapConformance") || document["rdapConformance"].is_null()) {
      warn("$.rdapConformance", "The required RDAP conformance declaration is missing.");
    }
    domain.events = events(document, "events", "$");
    domain.links = links(document, "links", "$");
    domain.notices = text_blocks(document, "notices", "$");
    domain.remarks = text_blocks(document, "remarks", "$");
    domain.nameservers = nameservers(document);
    domain.entities = entities(document, "entities", "$", 0U);
    domain.secure_dns = secure_dns(document);

    return DomainParseResult{std::move(domain), std::move(warnings_), redacted_, truncated_};
  }

private:
  void warn(std::string path, std::string message) {
    warnings_.push_back(ParseWarning{std::move(path), std::move(message)});
  }

  const nlohmann::json *field(const nlohmann::json &object, std::string_view name,
                              std::string_view base_path) {
    if (!object.is_object()) {
      warn(std::string(base_path), "Expected an object.");
      return nullptr;
    }
    const auto iterator = object.find(name);
    if (iterator == object.end() || iterator->is_null()) {
      return nullptr;
    }
    return &*iterator;
  }

  std::optional<std::string> string_field(const nlohmann::json &object, std::string_view name,
                                          std::string_view base_path) {
    const auto *value = field(object, name, base_path);
    if (value == nullptr) {
      return std::nullopt;
    }
    if (!value->is_string()) {
      warn(child_path(base_path, name), "Expected a string.");
      return std::nullopt;
    }
    return value->get<std::string>();
  }

  std::optional<bool> bool_field(const nlohmann::json &object, std::string_view name,
                                 std::string_view base_path) {
    const auto *value = field(object, name, base_path);
    if (value == nullptr) {
      return std::nullopt;
    }
    if (!value->is_boolean()) {
      warn(child_path(base_path, name), "Expected a boolean.");
      return std::nullopt;
    }
    return value->get<bool>();
  }

  std::optional<std::uint64_t> unsigned_field(const nlohmann::json &object, std::string_view name,
                                              std::string_view base_path) {
    const auto *value = field(object, name, base_path);
    if (value == nullptr) {
      return std::nullopt;
    }
    if (value->is_number_unsigned()) {
      return value->get<std::uint64_t>();
    }
    if (!value->is_number_integer()) {
      warn(child_path(base_path, name), "Expected a non-negative integer.");
      return std::nullopt;
    }
    const auto number = value->get<std::int64_t>();
    if (number < 0) {
      warn(child_path(base_path, name), "Expected a non-negative integer.");
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(number);
  }

  std::vector<std::string> strings(const nlohmann::json &value, std::string_view path) {
    std::vector<std::string> result;
    if (!value.is_array()) {
      warn(std::string(path), "Expected an array of strings.");
      return result;
    }
    const auto count = limited_size(value, path);
    for (std::size_t index = 0; index < count; ++index) {
      if (value[index].is_string()) {
        result.push_back(value[index].get<std::string>());
      } else if (!value[index].is_null()) {
        warn(item_path(path, index), "Expected a string.");
      }
    }
    return result;
  }

  std::vector<std::string> string_array(const nlohmann::json &object, std::string_view name,
                                        std::string_view base_path) {
    const auto *value = field(object, name, base_path);
    if (value == nullptr) {
      return {};
    }
    return strings(*value, child_path(base_path, name));
  }

  std::size_t limited_size(const nlohmann::json &array, std::string_view path) {
    if (array.size() > maximum_collection_items) {
      warn(std::string(path), "Collection truncated at 1024 items.");
      return maximum_collection_items;
    }
    return array.size();
  }

  std::vector<RdapLink> links(const nlohmann::json &object, std::string_view name,
                              std::string_view base_path) {
    std::vector<RdapLink> result;
    const auto *array = field(object, name, base_path);
    if (array == nullptr) {
      return result;
    }
    const auto path = child_path(base_path, name);
    if (!array->is_array()) {
      warn(path, "Expected an array of links.");
      return result;
    }
    const auto count = limited_size(*array, path);
    for (std::size_t index = 0; index < count; ++index) {
      const auto &item = (*array)[index];
      const auto current_path = item_path(path, index);
      if (!item.is_object()) {
        warn(current_path, "Expected a link object.");
        continue;
      }
      const auto value = string_field(item, "value", current_path);
      const auto relation = string_field(item, "rel", current_path);
      const auto href = string_field(item, "href", current_path);
      if (!value.has_value() || !relation.has_value() || !href.has_value()) {
        warn(current_path, "Link omitted because a required field is missing.");
        continue;
      }
      result.push_back(RdapLink{*value, *relation, *href, string_field(item, "title", current_path),
                                string_field(item, "type", current_path)});
    }
    return result;
  }

  std::vector<RdapEvent> events(const nlohmann::json &object, std::string_view name,
                                std::string_view base_path) {
    std::vector<RdapEvent> result;
    const auto *array = field(object, name, base_path);
    if (array == nullptr) {
      return result;
    }
    const auto path = child_path(base_path, name);
    if (!array->is_array()) {
      warn(path, "Expected an array of events.");
      return result;
    }
    const auto count = limited_size(*array, path);
    for (std::size_t index = 0; index < count; ++index) {
      const auto &item = (*array)[index];
      const auto current_path = item_path(path, index);
      if (!item.is_object()) {
        warn(current_path, "Expected an event object.");
        continue;
      }
      const auto action = string_field(item, "eventAction", current_path);
      const auto date = string_field(item, "eventDate", current_path);
      if (!action.has_value() || !date.has_value()) {
        warn(current_path, "Event omitted because a required field is missing.");
        continue;
      }
      result.push_back(RdapEvent{*action, *date, string_field(item, "eventActor", current_path),
                                 links(item, "links", current_path)});
    }
    return result;
  }

  void inspect_disclosure(const std::optional<std::string> &type) {
    if (!type.has_value()) {
      return;
    }
    const auto normalized = lowercase(*type);
    redacted_ = redacted_ || normalized.find("redacted") != std::string::npos;
    truncated_ = truncated_ || normalized.find("truncated") != std::string::npos;
  }

  std::vector<RdapTextBlock> text_blocks(const nlohmann::json &object, std::string_view name,
                                         std::string_view base_path) {
    std::vector<RdapTextBlock> result;
    const auto *array = field(object, name, base_path);
    if (array == nullptr) {
      return result;
    }
    const auto path = child_path(base_path, name);
    if (!array->is_array()) {
      warn(path, "Expected an array of notices or remarks.");
      return result;
    }
    const auto count = limited_size(*array, path);
    for (std::size_t index = 0; index < count; ++index) {
      const auto &item = (*array)[index];
      const auto current_path = item_path(path, index);
      if (!item.is_object()) {
        warn(current_path, "Expected a notice or remark object.");
        continue;
      }
      auto type = string_field(item, "type", current_path);
      inspect_disclosure(type);
      result.push_back(RdapTextBlock{string_field(item, "title", current_path), std::move(type),
                                     string_array(item, "description", current_path),
                                     links(item, "links", current_path)});
    }
    return result;
  }

  std::vector<PublicId> public_ids(const nlohmann::json &object, std::string_view base_path) {
    std::vector<PublicId> result;
    const auto *array = field(object, "publicIds", base_path);
    if (array == nullptr) {
      return result;
    }
    const auto path = child_path(base_path, "publicIds");
    if (!array->is_array()) {
      warn(path, "Expected an array of public identifiers.");
      return result;
    }
    const auto count = limited_size(*array, path);
    for (std::size_t index = 0; index < count; ++index) {
      const auto &item = (*array)[index];
      const auto current_path = item_path(path, index);
      if (!item.is_object()) {
        warn(current_path, "Expected a public identifier object.");
        continue;
      }
      const auto type = string_field(item, "type", current_path);
      const auto identifier = string_field(item, "identifier", current_path);
      if (type.has_value() && identifier.has_value()) {
        result.push_back(PublicId{*type, *identifier});
      }
    }
    return result;
  }

  std::vector<std::string> parameter_types(const nlohmann::json &parameters,
                                           std::string_view path) {
    if (!parameters.is_object()) {
      warn(std::string(path), "Expected a jCard parameter object.");
      return {};
    }
    const auto iterator = parameters.find("type");
    if (iterator == parameters.end() || iterator->is_null()) {
      return {};
    }
    if (iterator->is_string()) {
      return {iterator->get<std::string>()};
    }
    return strings(*iterator, child_path(path, "type"));
  }

  std::optional<unsigned int> parameter_preference(const nlohmann::json &parameters,
                                                   std::string_view path) {
    if (!parameters.is_object()) {
      return std::nullopt;
    }
    const auto iterator = parameters.find("pref");
    if (iterator == parameters.end() || iterator->is_null()) {
      return std::nullopt;
    }
    if (iterator->is_number_unsigned()) {
      const auto value = iterator->get<std::uint64_t>();
      if (value <= std::numeric_limits<unsigned int>::max()) {
        return static_cast<unsigned int>(value);
      }
    }
    if (iterator->is_string()) {
      unsigned int result{};
      const auto text = iterator->get<std::string>();
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
      if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) {
        return result;
      }
    }
    warn(child_path(path, "pref"), "Expected an unsigned preference value.");
    return std::nullopt;
  }

  void flatten_text(const nlohmann::json &value, std::vector<std::string> &output,
                    std::string_view path) {
    if (value.is_string()) {
      const auto text = value.get<std::string>();
      if (!text.empty()) {
        output.push_back(text);
      }
      return;
    }
    if (value.is_array()) {
      const auto count = limited_size(value, path);
      for (std::size_t index = 0; index < count; ++index) {
        flatten_text(value[index], output, item_path(path, index));
      }
      return;
    }
    if (!value.is_null()) {
      warn(std::string(path), "Expected text or structured text.");
    }
  }

  std::optional<JCardContact> jcard(const nlohmann::json &entity, std::string_view base_path) {
    const auto *card = field(entity, "vcardArray", base_path);
    if (card == nullptr) {
      return std::nullopt;
    }
    const auto path = child_path(base_path, "vcardArray");
    if (!card->is_array() || card->size() != 2U || !(*card)[0].is_string() ||
        (*card)[0] != "vcard" || !(*card)[1].is_array()) {
      warn(path, "Expected a jCard [\"vcard\", properties] array.");
      return std::nullopt;
    }

    JCardContact contact;
    const auto &properties = (*card)[1];
    const auto count = limited_size(properties, child_path(path, "properties"));
    for (std::size_t index = 0; index < count; ++index) {
      const auto &property = properties[index];
      const auto property_path = item_path(child_path(path, "properties"), index);
      if (!property.is_array() || property.size() < 4U || !property[0].is_string()) {
        warn(property_path, "Expected a jCard property array.");
        continue;
      }
      const auto name = lowercase(property[0].get<std::string>());
      const auto &parameters = property[1];
      std::vector<std::string> values;
      const auto value_count = limited_size(property, property_path);
      for (std::size_t value_index = 3U; value_index < value_count; ++value_index) {
        flatten_text(property[value_index], values, item_path(property_path, value_index));
      }

      if (name == "fn" && !values.empty()) {
        contact.full_name = values.front();
      } else if (name == "org") {
        contact.organizations.insert(contact.organizations.end(), values.begin(), values.end());
      } else if (name == "title") {
        contact.titles.insert(contact.titles.end(), values.begin(), values.end());
      } else if (name == "role") {
        contact.roles.insert(contact.roles.end(), values.begin(), values.end());
      } else if (name == "email" || name == "tel") {
        const auto types = parameter_types(parameters, child_path(property_path, "parameters"));
        const auto preference =
            parameter_preference(parameters, child_path(property_path, "parameters"));
        auto &target = name == "email" ? contact.emails : contact.phones;
        for (auto &value : values) {
          target.push_back(LabeledValue{std::move(value), types, preference});
        }
      } else if (name == "adr") {
        PostalAddress address;
        address.components = std::move(values);
        address.types = parameter_types(parameters, child_path(property_path, "parameters"));
        address.preference =
            parameter_preference(parameters, child_path(property_path, "parameters"));
        if (parameters.is_object()) {
          const auto label = parameters.find("label");
          if (label != parameters.end() && label->is_string()) {
            address.label = label->get<std::string>();
          }
        }
        if (!address.components.empty() || address.label.has_value()) {
          contact.addresses.push_back(std::move(address));
        }
      }
    }
    return contact;
  }

  RdapEntity entity(const nlohmann::json &item, std::string_view path, std::size_t depth) {
    RdapEntity result;
    result.handle = string_field(item, "handle", path);
    result.roles = string_array(item, "roles", path);
    result.public_ids = public_ids(item, path);
    result.contact = jcard(item, path);
    result.status = string_array(item, "status", path);
    result.events = events(item, "events", path);
    result.remarks = text_blocks(item, "remarks", path);
    result.links = links(item, "links", path);
    result.entities = entities(item, "entities", path, depth + 1U);
    return result;
  }

  std::vector<RdapEntity> entities(const nlohmann::json &object, std::string_view name,
                                   std::string_view base_path, std::size_t depth) {
    std::vector<RdapEntity> result;
    const auto *array = field(object, name, base_path);
    if (array == nullptr) {
      return result;
    }
    const auto path = child_path(base_path, name);
    if (!array->is_array()) {
      warn(path, "Expected an array of entities.");
      return result;
    }
    if (depth >= maximum_entity_depth) {
      warn(path, "Entity hierarchy truncated at eight levels.");
      return result;
    }
    const auto count = limited_size(*array, path);
    for (std::size_t index = 0; index < count; ++index) {
      if (entity_count_ >= maximum_entities) {
        warn(path, "Entity hierarchy truncated at 256 entities.");
        break;
      }
      const auto &item = (*array)[index];
      const auto current_path = item_path(path, index);
      if (!item.is_object()) {
        warn(current_path, "Expected an entity object.");
        continue;
      }
      ++entity_count_;
      result.push_back(entity(item, current_path, depth));
    }
    return result;
  }

  std::vector<RdapNameserver> nameservers(const nlohmann::json &document) {
    std::vector<RdapNameserver> result;
    const auto *array = field(document, "nameservers", "$");
    if (array == nullptr) {
      return result;
    }
    constexpr std::string_view path = "$.nameservers";
    if (!array->is_array()) {
      warn(std::string(path), "Expected an array of nameservers.");
      return result;
    }
    const auto count = limited_size(*array, path);
    for (std::size_t index = 0; index < count; ++index) {
      const auto &item = (*array)[index];
      const auto current_path = item_path(path, index);
      if (!item.is_object()) {
        warn(current_path, "Expected a nameserver object.");
        continue;
      }
      RdapNameserver nameserver;
      nameserver.handle = string_field(item, "handle", current_path);
      nameserver.ldh_name = string_field(item, "ldhName", current_path);
      nameserver.unicode_name = string_field(item, "unicodeName", current_path);
      nameserver.status = string_array(item, "status", current_path);
      nameserver.events = events(item, "events", current_path);
      nameserver.remarks = text_blocks(item, "remarks", current_path);
      nameserver.links = links(item, "links", current_path);
      if (const auto *addresses = field(item, "ipAddresses", current_path); addresses != nullptr) {
        if (!addresses->is_object()) {
          warn(child_path(current_path, "ipAddresses"), "Expected an address object.");
        } else {
          nameserver.ipv4_addresses =
              string_array(*addresses, "v4", child_path(current_path, "ipAddresses"));
          nameserver.ipv6_addresses =
              string_array(*addresses, "v6", child_path(current_path, "ipAddresses"));
        }
      }
      result.push_back(std::move(nameserver));
    }
    return result;
  }

  std::optional<SecureDns> secure_dns(const nlohmann::json &document) {
    const auto *value = field(document, "secureDNS", "$");
    if (value == nullptr) {
      return std::nullopt;
    }
    constexpr std::string_view path = "$.secureDNS";
    if (!value->is_object()) {
      warn(std::string(path), "Expected a secure DNS object.");
      return std::nullopt;
    }
    SecureDns result;
    result.zone_signed = bool_field(*value, "zoneSigned", path);
    result.delegation_signed = bool_field(*value, "delegationSigned", path);
    result.maximum_signature_life = unsigned_field(*value, "maxSigLife", path);
    if (const auto *array = field(*value, "dsData", path); array != nullptr) {
      if (!array->is_array()) {
        warn("$.secureDNS.dsData", "Expected an array of DS records.");
      } else {
        const auto count = limited_size(*array, "$.secureDNS.dsData");
        for (std::size_t index = 0; index < count; ++index) {
          const auto &item = (*array)[index];
          const auto current_path = item_path("$.secureDNS.dsData", index);
          if (!item.is_object()) {
            warn(current_path, "Expected a DS record object.");
            continue;
          }
          result.ds_data.push_back(DsData{unsigned_field(item, "keyTag", current_path),
                                          unsigned_field(item, "algorithm", current_path),
                                          unsigned_field(item, "digestType", current_path),
                                          string_field(item, "digest", current_path)});
        }
      }
    }
    return result;
  }

  std::vector<ParseWarning> warnings_;
  std::size_t entity_count_{0U};
  bool redacted_{false};
  bool truncated_{false};
};

} // namespace

Result<DomainParseResult> DomainRecordParser::parse(const nlohmann::json &document) {
  return Parser().parse(document);
}

} // namespace rdap
