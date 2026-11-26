module;

#include <huxerui/http.h>
#include <huxerui/task.h>

export module dbpp.driver_installer;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.adbc;

export namespace dbpp {

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

[[nodiscard]] std::string CurrentPlatformTuple();
[[nodiscard]] std::string AdbcRegistryBaseUrl();
[[nodiscard]] std::vector<RegistryDriverInfo> ParseRegistryIndexYaml(
    std::string_view yaml_text, std::string_view base_url, std::string_view platform_tuple);
[[nodiscard]] std::string Sha256HexFile(const std::filesystem::path& path);
[[nodiscard]] bool IsUserInstalledAdbcDriver(
    std::string_view slug, const std::filesystem::path& user_drivers_dir = {});
[[nodiscard]] DriverInstallResult InstallDriverArchiveBlocking(
    const RegistryDriverInfo& driver, const std::filesystem::path& archive_path,
    const std::filesystem::path& user_drivers_dir = {});
[[nodiscard]] DriverInstallResult UninstallUserAdbcDriverBlocking(
    std::string_view slug, const std::filesystem::path& user_drivers_dir = {});
huxerui::Task<RegistryCatalogResult> FetchRegistryCatalogAsync(
    std::shared_ptr<huxerui::HttpClient> http);
[[nodiscard]] RegistryCatalogResult FetchRegistryCatalogBlocking(
    std::string_view registry_url_override = {});
huxerui::Task<DriverInstallResult> InstallRegistryDriverAsync(
    std::shared_ptr<huxerui::HttpClient> http, RegistryDriverInfo driver,
    std::function<void(std::string)> on_progress = {});
[[nodiscard]] DriverInstallResult InstallRegistryDriverBlocking(
    const RegistryDriverInfo& driver, const std::filesystem::path& user_drivers_dir = {});

} // namespace dbpp
