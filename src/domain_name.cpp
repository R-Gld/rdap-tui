// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/domain_name.hpp"

#include <algorithm>
#include <cctype>

namespace rdap {
namespace {

Error invalid(std::string message) {
  return Error{ErrorCode::invalid_input, std::move(message), {}, std::nullopt, std::nullopt};
}

bool is_label_character(unsigned char character) {
  return std::isalnum(character) != 0 || character == '-';
}

} // namespace

Result<DomainName> DomainName::parse(std::string_view input) {
  while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())) != 0) {
    input.remove_prefix(1);
  }
  while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())) != 0) {
    input.remove_suffix(1);
  }

  if (input.empty()) {
    return invalid("Enter a domain name.");
  }

  std::string normalized(input);
  if (normalized.back() == '.') {
    normalized.pop_back();
  }
  if (normalized.empty() || normalized.back() == '.') {
    return invalid("The domain name has an invalid trailing dot.");
  }
  if (normalized.size() > 253U) {
    return invalid("The domain name is longer than 253 characters.");
  }
  if (std::any_of(normalized.begin(), normalized.end(),
                  [](unsigned char value) { return value > 0x7FU; })) {
    return invalid("Unicode domains are not supported yet; use their Punycode A-label.");
  }

  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

  std::size_t label_start = 0;
  while (label_start < normalized.size()) {
    const auto label_end = normalized.find('.', label_start);
    const auto end = label_end == std::string::npos ? normalized.size() : label_end;
    const auto label_size = end - label_start;
    if (label_size == 0U) {
      return invalid("The domain name contains an empty label.");
    }
    if (label_size > 63U) {
      return invalid("A domain label is longer than 63 characters.");
    }
    if (normalized[label_start] == '-' || normalized[end - 1U] == '-') {
      return invalid("A domain label cannot start or end with a hyphen.");
    }
    for (std::size_t index = label_start; index < end; ++index) {
      const auto character = static_cast<unsigned char>(normalized[index]);
      if (!is_label_character(character)) {
        return invalid("The domain name contains an unsupported character.");
      }
    }

    if (label_end == std::string::npos) {
      break;
    }
    label_start = label_end + 1U;
  }

  return DomainName(std::move(normalized));
}

} // namespace rdap
