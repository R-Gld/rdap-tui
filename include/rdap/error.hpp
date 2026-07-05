// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <variant>

namespace rdap {

enum class ErrorCode {
  invalid_input,
  bootstrap,
  transport,
  tls,
  timeout,
  cancelled,
  http,
  response_too_large,
  invalid_json,
};

struct Error {
  ErrorCode code;
  std::string message;
  std::string detail;
  std::optional<long> http_status;
  std::optional<std::string> retry_after;
};

template <class T> using Result = std::variant<T, Error>;

} // namespace rdap
