#include "virtual_desktop_controller_internal.h"

#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace locking_glass::integration::internal {

DesktopReturnReplayState LoadDesktopReturnScript(const std::string& script_path) {
  std::ifstream input(script_path);
  if (!input.is_open()) {
    return {};
  }

  DesktopReturnReplayState state;
  std::map<std::string, std::size_t> window_indexes;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const auto fields = SplitFields(line);
    if (fields.empty()) {
      continue;
    }

    if (fields[0] == "desktop" && (fields.size() == 4U || fields.size() == 5U)) {
      int desktop_number = -1;
      if (!ParseIntField(fields[1], &desktop_number)) {
        return {};
      }
      auto desktop = MakeDesktopIdentity(desktop_number, fields[2], fields[3]);
      if (fields.size() == 5U && !fields[4].empty()) {
        desktop.display_id = fields[4];
      }
      state.desktops.push_back(std::move(desktop));
      continue;
    }

    if (fields[0] == "current" && (fields.size() == 4U || fields.size() == 5U)) {
      int desktop_number = -1;
      if (!ParseIntField(fields[1], &desktop_number)) {
        return {};
      }
      auto desktop = MakeDesktopIdentity(desktop_number, fields[2], fields[3]);
      if (fields.size() == 5U && !fields[4].empty()) {
        desktop.display_id = fields[4];
      }
      state.current_desktop = std::move(desktop);
      continue;
    }

    if (fields[0] == "window" && (fields.size() == 10U || fields.size() == 11U)) {
      int desktop_number = -1;
      bool is_top_level = false;
      bool can_move = false;
      if (!ParseIntField(fields[5], &desktop_number) ||
          !ParseBoolField(fields[fields.size() - 2U], &is_top_level) ||
          !ParseBoolField(fields[fields.size() - 1U], &can_move)) {
        return {};
      }

      DesktopIdentity desktop =
          MakeDesktopIdentity(desktop_number, fields[6], fields[7]);
      if (fields.size() == 11U && !fields[8].empty()) {
        desktop.display_id = fields[8];
      }
      const core::DesktopWindow window{
          .window_id = fields[1],
          .title = fields[2],
          .monitor_id = fields[3],
          .monitor_label = fields[4],
          .desktop_id = FormatDesktopIdentity(desktop),
          .is_top_level = is_top_level,
          .can_move = can_move,
      };
      window_indexes[window.window_id] = state.windows.size();
      state.windows.push_back(CapturedWindow{
          .window = window,
          .desktop = desktop,
          .handle = nullptr,
          .move_block_reason = can_move ? std::string{} : "window cannot be moved",
          .forced_failure_detail = {},
          .override_skip_reason = {},
          .extra_skip = std::nullopt,
      });
      continue;
    }

    if (fields[0] == "failure" && fields.size() == 3U) {
      const auto window_it = window_indexes.find(fields[1]);
      if (window_it == window_indexes.end()) {
        return {};
      }
      state.windows[window_it->second].forced_failure_detail = fields[2];
      continue;
    }

    return {};
  }

  return state;
}

std::vector<core::DesktopSwitchScenario> LoadDesktopScript(
    const std::string& script_path) {
  std::ifstream input(script_path);
  if (!input.is_open()) {
    return {};
  }

  std::vector<core::DesktopSwitchScenario> scenarios;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const auto fields = SplitFields(line);
    if (fields.empty()) {
      continue;
    }

    // The optional sixth replay field is the staging desktop id. It exercises
    // production move policy without pretending to be live helper proof.
    if (fields[0] == "event" && (fields.size() == 5U || fields.size() == 6U) &&
        fields[1] == "desktop-switch") {
      scenarios.push_back(core::DesktopSwitchScenario{
          .trigger = fields[2],
          .source_desktop_id = fields[3],
          .target_desktop_id = fields[4],
          .staging_desktop_id = fields.size() == 6U ? fields[5] : std::string{},
          .monitors = {},
          .windows = {},
          .use_staging_restore_hints = false,
          .staging_restore_hints = {},
      });
      continue;
    }

    if (fields[0] == "monitor" && fields.size() == 11U && !scenarios.empty()) {
      bool is_primary = false;
      platform::MonitorBounds bounds;
      if (!ParseIntField(fields[6], &bounds.left) ||
          !ParseIntField(fields[7], &bounds.top) ||
          !ParseIntField(fields[8], &bounds.right) ||
          !ParseIntField(fields[9], &bounds.bottom) ||
          !ParseBoolField(fields[10], &is_primary)) {
        return {};
      }

      scenarios.back().monitors.push_back(platform::MonitorDescriptor{
          .stable_id = fields[1],
          .device_path = fields[2],
          .edid_serial = fields[3],
          .display_name = fields[4],
          .label = fields[5],
          .bounds = bounds,
          .is_primary = is_primary,
      });
      continue;
    }

    if (fields[0] == "window" && fields.size() == 8U && !scenarios.empty()) {
      bool is_top_level = false;
      bool can_move = false;
      if (!ParseBoolField(fields[6], &is_top_level) ||
          !ParseBoolField(fields[7], &can_move)) {
        return {};
      }

      scenarios.back().windows.push_back(core::DesktopWindow{
          .window_id = fields[1],
          .title = fields[2],
          .monitor_id = fields[3],
          .monitor_label = fields[4],
          .desktop_id = fields[5],
          .is_top_level = is_top_level,
          .can_move = can_move,
      });
      continue;
    }

    return {};
  }

  return scenarios;
}

DesktopSwitchReport BuildDesktopSwitchReport(
    const core::SessionStore& store,
    const core::DesktopSwitchScenario& scenario) {
  DesktopSwitchReport report{
      .plan = core::BuildMonitorLockingPlan(store, scenario),
      .move_results = {},
      .resulting_windows = scenario.windows,
  };

  for (const auto& move : report.plan.moves) {
    auto* result_window = FindResultWindow(&report.resulting_windows, move);
    if (result_window == nullptr) {
      report.move_results.push_back(WindowMoveResult{
          .window = move.window,
          .from_desktop = MakeDisplayOnlyDesktopIdentity(move.from_desktop_id),
          .to_desktop = MakeDisplayOnlyDesktopIdentity(move.to_desktop_id),
          .from_desktop_id = move.from_desktop_id,
          .to_desktop_id = move.to_desktop_id,
          .success = false,
          .detail = "window was missing from the replay snapshot",
      });
      continue;
    }

    result_window->desktop_id = move.to_desktop_id;
    report.move_results.push_back(WindowMoveResult{
        .window = move.window,
        .from_desktop = MakeDisplayOnlyDesktopIdentity(move.from_desktop_id),
        .to_desktop = MakeDisplayOnlyDesktopIdentity(move.to_desktop_id),
        .from_desktop_id = move.from_desktop_id,
        .to_desktop_id = move.to_desktop_id,
        .success = true,
        .detail = "scripted move applied",
    });
  }

  return report;
}

UnlockReturnReport BuildScriptedUnlockReturnReport(
    const UnlockReturnRequest& request,
    const DesktopReturnReplayState& replay_state) {
  UnlockReturnRequest replay_request = request;
  if (replay_state.current_desktop.has_value()) {
    replay_request.current_desktop = replay_state.current_desktop;
  }

  return BuildUnlockReturnReport(
      replay_request, replay_state.desktops, replay_state.windows,
      [](const CapturedWindow& current_window,
         const DesktopIdentity& remembered_desktop) {
        if (!current_window.forced_failure_detail.empty()) {
          return WindowMoveResult{
              .window = current_window.window,
              .from_desktop = current_window.desktop,
              .to_desktop = remembered_desktop,
              .from_desktop_id = current_window.window.desktop_id,
              .to_desktop_id = FormatDesktopIdentity(remembered_desktop),
              .success = false,
              .detail = current_window.forced_failure_detail,
          };
        }

        return WindowMoveResult{
            .window = current_window.window,
            .from_desktop = current_window.desktop,
            .to_desktop = remembered_desktop,
            .from_desktop_id = current_window.window.desktop_id,
            .to_desktop_id = FormatDesktopIdentity(remembered_desktop),
            .success = true,
            .detail = "scripted return move applied",
        };
      });
}

}  // namespace locking_glass::integration::internal
