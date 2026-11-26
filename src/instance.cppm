export module dbpp.instance;

import std;
import dbpp.adbc;

export namespace dbpp {

/// File/URI + optional ADBC driver passed from the OS or a secondary `dbpp`
/// GUI invocation so the primary single instance opens it directly.
struct InstanceOpenRequest {
  std::string uri;
  std::string driver_path;
};

[[nodiscard]] std::optional<InstanceOpenRequest> PeekEnvOpenRequest();
[[nodiscard]] std::optional<InstanceOpenRequest> TakeEnvOpenRequest();
[[nodiscard]] bool AcquireSingleInstanceOrActivate(
    bool activate = true,
    const std::optional<InstanceOpenRequest>& open_request = std::nullopt,
    const std::filesystem::path& lock_dir_override = {});
void MarkInstanceClosing();
void MarkInstanceRunning();
void ReleaseSingleInstance();
[[nodiscard]] bool ConsumeInstanceActivation();
[[nodiscard]] std::optional<InstanceOpenRequest> TakePendingInstanceOpenRequest(
    const std::filesystem::path& lock_dir_override = {});

} // namespace dbpp
