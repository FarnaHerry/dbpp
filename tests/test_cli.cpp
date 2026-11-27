#include <huxerui/huxerui.h>
#include <huxerui/sqlite.h>
#include <cstdio>
#include <cstdlib>
#include "test_cli_helpers.h"

import std;
import dbpp.settings;
import dbpp.types;
import dbpp.adbc;
import dbpp.driver_installer;
import dbpp.db;
import dbpp.config_db;
import dbpp.cli;

#include "ui/theme_palette.h"

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
    path = fs::temp_directory_path() / ("dbpp_test_cli_" + std::to_string(stamp));
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

struct CliRunOutput {
  int code = -1;
  std::string out;
  std::string err;
};

CliRunOutput InvokeCli(const std::vector<std::string>& args, std::string_view stdin_text = {}) {
  std::ostringstream out;
  std::ostringstream err;
  std::istringstream in{std::string(stdin_text)};
  const int code = dbpp::RunCli(args, out, err, in);
  return CliRunOutput{.code = code, .out = out.str(), .err = err.str()};
}

std::string PathToFileUrl(const fs::path& path) {
  std::string generic = fs::absolute(path).generic_string();
#if defined(_WIN32)
  if (!generic.empty() && generic.front() != '/') {
    generic.insert(generic.begin(), '/');
  }
#endif
  return "file://" + generic;
}

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

void WriteTextFile(const fs::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
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

void TestDispatchRules() {
  Expect(!dbpp::ShouldDispatchCli({}), "Empty args must launch GUI");
  Expect(!dbpp::ShouldDispatchCli({"/tmp/demo.sqlite"}),
         "Single file path arg must launch GUI for .desktop compatibility");
  Expect(!dbpp::ShouldDispatchCli({"gui"}), "gui subcommand launches GUI");
  Expect(!dbpp::ShouldDispatchCli({"open", "/tmp/demo.sqlite"}),
         "open subcommand launches GUI");
  Expect(dbpp::ShouldDispatchCli({"open", "--help"}),
         "open --help must print CLI help");
  Expect(dbpp::ShouldDispatchCli({"--help"}), "--help dispatches CLI");
  Expect(dbpp::ShouldDispatchCli({"-v"}), "-v dispatches CLI");
  Expect(dbpp::ShouldDispatchCli({"doctor"}), "doctor dispatches CLI");
  Expect(dbpp::ShouldDispatchCli({"query", "--sqlite", ":memory:", "SELECT 1"}),
         "query dispatches CLI");
}

void TestVersionHelpDoctor(const fs::path& dir) {
  ScopedEnvGuard data_dir_guard("DBPP_DATA_DIR", (dir / "appdata").string());

  const auto ver = InvokeCli({"version", "--json"});
  Expect(ver.code == 0, "version --json exit 0");
  Expect(ver.out.find("\"ok\":true") != std::string::npos, "version JSON has ok:true");
  Expect(ver.out.find("\"name\":\"dbpp\"") != std::string::npos, "version JSON has name");

  const auto help = InvokeCli({"--help"});
  Expect(help.code == 0, "--help exit 0");
  Expect(help.out.find("dbpp doctor") != std::string::npos, "help mentions doctor");

  const auto doc = InvokeCli({"doctor", "--json"});
  Expect(doc.code == 0, "doctor --json exit 0: " + doc.err);
  Expect(doc.out.find("\"sqlite_driver_exists\":true") != std::string::npos,
         "doctor reports bundled sqlite driver exists");
  Expect(doc.out.find("\"profiles_count\":0") != std::string::npos,
         "doctor reports 0 profiles in fresh temp dir");
}

void TestProfilesLifecycle(const fs::path& dir) {
  const fs::path profiles_file = dir / "dbpp.sqlite3";
  const fs::path sqlite_db = dir / "profile_target.sqlite";
  const std::string driver = dbpp::DefaultSqliteDriverPath();

  const auto saved = InvokeCli({
      "profile",
      "save",
      "local_demo",
      "--driver",
      driver,
      "--uri",
      sqlite_db.string(),
      "--user",
      "alice",
      "--profiles",
      profiles_file.string(),
      "--json",
  });
  Expect(saved.code == 0, "profile save failed: " + saved.err);
  Expect(saved.out.find("\"updated\":false") != std::string::npos,
         "first save should report updated:false");

  const std::string raw_sqlite = ReadBinaryFile(profiles_file);
  Expect(raw_sqlite.starts_with("SQLite format 3"), "dbpp.sqlite3 must be a valid SQLite file");

  const auto pw_saved = InvokeCli({
      "profile",
      "save",
      "auth_demo",
      "--driver",
      "postgresql",
      "--uri",
      "postgresql://host/db",
      "--user",
      "bob",
      "--password",
      "secret",
      "--profiles",
      profiles_file.string(),
      "--json",
  });
  Expect(pw_saved.code == 0 && pw_saved.out.find("\"has_password\":true") != std::string::npos &&
             pw_saved.out.find("secret") == std::string::npos,
         "profile save with --password must succeed without leaking secret: " + pw_saved.err);

  const auto auth_shown = InvokeCli({
      "profile",
      "show",
      "auth_demo",
      "--profiles",
      profiles_file.string(),
      "--json",
  });
  Expect(auth_shown.code == 0 &&
             auth_shown.out.find("\"has_password\":true") != std::string::npos &&
             auth_shown.out.find("secret") == std::string::npos,
         "profile show must report has_password:true without leaking plaintext password");

  const std::vector<dbpp::ConnectionProfile> loaded_profiles =
      dbpp::LoadConnectionProfiles(profiles_file.string());
  Expect(loaded_profiles.size() == 2 && loaded_profiles[1].name == "auth_demo" &&
             loaded_profiles[1].password == "secret",
         "LoadConnectionProfiles must recover saved password");

  // Backward compatibility: legacy 4-column rows load with empty password.
  {
    const fs::path legacy_file = dir / "legacy_connections.tsv";
    WriteTextFile(legacy_file,
                  "# db++ adbc connections v1\n"
                  "legacy_pg\tpostgresql\tpostgresql://127.0.0.1:5432/app\tpostgres\n");
    const std::vector<dbpp::ConnectionProfile> legacy_loaded =
        dbpp::LoadConnectionProfiles(legacy_file.string());
    Expect(legacy_loaded.size() == 1 && legacy_loaded[0].name == "legacy_pg" &&
               legacy_loaded[0].username == "postgres" && legacy_loaded[0].password.empty(),
           "Legacy 4-column connections.tsv must still load cleanly");
  }

  const auto created = InvokeCli({
      "exec",
      "--profile",
      "local_demo",
      "--profiles",
      profiles_file.string(),
      "CREATE TABLE greetings (id INTEGER PRIMARY KEY, msg TEXT)",
      "--json",
  });
  Expect(created.code == 0, "exec via --profile failed: " + created.err);

  const auto listed = InvokeCli({
      "profile",
      "list",
      "--profiles",
      profiles_file.string(),
      "--json",
  });
  Expect(listed.code == 0 && listed.out.find("\"count\":2") != std::string::npos &&
             listed.out.find("secret") == std::string::npos,
         "profile list failed: " + listed.err);

  const auto shown = InvokeCli({
      "profile",
      "show",
      "local_demo",
      "--profiles",
      profiles_file.string(),
      "--json",
  });
  Expect(shown.code == 0 && shown.out.find("\"user\":\"alice\"") != std::string::npos,
         "profile show failed: " + shown.err);

  const auto removed = InvokeCli({
      "profile",
      "remove",
      "local_demo",
      "--profiles",
      profiles_file.string(),
      "--json",
  });
  Expect(removed.code == 0 && removed.out.find("\"removed\":true") != std::string::npos,
         "profile remove failed: " + removed.err);

  const auto removed_auth = InvokeCli({
      "profile",
      "remove",
      "auth_demo",
      "--profiles",
      profiles_file.string(),
      "--json",
  });
  Expect(removed_auth.code == 0 && removed_auth.out.find("\"removed\":true") != std::string::npos,
         "profile remove auth_demo failed: " + removed_auth.err);

  // Settings + HuxerUI::SQLite read verification + legacy TSV auto-migration.
  {
    const dbpp::TraySettings custom_settings{
        .tray_enabled = false,
        .lightweight = true,
        .theme_mode = dbpp::ThemeMode::Dark,
        .theme_preset = dbpp::ThemePreset::Emerald,
    };
    Expect(dbpp::SaveTraySettings(profiles_file.string(), custom_settings),
           "SaveTraySettings to dbpp.sqlite3 failed");
    const dbpp::TraySettings loaded_settings = dbpp::LoadTraySettings(profiles_file.string());
    Expect(loaded_settings == custom_settings, "LoadTraySettings round-trip failed");
    const dbpp::TraySettings startup_settings =
        dbpp::ConfigDbService::ReadStartupSettingsSync(profiles_file.string());
    Expect(startup_settings == custom_settings,
           "ConfigDbService::ReadStartupSettingsSync round-trip failed");

    std::int64_t user_version = 0;
    auto version_res = huxerui::sqlite::Database::QueryReadOnlySync(
        huxerui::File{profiles_file.string()}, "PRAGMA user_version",
        [&user_version](const huxerui::sqlite::RowView& row) -> huxerui::sqlite::Result<void> {
          auto v = row.Get<std::int64_t>(0);
          if (!v) return v.Error();
          user_version = *v;
          return {};
        });
    Expect(static_cast<bool>(version_res) && user_version == 2,
           "dbpp.sqlite3 user_version must be 2");
  }

  {
    const fs::path mig_dir = dir / "legacy_upgrade";
    fs::create_directories(mig_dir);
    const fs::path old_settings = mig_dir / "settings.tsv";
    const fs::path old_conns = mig_dir / "connections.tsv";
    const fs::path new_db = mig_dir / "dbpp.sqlite3";
    WriteTextFile(old_settings,
                  "tray.enabled\tfalse\n"
                  "tray.lightweight\ttrue\n"
                  "theme.mode\tdark\n"
                  "theme.preset\twarm_amber\n");
    WriteTextFile(old_conns,
                  "# db++ adbc connections v1\n"
                  "migrated_pg\tpostgresql\tpostgresql://127.0.0.1:5432/demo\tdev\n");

    const dbpp::TraySettings pre_open = dbpp::ConfigDbService::ReadStartupSettingsSync(new_db.string());
    Expect(!pre_open.tray_enabled && pre_open.lightweight &&
               pre_open.theme_mode == dbpp::ThemeMode::Dark &&
               pre_open.theme_preset == dbpp::ThemePreset::WarmAmber,
           "ReadStartupSettingsSync must fall back to settings.tsv before dbpp.sqlite3 exists");

    const auto migrated_conns = dbpp::LoadConnectionProfiles(new_db.string());
    const auto migrated_settings = dbpp::LoadTraySettings(new_db.string());
    Expect(fs::exists(new_db) && fs::exists(old_settings) && fs::exists(old_conns),
           "Migration must create dbpp.sqlite3 while preserving legacy TSV files");
    Expect(migrated_conns.size() == 1 && migrated_conns[0].name == "migrated_pg" &&
               migrated_conns[0].username == "dev",
           "Legacy connections.tsv must auto-migrate into dbpp.sqlite3");
    Expect(!migrated_settings.tray_enabled && migrated_settings.lightweight &&
               migrated_settings.theme_mode == dbpp::ThemeMode::Dark &&
               migrated_settings.theme_preset == dbpp::ThemePreset::WarmAmber,
           "Legacy settings.tsv must auto-migrate into dbpp.sqlite3");
  }

  const auto missing = InvokeCli({
      "profile",
      "show",
      "local_demo",
      "--profiles",
      profiles_file.string(),
      "--json",
  });
  Expect(missing.code == 1, "removed profile show must return exit 1");
}


} // namespace

namespace dbpp_test {

void Expect(bool condition, std::string_view message) { ::Expect(condition, message); }
CliRunOutput InvokeCli(const std::vector<std::string>& args, std::string_view stdin_text) {
  const auto res = ::InvokeCli(args, stdin_text);
  return CliRunOutput{.code = res.code, .out = res.out, .err = res.err};
}
std::string PathToFileUrl(const fs::path& path) { return ::PathToFileUrl(path); }
std::string ReadBinaryFile(const fs::path& path) { return ::ReadBinaryFile(path); }
void WriteBinaryFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  ::WriteBinaryFile(path, bytes);
}
void WriteTextFile(const fs::path& path, std::string_view text) { ::WriteTextFile(path, text); }
std::vector<std::uint8_t> BuildTarGz(
    const std::vector<std::pair<std::string, std::string>>& entries) {
  return ::BuildTarGz(entries);
}

} // namespace dbpp_test

int main() {
  ScopedTempDir temp;
  TestDispatchRules();
  TestVersionHelpDoctor(temp.path);
  TestProfilesLifecycle(temp.path);
  dbpp_test::RunCliCommandTests(temp.path, dbpp::DefaultSqliteDriverPath());

  if (g_failures != 0) {
    std::cerr << g_failures << " CLI test assertion(s) failed\n";
    return 1;
  }
  std::cout << "test_cli passed\n";
  return 0;
}
