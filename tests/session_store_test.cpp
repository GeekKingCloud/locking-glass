#include "test_helpers.h"

namespace locking_glass::tests {

bool RunSessionStoreChecks() {
  // Covers monitor identity reconciliation, startup unlock reset, topology
  // persistence, and fail-closed recovery from rejected session files.
  int failures = 0;

  const auto temp_directory = MakeTempDirectory();
  const auto session_path = temp_directory / "monitor-session-state.tsv";
  const locking_glass::core::SessionStore session_store(session_path);

  const auto left_monitor =
      MakeMonitor("\\\\.\\DISPLAY1", "DISPLAY#LEFT", "SERIAL-LEFT",
                  "Dell U2720Q", "Display 1", 0, 0, 2560, 1440, true);
  const auto right_monitor =
      MakeMonitor("\\\\.\\DISPLAY2", "DISPLAY#RIGHT", "SERIAL-RIGHT",
                  "Dell U2720Q", "Display 2", 2560, 0, 5120, 1440, false);
  const auto new_monitor =
      MakeMonitor("\\\\.\\DISPLAY3", "DISPLAY#NEW", "SERIAL-NEW",
                  "LG UltraFine", "Display 3", -1920, 0, 0, 1080, false);

  auto initial = session_store.Restore({left_monitor, right_monitor});
  failures += !Expect(!initial.loaded_from_disk,
                      "initial restore should bootstrap from an empty session file");
  failures += !Expect(initial.new_monitors == 2U,
                      "initial restore should register each live monitor as new");
  failures += !Expect(initial.review_monitors == 2U,
                      "new monitors should require confirmation by default");
  failures += !Expect(session_store.SetLocked(&initial.snapshot, left_monitor, true),
                      "left monitor should be lockable in the session snapshot");
  failures += !Expect(session_store.SetLocked(&initial.snapshot, right_monitor, true),
                      "right monitor should be lockable in the session snapshot");
  failures += !Expect(session_store.Save(initial.snapshot),
                      "session snapshot should persist to disk");
  failures += !Expect(std::filesystem::exists(session_path),
                      "saving the session should create the session file");

  auto restarted =
      locking_glass::core::SessionStore(session_path).Restore({left_monitor, right_monitor});
  failures += !Expect(restarted.loaded_from_disk,
                      "restart simulation should reload the persisted session file");
  failures += !Expect(restarted.restored_locked_monitors == 2U,
                      "restart simulation should restore both confirmed locked monitors");
  failures += !Expect(restarted.review_monitors == 0U,
                      "confirmed monitor locks should not require review on restart");

  const auto* restarted_left = FindMonitorState(restarted.snapshot, left_monitor);
  failures += !Expect(restarted_left != nullptr,
                      "restarted snapshot should contain the left monitor");
  if (restarted_left != nullptr) {
    failures += !Expect(restarted_left->is_present,
                        "left monitor should be active after restart");
    failures += !Expect(restarted_left->locked,
                        "left monitor lock should survive restart");
    failures += !Expect(!restarted_left->requires_confirmation,
                        "confirmed left monitor should not require review");
  }

  auto topology_change =
      locking_glass::core::SessionStore(session_path).Restore({left_monitor, new_monitor});
  failures += !Expect(topology_change.disconnected_monitors == 1U,
                      "removing a monitor should keep one saved monitor inactive");
  failures += !Expect(topology_change.new_monitors == 1U,
                      "adding a monitor should create one new session record");
  failures += !Expect(topology_change.review_monitors == 1U,
                      "new monitors should remain unlocked until reviewed");
  failures += !Expect(topology_change.restored_locked_monitors == 1U,
                      "only active confirmed monitors should count as restored locks");

  const auto* disconnected_right = FindMonitorState(topology_change.snapshot, right_monitor);
  failures += !Expect(disconnected_right != nullptr,
                      "removed monitor should remain in the session snapshot");
  if (disconnected_right != nullptr) {
    failures += !Expect(!disconnected_right->is_present,
                        "removed monitor should be marked inactive");
    failures += !Expect(disconnected_right->locked,
                        "removed monitor should keep its saved lock state");
  }

  const auto* added_monitor = FindMonitorState(topology_change.snapshot, new_monitor);
  failures += !Expect(added_monitor != nullptr,
                      "added monitor should be tracked in the session snapshot");
  if (added_monitor != nullptr) {
    failures += !Expect(added_monitor->is_present,
                        "added monitor should be active immediately");
    failures += !Expect(!added_monitor->locked,
                        "added monitor should default to unlocked");
    failures += !Expect(added_monitor->requires_confirmation,
                        "added monitor should require user confirmation");
  }

  auto restored_topology = locking_glass::core::SessionStore(session_path)
                               .Restore({left_monitor, right_monitor, new_monitor});
  failures += !Expect(restored_topology.restored_locked_monitors == 2U,
                      "reconnecting a saved monitor should reapply its lock state");

  const auto* reconnected_right =
      FindMonitorState(restored_topology.snapshot, right_monitor);
  failures += !Expect(reconnected_right != nullptr,
                      "reconnected monitor should still exist in the session snapshot");
  if (reconnected_right != nullptr) {
    failures += !Expect(reconnected_right->is_present,
                        "reconnected monitor should be active again");
    failures += !Expect(reconnected_right->locked,
                        "reconnected monitor should recover its saved lock state");
  }

  auto app_start = locking_glass::core::SessionStore(session_path)
                       .StartUnlocked({left_monitor, right_monitor, new_monitor});
  failures += !Expect(app_start.restored_locked_monitors == 0U,
                      "app startup should report all monitors as unlocked");
  const auto* app_start_left = FindMonitorState(app_start.snapshot, left_monitor);
  const auto* app_start_right =
      FindMonitorState(app_start.snapshot, right_monitor);
  failures += !Expect(app_start_left != nullptr && app_start_right != nullptr,
                      "app startup should keep known monitors in the session snapshot");
  if (app_start_left != nullptr) {
    failures += !Expect(!app_start_left->locked,
                        "app startup should clear the left monitor lock");
  }
  if (app_start_right != nullptr) {
    failures += !Expect(!app_start_right->locked,
                        "app startup should clear the right monitor lock");
  }
  const auto after_start =
      locking_glass::core::SessionStore(session_path)
          .Restore({left_monitor, right_monitor, new_monitor});
  failures += !Expect(after_start.restored_locked_monitors == 0U,
                      "startup-unlocked state should be persisted immediately");

  const auto invalid_backup_path =
      std::filesystem::path(session_path.string() + ".invalid");
  const std::string malformed_contents =
      "version\t1\n"
      "monitor\tbad-stable\tbad-device\tbad-serial\tDell U2720Q\tDisplay 9\t0\t0\t1920\t1080\t1\tnot-a-bool\t0\n";
  WriteTextFile(session_path, malformed_contents);

  auto malformed_restore =
      locking_glass::core::SessionStore(session_path).Restore({left_monitor});
  failures += !Expect(malformed_restore.loaded_from_disk,
                      "malformed session recovery should detect the edited session file");
  failures += !Expect(
      malformed_restore.storage_issue ==
          locking_glass::core::SessionStorageIssue::kMalformedRecord,
      "malformed session recovery should classify invalid monitor rows");
  failures += !Expect(malformed_restore.recovered_invalid_data,
                      "malformed session recovery should rebuild the active session file");
  failures += !Expect(
      malformed_restore.invalid_storage_backup_path == invalid_backup_path,
      "malformed session recovery should preserve the rejected file at a deterministic path");
  failures += !Expect(std::filesystem::exists(invalid_backup_path),
                      "malformed session recovery should emit the rejected-data backup");
  failures += !Expect(ReadTextFile(invalid_backup_path) == malformed_contents,
                      "rejected malformed session data should remain inspectable");
  failures += !Expect(malformed_restore.restored_locked_monitors == 0U,
                      "malformed session recovery should not trust rejected lock data");
  failures += !Expect(malformed_restore.new_monitors == 1U,
                      "malformed session recovery should rebuild from live monitors");
  failures += !Expect(malformed_restore.review_monitors == 1U,
                      "malformed session recovery should require user review");
  failures += !Expect(
      malformed_restore.storage_detail.find("malformed monitor record") !=
          std::string::npos,
      "malformed session recovery should explain why the stored data was rejected");

  const auto* malformed_left =
      FindMonitorState(malformed_restore.snapshot, left_monitor);
  failures += !Expect(malformed_left != nullptr,
                      "malformed session recovery should still return the live monitor");
  if (malformed_left != nullptr) {
    failures += !Expect(!malformed_left->locked,
                        "malformed session recovery should reset the live monitor to unlocked");
    failures += !Expect(malformed_left->requires_confirmation,
                        "malformed session recovery should require the user to reconfirm");
  }

  const std::string unsupported_contents = "version\t99\n";
  WriteTextFile(session_path, unsupported_contents);

  auto unsupported_preview =
      locking_glass::core::SessionStore(session_path).Preview({left_monitor});
  failures += !Expect(
      unsupported_preview.storage_issue ==
          locking_glass::core::SessionStorageIssue::kUnsupportedVersion,
      "preview should flag unsupported session format versions");
  failures += !Expect(!unsupported_preview.recovered_invalid_data,
                      "preview should inspect unsupported data without mutating it");
  failures += !Expect(ReadTextFile(session_path) == unsupported_contents,
                      "preview should leave unsupported session files untouched");

  auto unsupported_restore =
      locking_glass::core::SessionStore(session_path).Restore({left_monitor});
  failures += !Expect(
      unsupported_restore.storage_issue ==
          locking_glass::core::SessionStorageIssue::kUnsupportedVersion,
      "restore should classify unsupported session versions");
  failures += !Expect(unsupported_restore.recovered_invalid_data,
                      "restore should rebuild the session after an unsupported format");
  failures += !Expect(
      unsupported_restore.invalid_storage_backup_path == invalid_backup_path,
      "unsupported session recovery should reuse the deterministic rejected-data path");
  failures += !Expect(ReadTextFile(invalid_backup_path) == unsupported_contents,
                      "unsupported session recovery should preserve the rejected file contents");

  const auto repaired_preview =
      locking_glass::core::SessionStore(session_path).Preview({left_monitor});
  failures += !Expect(
      repaired_preview.storage_issue == locking_glass::core::SessionStorageIssue::kNone,
      "rebuilt session storage should deserialize cleanly after recovery");
  failures += !Expect(repaired_preview.review_monitors == 1U,
                      "rebuilt session storage should keep the live monitor pending review");

  const std::string startup_invalid_contents = "version\t99\n";
  WriteTextFile(session_path, startup_invalid_contents);

  auto startup_invalid =
      locking_glass::core::SessionStore(session_path).StartUnlocked({left_monitor});
  failures += !Expect(
      startup_invalid.storage_issue ==
          locking_glass::core::SessionStorageIssue::kUnsupportedVersion,
      "startup-unlocked recovery should classify unsupported session versions");
  failures += !Expect(startup_invalid.recovered_invalid_data,
                      "startup-unlocked recovery should rebuild invalid storage");
  failures += !Expect(startup_invalid.restored_locked_monitors == 0U,
                      "startup-unlocked recovery should not report restored locks");
  failures += !Expect(
      startup_invalid.invalid_storage_backup_path == invalid_backup_path,
      "startup-unlocked recovery should preserve rejected data for inspection");
  failures += !Expect(ReadTextFile(invalid_backup_path) == startup_invalid_contents,
                      "startup-unlocked recovery should preserve rejected file contents");
  const auto startup_repaired =
      locking_glass::core::SessionStore(session_path).Preview({left_monitor});
  failures += !Expect(
      startup_repaired.storage_issue == locking_glass::core::SessionStorageIssue::kNone,
      "startup-unlocked recovery should leave a readable active session file");

  std::filesystem::remove_all(temp_directory);
  return failures == 0;
}

}  // namespace locking_glass::tests
