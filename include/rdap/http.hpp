// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rdap/error.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rdap {

class CancellationSource;

class CancellationToken {
public:
  CancellationToken();
  [[nodiscard]] bool stop_requested() const noexcept;

private:
  friend class CancellationSource;
  explicit CancellationToken(std::shared_ptr<std::atomic_bool> state) : state_(std::move(state)) {}
  std::shared_ptr<std::atomic_bool> state_;
};

class CancellationSource {
public:
  CancellationSource();
  [[nodiscard]] CancellationToken token() const { return CancellationToken(state_); }
  void request_stop() const noexcept;

private:
  std::shared_ptr<std::atomic_bool> state_;
};

struct HttpRequest {
  std::string url;
  std::vector<std::string> headers;
  std::size_t maximum_body_size{10U * 1024U * 1024U};
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds total_timeout{15000};
  long maximum_redirects{5};
};

struct HttpResponse {
  long status{};
  std::string effective_url;
  std::string content_type;
  std::string body;
  std::map<std::string, std::string, std::less<>> headers;
};

class HttpClient {
public:
  virtual ~HttpClient() = default;
  virtual Result<HttpResponse> get(const HttpRequest &request,
                                   const CancellationToken &cancellation) = 0;
};

class CurlHttpClient final : public HttpClient {
public:
  CurlHttpClient();
  ~CurlHttpClient() override;

  CurlHttpClient(const CurlHttpClient &) = delete;
  CurlHttpClient &operator=(const CurlHttpClient &) = delete;

  Result<HttpResponse> get(const HttpRequest &request,
                           const CancellationToken &cancellation) override;

private:
  bool initialized_{false};
};

} // namespace rdap
