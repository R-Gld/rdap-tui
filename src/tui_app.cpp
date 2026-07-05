// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/tui_app.hpp"

#include "rdap/domain_name.hpp"
#include "rdap/http.hpp"
#include "rdap/rdap_client.hpp"

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
  TuiApplication()
      : screen_(ScreenInteractive::Fullscreen()), client_(http_client_),
        input_(Input(&domain_input_, "example.com")),
        search_(Button("Search", [this] { start_lookup(); })),
        view_toggle_(Toggle(&view_names_, &view_index_)),
        pretty_menu_(Menu(&pretty_lines_, &pretty_index_)),
        raw_menu_(Menu(&raw_lines_, &raw_index_)),
        result_tabs_(Container::Tab({pretty_menu_, raw_menu_}, &view_index_)) {
    auto controls = Container::Horizontal({input_, search_});
    auto interactive = Container::Vertical({controls, view_toggle_, result_tabs_});

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
                 hbox({text(" Domain  "), input_->Render() | flex, text(" "), search_->Render()}),
                 hbox({text(" Status  "), status}),
                 vbox(std::move(metadata)),
                 separator(),
                 hbox({text(" Response  "), view_toggle_->Render()}),
                 result_tabs_->Render() | vscroll_indicator | frame | flex,
                 separator(),
                 text("Enter: search  Tab: focus  ↑/↓/PgUp/PgDn: scroll  Ctrl+C/Esc: quit") | dim,
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

    auto domain_result = DomainName::parse(domain_input_);
    if (const auto *error = std::get_if<Error>(&domain_result)) {
      show_error(*error);
      return;
    }
    auto domain = std::get<DomainName>(std::move(domain_result));

    if (worker_.joinable()) {
      worker_.join();
    }
    state_ = ViewState::loading_bootstrap;
    metadata_.clear();
    pretty_lines_ = {"Waiting for the RDAP response..."};
    raw_lines_ = pretty_lines_;
    pretty_index_ = 0;
    raw_index_ = 0;

    cancellation_ = CancellationSource();
    const auto cancellation = cancellation_.token();
    worker_ = std::thread([this, domain = std::move(domain), cancellation] {
      auto progress = [this](LookupStage stage) {
        {
          std::scoped_lock lock(pending_mutex_);
          pending_stage_ = stage;
        }
        screen_.PostEvent(Event::Custom);
      };
      auto result = client_.lookup_domain(domain, cancellation, progress);
      {
        std::scoped_lock lock(pending_mutex_);
        pending_result_ = std::move(result);
      }
      screen_.PostEvent(Event::Custom);
    });
  }

  void consume_worker_update() {
    std::optional<LookupStage> stage;
    std::optional<Result<RdapResponse>> result;
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

    auto response = std::get<RdapResponse>(std::move(*result));
    state_ = ViewState::success;
    metadata_ = std::string(response.query.ascii()) + "  HTTP " +
                std::to_string(response.http.status) + "  " + response.http.effective_url;
    pretty_lines_ = lines(response.document.dump(2));
    raw_lines_ = lines(response.http.body);
    pretty_index_ = 0;
    raw_index_ = 0;
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
    pretty_lines_ = lines(message.str());
    raw_lines_ = pretty_lines_;
    pretty_index_ = 0;
    raw_index_ = 0;
  }

  ScreenInteractive screen_;
  CurlHttpClient http_client_;
  RdapClient client_;
  std::thread worker_;
  CancellationSource cancellation_;

  std::mutex pending_mutex_;
  std::optional<LookupStage> pending_stage_;
  std::optional<Result<RdapResponse>> pending_result_;

  ViewState state_{ViewState::idle};
  std::string domain_input_;
  std::string metadata_;
  std::vector<std::string> view_names_{"Pretty", "Raw"};
  int view_index_{0};
  std::vector<std::string> pretty_lines_{"Enter a domain name to start."};
  std::vector<std::string> raw_lines_{"Enter a domain name to start."};
  int pretty_index_{0};
  int raw_index_{0};

  Component input_;
  Component search_;
  Component view_toggle_;
  Component pretty_menu_;
  Component raw_menu_;
  Component result_tabs_;
  Component root_;
};

} // namespace

int run_tui() {
  TuiApplication application;
  return application.run();
}

} // namespace rdap
