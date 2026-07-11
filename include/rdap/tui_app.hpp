// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace rdap {

struct AppConfig;
struct AppState;
class AppStateStore;
class BootstrapCache;

int run_tui(BootstrapCache *disk_cache, AppConfig config, AppState state,
            AppStateStore *state_store);

} // namespace rdap
