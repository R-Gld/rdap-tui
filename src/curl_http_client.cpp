// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/http.hpp"

#include <algorithm>
#include <cctype>
#include <curl/curl.h>
#include <limits>
#include <memory>
#include <string_view>

namespace rdap {
namespace {

struct TransferContext {
  std::string body;
  std::map<std::string, std::string, std::less<>> headers;
  std::size_t maximum_body_size{};
  bool body_too_large{false};
  CancellationToken cancellation;
};

std::string trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

std::string lowercase(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

std::size_t write_body(char *data, std::size_t size, std::size_t count, void *userdata) {
  auto &context = *static_cast<TransferContext *>(userdata);
  if (size != 0U && count > std::numeric_limits<std::size_t>::max() / size) {
    context.body_too_large = true;
    return 0U;
  }
  const auto bytes = size * count;
  if (bytes >
      context.maximum_body_size - std::min(context.body.size(), context.maximum_body_size)) {
    context.body_too_large = true;
    return 0U;
  }
  context.body.append(data, bytes);
  return bytes;
}

std::size_t write_header(char *data, std::size_t size, std::size_t count, void *userdata) {
  auto &context = *static_cast<TransferContext *>(userdata);
  const auto bytes = size * count;
  const std::string_view line(data, bytes);
  if (line.starts_with("HTTP/")) {
    context.headers.clear();
    return bytes;
  }
  const auto separator = line.find(':');
  if (separator != std::string_view::npos) {
    context.headers[lowercase(line.substr(0, separator))] = trim(line.substr(separator + 1U));
  }
  return bytes;
}

int progress_callback(void *userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  const auto &context = *static_cast<TransferContext *>(userdata);
  return context.cancellation.stop_requested() ? 1 : 0;
}

Error curl_error(CURLcode code, const TransferContext &context, const char *details) {
  if (context.body_too_large) {
    return Error{ErrorCode::response_too_large, "The server response exceeded the safety limit.",
                 details, std::nullopt, std::nullopt};
  }
  if (context.cancellation.stop_requested() || code == CURLE_ABORTED_BY_CALLBACK) {
    return Error{ErrorCode::cancelled, "The request was cancelled.", details, std::nullopt,
                 std::nullopt};
  }
  if (code == CURLE_OPERATION_TIMEDOUT) {
    return Error{ErrorCode::timeout, "The request timed out.", details, std::nullopt, std::nullopt};
  }
  if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR ||
      code == CURLE_SSL_CERTPROBLEM) {
    return Error{ErrorCode::tls, "The TLS connection could not be verified.", details, std::nullopt,
                 std::nullopt};
  }
  return Error{ErrorCode::transport, "The HTTP request failed.", details, std::nullopt,
               std::nullopt};
}

} // namespace

CurlHttpClient::CurlHttpClient()
    : initialized_(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}

CurlHttpClient::~CurlHttpClient() {
  if (initialized_) {
    curl_global_cleanup();
  }
}

CancellationToken::CancellationToken() : state_(std::make_shared<std::atomic_bool>(false)) {}

bool CancellationToken::stop_requested() const noexcept {
  return state_->load(std::memory_order_relaxed);
}

CancellationSource::CancellationSource() : state_(std::make_shared<std::atomic_bool>(false)) {}

void CancellationSource::request_stop() const noexcept {
  state_->store(true, std::memory_order_relaxed);
}

Result<HttpResponse> CurlHttpClient::get(const HttpRequest &request,
                                         const CancellationToken &cancellation) {
  if (!initialized_) {
    return Error{
        ErrorCode::transport, "libcurl could not be initialized.", {}, std::nullopt, std::nullopt};
  }

  using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
  CurlHandle handle(curl_easy_init(), &curl_easy_cleanup);
  if (!handle) {
    return Error{ErrorCode::transport,
                 "An HTTP handle could not be created.",
                 {},
                 std::nullopt,
                 std::nullopt};
  }

  curl_slist *header_list = nullptr;
  for (const auto &header : request.headers) {
    auto *updated_headers = curl_slist_append(header_list, header.c_str());
    if (updated_headers == nullptr) {
      curl_slist_free_all(header_list);
      return Error{ErrorCode::transport,
                   "HTTP request headers could not be allocated.",
                   {},
                   std::nullopt,
                   std::nullopt};
    }
    header_list = updated_headers;
  }
  using HeaderList = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
  HeaderList headers(header_list, &curl_slist_free_all);

  TransferContext context{{}, {}, request.maximum_body_size, false, cancellation};
  char error_buffer[CURL_ERROR_SIZE] = {};

  curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "rdap-tui/0.0.1");
  curl_easy_setopt(handle.get(), CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_MAXREDIRS, request.maximum_redirects);
  curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(request.connect_timeout.count()));
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS,
                   static_cast<long>(request.total_timeout.count()));
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, &write_body);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &context);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, &write_header);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &context);
  curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, &progress_callback);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &context);
  curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, error_buffer);

  const auto result = curl_easy_perform(handle.get());
  if (result != CURLE_OK) {
    const char *details = error_buffer[0] == '\0' ? curl_easy_strerror(result) : error_buffer;
    return curl_error(result, context, details);
  }

  HttpResponse response;
  curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response.status);
  char *effective_url = nullptr;
  curl_easy_getinfo(handle.get(), CURLINFO_EFFECTIVE_URL, &effective_url);
  if (effective_url != nullptr) {
    response.effective_url = effective_url;
  }
  char *content_type = nullptr;
  curl_easy_getinfo(handle.get(), CURLINFO_CONTENT_TYPE, &content_type);
  if (content_type != nullptr) {
    response.content_type = content_type;
  }
  response.body = std::move(context.body);
  response.headers = std::move(context.headers);
  return response;
}

} // namespace rdap
