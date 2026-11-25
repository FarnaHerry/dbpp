#pragma once

#include <huxerui/http.h>
#include <huxerui/task.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dbpp {

/// Built-in ADBC Driver Registry client and local installer.
///
/// Fetches the ADBC Driver Registry catalog (`index.yaml`), selects the latest
/// compatible package for the current platform tuple, streams `.tar.gz`
/// archives via `huxerui::HttpClient`, verifies SHA-256 checksums, safely
/// extracts files under `UserAdbcDriversDir()/.packages/<slug>/<version>/`,
/// and atomically writes standard ADBC v1 `.toml` manifests into
/// `UserAdbcDriversDir()`. Blocking archive/filesystem work runs on worker
/// threads via `huxerui::RunWorker`, never on UI composition callbacks.

struct RegistryDriverInfo {
  std::string slug;
  std::string name;
  std::string description;
  std::string license;
  std::string version;
  std::string package_url;
  std::string sha256;

  bool operator==(const RegistryDriverInfo&) const = default;
};

struct RegistryCatalogResult {
  std::vector<RegistryDriverInfo> drivers;
  std::string error;
};

struct DriverInstallResult {
  bool ok = false;
  std::string slug;
  std::string version;
  std::string manifest_path;
  std::string message;
};

/// Returns the ADBC platform tuple (`<os>_<arch>`, e.g. `linux_amd64`).
[[nodiscard]] std::string CurrentPlatformTuple();

/// Returns the registry base URL (`DBPP_ADBC_REGISTRY_URL` override or
/// `https://dbc-cdn.columnar.tech`, without a trailing slash).
[[nodiscard]] std::string AdbcRegistryBaseUrl();

/// Parses an ADBC Driver Registry `index.yaml` document and selects the best
/// matching package per driver for `platform_tuple`.
[[nodiscard]] std::vector<RegistryDriverInfo> ParseRegistryIndexYaml(
    std::string_view yaml_text, std::string_view base_url, std::string_view platform_tuple);

/// Computes the lowercase hex SHA-256 digest of `path`, or empty on error.
[[nodiscard]] std::string Sha256HexFile(const std::filesystem::path& path);

/// Returns true if `<user_drivers_dir>/<slug>.toml` currently exists as a file.
[[nodiscard]] bool IsUserInstalledAdbcDriver(
    std::string_view slug, const std::filesystem::path& user_drivers_dir = {});

/// Verifies, safely extracts, and installs a driver `.tar.gz` archive and its
/// standard ADBC v1 manifest into `user_drivers_dir` (defaults to
/// `UserAdbcDriversDir()`).
[[nodiscard]] DriverInstallResult InstallDriverArchiveBlocking(
    const RegistryDriverInfo& driver, const std::filesystem::path& archive_path,
    const std::filesystem::path& user_drivers_dir = {});

/// Removes `<user_drivers_dir>/<slug>.toml` and `<user_drivers_dir>/.packages/<slug>`.
[[nodiscard]] DriverInstallResult UninstallUserAdbcDriverBlocking(
    std::string_view slug, const std::filesystem::path& user_drivers_dir = {});

/// Fetches and parses the ADBC Driver Registry catalog asynchronously.
huxerui::Task<RegistryCatalogResult> FetchRegistryCatalogAsync(
    std::shared_ptr<huxerui::HttpClient> http);

/// Synchronously fetches and parses the ADBC Driver Registry catalog for
/// headless CLI execution (supports `http(s)://` via curl and `file://` URLs).
[[nodiscard]] RegistryCatalogResult FetchRegistryCatalogBlocking(
    std::string_view registry_url_override = {});

/// Downloads, verifies, extracts, and installs `driver` asynchronously, invoking
/// `on_progress` on the application thread with Chinese status text.
huxerui::Task<DriverInstallResult> InstallRegistryDriverAsync(
    std::shared_ptr<huxerui::HttpClient> http, RegistryDriverInfo driver,
    std::function<void(std::string)> on_progress = {});

/// Synchronously downloads (or copies for `file://`), verifies, extracts, and
/// installs `driver` for headless CLI execution.
[[nodiscard]] DriverInstallResult InstallRegistryDriverBlocking(
    const RegistryDriverInfo& driver, const std::filesystem::path& user_drivers_dir = {});

} // namespace dbpp
