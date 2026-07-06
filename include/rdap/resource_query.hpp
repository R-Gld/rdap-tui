// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rdap/domain_name.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace rdap {

enum class IpFamily { v4, v6 };

class IpNetworkQuery {
public:
  static Result<IpNetworkQuery> parse(std::string_view input);

  [[nodiscard]] IpFamily family() const noexcept { return family_; }
  [[nodiscard]] const std::array<std::uint8_t, 16> &bytes() const noexcept { return bytes_; }
  [[nodiscard]] const std::optional<std::uint8_t> &prefix_length() const noexcept {
    return prefix_length_;
  }
  [[nodiscard]] std::string_view canonical() const noexcept { return canonical_; }

private:
  IpNetworkQuery(IpFamily family, std::array<std::uint8_t, 16> bytes,
                 std::optional<std::uint8_t> prefix_length, std::string canonical)
      : family_(family), bytes_(bytes), prefix_length_(prefix_length),
        canonical_(std::move(canonical)) {}

  IpFamily family_;
  std::array<std::uint8_t, 16> bytes_{};
  std::optional<std::uint8_t> prefix_length_;
  std::string canonical_;
};

class AutnumQuery {
public:
  static Result<AutnumQuery> parse(std::string_view input);

  [[nodiscard]] std::uint32_t number() const noexcept { return number_; }
  [[nodiscard]] std::string canonical() const;

private:
  explicit AutnumQuery(std::uint32_t number) : number_(number) {}
  std::uint32_t number_{};
};

using ResourceQuery = std::variant<DomainName, IpNetworkQuery, AutnumQuery>;

class ResourceQueryParser {
public:
  static Result<ResourceQuery> parse(std::string_view input);
};

[[nodiscard]] std::string query_text(const ResourceQuery &query);

} // namespace rdap
