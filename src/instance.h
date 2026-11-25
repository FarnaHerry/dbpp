#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace dbpp {

/// File/URI + optional ADBC driver passed from the OS or a secondary `dbpp`
/// GUI invocation so the primary single instance opens it directly.
struct InstanceOpenRequest {
  std::string uri;
  std::string driver_path;
};

/// Single-instance process coordination & activation handoff across Linux,
/// macOS, and Windows.
///
/// Only desktop GUI launches acquire the singleton lock; headless CLI
/// subcommands bypass it so CLI tools and agents never block on or interfere
/// with an open desktop window.
///
/// When a second GUI launch finds an already-running healthy owner:
/// - if `activate` is true, any `open_request` is written atomically into the
///   instance handoff file and the owner is signaled (`SIGUSR1` on POSIX,
///   auto-reset Event + `SetForegroundWindow` on Windows) before returning false;
/// - if the previous owner recorded `"closing"` state and appears wedged, the
///   new process verifies the PID image, cleans up the stuck process, and takes
///   over ownership.

/// Reads `DBPP_OPEN` and optional `DBPP_DRIVER` without clearing them.
[[nodiscard]] std::optional<InstanceOpenRequest> PeekEnvOpenRequest();

/// Reads `DBPP_OPEN` and optional `DBPP_DRIVER` and clears them so lightweight
/// window rebuilds do not re-open initial startup files.
[[nodiscard]] std::optional<InstanceOpenRequest> TakeEnvOpenRequest();

/// Acquires the single-instance lock or signals the existing owner.
/// Returns true if the caller now owns the lock; false if another healthy
/// instance already owns it.
[[nodiscard]] bool AcquireSingleInstanceOrActivate(
    bool activate = true,
    const std::optional<InstanceOpenRequest>& open_request = std::nullopt,
    const std::filesystem::path& lock_dir_override = {});

/// Records `"closing"` state in the lock file while tearing down the owner.
void MarkInstanceClosing();

/// Records `"running"` state in the lock file.
void MarkInstanceRunning();

/// Releases the held single-instance lock and OS handles.
void ReleaseSingleInstance();

/// Returns true once per external activation signal (`SIGUSR1` / Win32 Event).
[[nodiscard]] bool ConsumeInstanceActivation();

/// Atomically reads and removes any forwarded `InstanceOpenRequest` left by
/// a secondary launch.
[[nodiscard]] std::optional<InstanceOpenRequest> TakePendingInstanceOpenRequest(
    const std::filesystem::path& lock_dir_override = {});

} // namespace dbpp
