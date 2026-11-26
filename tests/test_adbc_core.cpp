#include <cstdlib>

import std;
import dbpp.settings;
import dbpp.i18n;
import dbpp.types;
import dbpp.adbc;
import dbpp.db;
import dbpp.driver_installer;

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

void TestPostgresUriAndSqlReadDetection() {
  Expect(dbpp::BuildPostgresqlUriWithCredentials("postgresql://127.0.0.1:5432/myapp", "dev",
                                               "p@ss:w/ord 100%") ==
             "postgresql://dev:p%40ss%3Aw%2Ford%20100%25@127.0.0.1:5432/myapp",
         "BuildPostgresqlUriWithCredentials plain URI mismatch");

  Expect(dbpp::BuildPostgresqlUriWithCredentials("postgres://olduser@db.local:5432/app?sslmode=disable",
                                               "", "sec&ret") ==
             "postgres://olduser:sec%26ret@db.local:5432/app?sslmode=disable",
         "BuildPostgresqlUriWithCredentials existing user override mismatch");

  Expect(dbpp::BuildPostgresqlUriWithCredentials("host=127.0.0.1 port=5432 dbname=app", "dev",
                                               "a'b\\c") ==
             "host=127.0.0.1 port=5432 dbname=app user='dev' password='a\\'b\\\\c'",
         "BuildPostgresqlUriWithCredentials libpq conninfo mismatch");

  Expect(dbpp::IsSqlReadStatement("-- comment\n/* block */\nSELECT 1"),
         "IsSqlReadStatement should allow comments before SELECT");
  Expect(dbpp::IsSqlReadStatement("SHOW TABLES"), "SHOW should be read statement");
  Expect(dbpp::IsSqlReadStatement("DESCRIBE users"), "DESCRIBE should be read statement");
  Expect(dbpp::IsSqlReadStatement("desc users"), "desc should be read statement");
  Expect(dbpp::IsSqlReadStatement("INSERT INTO t VALUES (1) RETURNING id"),
         "RETURNING should be treated as returning rows");
  Expect(!dbpp::IsSqlReadStatement("DELETE FROM t WHERE id = 1"),
         "DELETE without RETURNING should not be read statement");

  const auto make_obj = [](std::string schema, std::string name, std::string table_type,
                           bool is_view) {
    dbpp::DbObject obj;
    obj.db_schema = std::move(schema);
    obj.name = std::move(name);
    obj.table_type = std::move(table_type);
    obj.is_view = is_view;
    return obj;
  };
  Expect(dbpp::BuildDropObjectSql(dbpp::Dialect::Sqlite,
                                  make_obj({}, "items", "TABLE", false)) ==
             "DROP TABLE \"items\"",
         "BuildDropObjectSql SQLite table mismatch");
  Expect(dbpp::BuildDropObjectSql(dbpp::Dialect::Sqlite,
                                  make_obj({}, "v_items", "VIEW", true)) ==
             "DROP VIEW \"v_items\"",
         "BuildDropObjectSql SQLite view mismatch");
  Expect(dbpp::BuildDropObjectSql(dbpp::Dialect::Postgresql,
                                  make_obj("public", "users", "BASE TABLE", false)) ==
             "DROP TABLE \"public\".\"users\"",
         "BuildDropObjectSql PostgreSQL table mismatch");
  Expect(dbpp::BuildDropObjectSql(dbpp::Dialect::Postgresql,
                                  make_obj("analytics", "daily", "MATERIALIZED VIEW", true)) ==
             "DROP MATERIALIZED VIEW \"analytics\".\"daily\"",
         "BuildDropObjectSql materialized view mismatch");
  Expect(dbpp::BuildDropObjectSql(dbpp::Dialect::Postgresql,
                                  make_obj("ext", "events", "FOREIGN TABLE", false)) ==
             "DROP FOREIGN TABLE \"ext\".\"events\"",
         "BuildDropObjectSql foreign table mismatch");
  Expect(dbpp::BuildDropObjectSql(dbpp::Dialect::Mysql,
                                  make_obj({}, "orders", "BASE TABLE", false)) ==
             "DROP TABLE `orders`",
         "BuildDropObjectSql MySQL table mismatch");
}

void TestDatabasePresetsAndUriComposer(const fs::path& dir) {
  const std::vector<dbpp::DatabasePreset>& presets = dbpp::SupportedDatabasePresets();
  Expect(!presets.empty(), "SupportedDatabasePresets must not be empty");

  auto find_preset = [&](std::string_view id) -> const dbpp::DatabasePreset* {
    for (const auto& preset : presets) {
      Expect(!preset.id.empty() && !preset.name.empty(), "Each preset needs id and name");
      if (preset.id == id) {
        return &preset;
      }
    }
    return nullptr;
  };

  const auto* sqlite = find_preset("sqlite");
  const auto* pg = find_preset("postgresql");
  const auto* mysql = find_preset("mysql");
  const auto* mariadb = find_preset("mariadb");
  const auto* duckdb = find_preset("duckdb");
  const auto* redis = find_preset("redis");
  const auto* mssql = find_preset("mssql");
  const auto* clickhouse = find_preset("clickhouse");
  const auto* flightsql = find_preset("flightsql");
  const auto* custom = find_preset("custom");
  Expect(sqlite && sqlite->builtin && pg && mysql && mariadb && duckdb && redis &&
             redis->builtin && mssql && clickhouse && flightsql && custom,
         "Expected core supported presets in SupportedDatabasePresets");

  if (redis != nullptr) {
    Expect(dbpp::ComposePresetUri(*redis, "127.0.0.1", "6379", "2") ==
               "redis://127.0.0.1:6379/2",
           "ComposePresetUri Redis mismatch");
    const auto matched_redis = dbpp::FindInstalledDriverForPreset(*redis, {});
    Expect(matched_redis.has_value() && matched_redis->path == "redis",
           "Builtin Redis preset must always resolve built-in redis driver");
  }

  if (pg != nullptr) {
    Expect(dbpp::ComposePresetUri(*pg, "::1", "5432", "app db") ==
               "postgresql://[::1]:5432/app%20db",
           "ComposePresetUri PostgreSQL IPv6/db encoding mismatch");
  }
  if (mysql != nullptr) {
    Expect(dbpp::ComposePresetUri(*mysql, "db.local", "3306", "") == "mysql://db.local:3306",
           "ComposePresetUri MySQL without db mismatch");
    Expect(dbpp::ComposePresetUri(*mysql, "db.local", "3306", "shop") ==
               "mysql://db.local:3306/shop",
           "ComposePresetUri MySQL with db mismatch");
  }
  if (mssql != nullptr) {
    Expect(dbpp::ComposePresetUri(*mssql, "10.0.0.5", "1433", "Sales DB") ==
               "mssql://10.0.0.5:1433?database=Sales%20DB",
           "ComposePresetUri SQL Server mismatch");
  }
  if (clickhouse != nullptr) {
    Expect(dbpp::ComposePresetUri(*clickhouse, "127.0.0.1", "8123", "analytics") ==
               "http://127.0.0.1:8123/?database=analytics",
           "ComposePresetUri ClickHouse mismatch");
  }

  if (sqlite != nullptr) {
    const auto matched_sqlite = dbpp::FindInstalledDriverForPreset(*sqlite, {});
    Expect(matched_sqlite.has_value() && !matched_sqlite->path.empty(),
           "Builtin SQLite preset must always resolve a driver");
  }
  if (pg != nullptr) {
    const std::vector<dbpp::AdbcDriverInfo> fake_drivers = {
        dbpp::AdbcDriverInfo{.name = "PostgreSQL",
                             .path = "postgresql",
                             .detail = "manifest · v1.13.0",
                             .version = "1.13.0",
                             .is_manifest = true},
    };
    const auto matched_pg = dbpp::FindInstalledDriverForPreset(*pg, fake_drivers);
    Expect(matched_pg.has_value() && matched_pg->path == "postgresql",
           "FindInstalledDriverForPreset should match manifest slug postgresql");
    Expect(!dbpp::FindInstalledDriverForPreset(*pg, {}).has_value(),
           "FindInstalledDriverForPreset should return nullopt when driver is absent");
  }
  (void)dir;
}

void TestUtf8ValidationAndSanitization() {
  Expect(dbpp::IsValidUtf8(""), "Empty string must be valid UTF-8");
  Expect(dbpp::IsValidUtf8("hello world"), "ASCII must be valid UTF-8");
  const std::string valid_utf8 = "连接失败: 数据库拒绝访问 🎉";
  Expect(dbpp::IsValidUtf8(valid_utf8), "Chinese + emoji must be valid UTF-8");
  Expect(dbpp::SanitizeUtf8(valid_utf8) == valid_utf8,
         "SanitizeUtf8 must preserve valid UTF-8 verbatim");

  // Raw GBK / CP936 bytes (e.g. from Windows FormatMessageA / gai_strerror).
  const std::string gbk_bytes = "\xD3\xC9\xD3\xDA\xC4\xBF\xB1\xEA";
  Expect(!dbpp::IsValidUtf8(gbk_bytes), "Raw GBK bytes must fail IsValidUtf8");
  const std::string cleaned_gbk = dbpp::SanitizeUtf8(gbk_bytes);
  Expect(dbpp::IsValidUtf8(cleaned_gbk) &&
             cleaned_gbk.find("\xEF\xBF\xBD") != std::string::npos,
         "SanitizeUtf8 must convert GBK bytes to valid UTF-8 with U+FFFD");

  const std::string mixed = "连接失败: \xD3\xC9\xD3\xDA";
  const std::string cleaned_mixed = dbpp::SanitizeUtf8(mixed);
  Expect(dbpp::IsValidUtf8(cleaned_mixed) && cleaned_mixed.starts_with("连接失败: "),
         "SanitizeUtf8 must keep valid UTF-8 prefix intact");

  for (const std::string_view bad : {
           std::string_view("\xC0\xAF", 2),         // overlong 2-byte
           std::string_view("\xED\xA0\x80", 3),     // UTF-16 high surrogate
           std::string_view("\xE8\xBF", 2),         // truncated 3-byte
           std::string_view("\xF4\x90\x80\x80", 4), // > U+10FFFF
       }) {
    Expect(!dbpp::IsValidUtf8(bad), "Malformed UTF-8 edge case must be rejected");
    Expect(dbpp::IsValidUtf8(dbpp::SanitizeUtf8(bad)),
           "Malformed UTF-8 edge case must sanitize cleanly");
  }
}

void TestDatabaseBrandDetection() {
  using dbpp::DatabaseBrand;
  using dbpp::DetectDatabaseBrand;
  using dbpp::Dialect;

  // All 15 preset IDs map 1:1.
  Expect(DetectDatabaseBrand("sqlite") == DatabaseBrand::Sqlite, "sqlite preset brand");
  Expect(DetectDatabaseBrand("postgresql") == DatabaseBrand::Postgresql, "postgresql preset brand");
  Expect(DetectDatabaseBrand("mysql") == DatabaseBrand::Mysql, "mysql preset brand");
  Expect(DetectDatabaseBrand("mariadb") == DatabaseBrand::Mariadb, "mariadb preset brand");
  Expect(DetectDatabaseBrand("duckdb") == DatabaseBrand::Duckdb, "duckdb preset brand");
  Expect(DetectDatabaseBrand("redis") == DatabaseBrand::Redis, "redis preset brand");
  Expect(DetectDatabaseBrand("mssql") == DatabaseBrand::Mssql, "mssql preset brand");
  Expect(DetectDatabaseBrand("clickhouse") == DatabaseBrand::Clickhouse, "clickhouse preset brand");
  Expect(DetectDatabaseBrand("flightsql") == DatabaseBrand::FlightSql, "flightsql preset brand");
  Expect(DetectDatabaseBrand("snowflake") == DatabaseBrand::Snowflake, "snowflake preset brand");
  Expect(DetectDatabaseBrand("bigquery") == DatabaseBrand::BigQuery, "bigquery preset brand");
  Expect(DetectDatabaseBrand("trino") == DatabaseBrand::Trino, "trino preset brand");
  Expect(DetectDatabaseBrand("databricks") == DatabaseBrand::Databricks, "databricks preset brand");
  Expect(DetectDatabaseBrand("redshift") == DatabaseBrand::Redshift, "redshift preset brand");
  Expect(DetectDatabaseBrand("custom") == DatabaseBrand::Custom, "custom preset brand");

  // Precedence checks: Redshift over PostgreSQL, MariaDB over MySQL.
  Expect(DetectDatabaseBrand("redshift", "postgresql://cluster.redshift:5439/dev",
                             Dialect::Postgresql) == DatabaseBrand::Redshift,
         "redshift driver must win over postgresql URI/dialect");
  Expect(DetectDatabaseBrand("", "postgresql://my-cluster.redshift.amazonaws.com:5439/dev") ==
             DatabaseBrand::Redshift,
         "redshift URI must be detected before postgresql");
  Expect(DetectDatabaseBrand("mariadb", "mysql://127.0.0.1:3306/app", Dialect::Mysql) ==
             DatabaseBrand::Mariadb,
         "mariadb driver must win over mysql URI/dialect");

  // Shared library path, URI scheme, and dialect fallbacks.
  Expect(DetectDatabaseBrand("/usr/lib/libadbc_driver_duckdb.so") == DatabaseBrand::Duckdb,
         "duckdb shared library path detection");
  Expect(DetectDatabaseBrand("adbc_driver_sqlite.dll") == DatabaseBrand::Sqlite,
         "sqlite Windows DLL path detection");
  Expect(DetectDatabaseBrand("", "rediss://cache.example.com:6379/1") == DatabaseBrand::Redis,
         "rediss:// URI detection");
  Expect(DetectDatabaseBrand("", "demo.sqlite") == DatabaseBrand::Sqlite,
         "sqlite file extension detection");
  Expect(DetectDatabaseBrand("", "", Dialect::Redis) == DatabaseBrand::Redis,
         "dialect fallback to Redis");
  Expect(DetectDatabaseBrand("", "", Dialect::Generic) == DatabaseBrand::Custom,
         "generic fallback to Custom");
}

void TestI18nResolutionAndFormatting() {
  struct LangRestoreGuard {
    dbpp::UiLanguage prev = dbpp::ActiveUiLanguage();
    ~LangRestoreGuard() { dbpp::SetActiveUiLanguage(prev); }
  } restore_guard;

  using dbpp::LanguageMode;
  using dbpp::ResolveUiLanguage;
  using dbpp::UiLanguage;
  Expect(ResolveUiLanguage(LanguageMode::System, "zh") == UiLanguage::Zh, "system zh");
  Expect(ResolveUiLanguage(LanguageMode::System, "zh-CN") == UiLanguage::Zh, "system zh-CN");
  Expect(ResolveUiLanguage(LanguageMode::System, "zh-Hans-CN") == UiLanguage::Zh,
         "system zh-Hans-CN");
  Expect(ResolveUiLanguage(LanguageMode::System, "ZH_cn") == UiLanguage::Zh, "system ZH_cn");
  Expect(ResolveUiLanguage(LanguageMode::System, "en-US") == UiLanguage::En, "system en-US");
  Expect(ResolveUiLanguage(LanguageMode::System, "fr") == UiLanguage::En, "system fr fallback");
  Expect(ResolveUiLanguage(LanguageMode::System, "") == UiLanguage::En, "system empty fallback");
  Expect(ResolveUiLanguage(LanguageMode::Zh, "en-US") == UiLanguage::Zh, "explicit zh");
  Expect(ResolveUiLanguage(LanguageMode::En, "zh-CN") == UiLanguage::En, "explicit en");

  dbpp::SetActiveUiLanguage(UiLanguage::En);
  Expect(dbpp::ActiveUiLanguage() == UiLanguage::En && !dbpp::IsChineseUi(),
         "ActiveUiLanguage En check");
  Expect(dbpp::Tr("Connected", "已连接") == "Connected", "Tr En check");
  Expect(dbpp::TrFmt("Loaded {0} of {1} {{ok}}", "已加载 {0}/{1} {{ok}}", {"12", "40"}) ==
             "Loaded 12 of 40 {ok}",
         "TrFmt En check");

  dbpp::SetActiveUiLanguage(UiLanguage::Zh);
  Expect(dbpp::ActiveUiLanguage() == UiLanguage::Zh && dbpp::IsChineseUi(),
         "ActiveUiLanguage Zh check");
  Expect(dbpp::Tr("Connected", "已连接") == "已连接", "Tr Zh check");
  Expect(dbpp::TrFmt("Loaded {0} of {1} {{ok}}", "已加载 {0}/{1} {{ok}}", {"12", "40"}) ==
             "已加载 12/40 {ok}",
         "TrFmt Zh check");
}

void TestUriDecomposeAndDatabaseHierarchy() {
  const auto& pg = dbpp::ResolvePresetForProfile("postgresql", "postgresql://127.0.0.1:5432/app");
  Expect(pg.id == "postgresql", "ResolvePresetForProfile postgresql");
  const auto pg_parts = dbpp::DecomposePresetUri(
      pg, "postgresql://alice:s%40cret@db.example.com:5433/app%20db?sslmode=require");
  Expect(pg_parts.parsed && pg_parts.host == "db.example.com" && pg_parts.port == "5433" &&
             pg_parts.database == "app db" && pg_parts.username == "alice" &&
             pg_parts.password == "s@cret" && pg_parts.extra_query == "sslmode=require",
         "DecomposePresetUri postgresql fields mismatch");
  Expect(dbpp::ComposePresetUri(pg, pg_parts.host, pg_parts.port, pg_parts.database,
                                pg_parts.extra_query) ==
             "postgresql://db.example.com:5433/app%20db?sslmode=require",
         "ComposePresetUri round-trip with extra query mismatch");

  const auto& mysql = dbpp::ResolvePresetForProfile("mysql", "mysql://127.0.0.1:3306/shop");
  Expect(mysql.id == "mysql", "ResolvePresetForProfile mysql");
  const auto mysql_v6 = dbpp::DecomposePresetUri(mysql, "mysql://[::1]:3307/orders");
  Expect(mysql_v6.parsed && mysql_v6.host == "::1" && mysql_v6.port == "3307" &&
             mysql_v6.database == "orders",
         "DecomposePresetUri IPv6 mismatch");
  Expect(dbpp::ComposePresetUri(mysql, mysql_v6.host, mysql_v6.port, mysql_v6.database) ==
             "mysql://[::1]:3307/orders",
         "ComposePresetUri IPv6 mismatch");

  const auto& mssql = dbpp::ResolvePresetForProfile("mssql", "mssql://127.0.0.1:1433");
  const auto mssql_parts = dbpp::DecomposePresetUri(
      mssql, "mssql://10.0.0.2:1434?database=erp&encrypt=true");
  Expect(mssql_parts.parsed && mssql_parts.host == "10.0.0.2" && mssql_parts.port == "1434" &&
             mssql_parts.database == "erp" && mssql_parts.extra_query == "encrypt=true",
         "DecomposePresetUri mssql query database mismatch");
  Expect(dbpp::ComposePresetUri(mssql, mssql_parts.host, mssql_parts.port, mssql_parts.database,
                                mssql_parts.extra_query) ==
             "mssql://10.0.0.2:1434?database=erp&encrypt=true",
         "ComposePresetUri mssql round-trip mismatch");

  const auto& ch = dbpp::ResolvePresetForProfile("clickhouse", "http://127.0.0.1:8123/");
  const auto ch_parts =
      dbpp::DecomposePresetUri(ch, "http://ch.local:8123/?database=analytics&ssl=1");
  Expect(ch_parts.parsed && ch_parts.host == "ch.local" && ch_parts.port == "8123" &&
             ch_parts.database == "analytics" && ch_parts.extra_query == "ssl=1",
         "DecomposePresetUri clickhouse mismatch");

  const auto& redis = dbpp::ResolvePresetForProfile("redis", "redis://127.0.0.1:6379/2");
  Expect(redis.id == "redis", "ResolvePresetForProfile redis");
  const auto redis_parts = dbpp::DecomposePresetUri(redis, "redis://:tok%2F1@127.0.0.1:6379/2");
  Expect(redis_parts.parsed && redis_parts.password == "tok/1" && redis_parts.database == "2",
         "DecomposePresetUri redis token password mismatch");

  dbpp::DbObject mysql_cat_obj;
  mysql_cat_obj.name = "users";
  mysql_cat_obj.catalog = "shop";
  Expect(dbpp::ObjectDatabaseName(dbpp::Dialect::Mysql, mysql_cat_obj) == "shop",
         "ObjectDatabaseName mysql catalog fallback");
  Expect(dbpp::QualifiedName(dbpp::Dialect::Mysql, mysql_cat_obj) == "`shop`.`users`",
         "QualifiedName mysql catalog fallback");

  dbpp::DbObject mysql_def_obj;
  mysql_def_obj.name = "users";
  mysql_def_obj.catalog = "def";
  mysql_def_obj.db_schema = "app_db";
  Expect(dbpp::ObjectDatabaseName(dbpp::Dialect::Mysql, mysql_def_obj) == "app_db",
         "ObjectDatabaseName mysql schema over def catalog");

  Expect(dbpp::IsSystemDatabaseOrSchema("information_schema") &&
             dbpp::IsSystemDatabaseOrSchema("PERFORMANCE_SCHEMA") &&
             dbpp::IsSystemDatabaseOrSchema("pg_catalog") &&
             dbpp::IsSystemDatabaseOrSchema("pg_temp_3") &&
             !dbpp::IsSystemDatabaseOrSchema("shop"),
         "IsSystemDatabaseOrSchema classification check");
}

void TestConnectionLostAndSessionLifecycle(const fs::path& dir, const std::string& driver_path) {
  // Positive disconnect / transport failure patterns.
  Expect(dbpp::IsConnectionLostError(10 /* ADBC_STATUS_IO */, ""),
         "ADBC_STATUS_IO must be treated as connection_lost");
  for (const std::string_view msg : {
           "sql: connection is already closed",
           "MySQL server has gone away",
           "driver: bad connection",
           "invalid connection",
           "FATAL: terminating connection due to administrator command",
           "server closed the connection unexpectedly",
           "no connection to the server",
           "write unix: broken pipe",
           "read tcp 127.0.0.1: connection reset by peer",
           "unexpected EOF",
           "use of closed network connection",
           "Redis 服务器已关闭连接",
           "连接已关闭",
       }) {
    Expect(dbpp::IsConnectionLostError(0, msg),
           std::string("Expected connection-lost classification for: ") + std::string(msg));
  }

  // Negative cases: ordinary SQL/schema errors must NOT drop the session.
  for (const std::string_view msg : {
           "syntax error at or near \"SELECT\"",
           "no such table: users",
           "no such column: eof_marker",
           "UNIQUE constraint failed: items.id",
           "permission denied for table secrets",
           "",
       }) {
    Expect(!dbpp::IsConnectionLostError(0, msg),
           std::string("Ordinary error misclassified as connection_lost: ") + std::string(msg));
  }

  // Health-check eligibility across local vs remote backends.
  Expect(!dbpp::ShouldHealthCheckSession(dbpp::Dialect::Sqlite, (dir / "x.sqlite").string()),
         "SQLite should not trigger periodic network health checks");
  Expect(!dbpp::ShouldHealthCheckSession(dbpp::Dialect::Duckdb, "duckdb::memory:"),
         "DuckDB should not trigger periodic network health checks");
  Expect(dbpp::ShouldHealthCheckSession(dbpp::Dialect::Mysql, "mysql://127.0.0.1:3306/app"),
         "MySQL should enable periodic health checks");
  Expect(dbpp::ShouldHealthCheckSession(dbpp::Dialect::Postgresql,
                                        "postgresql://127.0.0.1:5432/app"),
         "PostgreSQL should enable periodic health checks");
  Expect(dbpp::ShouldHealthCheckSession(dbpp::Dialect::Redis, "redis://127.0.0.1:6379/0"),
         "Redis should enable periodic health checks");
  Expect(dbpp::ShouldHealthCheckSession(dbpp::Dialect::Generic, "grpc+tls://host:31337"),
         "Generic scheme URI should enable periodic health checks");

  // Session RAII / explicit close lifecycle on a real SQLite ADBC session.
  const fs::path db_path = dir / "lifecycle.sqlite";
  const dbpp::AdbcOpenResult opened = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = driver_path,
      .uri = db_path.string(),
      .username = {},
      .password = {},
      .title = "lifecycle",
  });
  Expect(opened.error.empty() && opened.session != nullptr,
         "Open SQLite session for lifecycle test failed: " + opened.error);
  if (!opened.session) {
    return;
  }
  Expect(opened.session->IsAlive() && !opened.session->IsClosed(),
         "Newly opened session must be alive and not closed");

  const dbpp::AdbcQueryResult ping = dbpp::PingSessionBlocking(*opened.session);
  Expect(ping.error.empty() && !ping.connection_lost && opened.session->IsAlive(),
         "PingSessionBlocking on live SQLite session failed: " + ping.error);

  opened.session->MarkClosed();
  opened.session->ReleaseResourcesBlocking();
  Expect(opened.session->IsClosed() && !opened.session->IsAlive() &&
             !opened.session->connection_created && !opened.session->database_created,
         "MarkClosed + ReleaseResourcesBlocking must release handles and clear flags");

  // Idempotent second release must be safe.
  opened.session->ReleaseResourcesBlocking();

  const dbpp::AdbcQueryResult exec_after_close =
      dbpp::ExecuteBlocking(*opened.session, "SELECT 1", true, 10);
  Expect(exec_after_close.connection_lost && !exec_after_close.error.empty(),
         "ExecuteBlocking on closed session must return connection_lost");

  const dbpp::AdbcObjectsResult objs_after_close = dbpp::LoadObjectsBlocking(*opened.session);
  Expect(objs_after_close.connection_lost && !objs_after_close.error.empty(),
         "LoadObjectsBlocking on closed session must return connection_lost");

  dbpp::DbObject test_obj;
  test_obj.name = "t";

  const dbpp::AdbcSchemaResult schema_after_close =
      dbpp::LoadSchemaBlocking(*opened.session, test_obj);
  Expect(schema_after_close.connection_lost && !schema_after_close.error.empty(),
         "LoadSchemaBlocking on closed session must return connection_lost");

  const dbpp::TableModel table_after_close =
      dbpp::LoadTableBlocking(*opened.session, 1, test_obj, 0, 50, std::nullopt, {});
  Expect(table_after_close.connection_lost && !table_after_close.error.empty(),
         "LoadTableBlocking on closed session must return connection_lost");
}

void TestSqlFilterNormalizationAndFiltering(const fs::path& dir, const std::string& driver_path) {
  Expect(dbpp::NormalizeSqlFilter("  id > 1  ") == "id > 1", "Trim whitespace");
  Expect(dbpp::NormalizeSqlFilter("WHERE id > 1") == "id > 1", "Strip uppercase WHERE");
  Expect(dbpp::NormalizeSqlFilter("where\n (id = 2 OR id = 3);;") == "(id = 2 OR id = 3)",
         "Strip lowercase where and trailing semicolons");
  Expect(dbpp::NormalizeSqlFilter("where_col = 5") == "where_col = 5",
         "Do not strip column name starting with where_");
  Expect(dbpp::NormalizeSqlFilter("   ;  ") == "", "Empty after stripping punctuation");

  const fs::path db_path = dir / "filter_test.sqlite";
  auto opened = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = driver_path,
      .uri = db_path.string(),
  });
  Expect(opened.error.empty() && opened.session != nullptr,
         "Open SQLite for filter test failed: " + opened.error);
  if (!opened.session) {
    return;
  }
  Expect(dbpp::ExecuteBlocking(
             *opened.session,
             "CREATE TABLE products (id INTEGER PRIMARY KEY, name TEXT, price REAL);", false, 0)
             .error.empty(),
         "CREATE TABLE products failed");
  Expect(dbpp::ExecuteBlocking(
             *opened.session,
             "INSERT INTO products VALUES (1, 'Apple', 5.0), (2, 'Apricot', 12.5), "
             "(3, 'Banana', 20.0);",
             false, 0)
             .error.empty(),
         "Seed products failed");

  dbpp::DbObject obj;
  obj.name = "products";

  const dbpp::TableModel f1 =
      dbpp::LoadTableBlocking(*opened.session, 1, obj, 0, 50, std::nullopt,
                              "WHERE price > 10 AND name LIKE 'A%';");
  Expect(f1.error.empty() && f1.total == 1 && f1.filter == "price > 10 AND name LIKE 'A%'" &&
             f1.snapshot && f1.snapshot->rows.size() == 1 &&
             f1.snapshot->rows[0].values[1].text == "Apricot",
         "SQL filter with WHERE and semicolon should match Apricot: " + f1.error);

  const dbpp::TableModel cleared =
      dbpp::LoadTableBlocking(*opened.session, 1, obj, 0, 50, std::nullopt, "   ");
  Expect(cleared.error.empty() && cleared.total == 3 && cleared.filter.empty(),
         "Clearing filter should restore all 3 rows");

  Expect(dbpp::kPageSize == 100, "Default page size should be 100");
  const dbpp::TableModel page0 =
      dbpp::LoadTableBlocking(*opened.session, 1, obj, 0, 2, std::nullopt, {});
  Expect(page0.error.empty() && page0.total == 3 && page0.page == 0 && page0.page_size == 2 &&
             page0.snapshot && page0.snapshot->rows.size() == 2 &&
             page0.elapsed_us >= 0 &&
             page0.executed_sql.find("LIMIT 2 OFFSET 0") != std::string::npos,
         "Page 0 with page_size=2 should return 2 rows and populate audit SQL/duration");
  const dbpp::TableModel page1 =
      dbpp::LoadTableBlocking(*opened.session, 1, obj, 1, 2, std::nullopt, {});
  Expect(page1.error.empty() && page1.total == 3 && page1.page == 1 && page1.page_size == 2 &&
             page1.snapshot && page1.snapshot->rows.size() == 1 &&
             page1.elapsed_us >= 0 &&
             page1.executed_sql.find("LIMIT 2 OFFSET 2") != std::string::npos,
         "Page 1 with page_size=2 should return 1 row and populate audit SQL/duration");
  const dbpp::TableModel default_page =
      dbpp::LoadTableBlocking(*opened.session, 1, obj, 0, dbpp::kPageSize, std::nullopt, {});
  Expect(default_page.error.empty() && default_page.page_size == 100 &&
             default_page.snapshot && default_page.snapshot->rows.size() == 3,
         "Default page size=100 should return all 3 rows");

  using SortSpec = std::remove_cvref_t<decltype(*default_page.sort)>;
  using SortDir = decltype(SortSpec::direction);

  const dbpp::TableModel sort_desc = dbpp::LoadTableBlocking(
      *opened.session, 1, obj, 0, dbpp::kPageSize,
      SortSpec{.column_id = "price", .direction = SortDir::Descending}, {});
  Expect(sort_desc.error.empty() && sort_desc.sort.has_value() && sort_desc.snapshot &&
             sort_desc.snapshot->rows.size() == 3 &&
             sort_desc.snapshot->rows[0].values[1].text == "Banana" &&
             sort_desc.snapshot->rows[2].values[1].text == "Apple",
         "Descending sort on price should order Banana first and Apple last");

  const dbpp::TableModel sort_asc = dbpp::LoadTableBlocking(
      *opened.session, 1, obj, 0, dbpp::kPageSize,
      SortSpec{.column_id = "name", .direction = SortDir::Ascending}, {});
  Expect(sort_asc.error.empty() && sort_asc.sort.has_value() && sort_asc.snapshot &&
             sort_asc.snapshot->rows.size() == 3 &&
             sort_asc.snapshot->rows[0].values[1].text == "Apple",
         "Ascending sort on name should put Apple first");
}

void TestEditTableDdlGenerationAndExecution(const fs::path& dir, const std::string& driver_path) {
  dbpp::DbObject sql_obj;
  sql_obj.name = "orders";
  sql_obj.db_schema = "public";
  Expect(dbpp::BuildRenameTableSql(dbpp::Dialect::Postgresql, sql_obj, "orders_v2") ==
             R"(ALTER TABLE "public"."orders" RENAME TO "orders_v2")",
         "BuildRenameTableSql mismatch");
  Expect(dbpp::BuildAddColumnSql(dbpp::Dialect::Postgresql, sql_obj, "note", "TEXT", false, "''") ==
             R"(ALTER TABLE "public"."orders" ADD COLUMN "note" TEXT NOT NULL DEFAULT '')",
         "BuildAddColumnSql mismatch");
  Expect(dbpp::BuildRenameColumnSql(dbpp::Dialect::Mysql, dbpp::DbObject{.name = "orders"}, "old",
                                    "new_col") ==
             "ALTER TABLE `orders` RENAME COLUMN `old` TO `new_col`",
         "BuildRenameColumnSql MySQL mismatch");
  Expect(dbpp::BuildDropColumnSql(dbpp::Dialect::Sqlite, dbpp::DbObject{.name = "orders"},
                                  "unused") == R"(ALTER TABLE "orders" DROP COLUMN "unused")",
         "BuildDropColumnSql SQLite mismatch");
  Expect(dbpp::BuildAlterColumnTypeSql(dbpp::Dialect::Sqlite, dbpp::DbObject{.name = "orders"},
                                       "a", "TEXT", true)
             .empty(),
         "SQLite should not emit direct ALTER COLUMN TYPE");
  Expect(dbpp::BuildAlterColumnTypeSql(dbpp::Dialect::Mysql, dbpp::DbObject{.name = "orders"},
                                       "a", "VARCHAR(64)", false) ==
             "ALTER TABLE `orders` MODIFY COLUMN `a` VARCHAR(64) NOT NULL",
         "MySQL MODIFY COLUMN mismatch");

  const fs::path db_path = dir / "edit_table.sqlite";
  auto opened = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = driver_path,
      .uri = db_path.string(),
  });
  Expect(opened.error.empty() && opened.session != nullptr, "Open SQLite edit test failed");
  if (!opened.session) {
    return;
  }
  dbpp::DbObject tbl;
  tbl.name = "users";
  Expect(dbpp::ExecuteBlocking(*opened.session,
                               "CREATE TABLE users (id INTEGER PRIMARY KEY, title TEXT);", false,
                               0)
             .error.empty(),
         "CREATE TABLE users failed");
  Expect(dbpp::ExecuteBlocking(
             *opened.session,
             dbpp::BuildAddColumnSql(dbpp::Dialect::Sqlite, tbl, "age", "INTEGER", true, "0"),
             false, 0)
             .error.empty(),
         "ADD COLUMN failed");
  Expect(dbpp::ExecuteBlocking(
             *opened.session,
             dbpp::BuildRenameColumnSql(dbpp::Dialect::Sqlite, tbl, "title", "full_name"), false,
             0)
             .error.empty(),
         "RENAME COLUMN failed");
  Expect(dbpp::ExecuteBlocking(
             *opened.session, dbpp::BuildDropColumnSql(dbpp::Dialect::Sqlite, tbl, "age"), false,
             0)
             .error.empty(),
         "DROP COLUMN failed");
  Expect(dbpp::ExecuteBlocking(
             *opened.session,
             dbpp::BuildRenameTableSql(dbpp::Dialect::Sqlite, tbl, "members"), false, 0)
             .error.empty(),
         "RENAME TABLE failed");

  dbpp::DbObject renamed;
  renamed.name = "members";
  const dbpp::AdbcSchemaResult schema = dbpp::LoadSchemaBlocking(*opened.session, renamed);
  Expect(schema.error.empty() && schema.columns.size() == 2 &&
             schema.columns[1].name == "full_name",
         "Edited table schema mismatch after DDL sequence");
}

void TestMcpSettingsPersistence(const fs::path& dir) {
  const fs::path cfg_db = dir / "mcp_settings.sqlite3";
  dbpp::TraySettings saved;
  saved.mcp_enabled = true;
  saved.mcp_host = "127.0.0.1";
  saved.mcp_port = 19200;
  saved.mcp_read_only = false;
  Expect(dbpp::SaveTraySettings(cfg_db.string(), saved), "SaveTraySettings for MCP failed");

  const dbpp::TraySettings loaded = dbpp::LoadTraySettings(cfg_db.string());
  Expect(loaded.mcp_enabled && loaded.mcp_host == "127.0.0.1" && loaded.mcp_port == 19200 &&
             !loaded.mcp_read_only,
         "LoadTraySettings MCP roundtrip mismatch");

  Expect(dbpp::TryParsePageSize("250") == std::optional<std::int64_t>{250} &&
             dbpp::TryParsePageSize("100001") == std::optional<std::int64_t>{100001} &&
             dbpp::TryParsePageSize("1000000") == std::optional<std::int64_t>{1000000},
         "TryParsePageSize valid input mismatch");
  Expect(!dbpp::TryParsePageSize("0").has_value() &&
             !dbpp::TryParsePageSize("-5").has_value() &&
             !dbpp::TryParsePageSize("9223372036854775807").has_value() &&
             !dbpp::TryParsePageSize("abc").has_value(),
         "TryParsePageSize should reject non-positive, overflowing, or non-numeric values");
  Expect(dbpp::ParsePageSize("bad", 200) == 200, "ParsePageSize fallback mismatch");

  dbpp::TraySettings page_saved = loaded;
  page_saved.default_page_size = 250000;
  Expect(dbpp::SaveTraySettings(cfg_db.string(), page_saved),
         "SaveTraySettings default_page_size failed");
  const dbpp::TraySettings page_loaded = dbpp::LoadTraySettings(cfg_db.string());
  Expect(page_loaded.default_page_size == 250000, "LoadTraySettings default_page_size mismatch");
}

void TestSqlDatabaseContextHelpers() {
  Expect(dbpp::BuildSwitchDatabaseSql(dbpp::Dialect::Mysql, "app_db") == "USE `app_db`",
         "MySQL switch DB SQL mismatch");
  Expect(dbpp::BuildSwitchDatabaseSql(dbpp::Dialect::Postgresql, "analytics") ==
             "SET search_path TO \"analytics\"",
         "PostgreSQL switch schema SQL mismatch");
  Expect(dbpp::BuildSwitchDatabaseSql(dbpp::Dialect::Postgresql, "prod.analytics") ==
             "SET search_path TO \"analytics\"",
         "PostgreSQL catalog.schema switch SQL mismatch");
  Expect(dbpp::BuildSwitchDatabaseSql(dbpp::Dialect::Duckdb, "main") == "USE \"main\"",
         "DuckDB switch DB SQL mismatch");
  Expect(dbpp::BuildSwitchDatabaseSql(dbpp::Dialect::Generic, "cat.sch") ==
             "USE \"cat\".\"sch\"",
         "Generic multi-part switch DB SQL mismatch");
  Expect(dbpp::BuildSwitchDatabaseSql(dbpp::Dialect::Sqlite, "main").empty(),
         "SQLite switch DB SQL should be empty");
  Expect(dbpp::BuildSwitchDatabaseSql(dbpp::Dialect::Redis, "db0").empty(),
         "Redis switch DB SQL should be empty");

  const auto make_obj = [](std::string name, std::string catalog, std::string schema) {
    dbpp::DbObject obj;
    obj.name = std::move(name);
    obj.catalog = std::move(catalog);
    obj.db_schema = std::move(schema);
    return obj;
  };

  dbpp::DbSession sqlite_empty;
  sqlite_empty.dialect = dbpp::Dialect::Sqlite;
  sqlite_empty.uri = "sample.sqlite3";
  Expect(dbpp::ListSessionDatabases(sqlite_empty) == std::vector<std::string>{"main"} &&
             dbpp::DefaultSessionDatabase(sqlite_empty) == "main",
         "Empty SQLite session database list/default mismatch");

  dbpp::DbSession mysql_session;
  mysql_session.dialect = dbpp::Dialect::Mysql;
  mysql_session.uri = "mysql://127.0.0.1:3306/orders";
  mysql_session.objects = {
      make_obj("user", "def", "mysql"),
      make_obj("metrics", "def", "analytics"),
      make_obj("line_items", "def", "orders"),
  };
  const auto mysql_dbs = dbpp::ListSessionDatabases(mysql_session);
  Expect(mysql_dbs == std::vector<std::string>{"orders", "analytics", "mysql"} &&
             dbpp::DefaultSessionDatabase(mysql_session) == "orders",
         "MySQL session database ordering/default mismatch");

  dbpp::DbSession pg_session;
  pg_session.dialect = dbpp::Dialect::Postgresql;
  pg_session.uri = "postgresql://127.0.0.1:5432/appdb";
  pg_session.objects = {
      make_obj("pg_class", "appdb", "pg_catalog"),
      make_obj("events", "appdb", "app"),
      make_obj("users", "appdb", "public"),
  };
  Expect(dbpp::DefaultSessionDatabase(pg_session) == "public",
         "PostgreSQL session default schema should prefer public");

  dbpp::DbSession redis_session;
  redis_session.dialect = dbpp::Dialect::Redis;
  redis_session.objects = {
      make_obj("db0", "", ""),
      make_obj("db1", "", ""),
  };
  Expect(dbpp::ListSessionDatabases(redis_session) == std::vector<std::string>{"db0", "db1"} &&
             dbpp::DefaultSessionDatabase(redis_session) == "db0",
         "Redis session database list/default mismatch");
}

void TestImplicitSqlLimitAndExecution(const fs::path& dir, const std::string& driver_path) {
  {
    const auto r = dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Sqlite, "SELECT * FROM items", 100);
    Expect(r.applied && r.effective_limit == 100 && r.sql == "SELECT * FROM items\nLIMIT 101",
           "Bare SELECT implicit LIMIT rewrite mismatch");
  }
  {
    const auto r = dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Sqlite, "SELECT * FROM items", 500000);
    Expect(r.applied && r.effective_limit == 500000 && r.sql == "SELECT * FROM items\nLIMIT 500001",
           "Large integer implicit LIMIT rewrite mismatch");
  }
  {
    const auto r = dbpp::ApplyImplicitSqlLimit(
        dbpp::Dialect::Sqlite, "SELECT * FROM items; -- trailing comment", 50);
    Expect(r.applied && r.effective_limit == 50 && r.sql == "SELECT * FROM items\nLIMIT 51",
           "SELECT with semicolon and trailing comment rewrite mismatch");
  }
  {
    Expect(!dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Sqlite, "SELECT * FROM items LIMIT 10", 100)
                .applied,
           "Explicit LIMIT should not be rewritten");
    Expect(!dbpp::ApplyImplicitSqlLimit(
                dbpp::Dialect::Postgresql, "SELECT * FROM items FETCH FIRST 10 ROWS ONLY", 100)
                .applied,
           "FETCH FIRST should not be rewritten");
    Expect(!dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Generic, "SELECT TOP 10 * FROM items", 100)
                .applied,
           "SELECT TOP should not be rewritten");
    Expect(!dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Postgresql, "SELECT * FROM items FOR UPDATE",
                                        100)
                .applied,
           "FOR UPDATE should not be rewritten");
    Expect(!dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Postgresql, "SELECT * FROM items OFFSET 5",
                                        100)
                .applied,
           "OFFSET should not be rewritten");
  }
  {
    const auto r = dbpp::ApplyImplicitSqlLimit(
        dbpp::Dialect::Sqlite, "SELECT * FROM (SELECT * FROM items LIMIT 5) t", 100);
    Expect(r.applied &&
               r.sql == "SELECT * FROM (SELECT * FROM items LIMIT 5) t\nLIMIT 101",
           "Subquery LIMIT should still allow outer implicit LIMIT");
  }
  {
    const auto r = dbpp::ApplyImplicitSqlLimit(
        dbpp::Dialect::Sqlite, "SELECT 'LIMIT 5' AS txt FROM items", 100);
    Expect(r.applied && r.sql == "SELECT 'LIMIT 5' AS txt FROM items\nLIMIT 101",
           "Quoted LIMIT literal should not block outer implicit LIMIT");
  }
  {
    const auto r = dbpp::ApplyImplicitSqlLimit(
        dbpp::Dialect::Sqlite, "WITH cte AS (SELECT * FROM items) SELECT * FROM cte", 100);
    Expect(r.applied &&
               r.sql == "WITH cte AS (SELECT * FROM items) SELECT * FROM cte\nLIMIT 101",
           "Read-only WITH CTE implicit LIMIT mismatch");
  }
  {
    Expect(!dbpp::ApplyImplicitSqlLimit(
                dbpp::Dialect::Postgresql, "WITH c AS (SELECT 1) INSERT INTO t SELECT * FROM c",
                100)
                .applied,
           "Writable WITH CTE must not be rewritten");
    Expect(!dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Sqlite, "PRAGMA table_info(items)", 100)
                .applied,
           "PRAGMA must not be rewritten");
    Expect(!dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Sqlite, "SELECT 1; SELECT 2;", 100)
                .applied,
           "Multi-statement SQL must not be rewritten");
    Expect(!dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Redis, "KEYS *", 100).applied,
           "Redis dialect must not rewrite queries");
  }

  const fs::path db_file = dir / "implicit_limit_test.sqlite3";
  const dbpp::AdbcOpenResult opened = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = driver_path,
      .uri = db_file.string(),
      .username = {},
      .password = {},
      .title = {},
  });
  Expect(opened.session != nullptr, "Failed to open SQLite DB for implicit LIMIT test");
  if (!opened.session) {
    return;
  }
  Expect(dbpp::ExecuteBlocking(*opened.session,
                               "CREATE TABLE items (id INTEGER PRIMARY KEY);", false, 0)
             .error.empty(),
         "CREATE TABLE items failed");
  for (int i = 1; i <= 15; ++i) {
    Expect(dbpp::ExecuteBlocking(*opened.session,
                                 "INSERT INTO items (id) VALUES (" + std::to_string(i) + ");",
                                 false, 0)
               .error.empty(),
           "INSERT item failed");
  }

  Expect(dbpp::FormatExecutionDuration(-1).empty(), "Negative duration should format empty");
  Expect(dbpp::FormatExecutionDuration(0) == "0.0 ms" &&
             dbpp::FormatExecutionDuration(420) == "0.4 ms" &&
             dbpp::FormatExecutionDuration(12340) == "12 ms" &&
             dbpp::FormatExecutionDuration(1250000) == "1.25 s",
         "FormatExecutionDuration unit formatting mismatch");
  Expect(dbpp::CompactSqlPreview("SELECT *\n  FROM items\nLIMIT 6 OFFSET 5") ==
             "SELECT * FROM items LIMIT 6 OFFSET 5",
         "CompactSqlPreview whitespace normalization mismatch");
  Expect(dbpp::CompactSqlPreview("SELECT abcdefghij", 10) == "SELECT abc...",
         "CompactSqlPreview byte truncation mismatch");

  const auto limited =
      dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Sqlite, "SELECT * FROM items ORDER BY id", 5);
  Expect(limited.applied && limited.effective_offset == 0 &&
             limited.sql == "SELECT * FROM items ORDER BY id\nLIMIT 6",
         "Expected implicit LIMIT 5 on page 0");
  const dbpp::AdbcQueryResult five =
      dbpp::ExecuteBlocking(*opened.session, limited.sql, true, 5);
  Expect(five.error.empty() && five.elapsed_us >= 0 && five.rows.size() == 5 && five.truncated &&
             five.rows.front().values.front().text == "1" &&
             five.rows.back().values.front().text == "5",
         "Expected 5 rows (1..5) with truncated=true under implicit LIMIT 5 page 0");

  const auto page1 =
      dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Sqlite, "SELECT * FROM items ORDER BY id", 5, 1);
  Expect(page1.applied && page1.effective_offset == 5 &&
             page1.sql == "SELECT * FROM items ORDER BY id\nLIMIT 6 OFFSET 5",
         "Expected implicit LIMIT 5 OFFSET 5 on page 1");
  const dbpp::AdbcQueryResult five_p1 =
      dbpp::ExecuteBlocking(*opened.session, page1.sql, true, 5);
  Expect(five_p1.error.empty() && five_p1.elapsed_us >= 0 && five_p1.rows.size() == 5 &&
             five_p1.truncated && five_p1.rows.front().values.front().text == "6" &&
             five_p1.rows.back().values.front().text == "10",
         "Expected 5 rows (6..10) with truncated=true under implicit LIMIT 5 page 1");

  const auto page2 =
      dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Sqlite, "SELECT * FROM items ORDER BY id", 5, 2);
  Expect(page2.applied && page2.effective_offset == 10 &&
             page2.sql == "SELECT * FROM items ORDER BY id\nLIMIT 6 OFFSET 10",
         "Expected implicit LIMIT 5 OFFSET 10 on page 2");
  const dbpp::AdbcQueryResult five_p2 =
      dbpp::ExecuteBlocking(*opened.session, page2.sql, true, 5);
  Expect(five_p2.error.empty() && five_p2.rows.size() == 5 && !five_p2.truncated &&
             five_p2.rows.front().values.front().text == "11" &&
             five_p2.rows.back().values.front().text == "15",
         "Expected 5 rows (11..15) with truncated=false under implicit LIMIT 5 page 2");

  const auto larger =
      dbpp::ApplyImplicitSqlLimit(dbpp::Dialect::Sqlite, "SELECT * FROM items ORDER BY id", 20);
  const dbpp::AdbcQueryResult all =
      dbpp::ExecuteBlocking(*opened.session, larger.sql, true, 20);
  Expect(all.error.empty() && all.rows.size() == 15 && !all.truncated,
         "Expected all 15 rows with truncated=false under implicit LIMIT 20");
}

} // namespace

int main() {
  ScopedTempDir temp;
  const std::string driver_path = dbpp::DefaultSqliteDriverPath();

  TestDefaultDriverAndSqliteCrud(temp.path, driver_path);
  TestSha256AndYamlParser(temp.path);
  TestDriverInstallDiscoverLoadUninstall(temp.path, driver_path);
  TestPostgresUriAndSqlReadDetection();
  TestDatabasePresetsAndUriComposer(temp.path);
  TestUtf8ValidationAndSanitization();
  TestDatabaseBrandDetection();
  TestI18nResolutionAndFormatting();
  TestUriDecomposeAndDatabaseHierarchy();
  TestConnectionLostAndSessionLifecycle(temp.path, driver_path);
  TestSqlFilterNormalizationAndFiltering(temp.path, driver_path);
  TestEditTableDdlGenerationAndExecution(temp.path, driver_path);
  TestMcpSettingsPersistence(temp.path);
  TestSqlDatabaseContextHelpers();
  TestImplicitSqlLimitAndExecution(temp.path, driver_path);

  if (g_failures != 0) {
    std::cerr << g_failures << " test assertion(s) failed\n";
    return 1;
  }
  std::cout << "test_adbc_core passed\n";
  return 0;
}
