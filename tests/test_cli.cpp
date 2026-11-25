#include "adbc_session.h"
#include "cli.h"
#include "driver_installer.h"
#include "model.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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
  const fs::path profiles_file = dir / "connections.tsv";
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

  const std::string raw_tsv = ReadBinaryFile(profiles_file);
  Expect(raw_tsv.find("local_demo") != std::string::npos, "connections.tsv contains profile name");

  const auto pw_rejected = InvokeCli({
      "profile",
      "save",
      "bad",
      "--driver",
      "postgresql",
      "--uri",
      "postgresql://host/db",
      "--password",
      "secret",
      "--profiles",
      profiles_file.string(),
      "--json",
  });
  Expect(pw_rejected.code == 2 && pw_rejected.err.find("\"ok\":false") != std::string::npos,
         "profile save must reject --password");

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
  Expect(listed.code == 0 && listed.out.find("\"count\":1") != std::string::npos,
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

void TestDatabaseCommands(const fs::path& dir) {
  const fs::path db_path = dir / "cli_test.sqlite";
  const std::string script =
      "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, note TEXT);\n"
      "INSERT INTO items (id, name, note) VALUES (1, 'alpha', 'line1\nline2 \"quoted\" \\\\ end');\n"
      "INSERT INTO items (id, name, note) VALUES (2, '中文名', NULL);\n"
      "INSERT INTO items (id, name, note) VALUES (3, 'gamma,csv', 'ok');\n"
      "CREATE VIEW item_names AS SELECT id, name FROM items;\n";

  const auto exec_script = InvokeCli(
      {"exec", "--sqlite", db_path.string(), "--script", "--json", "-"}, script);
  Expect(exec_script.code == 0, "exec --script failed: " + exec_script.err);
  Expect(exec_script.out.find("\"statements_executed\":5") != std::string::npos,
         "Expected 5 statements executed: " + exec_script.out);

  const auto tables_all = InvokeCli({"tables", "--sqlite", db_path.string(), "--json"});
  Expect(tables_all.code == 0, "tables failed: " + tables_all.err);
  Expect(tables_all.out.find("\"count\":2") != std::string::npos &&
             tables_all.out.find("\"name\":\"items\"") != std::string::npos &&
             tables_all.out.find("\"name\":\"item_names\"") != std::string::npos,
         "tables JSON mismatch: " + tables_all.out);

  const auto tables_view =
      InvokeCli({"tables", "--sqlite", db_path.string(), "--type", "view", "--json"});
  Expect(tables_view.code == 0 &&
             tables_view.out.find("\"count\":1") != std::string::npos &&
             tables_view.out.find("\"name\":\"item_names\"") != std::string::npos,
         "tables --type view mismatch: " + tables_view.out);

  const auto schema_one = InvokeCli({"schema", "--sqlite", db_path.string(), "items", "--json"});
  Expect(schema_one.code == 0 &&
             schema_one.out.find("\"table\":\"items\"") != std::string::npos &&
             schema_one.out.find("\"primary_key\":true") != std::string::npos,
         "schema single table mismatch: " + schema_one.out);

  const auto schema_all = InvokeCli({"schema", "--sqlite", db_path.string(), "--json"});
  Expect(schema_all.code == 0 &&
             schema_all.out.find("\"count\":2") != std::string::npos &&
             schema_all.out.find("\"tables\":[") != std::string::npos,
         "schema all tables mismatch: " + schema_all.out);

  const auto rows_res = InvokeCli({
      "rows",
      "--sqlite",
      db_path.string(),
      "items",
      "--where",
      "id >= 1",
      "--order",
      "id:desc",
      "--limit=1",
      "--offset=1",
      "--json",
  });
  Expect(rows_res.code == 0, "rows failed: " + rows_res.err);
  Expect(rows_res.out.find("\"total\":3") != std::string::npos &&
             rows_res.out.find("\"returned\":1") != std::string::npos &&
             rows_res.out.find("\"truncated\":true") != std::string::npos &&
             rows_res.out.find("[\"2\",\"中文名\",null]") != std::string::npos,
         "rows JSON mismatch: " + rows_res.out);

  const fs::path sql_file = dir / "q.sql";
  WriteTextFile(sql_file, "SELECT id, name, note FROM items ORDER BY id");

  const auto q_json = InvokeCli(
      {"query", "--sqlite", db_path.string(), "--file", sql_file.string(), "--json"});
  Expect(q_json.code == 0, "query --file failed: " + q_json.err);
  Expect(q_json.out.find("\"row_count\":3") != std::string::npos &&
             q_json.out.find("\\nline2 \\\"quoted\\\"") != std::string::npos &&
             q_json.out.find("[\"2\",\"中文名\",null]") != std::string::npos,
         "query JSON escaping/null mismatch: " + q_json.out);

  const auto q_csv = InvokeCli({
      "query",
      "--sqlite",
      db_path.string(),
      "SELECT id, name FROM items WHERE id = 3",
      "--format",
      "csv",
  });
  Expect(q_csv.code == 0 && q_csv.out == "id,name\n3,\"gamma,csv\"\n",
         "CSV output mismatch: " + q_csv.out);

  const auto q_tsv = InvokeCli({
      "query",
      "--sqlite",
      db_path.string(),
      "SELECT id, note FROM items WHERE id = 2",
      "--format=tsv",
  });
  Expect(q_tsv.code == 0 && q_tsv.out == "id\tnote\n2\t\\N\n",
         "TSV NULL output mismatch: " + q_tsv.out);

  const auto q_ro_reject = InvokeCli({
      "query",
      "--sqlite",
      db_path.string(),
      "--read-only",
      "DELETE FROM items WHERE id = 1",
      "--json",
  });
  Expect(q_ro_reject.code == 2 && q_ro_reject.err.find("\"ok\":false") != std::string::npos,
         "--read-only must reject DELETE");

  const auto sql_insert = InvokeCli({
      "sql",
      "--sqlite",
      db_path.string(),
      "INSERT INTO items (id, name, note) VALUES (4, 'delta', 'added')",
      "--json",
  });
  Expect(sql_insert.code == 0 &&
             sql_insert.out.find("\"mode\":\"exec\"") != std::string::npos &&
             sql_insert.out.find("\"affected_rows\":1") != std::string::npos,
         "sql auto exec mismatch: " + sql_insert.out);

  const auto sql_select = InvokeCli({
      "sql",
      "--sqlite",
      db_path.string(),
      "SELECT COUNT(*) AS c FROM items",
      "--json",
  });
  Expect(sql_select.code == 0 &&
             sql_select.out.find("\"mode\":\"query\"") != std::string::npos &&
             sql_select.out.find("[\"4\"]") != std::string::npos,
         "sql auto query mismatch: " + sql_select.out);
}

void TestDriverCatalogAndInstallOffline(const fs::path& dir) {
  const fs::path registry_dir = dir / "registry";
  const fs::path drivers_dir = dir / "user_drivers";
  fs::create_directories(registry_dir);
  fs::create_directories(drivers_dir);

  const std::string sqlite_bin_path = dbpp::DefaultSqliteDriverPath();
  const std::string driver_bytes = ReadBinaryFile(sqlite_bin_path);
  Expect(!driver_bytes.empty(), "Failed to read SQLite driver binary for CLI registry test");
  const std::string driver_filename = fs::path(sqlite_bin_path).filename().string();

  const std::string manifest_toml =
      "manifest_version = 1\n"
      "name = \"SQLite CLI Test\"\n"
      "publisher = \"db++ test\"\n"
      "license = \"Apache-2.0\"\n"
      "version = \"1.2.3\"\n"
      "[Files]\n"
      "driver = \"" + driver_filename + "\"\n";

  const fs::path tgz_path = registry_dir / "sqlite_cli_test-1.2.3.tar.gz";
  WriteBinaryFile(tgz_path,
                  BuildTarGz({{"MANIFEST", manifest_toml}, {driver_filename, driver_bytes}}));
  const std::string sha = dbpp::Sha256HexFile(tgz_path);

  const std::string index_yaml =
      "- name: SQLite CLI Test\n"
      "  description: >-\n"
      "    Offline CLI driver fixture.\n"
      "  license: Apache-2.0\n"
      "  path: sqlite_cli_test\n"
      "  pkginfo:\n"
      "  - packages:\n"
      "    - platform: " + dbpp::CurrentPlatformTuple() + "\n"
      "      url: sqlite_cli_test-1.2.3.tar.gz\n"
      "      hash: sha256:" + sha + "\n"
      "    version: 1.2.3\n";
  WriteTextFile(registry_dir / "index.yaml", index_yaml);

  const std::string registry_url = PathToFileUrl(registry_dir);

  const auto catalog = InvokeCli({
      "driver",
      "catalog",
      "--registry",
      registry_url,
      "--drivers-dir",
      drivers_dir.string(),
      "--filter",
      "cli",
      "--json",
  });
  Expect(catalog.code == 0, "driver catalog failed: " + catalog.err);
  Expect(catalog.out.find("\"slug\":\"sqlite_cli_test\"") != std::string::npos &&
             catalog.out.find("\"installed\":false") != std::string::npos,
         "driver catalog JSON mismatch: " + catalog.out);

  const auto installed = InvokeCli({
      "driver",
      "install",
      "sqlite_cli_test",
      "--registry",
      registry_url,
      "--drivers-dir",
      drivers_dir.string(),
      "--json",
  });
  Expect(installed.code == 0, "driver install failed: " + installed.err);
  Expect(installed.out.find("\"version\":\"1.2.3\"") != std::string::npos &&
             dbpp::IsUserInstalledAdbcDriver("sqlite_cli_test", drivers_dir),
         "driver install did not write manifest: " + installed.out);

  const auto listed = InvokeCli({
      "driver",
      "list",
      "--drivers-dir",
      drivers_dir.string(),
      "--json",
  });
  Expect(listed.code == 0 &&
             listed.out.find("\"path\":\"sqlite_cli_test\"") != std::string::npos,
         "driver list did not include installed manifest: " + listed.out);

  const auto uninstalled = InvokeCli({
      "driver",
      "uninstall",
      "sqlite_cli_test",
      "--drivers-dir",
      drivers_dir.string(),
      "--json",
  });
  Expect(uninstalled.code == 0 &&
             !dbpp::IsUserInstalledAdbcDriver("sqlite_cli_test", drivers_dir),
         "driver uninstall failed: " + uninstalled.err);
}

void TestErrorsAndExitCodes(const fs::path& dir) {
  const auto unknown_cmd = InvokeCli({"no_such_command", "--json"});
  Expect(unknown_cmd.code == 2 && unknown_cmd.out.empty() &&
             unknown_cmd.err.find("\"ok\":false") != std::string::npos,
         "Unknown command should exit 2 with JSON error on stderr");

  const auto missing_conn = InvokeCli({"tables", "--json"});
  Expect(missing_conn.code == 2 && missing_conn.err.find("\"ok\":false") != std::string::npos,
         "Missing connection args should exit 2");

  const auto bad_sql =
      InvokeCli({"query", "--sqlite", ":memory:", "SELECT * FROM missing_table", "--json"});
  Expect(bad_sql.code == 1 && bad_sql.out.empty() &&
             bad_sql.err.find("\"ok\":false") != std::string::npos,
         "Bad SQL should exit 1 with JSON error on stderr");

  const auto missing_pw_env = InvokeCli({
      "tables",
      "--sqlite",
      (dir / "pw.sqlite").string(),
      "--password-env",
      "DBPP_TEST_DEFINITELY_UNSET_ENV_12345",
      "--json",
  });
  Expect(missing_pw_env.code == 1 &&
             missing_pw_env.err.find("DBPP_TEST_DEFINITELY_UNSET_ENV_12345") != std::string::npos,
         "Unset --password-env should exit 1 clearly");
}

void TestBatchInsertAndWorkspaceModel(const fs::path& dir, const std::string& driver_path) {
  const fs::path db_path = dir / "batch_insert.sqlite";
  dbpp::ConnectionRequest req;
  req.driver_path = driver_path;
  req.uri = db_path.string();

  dbpp::AdbcOpenResult opened = dbpp::OpenAdbcBlocking(req);
  Expect(opened.session != nullptr && opened.error.empty(),
         "OpenAdbcBlocking failed: " + opened.error);
  if (!opened.session) {
    return;
  }

  dbpp::AdbcQueryResult ddl = dbpp::ExecuteBlocking(
      *opened.session,
      "CREATE TABLE entries (id INTEGER PRIMARY KEY AUTOINCREMENT, code TEXT NOT NULL UNIQUE, "
      "note TEXT)",
      false, 0);
  Expect(ddl.error.empty(), "CREATE TABLE entries failed: " + ddl.error);

  dbpp::AdbcObjectsResult objs = dbpp::LoadObjectsBlocking(*opened.session);
  const dbpp::DbObject* entries_obj = nullptr;
  for (const auto& obj : objs.objects) {
    if (obj.name == "entries") {
      entries_obj = &obj;
      break;
    }
  }
  Expect(objs.error.empty() && entries_obj != nullptr, "Expected entries table object");
  if (entries_obj == nullptr) {
    return;
  }

  dbpp::AdbcSchemaResult schema = dbpp::LoadSchemaBlocking(*opened.session, *entries_obj);
  Expect(schema.error.empty() && schema.columns.size() == 3, "Expected 3 columns");

  dbpp::TableModel table_model;
  table_model.session_id = 1;
  table_model.object = "entries";
  table_model.qualified_name = dbpp::QualifiedName(opened.session->dialect, *entries_obj);
  table_model.editable = true;
  table_model.uses_rowid = true;
  table_model.columns = schema.columns;

  dbpp::PendingInsertRow row1;
  row1.draft_id = 1;
  row1.values = {"", "A1", "first"};
  const dbpp::BatchInsertResult single_res =
      dbpp::InsertRowsBlocking(*opened.session, table_model, {row1});
  Expect(single_res.inserted == 1 && single_res.error.empty(),
         "Single draft row insert failed: " + single_res.error);

  dbpp::PendingInsertRow row2;
  row2.draft_id = 2;
  row2.values = {"", "A2", "ok"};
  dbpp::PendingInsertRow row_dup;
  row_dup.draft_id = 3;
  row_dup.values = {"", "A1", "duplicate"};
  const dbpp::BatchInsertResult failed_batch =
      dbpp::InsertRowsBlocking(*opened.session, table_model, {row2, row_dup});
  Expect(failed_batch.inserted == 0 && failed_batch.rolled_back && !failed_batch.error.empty(),
         "Batch insert duplicate must trigger full rollback");

  const dbpp::AdbcQueryResult count_after_rb =
      dbpp::ExecuteBlocking(*opened.session, "SELECT COUNT(*) FROM entries", true, 1);
  Expect(count_after_rb.error.empty() && count_after_rb.rows.size() == 1 &&
             count_after_rb.rows[0].values[0].text == "1",
         "Table row count must stay 1 after rolled back batch insert");

  row_dup.values = {"", "A3", ""};
  const dbpp::BatchInsertResult ok_batch =
      dbpp::InsertRowsBlocking(*opened.session, table_model, {row2, row_dup});
  Expect(ok_batch.inserted == 2 && !ok_batch.rolled_back && ok_batch.error.empty(),
         "Fixed batch insert failed: " + ok_batch.error);

  const dbpp::AdbcQueryResult rows_after = dbpp::ExecuteBlocking(
      *opened.session, "SELECT code, note FROM entries ORDER BY id", true, 10);
  Expect(rows_after.error.empty() && rows_after.rows.size() == 3,
         "Expected 3 rows after committed batch");
  if (rows_after.rows.size() == 3) {
    Expect(rows_after.rows[1].values[0].text == "A2" && rows_after.rows[1].values[1].text == "ok",
           "Row 2 mismatch");
    Expect(rows_after.rows[2].values[0].text == "A3" && rows_after.rows[2].values[1].is_null,
           "Blank draft cell should persist SQL NULL");
  }

  dbpp::WorkspaceModel ws;
  dbpp::WorkspaceTab tab1;
  tab1.id = ws.next_tab_id++;
  tab1.kind = dbpp::WorkspaceTabKind::Table;
  tab1.session_id = 1;
  tab1.object_name = "entries";
  tab1.object_schema = "main";
  tab1.title = "entries";

  dbpp::WorkspaceTab tab2;
  tab2.id = ws.next_tab_id++;
  tab2.kind = dbpp::WorkspaceTabKind::Sql;
  tab2.session_id = 1;
  tab2.title = "查询 1";

  ws.tabs.push_back(tab1);
  ws.tabs.push_back(tab2);
  ws.active_tab_id = tab2.id;

  Expect(ws.ActiveTab() != nullptr && ws.ActiveTab()->id == tab2.id,
         "WorkspaceModel::ActiveTab mismatch");
  Expect(ws.FindTableTab(1, "entries", "main", false) != nullptr &&
             ws.FindTableTab(1, "entries", "main", false)->id == tab1.id,
         "WorkspaceModel::FindTableTab mismatch");
  Expect(ws.FindTableTab(1, "entries", "main", true) == nullptr,
         "FindTableTab must distinguish table vs view");
}

void TestConnectionOrdering() {
  const std::vector<std::string> base = {"A", "B", "C"};
  Expect(dbpp::OrderConnectionKeys(base, {}) == std::vector<std::string>({"A", "B", "C"}),
         "OrderConnectionKeys default order mismatch");

  const std::vector<std::string> moved_up =
      dbpp::ReorderConnectionKeys(base, {}, "C", "A");
  Expect(moved_up == std::vector<std::string>({"C", "A", "B"}),
         "ReorderConnectionKeys move-up mismatch");

  const std::vector<std::string> moved_down =
      dbpp::ReorderConnectionKeys(base, moved_up, "C", "B");
  Expect(moved_down == std::vector<std::string>({"A", "B", "C"}),
         "ReorderConnectionKeys move-down mismatch");

  Expect(dbpp::OrderConnectionKeys({"A", "B", "C", "D"}, {"B", "A", "C"}) ==
             std::vector<std::string>({"B", "A", "C", "D"}),
         "OrderConnectionKeys must keep custom order when appending new connections");

  Expect(dbpp::OrderConnectionKeys({"B", "C"}, {"B", "A", "C"}) ==
             std::vector<std::string>({"B", "C"}),
         "OrderConnectionKeys must prune removed connections cleanly");
}

void TestTableCellPreviewAndAutoWidth() {
  Expect(dbpp::SingleLineCellPreview("hello world") == "hello world",
         "SingleLineCellPreview should preserve single-line ASCII");
  Expect(dbpp::SingleLineCellPreview("line1\r\nline2\n\tline3") == "line1 line2 line3",
         "SingleLineCellPreview must flatten CRLF, LF, and tabs into single spaces");

  std::string long_cjk;
  for (int i = 0; i < 400; ++i) {
    long_cjk += "中";
  }
  const std::string preview = dbpp::SingleLineCellPreview(long_cjk);
  const std::string ellipsis = "…";
  Expect(preview.ends_with(ellipsis) && preview.size() <= 1024 + ellipsis.size(),
         "SingleLineCellPreview must truncate oversized strings with ellipsis");
  Expect((preview.size() - ellipsis.size()) % 3 == 0,
         "SingleLineCellPreview must truncate on a valid UTF-8 boundary");

  dbpp::GridSnapshot snap;
  snap.rows = {
      {.values = {{.text = "1", .is_null = false},
                  {.text = "2026-11-25 09:12:00", .is_null = false},
                  {.text = "中文长标题测试内容", .is_null = false},
                  {.text = "abcdefghi", .is_null = false},
                  {.text = std::string(500, 'x'), .is_null = false}},
       .key_values = {},
       .rowid = std::nullopt},
      {.values = {{.text = "42", .is_null = false},
                  {.text = "", .is_null = true},
                  {.text = "短", .is_null = false},
                  {.text = "a", .is_null = false},
                  {.text = "ok", .is_null = false}},
       .key_values = {},
       .rowid = std::nullopt},
  };

  const float id_width = dbpp::ComputeAutoColumnWidth("id", 0, &snap);
  Expect(id_width == dbpp::kAutoMinColumnWidth,
         "Short id column should clamp at kAutoMinColumnWidth");

  const float ts_width = dbpp::ComputeAutoColumnWidth("created_at", 1, &snap);
  Expect(ts_width > dbpp::kAutoMinColumnWidth && ts_width < dbpp::kAutoMaxColumnWidth,
         "Timestamp column should auto-size between min and max bounds");

  const float cjk_width = dbpp::ComputeAutoColumnWidth("c", 2, &snap);
  const float ascii_width = dbpp::ComputeAutoColumnWidth("c", 3, &snap);
  Expect(cjk_width > ascii_width,
         "CJK cell text should produce a wider auto width than equal-count ASCII");

  const float long_width = dbpp::ComputeAutoColumnWidth("payload", 4, &snap);
  Expect(long_width == dbpp::kAutoMaxColumnWidth,
         "Oversized cell column should clamp at kAutoMaxColumnWidth");

  const float empty_snap_width = dbpp::ComputeAutoColumnWidth("description", 0, nullptr);
  Expect(empty_snap_width >= dbpp::kAutoMinColumnWidth &&
             empty_snap_width <= dbpp::kAutoMaxColumnWidth,
         "Null snapshot width must remain within auto-fit bounds");
}

} // namespace

int main() {
  ScopedTempDir temp;
  TestDispatchRules();
  TestVersionHelpDoctor(temp.path);
  TestProfilesLifecycle(temp.path);
  TestDatabaseCommands(temp.path);
  TestDriverCatalogAndInstallOffline(temp.path);
  TestErrorsAndExitCodes(temp.path);
  TestBatchInsertAndWorkspaceModel(temp.path, dbpp::DefaultSqliteDriverPath());
  TestConnectionOrdering();
  TestTableCellPreviewAndAutoWidth();

  if (g_failures != 0) {
    std::cerr << g_failures << " CLI test assertion(s) failed\n";
    return 1;
  }
  std::cout << "test_cli passed\n";
  return 0;
}
