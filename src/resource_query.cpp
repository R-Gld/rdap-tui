// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/resource_query.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace rdap {
namespace {

Error invalid(std::string message) {
  return Error{ErrorCode::invalid_input, std::move(message), {}, std::nullopt, std::nullopt};
}

std::string_view trim(std::string_view input) {
  while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())) != 0) {
    input.remove_prefix(1U);
  }
  while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())) != 0) {
    input.remove_suffix(1U);
  }
  return input;
}

bool digits_only(std::string_view input) {
  return !input.empty() && std::all_of(input.begin(), input.end(), [](unsigned char value) {
    return std::isdigit(value) != 0;
  });
}

bool ipv4_shape(std::string_view input) {
  return !input.empty() && std::all_of(input.begin(), input.end(), [](unsigned char value) {
    return std::isdigit(value) != 0 || value == '.';
  });
}

bool valid_ipv4_text(std::string_view input) {
  std::size_t start = 0U;
  std::size_t count = 0U;
  while (start <= input.size()) {
    const auto end = input.find('.', start);
    const auto stop = end == std::string_view::npos ? input.size() : end;
    const auto part = input.substr(start, stop - start);
    if (part.empty() || (part.size() > 1U && part.front() == '0')) {
      return false;
    }
    unsigned int value{};
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || value > 255U) {
      return false;
    }
    ++count;
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1U;
  }
  return count == 4U;
}

std::string format_ipv4(const std::array<std::uint8_t, 16> &bytes) {
  return std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
         std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
}

std::string format_ipv6(const std::array<std::uint8_t, 16> &bytes) {
  std::array<std::uint16_t, 8> groups{};
  for (std::size_t index = 0U; index < groups.size(); ++index) {
    groups[index] = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[index * 2U]) << 8U) | bytes[index * 2U + 1U]);
  }

  std::size_t best_start = groups.size();
  std::size_t best_length = 0U;
  for (std::size_t index = 0U; index < groups.size();) {
    if (groups[index] != 0U) {
      ++index;
      continue;
    }
    const auto start = index;
    while (index < groups.size() && groups[index] == 0U) {
      ++index;
    }
    const auto length = index - start;
    if (length >= 2U && length > best_length) {
      best_start = start;
      best_length = length;
    }
  }

  std::ostringstream output;
  for (std::size_t index = 0U; index < groups.size();) {
    if (index == best_start) {
      output << "::";
      index += best_length;
      continue;
    }
    if (index != 0U && index != best_start + best_length) {
      output << ':';
    }
    output << std::hex << std::nouppercase << groups[index];
    ++index;
  }
  return output.str();
}

std::string format_address(IpFamily family, const std::array<std::uint8_t, 16> &bytes) {
  return family == IpFamily::v4 ? format_ipv4(bytes) : format_ipv6(bytes);
}

void apply_prefix(std::array<std::uint8_t, 16> &bytes, std::uint8_t prefix,
                  std::size_t byte_count) {
  for (std::size_t index = 0U; index < byte_count; ++index) {
    const auto bit_start = index * 8U;
    if (bit_start >= prefix) {
      bytes[index] = 0U;
      continue;
    }
    const auto remaining = static_cast<unsigned int>(prefix) - static_cast<unsigned int>(bit_start);
    if (remaining < 8U) {
      bytes[index] &= static_cast<std::uint8_t>(0xFFU << (8U - remaining));
    }
  }
}

} // namespace

Result<IpNetworkQuery> IpNetworkQuery::parse(std::string_view input) {
  input = trim(input);
  if (input.empty()) {
    return invalid("Enter an IP address or CIDR prefix.");
  }
  if (input.find('%') != std::string_view::npos) {
    return invalid("IPv6 zone identifiers are not valid RDAP queries.");
  }

  const auto slash = input.find('/');
  if (slash != std::string_view::npos && input.find('/', slash + 1U) != std::string_view::npos) {
    return invalid("The CIDR prefix contains more than one '/'.");
  }
  const auto address_text = input.substr(0U, slash);
  const auto prefix_text =
      slash == std::string_view::npos ? std::string_view{} : input.substr(slash + 1U);

  const auto family =
      address_text.find(':') == std::string_view::npos ? IpFamily::v4 : IpFamily::v6;
  const auto maximum_prefix = family == IpFamily::v4 ? 32U : 128U;
  std::optional<std::uint8_t> prefix;
  if (slash != std::string_view::npos) {
    unsigned int parsed_prefix{};
    const auto parsed =
        std::from_chars(prefix_text.data(), prefix_text.data() + prefix_text.size(), parsed_prefix);
    if (prefix_text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != prefix_text.data() + prefix_text.size() || parsed_prefix > maximum_prefix) {
      return invalid("The CIDR prefix length is invalid for this address family.");
    }
    prefix = static_cast<std::uint8_t>(parsed_prefix);
  }

  std::array<std::uint8_t, 16> bytes{};
  if (family == IpFamily::v4) {
    if (!valid_ipv4_text(address_text) ||
        inet_pton(AF_INET, std::string(address_text).c_str(), bytes.data()) != 1) {
      return invalid("Enter an IPv4 address using four decimal octets from 0 to 255.");
    }
  } else if (inet_pton(AF_INET6, std::string(address_text).c_str(), bytes.data()) != 1) {
    return invalid("Enter a valid IPv6 address.");
  }

  if (prefix.has_value()) {
    apply_prefix(bytes, *prefix, family == IpFamily::v4 ? 4U : 16U);
  }
  auto canonical = format_address(family, bytes);
  if (prefix.has_value()) {
    canonical += "/" + std::to_string(*prefix);
  }
  return IpNetworkQuery(family, bytes, prefix, std::move(canonical));
}

Result<AutnumQuery> AutnumQuery::parse(std::string_view input) {
  input = trim(input);
  if (input.size() >= 2U && (input[0] == 'a' || input[0] == 'A') &&
      (input[1] == 's' || input[1] == 'S')) {
    input.remove_prefix(2U);
  }
  if (!digits_only(input)) {
    return invalid("Enter an ASN in asplain form, for example AS13335 or 13335.");
  }
  std::uint64_t number{};
  const auto parsed = std::from_chars(input.data(), input.data() + input.size(), number);
  if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() ||
      number > std::numeric_limits<std::uint32_t>::max()) {
    return invalid("The ASN must be an unsigned 32-bit number.");
  }
  return AutnumQuery(static_cast<std::uint32_t>(number));
}

std::string AutnumQuery::canonical() const { return "AS" + std::to_string(number_); }

Result<ResourceQuery> ResourceQueryParser::parse(std::string_view input) {
  input = trim(input);
  if (input.empty()) {
    return invalid("Enter a domain, IP address, CIDR prefix, or ASN.");
  }
  const bool starts_as = input.size() >= 2U && (input[0] == 'a' || input[0] == 'A') &&
                         (input[1] == 's' || input[1] == 'S');
  const bool as_prefix =
      starts_as && (input.size() == 2U || std::isdigit(static_cast<unsigned char>(input[2])) != 0);
  if (as_prefix || digits_only(input)) {
    auto result = AutnumQuery::parse(input);
    if (const auto *error = std::get_if<Error>(&result)) {
      return *error;
    }
    return ResourceQuery{std::get<AutnumQuery>(std::move(result))};
  }
  if (input.find(':') != std::string_view::npos || input.find('/') != std::string_view::npos ||
      ipv4_shape(input)) {
    auto result = IpNetworkQuery::parse(input);
    if (const auto *error = std::get_if<Error>(&result)) {
      return *error;
    }
    return ResourceQuery{std::get<IpNetworkQuery>(std::move(result))};
  }
  auto result = DomainName::parse(input);
  if (const auto *error = std::get_if<Error>(&result)) {
    return *error;
  }
  return ResourceQuery{std::get<DomainName>(std::move(result))};
}

std::string query_text(const ResourceQuery &query) {
  return std::visit(
      [](const auto &value) -> std::string {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, DomainName>) {
          return std::string(value.ascii());
        } else if constexpr (std::is_same_v<Value, IpNetworkQuery>) {
          return std::string(value.canonical());
        } else {
          return value.canonical();
        }
      },
      query);
}

} // namespace rdap
