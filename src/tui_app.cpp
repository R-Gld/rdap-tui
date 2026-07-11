// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/tui_app.hpp"

#include "rdap/bootstrap_cache.hpp"
#include "rdap/domain_name.hpp"
#include "rdap/domain_record_parser.hpp"
#include "rdap/domain_view_formatter.hpp"
#include "rdap/http.hpp"
#include "rdap/network_record_parser.hpp"
#include "rdap/network_view_formatter.hpp"
#include "rdap/rdap_client.hpp"
#include "rdap/resource_query.hpp"

#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace rdap {
namespace {

using namespace ftxui;

enum class ViewState { idle, loading_bootstrap, querying, success, error };

struct CompletedLookup {
  RdapResponse response;
  std::variant<DomainParseResult, NetworkParseResult, AutnumParseResult, Error> projection;
};

using ViewLookupResult = Result<CompletedLookup>;

std::vector<std::string> lines(std::string_view text) {
  std::vector<std::string> result;
  std::istringstream stream{std::string(text)};
  for (std::string line; std::getline(stream, line);) {
    result.push_back(std::move(line));
  }
  if (result.empty()) {
    result.emplace_back();
  }
  return result;
}

std::string state_label(ViewState state) {
  switch (state) {
  case ViewState::idle:
    return "Ready";
  case ViewState::loading_bootstrap:
    return "Loading IANA bootstrap...";
  case ViewState::querying:
    return "Querying registry...";
  case ViewState::success:
    return "Request completed";
  case ViewState::error:
    return "Request failed";
  }
  return {};
}

Color state_color(ViewState state) {
  switch (state) {
  case ViewState::success:
    return Color::Green;
  case ViewState::error:
    return Color::Red;
  case ViewState::loading_bootstrap:
  case ViewState::querying:
    return Color::Yellow;
  case ViewState::idle:
    return Color::Cyan;
  }
  return Color::Default;
}

class TuiApplication {
public:
  explicit TuiApplication(BootstrapCache *disk_cache)
      : screen_(ScreenInteractive::Fullscreen()), client_(http_client_, disk_cache),
        input_(Input(&query_input_, "example.com, 1.1.1.1, or AS13335")),
        search_(Button("Search", [this] { start_lookup(); })),
        section_toggle_(Toggle(&section_names_, &section_index_)),
        overview_menu_(Menu(&overview_lines_, &overview_index_)),
        contacts_menu_(Menu(&contacts_lines_, &contacts_index_)),
        dns_menu_(Menu(&dns_lines_, &dns_index_)),
        notices_menu_(Menu(&notices_lines_, &notices_index_)),
        json_toggle_(Toggle(&json_view_names_, &json_view_index_)),
        pretty_menu_(Menu(&pretty_lines_, &pretty_index_)),
        raw_menu_(Menu(&raw_lines_, &raw_index_)),
        json_tabs_(Container::Tab({pretty_menu_, raw_menu_}, &json_view_index_)),
        json_panel_(Renderer(Container::Vertical({json_toggle_, json_tabs_}),
                             [this] {
                               return vbox({hbox({text(" JSON  "), json_toggle_->Render()}),
                                            json_tabs_->Render() | flex});
                             })),
        result_tabs_(
            Container::Tab({overview_menu_, contacts_menu_, dns_menu_, notices_menu_, json_panel_},
                           &section_index_)) {
    auto controls = Container::Horizontal({input_, search_});
    auto interactive = Container::Vertical({controls, section_toggle_, result_tabs_});

    root_ = Renderer(interactive, [this] {
      const bool busy = state_ == ViewState::loading_bootstrap || state_ == ViewState::querying;
      auto status = text(state_label(state_)) | color(state_color(state_));
      if (busy) {
        status = hbox({text("● "), status});
      }

      Elements metadata;
      if (!metadata_.empty()) {
        metadata.push_back(text(metadata_) | dim);
      }

      return vbox({
                 text("rdap-tui") | bold | center,
                 separator(),
                 hbox({text(" Query  "), input_->Render() | flex, text(" "), search_->Render()}),
                 hbox({text(" Status  "), status}),
                 vbox(std::move(metadata)),
                 separator(),
                 hbox({text(" View  "), section_toggle_->Render()}),
                 result_tabs_->Render() | vscroll_indicator | frame | flex,
                 separator(),
                 text("Enter: search  1-5: views  Tab: focus  ↑/↓/PgUp/PgDn: scroll  "
                      "Ctrl+C/Esc: quit") |
                     dim,
             }) |
             border;
    });

    root_ = CatchEvent(root_, [this](const Event &event) {
      if (event == Event::Custom) {
        consume_worker_update();
        return true;
      }
      if (event == Event::CtrlC || event == Event::Escape) {
        screen_.ExitLoopClosure()();
        return true;
      }
      if (event == Event::Return && input_->Focused()) {
        start_lookup();
        return true;
      }
      if (!input_->Focused() && event.is_character()) {
        const auto character = event.character();
        if (character.size() == 1U && character[0] >= '1' && character[0] <= '5') {
          section_index_ = static_cast<int>(character[0] - '1');
          return true;
        }
      }
      return false;
    });
  }

  ~TuiApplication() {
    if (worker_.joinable()) {
      cancellation_.request_stop();
      worker_.join();
    }
  }

  int run() {
    screen_.Loop(root_);
    return 0;
  }

private:
  void start_lookup() {
    if (state_ == ViewState::loading_bootstrap || state_ == ViewState::querying) {
      return;
    }

    auto query_result = ResourceQueryParser::parse(query_input_);
    if (const auto *error = std::get_if<Error>(&query_result)) {
      show_error(*error);
      return;
    }
    auto query = std::get<ResourceQuery>(std::move(query_result));

    if (worker_.joinable()) {
      worker_.join();
    }
    state_ = ViewState::loading_bootstrap;
    metadata_.clear();
    section_names_[2] = detail_view_name(query);
    section_index_ = 0;
    json_view_index_ = 0;
    overview_lines_ = {"Waiting for the RDAP response..."};
    contacts_lines_ = overview_lines_;
    dns_lines_ = overview_lines_;
    notices_lines_ = overview_lines_;
    pretty_lines_ = {"Waiting for the RDAP response..."};
    raw_lines_ = pretty_lines_;
    reset_scroll();

    cancellation_ = CancellationSource();
    const auto cancellation = cancellation_.token();
    worker_ = std::thread([this, query = std::move(query), cancellation] {
      auto progress = [this](LookupStage stage) {
        {
          std::scoped_lock lock(pending_mutex_);
          pending_stage_ = stage;
        }
        screen_.PostEvent(Event::Custom);
      };
      auto result = client_.lookup(query, cancellation, progress);
      auto view_result = [&result]() -> ViewLookupResult {
        if (auto *response = std::get_if<RdapResponse>(&result)) {
          auto projection = std::visit(
              [&](const auto &value)
                  -> std::variant<DomainParseResult, NetworkParseResult, AutnumParseResult, Error> {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, DomainName>) {
                  auto parsed = DomainRecordParser::parse(response->document);
                  if (const auto *error = std::get_if<Error>(&parsed)) {
                    return *error;
                  }
                  return std::get<DomainParseResult>(std::move(parsed));
                } else if constexpr (std::is_same_v<Value, IpNetworkQuery>) {
                  auto parsed = NetworkRecordParser::parse(response->document);
                  if (const auto *error = std::get_if<Error>(&parsed)) {
                    return *error;
                  }
                  return std::get<NetworkParseResult>(std::move(parsed));
                } else {
                  auto parsed = AutnumRecordParser::parse(response->document);
                  if (const auto *error = std::get_if<Error>(&parsed)) {
                    return *error;
                  }
                  return std::get<AutnumParseResult>(std::move(parsed));
                }
              },
              response->query);
          return CompletedLookup{std::move(*response), std::move(projection)};
        }
        return std::get<Error>(std::move(result));
      }();
      {
        std::scoped_lock lock(pending_mutex_);
        pending_result_ = std::move(view_result);
      }
      screen_.PostEvent(Event::Custom);
    });
  }

  void consume_worker_update() {
    std::optional<LookupStage> stage;
    std::optional<ViewLookupResult> result;
    {
      std::scoped_lock lock(pending_mutex_);
      stage = std::exchange(pending_stage_, std::nullopt);
      result = std::move(pending_result_);
      pending_result_.reset();
    }

    if (stage.has_value()) {
      state_ = *stage == LookupStage::loading_bootstrap ? ViewState::loading_bootstrap
                                                        : ViewState::querying;
    }
    if (!result.has_value()) {
      return;
    }
    if (const auto *error = std::get_if<Error>(&*result)) {
      show_error(*error);
      return;
    }

    auto completed = std::get<CompletedLookup>(std::move(*result));
    auto &response = completed.response;
    state_ = ViewState::success;
    metadata_ = query_text(response.query) + "  HTTP " + std::to_string(response.http.status) +
                "  " + response.http.effective_url;
    pretty_lines_ = lines(response.document.dump(2));
    raw_lines_ = lines(response.http.body);
    if (const auto *projection_error = std::get_if<Error>(&completed.projection)) {
      overview_lines_ = {
          "Structured view unavailable", "", projection_error->message,
          projection_error->detail,      "", "Use the JSON view to inspect the response."};
      contacts_lines_ = overview_lines_;
      dns_lines_ = overview_lines_;
      notices_lines_ = overview_lines_;
      section_index_ = 4;
      metadata_ += "  Structured view unavailable";
    } else {
      auto formatted = std::visit(
          [](const auto &projection) -> DomainViewLines {
            using Projection = std::decay_t<decltype(projection)>;
            if constexpr (std::is_same_v<Projection, DomainParseResult>) {
              return DomainViewFormatter::format(projection);
            } else if constexpr (std::is_same_v<Projection, NetworkParseResult>) {
              return NetworkViewFormatter::format(projection);
            } else if constexpr (std::is_same_v<Projection, AutnumParseResult>) {
              return AutnumViewFormatter::format(projection);
            } else {
              return {};
            }
          },
          completed.projection);
      overview_lines_ = std::move(formatted.overview);
      contacts_lines_ = std::move(formatted.contacts);
      dns_lines_ = std::move(formatted.dns);
      notices_lines_ = std::move(formatted.notices);
      section_index_ = 0;
    }
    reset_scroll();
  }

  void show_error(const Error &error) {
    state_ = ViewState::error;
    metadata_.clear();
    std::ostringstream message;
    message << error.message;
    if (error.http_status.has_value()) {
      message << "\nHTTP status: " << *error.http_status;
    }
    if (error.retry_after.has_value()) {
      message << "\nRetry-After: " << *error.retry_after;
    }
    if (!error.detail.empty()) {
      constexpr std::size_t maximum_detail = 64U * 1024U;
      message << "\n\n" << error.detail.substr(0U, std::min(error.detail.size(), maximum_detail));
    }
    overview_lines_ = lines(message.str());
    contacts_lines_ = overview_lines_;
    dns_lines_ = overview_lines_;
    notices_lines_ = overview_lines_;
    pretty_lines_ = overview_lines_;
    raw_lines_ = overview_lines_;
    section_index_ = 0;
    reset_scroll();
  }

  void reset_scroll() {
    overview_index_ = 0;
    contacts_index_ = 0;
    dns_index_ = 0;
    notices_index_ = 0;
    pretty_index_ = 0;
    raw_index_ = 0;
  }

  static std::string detail_view_name(const ResourceQuery &query) {
    return std::visit(
        [](const auto &value) -> std::string {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, DomainName>) {
            return "DNS";
          } else if constexpr (std::is_same_v<Value, IpNetworkQuery>) {
            return "Network";
          } else {
            return "Autnum";
          }
        },
        query);
  }

  ScreenInteractive screen_;
  CurlHttpClient http_client_;
  RdapClient client_;
  std::thread worker_;
  CancellationSource cancellation_;

  std::mutex pending_mutex_;
  std::optional<LookupStage> pending_stage_;
  std::optional<ViewLookupResult> pending_result_;

  ViewState state_{ViewState::idle};
  std::string query_input_;
  std::string metadata_;
  std::vector<std::string> section_names_{"Overview", "Contacts", "DNS", "Notices", "JSON"};
  int section_index_{0};
  std::vector<std::string> overview_lines_{"Enter a domain name to start."};
  std::vector<std::string> contacts_lines_{"Enter a domain name to start."};
  std::vector<std::string> dns_lines_{"Enter a domain name to start."};
  std::vector<std::string> notices_lines_{"Enter a domain name to start."};
  int overview_index_{0};
  int contacts_index_{0};
  int dns_index_{0};
  int notices_index_{0};
  std::vector<std::string> json_view_names_{"Pretty", "Raw"};
  int json_view_index_{0};
  std::vector<std::string> pretty_lines_{"Enter a domain name to start."};
  std::vector<std::string> raw_lines_{"Enter a domain name to start."};
  int pretty_index_{0};
  int raw_index_{0};

  Component input_;
  Component search_;
  Component section_toggle_;
  Component overview_menu_;
  Component contacts_menu_;
  Component dns_menu_;
  Component notices_menu_;
  Component json_toggle_;
  Component pretty_menu_;
  Component raw_menu_;
  Component json_tabs_;
  Component json_panel_;
  Component result_tabs_;
  Component root_;
};

} // namespace

int run_tui(BootstrapCache *disk_cache) {
  TuiApplication application(disk_cache);
  return application.run();
}

} // namespace rdap
