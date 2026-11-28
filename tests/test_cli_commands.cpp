#include <huxerui/huxerui.h>
#include <huxerui/sqlite.h>
#include <cstdint>
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

void TestDatabaseCommands(const fs::path& dir) {
  const fs::path db_path = dir / "cli_test.sqlite";
  const std::string script =
      "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, note TEXT);\n"
      "INSERT INTO items (id, name, note) VALUES (1, 'alpha', 'line1\nline2 \"quoted\" \\\\ end');\n"
      "INSERT INTO items (id, name, note) VALUES (2, '中文名', NULL);\n"
      "INSERT INTO items (id, name, note) VALUES (3, 'gamma,csv', 'ok');\n"
      "CREATE VIEW item_names AS SELECT id, name FROM items;\n";

  const auto exec_script = dbpp_test::InvokeCli(
      {"exec", "--sqlite", db_path.string(), "--script", "--json", "-"}, script);
  dbpp_test::Expect(exec_script.code == 0, "exec --script failed: " + exec_script.err);
  dbpp_test::Expect(exec_script.out.find("\"statements_executed\":5") != std::string::npos,
         "Expected 5 statements executed: " + exec_script.out);

  const auto tables_all = dbpp_test::InvokeCli({"tables", "--sqlite", db_path.string(), "--json"});
  dbpp_test::Expect(tables_all.code == 0, "tables failed: " + tables_all.err);
  dbpp_test::Expect(tables_all.out.find("\"count\":2") != std::string::npos &&
             tables_all.out.find("\"name\":\"items\"") != std::string::npos &&
             tables_all.out.find("\"name\":\"item_names\"") != std::string::npos,
         "tables JSON mismatch: " + tables_all.out);

  const auto tables_view =
      dbpp_test::InvokeCli({"tables", "--sqlite", db_path.string(), "--type", "view", "--json"});
  dbpp_test::Expect(tables_view.code == 0 &&
             tables_view.out.find("\"count\":1") != std::string::npos &&
             tables_view.out.find("\"name\":\"item_names\"") != std::string::npos,
         "tables --type view mismatch: " + tables_view.out);

  const auto schema_one = dbpp_test::InvokeCli({"schema", "--sqlite", db_path.string(), "items", "--json"});
  dbpp_test::Expect(schema_one.code == 0 &&
             schema_one.out.find("\"table\":\"items\"") != std::string::npos &&
             schema_one.out.find("\"primary_key\":true") != std::string::npos,
         "schema single table mismatch: " + schema_one.out);

  const auto schema_all = dbpp_test::InvokeCli({"schema", "--sqlite", db_path.string(), "--json"});
  dbpp_test::Expect(schema_all.code == 0 &&
             schema_all.out.find("\"count\":2") != std::string::npos &&
             schema_all.out.find("\"tables\":[") != std::string::npos,
         "schema all tables mismatch: " + schema_all.out);

  const auto rows_res = dbpp_test::InvokeCli({
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
  dbpp_test::Expect(rows_res.code == 0, "rows failed: " + rows_res.err);
  dbpp_test::Expect(rows_res.out.find("\"total\":3") != std::string::npos &&
             rows_res.out.find("\"returned\":1") != std::string::npos &&
             rows_res.out.find("\"truncated\":true") != std::string::npos &&
             rows_res.out.find("[\"2\",\"中文名\",null]") != std::string::npos,
         "rows JSON mismatch: " + rows_res.out);

  const fs::path sql_file = dir / "q.sql";
  dbpp_test::WriteTextFile(sql_file, "SELECT id, name, note FROM items ORDER BY id");

  const auto q_json = dbpp_test::InvokeCli(
      {"query", "--sqlite", db_path.string(), "--file", sql_file.string(), "--json"});
  dbpp_test::Expect(q_json.code == 0, "query --file failed: " + q_json.err);
  dbpp_test::Expect(q_json.out.find("\"row_count\":3") != std::string::npos &&
             q_json.out.find("\\nline2 \\\"quoted\\\"") != std::string::npos &&
             q_json.out.find("[\"2\",\"中文名\",null]") != std::string::npos,
         "query JSON escaping/null mismatch: " + q_json.out);

  const auto q_csv = dbpp_test::InvokeCli({
      "query",
      "--sqlite",
      db_path.string(),
      "SELECT id, name FROM items WHERE id = 3",
      "--format",
      "csv",
  });
  dbpp_test::Expect(q_csv.code == 0 && q_csv.out == "id,name\n3,\"gamma,csv\"\n",
         "CSV output mismatch: " + q_csv.out);

  const auto q_tsv = dbpp_test::InvokeCli({
      "query",
      "--sqlite",
      db_path.string(),
      "SELECT id, note FROM items WHERE id = 2",
      "--format=tsv",
  });
  dbpp_test::Expect(q_tsv.code == 0 && q_tsv.out == "id\tnote\n2\t\\N\n",
         "TSV NULL output mismatch: " + q_tsv.out);

  const auto q_ro_reject = dbpp_test::InvokeCli({
      "query",
      "--sqlite",
      db_path.string(),
      "--read-only",
      "DELETE FROM items WHERE id = 1",
      "--json",
  });
  dbpp_test::Expect(q_ro_reject.code == 2 && q_ro_reject.err.find("\"ok\":false") != std::string::npos,
         "--read-only must reject DELETE");

  const auto sql_insert = dbpp_test::InvokeCli({
      "sql",
      "--sqlite",
      db_path.string(),
      "INSERT INTO items (id, name, note) VALUES (4, 'delta', 'added')",
      "--json",
  });
  dbpp_test::Expect(sql_insert.code == 0 &&
             sql_insert.out.find("\"mode\":\"exec\"") != std::string::npos &&
             sql_insert.out.find("\"affected_rows\":1") != std::string::npos,
         "sql auto exec mismatch: " + sql_insert.out);

  const auto sql_select = dbpp_test::InvokeCli({
      "sql",
      "--sqlite",
      db_path.string(),
      "SELECT COUNT(*) AS c FROM items",
      "--json",
  });
  dbpp_test::Expect(sql_select.code == 0 &&
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
  const std::string driver_bytes = dbpp_test::ReadBinaryFile(sqlite_bin_path);
  dbpp_test::Expect(!driver_bytes.empty(), "Failed to read SQLite driver binary for CLI registry test");
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
  dbpp_test::WriteBinaryFile(tgz_path,
                  dbpp_test::BuildTarGz({{"MANIFEST", manifest_toml}, {driver_filename, driver_bytes}}));
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
  dbpp_test::WriteTextFile(registry_dir / "index.yaml", index_yaml);

  const std::string registry_url = dbpp_test::PathToFileUrl(registry_dir);

  const auto catalog = dbpp_test::InvokeCli({
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
  dbpp_test::Expect(catalog.code == 0, "driver catalog failed: " + catalog.err);
  dbpp_test::Expect(catalog.out.find("\"slug\":\"sqlite_cli_test\"") != std::string::npos &&
             catalog.out.find("\"installed\":false") != std::string::npos,
         "driver catalog JSON mismatch: " + catalog.out);

  const auto installed = dbpp_test::InvokeCli({
      "driver",
      "install",
      "sqlite_cli_test",
      "--registry",
      registry_url,
      "--drivers-dir",
      drivers_dir.string(),
      "--json",
  });
  dbpp_test::Expect(installed.code == 0, "driver install failed: " + installed.err);
  dbpp_test::Expect(installed.out.find("\"version\":\"1.2.3\"") != std::string::npos &&
             dbpp::IsUserInstalledAdbcDriver("sqlite_cli_test", drivers_dir),
         "driver install did not write manifest: " + installed.out);

  const auto listed = dbpp_test::InvokeCli({
      "driver",
      "list",
      "--drivers-dir",
      drivers_dir.string(),
      "--json",
  });
  dbpp_test::Expect(listed.code == 0 &&
             listed.out.find("\"path\":\"sqlite_cli_test\"") != std::string::npos,
         "driver list did not include installed manifest: " + listed.out);

  const auto uninstalled = dbpp_test::InvokeCli({
      "driver",
      "uninstall",
      "sqlite_cli_test",
      "--drivers-dir",
      drivers_dir.string(),
      "--json",
  });
  dbpp_test::Expect(uninstalled.code == 0 &&
             !dbpp::IsUserInstalledAdbcDriver("sqlite_cli_test", drivers_dir),
         "driver uninstall failed: " + uninstalled.err);
}

void TestErrorsAndExitCodes(const fs::path& dir) {
  const auto unknown_cmd = dbpp_test::InvokeCli({"no_such_command", "--json"});
  dbpp_test::Expect(unknown_cmd.code == 2 && unknown_cmd.out.empty() &&
             unknown_cmd.err.find("\"ok\":false") != std::string::npos,
         "Unknown command should exit 2 with JSON error on stderr");

  const auto missing_conn = dbpp_test::InvokeCli({"tables", "--json"});
  dbpp_test::Expect(missing_conn.code == 2 && missing_conn.err.find("\"ok\":false") != std::string::npos,
         "Missing connection args should exit 2");

  const auto bad_sql =
      dbpp_test::InvokeCli({"query", "--sqlite", ":memory:", "SELECT * FROM missing_table", "--json"});
  dbpp_test::Expect(bad_sql.code == 1 && bad_sql.out.empty() &&
             bad_sql.err.find("\"ok\":false") != std::string::npos,
         "Bad SQL should exit 1 with JSON error on stderr");

  const auto missing_pw_env = dbpp_test::InvokeCli({
      "tables",
      "--sqlite",
      (dir / "pw.sqlite").string(),
      "--password-env",
      "DBPP_TEST_DEFINITELY_UNSET_ENV_12345",
      "--json",
  });
  dbpp_test::Expect(missing_pw_env.code == 1 &&
             missing_pw_env.err.find("DBPP_TEST_DEFINITELY_UNSET_ENV_12345") != std::string::npos,
         "Unset --password-env should exit 1 clearly");
}

void TestBatchInsertAndWorkspaceModel(const fs::path& dir, const std::string& driver_path) {
  const fs::path db_path = dir / "batch_insert.sqlite";
  dbpp::ConnectionRequest req;
  req.driver_path = driver_path;
  req.uri = db_path.string();

  dbpp::AdbcOpenResult opened = dbpp::OpenAdbcBlocking(req);
  dbpp_test::Expect(opened.session != nullptr && opened.error.empty(),
         "OpenAdbcBlocking failed: " + opened.error);
  if (!opened.session) {
    return;
  }

  dbpp::AdbcQueryResult ddl = dbpp::ExecuteBlocking(
      *opened.session,
      "CREATE TABLE entries (id INTEGER PRIMARY KEY AUTOINCREMENT, code TEXT NOT NULL UNIQUE, "
      "note TEXT)",
      false, 0);
  dbpp_test::Expect(ddl.error.empty(), "CREATE TABLE entries failed: " + ddl.error);

  dbpp::AdbcObjectsResult objs = dbpp::LoadObjectsBlocking(*opened.session);
  const dbpp::DbObject* entries_obj = nullptr;
  for (const auto& obj : objs.objects) {
    if (obj.name == "entries") {
      entries_obj = &obj;
      break;
    }
  }
  dbpp_test::Expect(objs.error.empty() && entries_obj != nullptr, "Expected entries table object");
  if (entries_obj == nullptr) {
    return;
  }

  dbpp::AdbcSchemaResult schema = dbpp::LoadSchemaBlocking(*opened.session, *entries_obj);
  dbpp_test::Expect(schema.error.empty() && schema.columns.size() == 3, "Expected 3 columns");

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
  dbpp_test::Expect(single_res.inserted == 1 && single_res.error.empty(),
         "Single draft row insert failed: " + single_res.error);

  dbpp::PendingInsertRow row2;
  row2.draft_id = 2;
  row2.values = {"", "A2", "ok"};
  dbpp::PendingInsertRow row_dup;
  row_dup.draft_id = 3;
  row_dup.values = {"", "A1", "duplicate"};
  const dbpp::BatchInsertResult failed_batch =
      dbpp::InsertRowsBlocking(*opened.session, table_model, {row2, row_dup});
  dbpp_test::Expect(failed_batch.inserted == 0 && failed_batch.rolled_back && !failed_batch.error.empty(),
         "Batch insert duplicate must trigger full rollback");

  const dbpp::AdbcQueryResult count_after_rb =
      dbpp::ExecuteBlocking(*opened.session, "SELECT COUNT(*) FROM entries", true, 1);
  dbpp_test::Expect(count_after_rb.error.empty() && count_after_rb.rows.size() == 1 &&
             count_after_rb.rows[0].values[0].text == "1",
         "Table row count must stay 1 after rolled back batch insert");

  row_dup.values = {"", "A3", ""};
  const dbpp::BatchInsertResult ok_batch =
      dbpp::InsertRowsBlocking(*opened.session, table_model, {row2, row_dup});
  dbpp_test::Expect(ok_batch.inserted == 2 && !ok_batch.rolled_back && ok_batch.error.empty(),
         "Fixed batch insert failed: " + ok_batch.error);

  const dbpp::AdbcQueryResult rows_after = dbpp::ExecuteBlocking(
      *opened.session, "SELECT code, note FROM entries ORDER BY id", true, 10);
  dbpp_test::Expect(rows_after.error.empty() && rows_after.rows.size() == 3,
         "Expected 3 rows after committed batch");
  if (rows_after.rows.size() == 3) {
    dbpp_test::Expect(rows_after.rows[1].values[0].text == "A2" && rows_after.rows[1].values[1].text == "ok",
           "Row 2 mismatch");
    dbpp_test::Expect(rows_after.rows[2].values[0].text == "A3" && rows_after.rows[2].values[1].is_null,
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

  dbpp_test::Expect(ws.ActiveTab() != nullptr && ws.ActiveTab()->id == tab2.id,
         "WorkspaceModel::ActiveTab mismatch");
  dbpp_test::Expect(ws.FindTableTab(1, "entries", "main", false) != nullptr &&
             ws.FindTableTab(1, "entries", "main", false)->id == tab1.id,
         "WorkspaceModel::FindTableTab mismatch");
  dbpp_test::Expect(ws.FindTableTab(1, "entries", "main", true) == nullptr,
         "FindTableTab must distinguish table vs view");
}

void TestConnectionOrdering() {
  const std::vector<std::string> base = {"A", "B", "C"};
  dbpp_test::Expect(dbpp::OrderConnectionKeys(base, {}) == std::vector<std::string>({"A", "B", "C"}),
         "OrderConnectionKeys default order mismatch");

  const std::vector<std::string> moved_up =
      dbpp::ReorderConnectionKeys(base, {}, "C", "A");
  dbpp_test::Expect(moved_up == std::vector<std::string>({"C", "A", "B"}),
         "ReorderConnectionKeys move-up mismatch");

  const std::vector<std::string> moved_down =
      dbpp::ReorderConnectionKeys(base, moved_up, "C", "B");
  dbpp_test::Expect(moved_down == std::vector<std::string>({"A", "B", "C"}),
         "ReorderConnectionKeys move-down mismatch");

  dbpp_test::Expect(dbpp::OrderConnectionKeys({"A", "B", "C", "D"}, {"B", "A", "C"}) ==
             std::vector<std::string>({"B", "A", "C", "D"}),
         "OrderConnectionKeys must keep custom order when appending new connections");

  dbpp_test::Expect(dbpp::OrderConnectionKeys({"B", "C"}, {"B", "A", "C"}) ==
             std::vector<std::string>({"B", "C"}),
         "OrderConnectionKeys must prune removed connections cleanly");
}

void TestTableCellPreviewAndAutoWidth() {
  dbpp_test::Expect(dbpp::SingleLineCellPreview("hello world") == "hello world",
         "SingleLineCellPreview should preserve single-line ASCII");
  dbpp_test::Expect(dbpp::SingleLineCellPreview("line1\r\nline2\n\tline3") == "line1 line2 line3",
         "SingleLineCellPreview must flatten CRLF, LF, and tabs into single spaces");

  std::string long_cjk;
  for (int i = 0; i < 400; ++i) {
    long_cjk += "中";
  }
  const std::string preview = dbpp::SingleLineCellPreview(long_cjk);
  const std::string ellipsis = "…";
  dbpp_test::Expect(preview.ends_with(ellipsis) && preview.size() <= 1024 + ellipsis.size(),
         "SingleLineCellPreview must truncate oversized strings with ellipsis");
  dbpp_test::Expect((preview.size() - ellipsis.size()) % 3 == 0,
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
  dbpp_test::Expect(id_width == dbpp::kAutoMinColumnWidth,
         "Short id column should clamp at kAutoMinColumnWidth");

  const float ts_width = dbpp::ComputeAutoColumnWidth("created_at", 1, &snap);
  dbpp_test::Expect(ts_width > dbpp::kAutoMinColumnWidth && ts_width < dbpp::kAutoMaxColumnWidth,
         "Timestamp column should auto-size between min and max bounds");

  const float cjk_width = dbpp::ComputeAutoColumnWidth("c", 2, &snap);
  const float ascii_width = dbpp::ComputeAutoColumnWidth("c", 3, &snap);
  dbpp_test::Expect(cjk_width > ascii_width,
         "CJK cell text should produce a wider auto width than equal-count ASCII");

  const float long_width = dbpp::ComputeAutoColumnWidth("payload", 4, &snap);
  dbpp_test::Expect(long_width == dbpp::kAutoMaxColumnWidth,
         "Oversized cell column should clamp at kAutoMaxColumnWidth");

  const float empty_snap_width = dbpp::ComputeAutoColumnWidth("description", 0, nullptr);
  dbpp_test::Expect(empty_snap_width >= dbpp::kAutoMinColumnWidth &&
             empty_snap_width <= dbpp::kAutoMaxColumnWidth,
         "Null snapshot width must remain within auto-fit bounds");
}

void TestSettingsAndThemePalettes(const fs::path& dir) {
  dbpp_test::Expect(dbpp::LanguageModeKey(dbpp::LanguageMode::System) == "system",
         "LanguageModeKey system mismatch");
  dbpp_test::Expect(dbpp::LanguageModeKey(dbpp::LanguageMode::Zh) == "zh",
         "LanguageModeKey zh mismatch");
  dbpp_test::Expect(dbpp::LanguageModeKey(dbpp::LanguageMode::En) == "en",
         "LanguageModeKey en mismatch");
  dbpp_test::Expect(dbpp::LanguageModeFromKey("zh") == dbpp::LanguageMode::Zh, "LanguageModeFromKey zh");
  dbpp_test::Expect(dbpp::LanguageModeFromKey("en") == dbpp::LanguageMode::En, "LanguageModeFromKey en");
  dbpp_test::Expect(dbpp::LanguageModeFromKey("system") == dbpp::LanguageMode::System,
         "LanguageModeFromKey system");
  dbpp_test::Expect(dbpp::LanguageModeFromKey("unknown") == dbpp::LanguageMode::System,
         "LanguageModeFromKey unknown fallback");

  const fs::path settings_path = dir / "sub" / "settings.tsv";
  dbpp::TraySettings saved;
  saved.tray_enabled = false;
  saved.lightweight = true;
  saved.theme_mode = dbpp::ThemeMode::Dark;
  saved.theme_preset = dbpp::ThemePreset::VioletDusk;
  saved.language = dbpp::LanguageMode::En;
  dbpp_test::Expect(dbpp::SaveTraySettings(settings_path.string(), saved),
         "SaveTraySettings should create parent directory and succeed");

  const dbpp::TraySettings loaded = dbpp::LoadTraySettings(settings_path.string());
  dbpp_test::Expect(loaded == saved, "LoadTraySettings round-trip with theme and language config mismatch");

  const fs::path sqlite_settings_dir = dir / "sqlite_settings";
  fs::create_directories(sqlite_settings_dir);
  const fs::path sqlite_db_path = sqlite_settings_dir / "dbpp.sqlite3";
  {
    dbpp::TraySettings sqlite_saved = saved;
    sqlite_saved.language = dbpp::LanguageMode::Zh;
    dbpp_test::Expect(dbpp::SaveTraySettings(sqlite_db_path.string(), sqlite_saved),
           "SaveTraySettings to dbpp.sqlite3 should succeed");
    const dbpp::TraySettings sqlite_loaded = dbpp::LoadTraySettings(sqlite_db_path.string());
    dbpp_test::Expect(sqlite_loaded == sqlite_saved,
           "LoadTraySettings round-trip from dbpp.sqlite3 mismatch");

    const dbpp::TraySettings startup_loaded =
        dbpp::ConfigDbService::ReadStartupSettingsSync(sqlite_db_path.string());
    dbpp_test::Expect(startup_loaded == sqlite_saved,
           "ReadStartupSettingsSync ui.language round-trip mismatch");

    sqlite_saved.language = dbpp::LanguageMode::En;
    dbpp_test::Expect(dbpp::SaveTraySettings(sqlite_db_path.string(), sqlite_saved),
           "SaveTraySettings updating ui.language=en in dbpp.sqlite3 should succeed");
    dbpp_test::Expect(dbpp::LoadTraySettings(sqlite_db_path.string()).language == dbpp::LanguageMode::En,
           "LoadTraySettings should reflect updated ui.language=en in dbpp.sqlite3");
    dbpp_test::Expect(dbpp::ConfigDbService::ReadStartupSettingsSync(sqlite_db_path.string()).language ==
               dbpp::LanguageMode::En,
           "ReadStartupSettingsSync update to ui.language=en mismatch");
  }

  const fs::path legacy_path = dir / "legacy_settings.tsv";
  dbpp_test::WriteTextFile(legacy_path, "tray.enabled\tfalse\ntray.lightweight\ttrue\n");
  const dbpp::TraySettings legacy = dbpp::LoadTraySettings(legacy_path.string());
  dbpp_test::Expect(!legacy.tray_enabled && legacy.lightweight &&
             legacy.theme_mode == dbpp::ThemeMode::System &&
             legacy.theme_preset == dbpp::ThemePreset::GeminiSpark,
         "Legacy 2-line settings.tsv must default theme to System + GeminiSpark");

  const fs::path bogus_path = dir / "bogus_settings.tsv";
  dbpp_test::WriteTextFile(bogus_path, "theme.mode\tbogus\ntheme.preset\tunknown\n");
  const dbpp::TraySettings bogus = dbpp::LoadTraySettings(bogus_path.string());
  dbpp_test::Expect(bogus.theme_mode == dbpp::ThemeMode::System &&
             bogus.theme_preset == dbpp::ThemePreset::GeminiSpark,
         "Unknown theme keys in settings.tsv must fall back safely");

  const auto& presets = dbpp::ui::AvailableThemePresets();
  dbpp_test::Expect(presets.size() == 5 && presets.front().preset == dbpp::ThemePreset::GeminiSpark,
         "AvailableThemePresets must expose 5 presets starting with GeminiSpark");

  for (const auto& meta : presets) {
    for (const bool dark : {false, true}) {
      const huxerui::ThemeSpec spec = dbpp::ui::BuildThemeSpec(meta.preset, dark);
      const dbpp::ui::SemanticColors sem = dbpp::ui::BuildSemanticColors(meta.preset, dark);
      dbpp_test::Expect(!(spec.colors.surface == spec.colors.surface_container_low),
             "Theme surface and surface_container_low must be layered: " + meta.name);
      dbpp_test::Expect(!(spec.colors.surface == spec.colors.on_surface),
             "Theme foreground and surface must remain distinct: " + meta.name);
      dbpp_test::Expect(spec.colors.scrim.alpha > 0.0F && spec.colors.scrim.alpha < 1.0F,
             "Theme scrim must remain translucent: " + meta.name);
      dbpp_test::Expect(sem.uses_gradient_chrome == (meta.preset != dbpp::ThemePreset::Monochrome),
             "Gradient chrome flag mismatch for preset: " + meta.name);
    }
  }
}

} // namespace

namespace dbpp_test {

void RunCliCommandTests(const fs::path& dir, const std::string& driver_path) {
  TestDatabaseCommands(dir);
  TestDriverCatalogAndInstallOffline(dir);
  TestErrorsAndExitCodes(dir);
  TestBatchInsertAndWorkspaceModel(dir, driver_path);
  TestConnectionOrdering();
  TestTableCellPreviewAndAutoWidth();
  TestSettingsAndThemePalettes(dir);

  // Extended ADBC CLI commands: info, stats, import, export, copy, query --param/--schema-only
  const fs::path cli_db = dir / "cli_adbc_ext.sqlite";
  const fs::path csv_file = dir / "cli_import.csv";
  dbpp_test::WriteTextFile(csv_file, "id,label\n1,one\n2,two\n");
  {
    const auto imp = dbpp_test::InvokeCli(
        {"import", "--sqlite", cli_db.string(), "ext_items", csv_file.string(), "--json"});
    dbpp_test::Expect(imp.code == dbpp::kCliExitOk && imp.out.find("\"ok\":true") != std::string::npos,
                      "CLI import failed: " + imp.err);

    const auto info = dbpp_test::InvokeCli({"info", "--sqlite", cli_db.string(), "--json"});
    dbpp_test::Expect(info.code == dbpp::kCliExitOk &&
                          info.out.find("SQLite") != std::string::npos,
                      "CLI info failed: " + info.err);

    const auto stats = dbpp_test::InvokeCli(
        {"stats", "--sqlite", cli_db.string(), "ext_items", "--json"});
    dbpp_test::Expect(stats.code == dbpp::kCliExitOk &&
                          stats.out.find("\"ok\":true") != std::string::npos,
                      "CLI stats failed: " + stats.err);

    const auto pq = dbpp_test::InvokeCli(
        {"query", "--sqlite", cli_db.string(),
         "SELECT label FROM ext_items WHERE id = ?", "--param", "2", "--json"});
    dbpp_test::Expect(pq.code == dbpp::kCliExitOk && pq.out.find("two") != std::string::npos,
                      "CLI parameterized query failed: " + pq.err);

    const auto dry = dbpp_test::InvokeCli(
        {"query", "--sqlite", cli_db.string(), "SELECT id, label FROM ext_items",
         "--schema-only", "--json"});
    dbpp_test::Expect(dry.code == dbpp::kCliExitOk &&
                          dry.out.find("\"label\"") != std::string::npos,
                      "CLI --schema-only failed: " + dry.err);

    const fs::path exp_file = dir / "cli_export.jsonl";
    const auto exp = dbpp_test::InvokeCli(
        {"export", "--sqlite", cli_db.string(), "SELECT id, label FROM ext_items ORDER BY id",
         "--out", exp_file.string(), "--json"});
    dbpp_test::Expect(exp.code == dbpp::kCliExitOk &&
                          exp.out.find("\"rows_written\":2") != std::string::npos,
                      "CLI export failed: " + exp.err);

    const fs::path exp_json = dir / "cli_export.json";
    const auto exp_j = dbpp_test::InvokeCli(
        {"export", "--sqlite", cli_db.string(), "SELECT id, label FROM ext_items ORDER BY id",
         "--type", "json", "--out", exp_json.string(), "--json"});
    dbpp_test::Expect(exp_j.code == dbpp::kCliExitOk &&
                          dbpp_test::ReadBinaryFile(exp_json).starts_with("[\n"),
                      "CLI export --type json failed: " + exp_j.err);

    const fs::path exp_sql = dir / "cli_export.sql";
    const auto exp_s = dbpp_test::InvokeCli(
        {"export", "--sqlite", cli_db.string(), "SELECT id, label FROM ext_items ORDER BY id",
         "--type", "sql", "--table", "custom_tbl", "--out", exp_sql.string(), "--json"});
    dbpp_test::Expect(exp_s.code == dbpp::kCliExitOk &&
                          dbpp_test::ReadBinaryFile(exp_sql).find("INSERT INTO \"custom_tbl\"") !=
                              std::string::npos,
                      "CLI export --type sql --table failed: " + exp_s.err);

    for (const char* ext : {".tsv", ".md", ".html", ".xml", ".xlsx"}) {
      const fs::path p = dir / (std::string("cli_auto") + ext);
      const auto r = dbpp_test::InvokeCli(
          {"export", "--sqlite", cli_db.string(), "SELECT id, label FROM ext_items ORDER BY id",
           "--out", p.string(), "--json"});
      dbpp_test::Expect(r.code == dbpp::kCliExitOk &&
                            r.out.find("\"rows_written\":2") != std::string::npos &&
                            fs::file_size(p) > 0,
                        std::string("CLI export extension auto-detect failed for ") + ext);
    }

    const auto bad_type = dbpp_test::InvokeCli(
        {"export", "--sqlite", cli_db.string(), "SELECT 1", "--type", "parquet", "--json"});
    dbpp_test::Expect(bad_type.code == dbpp::kCliExitUsage,
                      "CLI export invalid --type should exit with usage code 2");

    // Aligned CLI import across exported formats (auto-detected by extension + --type sql)
    for (const auto& [src, tbl] : std::vector<std::pair<fs::path, std::string>>{
             {exp_json, "imp_json"},
             {dir / "cli_auto.tsv", "imp_tsv"},
             {dir / "cli_auto.md", "imp_md"},
             {dir / "cli_auto.html", "imp_html"},
             {dir / "cli_auto.xml", "imp_xml"},
             {dir / "cli_auto.xlsx", "imp_xlsx"},
         }) {
      const auto ri = dbpp_test::InvokeCli(
          {"import", "--sqlite", cli_db.string(), tbl, src.string(), "--mode", "create", "--json"});
      dbpp_test::Expect(ri.code == dbpp::kCliExitOk &&
                            ri.out.find("\"rows_affected\":2") != std::string::npos,
                        "CLI import failed for " + src.filename().string() + ": " + ri.err);
    }
    const auto prep_sql = dbpp_test::InvokeCli(
        {"exec", "--sqlite", cli_db.string(), "CREATE TABLE custom_tbl (id INTEGER, label TEXT);",
         "--json"});
    dbpp_test::Expect(prep_sql.code == dbpp::kCliExitOk, "Prepare custom_tbl failed");
    const auto imp_sql = dbpp_test::InvokeCli(
        {"import", "--sqlite", cli_db.string(), "custom_tbl", exp_sql.string(), "--json"});
    dbpp_test::Expect(imp_sql.code == dbpp::kCliExitOk, "CLI import .sql failed: " + imp_sql.err);
    const auto bad_imp = dbpp_test::InvokeCli(
        {"import", "--sqlite", cli_db.string(), "t", csv_file.string(), "--type", "parquet",
         "--json"});
    dbpp_test::Expect(bad_imp.code == dbpp::kCliExitUsage,
                      "CLI import invalid --type should exit with usage code 2");

    const fs::path copy_dst = dir / "cli_copy_dst.sqlite";
    const auto cp = dbpp_test::InvokeCli(
        {"copy", "--src-sqlite", cli_db.string(), "--dst-sqlite", copy_dst.string(),
         "--table", "ext_items", "--json"});
    dbpp_test::Expect(cp.code == dbpp::kCliExitOk && cp.out.find("\"ok\":true") != std::string::npos,
                      "CLI copy failed: " + cp.err);
  }
}

} // namespace dbpp_test
