#include "adbc_session.h"
#include "driver_installer.h"
#include "model.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

void SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value != nullptr ? value : "");
#else
  if (value != nullptr && *value != '\0') {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
#endif
}

struct ScopedTempDir {
  fs::path path;

  ScopedTempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() / ("dbpp_test_adbc_core_" + std::to_string(stamp));
    fs::create_directories(path);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

struct ScopedEnvGuard {
  std::string name;
  bool had_value = false;
  std::string old_value;

  ScopedEnvGuard(std::string env_name, const std::string& new_value)
      : name(std::move(env_name)) {
    if (const char* current = std::getenv(name.c_str()); current != nullptr) {
      had_value = true;
      old_value = current;
    }
    SetEnv(name.c_str(), new_value.c_str());
  }

  ~ScopedEnvGuard() {
    if (had_value) {
      SetEnv(name.c_str(), old_value.c_str());
    } else {
      SetEnv(name.c_str(), nullptr);
    }
  }
};

std::string ReadBinaryFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

void WriteBinaryFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

void AppendTarEntry(std::vector<std::uint8_t>& tar, std::string_view name,
                    std::string_view data) {
  std::array<std::uint8_t, 512> header{};
  const std::size_t name_len = std::min<std::size_t>(name.size(), 99);
  std::memcpy(header.data(), name.data(), name_len);
  std::memcpy(header.data() + 100, "0000755\0", 8);
  std::memcpy(header.data() + 108, "0000000\0", 8);
  std::memcpy(header.data() + 116, "0000000\0", 8);

  char size_oct[12];
  std::snprintf(size_oct, sizeof(size_oct), "%011llo",
                static_cast<unsigned long long>(data.size()));
  std::memcpy(header.data() + 124, size_oct, 12);
  std::memcpy(header.data() + 136, "00000000000\0", 12);
  std::memset(header.data() + 148, ' ', 8);
  header[156] = '0';
  std::memcpy(header.data() + 257, "ustar\0", 6);
  std::memcpy(header.data() + 263, "00", 2);

  unsigned int checksum = 0;
  for (std::uint8_t b : header) {
    checksum += b;
  }
  char chk_oct[8];
  std::snprintf(chk_oct, sizeof(chk_oct), "%06o", checksum);
  chk_oct[6] = '\0';
  chk_oct[7] = ' ';
  std::memcpy(header.data() + 148, chk_oct, 8);

  tar.insert(tar.end(), header.begin(), header.end());
  tar.insert(tar.end(), reinterpret_cast<const std::uint8_t*>(data.data()),
             reinterpret_cast<const std::uint8_t*>(data.data() + data.size()));
  const std::size_t rem = data.size() % 512;
  if (rem != 0) {
    tar.resize(tar.size() + (512 - rem), 0);
  }
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t len) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
    }
  }
  return ~crc;
}

void AppendLe16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void AppendLe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFU));
}

std::vector<std::uint8_t> WrapGzipStored(const std::vector<std::uint8_t>& tar_bytes) {
  std::vector<std::uint8_t> gz = {0x1FU, 0x8BU, 0x08U, 0x00U, 0x00U,
                                  0x00U, 0x00U, 0x00U, 0x00U, 0xFFU};
  constexpr std::size_t kMaxBlock = 65535;
  std::size_t offset = 0;
  do {
    const std::size_t chunk = std::min(kMaxBlock, tar_bytes.size() - offset);
    const bool is_final = (offset + chunk) >= tar_bytes.size();
    gz.push_back(is_final ? 0x01U : 0x00U);
    const std::uint16_t len = static_cast<std::uint16_t>(chunk);
    const std::uint16_t nlen = static_cast<std::uint16_t>(~len);
    AppendLe16(gz, len);
    AppendLe16(gz, nlen);
    if (chunk > 0) {
      gz.insert(gz.end(), tar_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                tar_bytes.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
    }
    offset += chunk;
  } while (offset < tar_bytes.size());

  AppendLe32(gz, Crc32(tar_bytes.data(), tar_bytes.size()));
  AppendLe32(gz, static_cast<std::uint32_t>(tar_bytes.size() & 0xFFFFFFFFU));
  return gz;
}

std::vector<std::uint8_t> BuildTarGz(
    const std::vector<std::pair<std::string, std::string>>& entries) {
  std::vector<std::uint8_t> tar;
  for (const auto& [name, content] : entries) {
    AppendTarEntry(tar, name, content);
  }
  tar.resize(tar.size() + 1024, 0);
  return WrapGzipStored(tar);
}

void TestDefaultDriverAndSqliteCrud(const fs::path& dir, const std::string& driver_path) {
  std::error_code ec;
  Expect(!driver_path.empty(), "DefaultSqliteDriverPath must not be empty");
  Expect(fs::is_regular_file(driver_path, ec),
         "DefaultSqliteDriverPath must resolve to an existing file: " + driver_path);

  const fs::path db_path = dir / "sample.sqlite";
  dbpp::ConnectionRequest req;
  req.driver_path = driver_path;
  req.uri = db_path.string();
  dbpp::AdbcOpenResult opened = dbpp::OpenAdbcBlocking(req);
  Expect(opened.error.empty() && opened.session != nullptr,
         "OpenAdbcBlocking sqlite failed: " + opened.error);
  if (!opened.session) {
    return;
  }

  dbpp::AdbcQueryResult created = dbpp::ExecuteBlocking(
      *opened.session,
      "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL)",
      false, 100);
  Expect(created.error.empty(), "CREATE TABLE failed: " + created.error);

  const std::string insert_sql =
      "INSERT INTO items (id, name, score) VALUES (" +
      dbpp::SqlLiteral(dbpp::Dialect::Sqlite, "1", "INTEGER", false) + ", " +
      dbpp::SqlLiteral(dbpp::Dialect::Sqlite, "alpha", "TEXT", false) + ", " +
      dbpp::SqlLiteral(dbpp::Dialect::Sqlite, "3.14", "REAL", false) + "), (" +
      dbpp::SqlLiteral(dbpp::Dialect::Sqlite, "2", "INTEGER", false) + ", " +
      dbpp::SqlLiteral(dbpp::Dialect::Sqlite, "beta", "TEXT", false) + ", " +
      dbpp::SqlLiteral(dbpp::Dialect::Sqlite, "", "REAL", true) + ")";
  Expect(insert_sql.find("3.14") != std::string::npos,
         "SqlLiteral should format finite float without quotes");
  Expect(insert_sql.find("' 3.14'") == std::string::npos,
         "SqlLiteral check");

  dbpp::AdbcQueryResult inserted =
      dbpp::ExecuteBlocking(*opened.session, insert_sql, false, 100);
  Expect(inserted.error.empty(), "INSERT failed: " + inserted.error);

  dbpp::AdbcObjectsResult objects = dbpp::LoadObjectsBlocking(*opened.session);
  Expect(objects.error.empty(), "LoadObjectsBlocking failed: " + objects.error);
  Expect(objects.objects.size() == 1 && objects.objects[0].name == "items",
         "Expected items table in LoadObjectsBlocking");
  if (!objects.objects.empty()) {
    dbpp::AdbcSchemaResult schema =
        dbpp::LoadSchemaBlocking(*opened.session, objects.objects[0]);
    Expect(schema.error.empty() && schema.columns.size() == 3,
           "LoadSchemaBlocking failed: " + schema.error);
  }

  dbpp::AdbcQueryResult queried = dbpp::ExecuteBlocking(
      *opened.session, "SELECT id, name, score FROM items ORDER BY id", true, 100);
  Expect(queried.error.empty(), "SELECT failed: " + queried.error);
  Expect(queried.rows.size() == 2, "Expected 2 rows from items");
  if (queried.rows.size() == 2) {
    Expect(queried.rows[0].values[1].text == "alpha", "Row 0 name should be alpha");
    Expect(queried.rows[1].values[2].is_null, "Row 1 score should be NULL");
  }
}

void TestSha256AndYamlParser(const fs::path& dir) {
  const fs::path nist_file = dir / "abc.txt";
  WriteBinaryFile(nist_file, {'a', 'b', 'c'});
  Expect(dbpp::Sha256HexFile(nist_file) ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "Sha256HexFile NIST vector mismatch");

  const std::string platform = dbpp::CurrentPlatformTuple();
  Expect(platform.find('_') != std::string::npos && platform.find("unknown") == std::string::npos,
         "Unexpected CurrentPlatformTuple: " + platform);

  const std::string yaml =
      "- name: PostgreSQL\n"
      "  description: >-\n"
      "    An ADBC driver for PostgreSQL.\n"
      "  license: Apache-2.0\n"
      "  path: postgresql\n"
      "  pkginfo:\n"
      "  - packages:\n"
      "    - platform: " + platform + "\n"
      "      url: drivers/postgresql-1.9.0.tar.gz\n"
      "      hash: sha256:0000000000000000000000000000000000000000000000000000000000000001\n"
      "    version: 1.9.0\n"
      "  - packages:\n"
      "    - platform: " + platform + "\n"
      "      url: drivers/postgresql-1.10.0.tar.gz\n"
      "      hash: sha256:0000000000000000000000000000000000000000000000000000000000000002\n"
      "    version: 1.10.0\n"
      "- name: OtherOnly\n"
      "  license: MIT\n"
      "  path: otheronly\n"
      "  pkginfo:\n"
      "  - packages:\n"
      "    - platform: plan9_riscv64\n"
      "      url: https://example.com/other.tar.gz\n"
      "      hash: sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
      "    version: 0.1.0\n";

  const std::vector<dbpp::RegistryDriverInfo> parsed =
      dbpp::ParseRegistryIndexYaml(yaml, "https://dbc-cdn.columnar.tech", platform);
  Expect(parsed.size() == 1, "Expected exactly 1 driver matching current platform");
  if (parsed.size() == 1) {
    Expect(parsed[0].slug == "postgresql", "Parsed slug mismatch");
    Expect(parsed[0].version == "1.10.0", "Expected SemVer 1.10.0 > 1.9.0");
    Expect(parsed[0].package_url ==
               "https://dbc-cdn.columnar.tech/drivers/postgresql-1.10.0.tar.gz",
           "Relative URL resolution mismatch");
    Expect(parsed[0].sha256 ==
               "0000000000000000000000000000000000000000000000000000000000000002",
           "SHA-256 parse mismatch");
  }
}

void TestDriverInstallDiscoverLoadUninstall(const fs::path& dir, const std::string& driver_path) {
  const fs::path drivers_dir = dir / "drivers";
  fs::create_directories(drivers_dir);

  const std::string driver_bytes = ReadBinaryFile(driver_path);
  Expect(!driver_bytes.empty(), "Failed to read SQLite driver binary for archive fixture");
  const std::string driver_filename = fs::path(driver_path).filename().string();

  const std::string manifest_toml =
      "manifest_version = 1\n"
      "name = \"SQLite Test\"\n"
      "publisher = \"db++ test\"\n"
      "license = \"Apache-2.0\"\n"
      "version = \"0.1.0\"\n"
      "[Files]\n"
      "driver = \"" + driver_filename + "\"\n";

  // 1) Path-traversal archive must be rejected even with a matching SHA-256.
  const fs::path traversal_tgz = dir / "traversal.tar.gz";
  WriteBinaryFile(traversal_tgz,
                  BuildTarGz({{"../escape.so", "evil"}, {"MANIFEST", manifest_toml}}));
  dbpp::RegistryDriverInfo traversal_driver;
  traversal_driver.slug = "traversal_test";
  traversal_driver.name = "Traversal";
  traversal_driver.version = "0.1.0";
  traversal_driver.sha256 = dbpp::Sha256HexFile(traversal_tgz);
  const dbpp::DriverInstallResult traversal_res =
      dbpp::InstallDriverArchiveBlocking(traversal_driver, traversal_tgz, drivers_dir);
  Expect(!traversal_res.ok, "Path-traversal archive must be rejected");
  Expect(!fs::exists(drivers_dir / "escape.so"), "Escaped file must not be created");

  // 2) Valid archive with bad SHA-256 must fail.
  const fs::path valid_tgz = dir / "sqlite_test.tar.gz";
  WriteBinaryFile(valid_tgz,
                  BuildTarGz({{"MANIFEST", manifest_toml}, {driver_filename, driver_bytes}}));
  const std::string valid_sha = dbpp::Sha256HexFile(valid_tgz);

  dbpp::RegistryDriverInfo bad_hash_driver;
  bad_hash_driver.slug = "sqlite_test";
  bad_hash_driver.name = "SQLite Test";
  bad_hash_driver.version = "0.1.0";
  bad_hash_driver.sha256 = std::string(64, '0');
  const dbpp::DriverInstallResult bad_hash_res =
      dbpp::InstallDriverArchiveBlocking(bad_hash_driver, valid_tgz, drivers_dir);
  Expect(!bad_hash_res.ok, "Mismatched SHA-256 archive must be rejected");

  // 3) Install an unloaded slug and verify full uninstall removes both manifest and .packages.
  dbpp::RegistryDriverInfo uninstall_driver;
  uninstall_driver.slug = "sqlite_uninstall_test";
  uninstall_driver.name = "SQLite Uninstall Test";
  uninstall_driver.version = "0.1.0";
  uninstall_driver.sha256 = valid_sha;
  const dbpp::DriverInstallResult uninstall_prep =
      dbpp::InstallDriverArchiveBlocking(uninstall_driver, valid_tgz, drivers_dir);
  Expect(uninstall_prep.ok, "Uninstall fixture install failed: " + uninstall_prep.message);
  Expect(dbpp::IsUserInstalledAdbcDriver("sqlite_uninstall_test", drivers_dir),
         "sqlite_uninstall_test should be installed before uninstall");
  Expect(fs::exists(drivers_dir / ".packages" / "sqlite_uninstall_test"),
         ".packages/sqlite_uninstall_test should exist before uninstall");

  const dbpp::DriverInstallResult full_uninstall =
      dbpp::UninstallUserAdbcDriverBlocking("sqlite_uninstall_test", drivers_dir);
  Expect(full_uninstall.ok, "Unloaded uninstall failed: " + full_uninstall.message);
  Expect(!dbpp::IsUserInstalledAdbcDriver("sqlite_uninstall_test", drivers_dir),
         "sqlite_uninstall_test manifest should be gone after uninstall");
  Expect(!fs::exists(drivers_dir / ".packages" / "sqlite_uninstall_test"),
         ".packages/sqlite_uninstall_test should be gone after uninstall");

  // 4) Install with matching SHA-256 -> discover -> open by slug & manifest path -> uninstall.
  dbpp::RegistryDriverInfo valid_driver;
  valid_driver.slug = "sqlite_test";
  valid_driver.name = "SQLite Test";
  valid_driver.version = "0.1.0";
  valid_driver.sha256 = valid_sha;
  const dbpp::DriverInstallResult ok_res =
      dbpp::InstallDriverArchiveBlocking(valid_driver, valid_tgz, drivers_dir);
  Expect(ok_res.ok, "Valid archive install failed: " + ok_res.message);
  Expect(dbpp::IsUserInstalledAdbcDriver("sqlite_test", drivers_dir),
         "IsUserInstalledAdbcDriver should report true after install");

  {
    ScopedEnvGuard drivers_env("DBPP_ADBC_DRIVERS", drivers_dir.string());
    const std::vector<dbpp::AdbcDriverInfo> discovered = dbpp::DiscoverAdbcDrivers();
    const bool found_manifest = std::any_of(
        discovered.begin(), discovered.end(), [](const dbpp::AdbcDriverInfo& info) {
          return info.is_manifest && info.path == "sqlite_test" &&
                 info.name == "SQLite Test";
        });
    Expect(found_manifest, "DiscoverAdbcDrivers did not find installed sqlite_test manifest");

    const fs::path db_path = dir / "via_manifest.sqlite";
    dbpp::ConnectionRequest slug_req;
    slug_req.driver_path = "sqlite_test";
    slug_req.uri = db_path.string();
    dbpp::AdbcOpenResult by_slug = dbpp::OpenAdbcBlocking(slug_req);
    Expect(by_slug.error.empty() && by_slug.session != nullptr,
           "OpenAdbcBlocking via manifest slug failed: " + by_slug.error);
    if (by_slug.session) {
      dbpp::AdbcQueryResult res =
          dbpp::ExecuteBlocking(*by_slug.session, "SELECT 42 AS answer", true, 10);
      Expect(res.error.empty() && res.rows.size() == 1 && res.rows[0].values[0].text == "42",
             "Query via manifest slug failed: " + res.error);
    }

    dbpp::ConnectionRequest file_req;
    file_req.driver_path = ok_res.manifest_path;
    file_req.uri = db_path.string();
    dbpp::AdbcOpenResult by_file = dbpp::OpenAdbcBlocking(file_req);
    Expect(by_file.error.empty() && by_file.session != nullptr,
           "OpenAdbcBlocking via manifest path failed: " + by_file.error);
  }

  const dbpp::DriverInstallResult uninstalled =
      dbpp::UninstallUserAdbcDriverBlocking("sqlite_test", drivers_dir);
  Expect(uninstalled.ok, "Uninstall failed: " + uninstalled.message);
  Expect(!dbpp::IsUserInstalledAdbcDriver("sqlite_test", drivers_dir),
         "IsUserInstalledAdbcDriver should report false after uninstall");
}

} // namespace

int main() {
  ScopedTempDir temp;
  const std::string driver_path = dbpp::DefaultSqliteDriverPath();

  TestDefaultDriverAndSqliteCrud(temp.path, driver_path);
  TestSha256AndYamlParser(temp.path);
  TestDriverInstallDiscoverLoadUninstall(temp.path, driver_path);

  if (g_failures != 0) {
    std::cerr << g_failures << " test assertion(s) failed\n";
    return 1;
  }
  std::cout << "test_adbc_core passed\n";
  return 0;
}
