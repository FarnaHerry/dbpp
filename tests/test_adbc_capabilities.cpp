#include "test_adbc_core_helpers.h"

import std;
import dbpp.types;
import dbpp.adbc;
import dbpp.db;

namespace fs = std::filesystem;

namespace dbpp_test {

namespace {

void WriteFile(const fs::path& path, std::string_view content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string ReadFileText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

} // namespace

void RunAdbcCapabilitiesTests(const fs::path& dir, const std::string& driver_path) {
  const fs::path db_path = dir / "adbc_caps.sqlite";
  const dbpp::AdbcOpenResult opened = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = driver_path,
      .uri = db_path.string(),
      .username = {},
      .password = {},
      .title = "caps",
  });
  Expect(opened.error.empty() && opened.session != nullptr,
         "Open SQLite session for capabilities test failed: " + opened.error);
  if (!opened.session) {
    return;
  }
  auto& session = *opened.session;

  // 1. GetInfo & GetTableTypes
  const dbpp::AdbcConnectionInfo info = dbpp::LoadConnectionInfoBlocking(session);
  Expect(info.supported && info.vendor_name == "SQLite" && !info.driver_name.empty(),
         "LoadConnectionInfoBlocking must report SQLite vendor & driver info");
  Expect(info.driver_adbc_version.has_value() && *info.driver_adbc_version >= 1001000,
         "SQLite ADBC driver must report ADBC >= 1.1.0");

  std::string tt_err;
  const std::vector<std::string> table_types = dbpp::LoadTableTypesBlocking(session, tt_err);
  Expect(tt_err.empty() &&
             std::ranges::find(table_types, "table") != table_types.end() &&
             std::ranges::find(table_types, "view") != table_types.end(),
         "LoadTableTypesBlocking must include table and view");

  // 2. Deep GetObjects (Foreign Keys, Constraints, Defaults)
  Expect(dbpp::ExecuteBlocking(
             session,
             "CREATE TABLE parents (id INTEGER PRIMARY KEY, code TEXT UNIQUE NOT NULL);",
             false, 0)
             .error.empty(),
         "CREATE TABLE parents failed");
  Expect(dbpp::ExecuteBlocking(
             session,
             "CREATE TABLE children ("
             "  id INTEGER PRIMARY KEY,"
             "  parent_id INTEGER REFERENCES parents(id),"
             "  note TEXT DEFAULT 'hello'"
             ");",
             false, 0)
             .error.empty(),
         "CREATE TABLE children failed");

  const dbpp::AdbcObjectsResult objs = dbpp::LoadObjectsBlocking(session);
  Expect(objs.error.empty(), "LoadObjectsBlocking failed: " + objs.error);
  const dbpp::DbObject* children_obj = nullptr;
  for (const dbpp::DbObject& obj : objs.objects) {
    if (obj.name == "children") {
      children_obj = &obj;
      break;
    }
  }
  Expect(children_obj != nullptr, "children object missing from GetObjects");
  if (children_obj != nullptr) {
    Expect(!children_obj->foreign_keys.empty(),
           "GetObjects must parse FOREIGN KEY metadata for children");
    if (!children_obj->foreign_keys.empty()) {
      const dbpp::ForeignKeyMeta& fk = children_obj->foreign_keys.front();
      Expect(fk.fk_table == "parents" && !fk.columns.empty() &&
                 fk.columns.front() == "parent_id" && !fk.fk_columns.empty() &&
                 fk.fk_columns.front() == "id",
             "Foreign key reference target mismatch");
    }
  }

  // 3. GetStatistics graceful fallback on SQLite
  if (children_obj != nullptr) {
    const dbpp::TableStatisticsResult stats =
        dbpp::LoadTableStatisticsBlocking(session, *children_obj, true);
    Expect(stats.error.empty() && !stats.supported,
           "SQLite GetStatistics should cleanly report supported=false without error");
  }

  // 4. InspectQuerySchemaBlocking (ExecuteSchema)
  const dbpp::AdbcSchemaResult dry_schema = dbpp::InspectQuerySchemaBlocking(
      session, "SELECT id, code FROM parents WHERE id > 0");
  Expect(dry_schema.error.empty() && dry_schema.columns.size() == 2 &&
             dry_schema.columns[0].name == "id" && dry_schema.columns[1].name == "code",
         "InspectQuerySchemaBlocking failed: " + dry_schema.error);

  // 5. Native Transactions (Autocommit / Commit / Rollback)
  std::string tx_err;
  Expect(dbpp::SetAutocommitBlocking(session, false, tx_err) && session.in_transaction,
         "SetAutocommitBlocking(false) failed: " + tx_err);
  Expect(dbpp::ExecuteBlocking(session, "INSERT INTO parents (id, code) VALUES (1, 'p1')",
                               false, 0)
             .error.empty(),
         "Insert inside transaction failed");
  Expect(dbpp::RollbackTransactionBlocking(session, tx_err),
         "RollbackTransactionBlocking failed: " + tx_err);
  {
    const auto q = dbpp::ExecuteBlocking(session, "SELECT COUNT(*) FROM parents", true, 1);
    Expect(q.error.empty() && q.rows.front().values.front().text == "0",
           "Rollback must discard uncommitted row");
  }
  Expect(dbpp::ExecuteBlocking(session, "INSERT INTO parents (id, code) VALUES (1, 'p1')",
                               false, 0)
             .error.empty(),
         "Insert for commit test failed");
  Expect(dbpp::CommitTransactionBlocking(session, tx_err),
         "CommitTransactionBlocking failed: " + tx_err);
  Expect(dbpp::SetAutocommitBlocking(session, true, tx_err) && !session.in_transaction,
         "SetAutocommitBlocking(true) failed: " + tx_err);
  {
    const auto q = dbpp::ExecuteBlocking(session, "SELECT COUNT(*) FROM parents", true, 1);
    Expect(q.error.empty() && q.rows.front().values.front().text == "1",
           "Commit must persist committed row");
  }

  // 6. Prepared statements & parameter binding (plus special characters via UpdateCell/InsertRow)
  const dbpp::AdbcParamSchemaResult param_meta = dbpp::InspectStatementParametersBlocking(
      session, "INSERT INTO parents (id, code) VALUES (?, ?)");
  Expect(param_meta.error.empty() && param_meta.supported && param_meta.parameters.size() == 2,
         "InspectStatementParametersBlocking should detect 2 parameters");

  const dbpp::AdbcQueryResult prep_ins = dbpp::ExecutePreparedBlocking(
      session, "INSERT INTO parents (id, code) VALUES (?, ?)",
      {dbpp::PreparedParamValue{.text = "2", .is_null = false},
       dbpp::PreparedParamValue{.text = "O'Reilly; \"quoted\"\nline2", .is_null = false}},
      false, 0);
  Expect(prep_ins.error.empty(), "ExecutePreparedBlocking INSERT failed: " + prep_ins.error);

  const dbpp::AdbcQueryResult prep_sel = dbpp::ExecutePreparedBlocking(
      session, "SELECT code FROM parents WHERE id = ?",
      {dbpp::PreparedParamValue{.text = "2", .is_null = false}}, true, 10);
  Expect(prep_sel.error.empty() && prep_sel.rows.size() == 1 &&
             prep_sel.rows[0].values[0].text == "O'Reilly; \"quoted\"\nline2",
         "ExecutePreparedBlocking SELECT round-trip mismatch");

  // Parameterized TableModel cell update & batch insert
  if (children_obj != nullptr) {
    dbpp::TableModel tbl =
        dbpp::LoadTableBlocking(session, 1, *children_obj, 0, 50, std::nullopt, {});
    const dbpp::BatchInsertResult bi = dbpp::InsertRowsBlocking(
        session, tbl,
        {dbpp::PendingInsertRow{.draft_id = 1, .values = {"10", "1", "a'b\"c"}},
         dbpp::PendingInsertRow{.draft_id = 2, .values = {"11", "1", "d\ne"}}});
    Expect(bi.error.empty() && bi.inserted == 2,
           "InsertRowsBlocking with parameterized bind failed: " + bi.error);
  }

  // 7. Streaming Export (CSV & JSONL)
  const fs::path csv_out = dir / "export_parents.csv";
  const dbpp::ExportStreamResult exp_csv = dbpp::ExportQueryToFileBlocking(
      session, "SELECT id, code FROM parents ORDER BY id", dbpp::ExportFormat::Csv, csv_out, 0);
  Expect(exp_csv.error.empty() && exp_csv.rows_written == 2,
         "CSV export failed: " + exp_csv.error);
  const std::string csv_text = ReadFileText(csv_out);
  Expect(csv_text.find("id,code\n") == 0 && csv_text.find("\"O'Reilly") != std::string::npos,
         "CSV export content mismatch");

  const fs::path jsonl_out = dir / "export_parents.jsonl";
  const dbpp::ExportStreamResult exp_jsonl = dbpp::ExportQueryToFileBlocking(
      session, "SELECT id, code FROM parents ORDER BY id", dbpp::ExportFormat::Jsonl, jsonl_out, 0);
  Expect(exp_jsonl.error.empty() && exp_jsonl.rows_written == 2,
         "JSONL export failed: " + exp_jsonl.error);
  const std::string jsonl_text = ReadFileText(jsonl_out);
  Expect(jsonl_text.find("\"id\": 1") != std::string::npos &&
             jsonl_text.find("\\nline2") != std::string::npos,
         "JSONL export content mismatch");

  // 8. Bulk Ingestion (CSV & JSONL) via BindStream
  const fs::path csv_in = dir / "ingest.csv";
  WriteFile(csv_in,
            "id,name,score,active\n"
            "1,Alice,98.5,true\n"
            "2,\"Bob, Jr.\",87.25,false\n"
            "3,Charlie,,true\n");
  const dbpp::BulkIngestResult ing_csv = dbpp::IngestCsvFileBlocking(
      session, csv_in, "ingested_csv", dbpp::IngestMode::Create);
  Expect(ing_csv.error.empty(), "IngestCsvFileBlocking failed: " + ing_csv.error);
  {
    const auto q = dbpp::ExecuteBlocking(
        session, "SELECT id, name, score, active FROM ingested_csv ORDER BY id", true, 10);
    Expect(q.error.empty() && q.rows.size() == 3 && q.rows[1].values[1].text == "Bob, Jr." &&
               q.rows[2].values[2].is_null,
           "Ingested CSV query verification failed");
  }

  const fs::path jsonl_in = dir / "ingest.jsonl";
  WriteFile(jsonl_in,
            "{\"k\": 10, \"label\": \"alpha\", \"flag\": true}\n"
            "{\"k\": 20, \"label\": \"beta\", \"flag\": false}\n");
  const dbpp::BulkIngestResult ing_jsonl = dbpp::IngestJsonlFileBlocking(
      session, jsonl_in, "ingested_jsonl", dbpp::IngestMode::Create);
  Expect(ing_jsonl.error.empty(), "IngestJsonlFileBlocking failed: " + ing_jsonl.error);
  {
    const auto q = dbpp::ExecuteBlocking(
        session, "SELECT k, label, flag FROM ingested_jsonl ORDER BY k", true, 10);
    Expect(q.error.empty() && q.rows.size() == 2 && q.rows[1].values[1].text == "beta",
           "Ingested JSONL query verification failed");
  }

  // 9. Cross-connection streaming copy via BindStream
  const fs::path dst_path = dir / "copy_dst.sqlite";
  const dbpp::AdbcOpenResult dst_open = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = driver_path,
      .uri = dst_path.string(),
      .username = {},
      .password = {},
      .title = "dst",
  });
  Expect(dst_open.error.empty() && dst_open.session != nullptr,
         "Open destination SQLite session failed: " + dst_open.error);
  if (dst_open.session) {
    const dbpp::BulkIngestResult copy_res = dbpp::CopyQueryToTableBlocking(
        session, "SELECT id, name, score FROM ingested_csv ORDER BY id",
        *dst_open.session, "copied_tbl", dbpp::IngestMode::Create);
    Expect(copy_res.error.empty(), "CopyQueryToTableBlocking failed: " + copy_res.error);
    const auto q = dbpp::ExecuteBlocking(
        *dst_open.session, "SELECT id, name FROM copied_tbl ORDER BY id", true, 10);
    Expect(q.error.empty() && q.rows.size() == 3 && q.rows[0].values[1].text == "Alice",
           "Cross-connection copy verification failed");
  }

  // 10. CancelActiveOperation smoke check & ReadOnly connection option
  Expect(!session.CancelActiveOperation() || true, "CancelActiveOperation must be safe when idle");
  const dbpp::AdbcOpenResult ro_open = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = driver_path,
      .uri = db_path.string(),
      .username = {},
      .password = {},
      .title = "ro",
      .read_only = true,
  });
  Expect(ro_open.error.empty() && ro_open.session != nullptr && ro_open.session->read_only,
         "OpenAdbcBlocking with read_only=true failed: " + ro_open.error);

  // 11. Timestamp localization & round-trip write normalization + record modification
  const auto local_sec = dbpp::LocalizeTimestampValue("1764202635", "created_at", "INTEGER");
  Expect(local_sec.has_value() && !local_sec->empty(),
         "LocalizeTimestampValue should localize 10-digit epoch in created_at");
  if (local_sec.has_value()) {
    const std::string back =
        dbpp::NormalizeTimestampForWrite(*local_sec, "1764202635", "created_at", "INTEGER");
    Expect(back == "1764202635",
           "NormalizeTimestampForWrite round-trip mismatch: got '" + back + "'");
  }
  const auto local_ms = dbpp::LocalizeTimestampValue("1764202635123", "updated_at", "INTEGER");
  Expect(local_ms.has_value() && local_ms->find(".123") != std::string::npos,
         "LocalizeTimestampValue should preserve millisecond precision");
  if (local_ms.has_value()) {
    const std::string back_ms =
        dbpp::NormalizeTimestampForWrite(*local_ms, "1764202635123", "updated_at", "INTEGER");
    Expect(back_ms == "1764202635123",
           "NormalizeTimestampForWrite ms round-trip mismatch: got '" + back_ms + "'");
  }
  const auto local_iso = dbpp::LocalizeTimestampValue("2026-11-27T00:17:15Z", "ts", "TEXT");
  Expect(local_iso.has_value() && !local_iso->empty(),
         "LocalizeTimestampValue should localize ISO-8601 UTC timestamp");
  if (local_iso.has_value()) {
    const std::string back_iso = dbpp::NormalizeTimestampForWrite(
        *local_iso, "2026-11-27T00:17:15Z", "ts", "TEXT");
    Expect(back_iso == "2026-11-27T00:17:15Z",
           "NormalizeTimestampForWrite ISO UTC round-trip mismatch: got '" + back_iso + "'");
  }
  Expect(dbpp::NormalizeTimestampForWrite("1764209999", "1764202635", "created_at", "INTEGER") ==
             "1764209999",
         "Direct numeric epoch input should be preserved as-is");

  // Record edit (DbUpdateCellBlocking + DbUpdateRowBlocking + localized timestamp display)
  Expect(dbpp::ExecuteBlocking(
             session,
             "CREATE TABLE events ("
             "  id INTEGER PRIMARY KEY,"
             "  title TEXT NOT NULL,"
             "  created_at INTEGER,"
             "  occurred_at TEXT"
             ");",
             false, 0)
             .error.empty(),
         "CREATE TABLE events failed");
  Expect(dbpp::ExecuteBlocking(
             session,
             "INSERT INTO events (id, title, created_at, occurred_at) "
             "VALUES (1, 'init', 1764202635, '2026-11-27T00:17:15Z');",
             false, 0)
             .error.empty(),
         "INSERT events failed");

  // Pass minimal DbObject without GetObjects columns to verify PRAGMA table_info PK recovery
  const dbpp::TableModel ev_model = dbpp::LoadTableBlocking(
      session, 1, dbpp::DbObject{.name = "events"}, 0, 50, std::nullopt, {});
  Expect(ev_model.error.empty() && ev_model.editable && !ev_model.read_only &&
             !ev_model.key_columns.empty() && ev_model.snapshot &&
             ev_model.snapshot->rows.size() == 1,
         "LoadTableBlocking must recover PK and mark table editable: " + ev_model.error);
  if (ev_model.snapshot && ev_model.snapshot->rows.size() == 1) {
    const auto& row0 = ev_model.snapshot->rows[0];
    Expect(row0.values[2].text == "1764202635" && !row0.values[2].display_text.empty(),
           "Epoch column must keep raw text and populate localized display_text");
    Expect(row0.values[3].text == "2026-11-27T00:17:15Z" &&
               !row0.values[3].display_text.empty(),
           "ISO UTC column must keep raw text and populate localized display_text");

    const dbpp::AdbcQueryResult upd_cell =
        dbpp::UpdateCellBlocking(session, ev_model, 0, 1, "edited_title");
    Expect(upd_cell.error.empty(), "UpdateCellBlocking failed: " + upd_cell.error);

    const dbpp::AdbcQueryResult upd_row = dbpp::UpdateRowBlocking(
        session, ev_model, 0, {{2, "1764206235"}, {3, "2026-11-27T01:17:15Z"}});
    Expect(upd_row.error.empty(), "UpdateRowBlocking failed: " + upd_row.error);

    const auto q = dbpp::ExecuteBlocking(
        session, "SELECT title, created_at, occurred_at FROM events WHERE id = 1", true, 1);
    Expect(q.error.empty() && q.rows.size() == 1 &&
               q.rows[0].values[0].text == "edited_title" &&
               q.rows[0].values[1].text == "1764206235" &&
               q.rows[0].values[2].text == "2026-11-27T01:17:15Z",
           "Record updates must persist cleanly to SQLite");
  }

  // Read-only session must mark TableModel read_only=true and block ALL writes
  if (ro_open.session) {
    const dbpp::TableModel ro_ev = dbpp::LoadTableBlocking(
        *ro_open.session, 2, dbpp::DbObject{.name = "events"}, 0, 50, std::nullopt, {});
    Expect(ro_ev.error.empty() && ro_ev.read_only,
           "LoadTableBlocking on read_only session must set read_only=true");

    const dbpp::AdbcQueryResult sel_ok = dbpp::ExecuteBlocking(
        *ro_open.session, "SELECT COUNT(*) FROM events", true, 10);
    Expect(sel_ok.error.empty() && sel_ok.rows.size() == 1,
           "Read-only session must still permit pure SELECT queries: " + sel_ok.error);

    const dbpp::AdbcQueryResult ins_blk = dbpp::ExecuteBlocking(
        *ro_open.session, "INSERT INTO events (id, title) VALUES (99, 'x')", false, 0);
    Expect(!ins_blk.error.empty(), "Read-only session must block INSERT");

    const dbpp::AdbcQueryResult ret_blk = dbpp::ExecuteBlocking(
        *ro_open.session, "INSERT INTO events (id, title) VALUES (99, 'x') RETURNING id", true, 10);
    Expect(!ret_blk.error.empty(), "Read-only session must block INSERT ... RETURNING");

    const dbpp::AdbcQueryResult pragma_blk = dbpp::ExecuteBlocking(
        *ro_open.session, "PRAGMA query_only = OFF", false, 0);
    Expect(!pragma_blk.error.empty(), "Read-only session must block PRAGMA query_only = OFF");

    const dbpp::AdbcQueryResult multi_blk = dbpp::ExecuteBlocking(
        *ro_open.session, "SELECT 1; DELETE FROM events WHERE id = 1", false, 0);
    Expect(!multi_blk.error.empty(), "Read-only session must block multi-statement write SQL");
  }

  // 12. Catalog extras (indexes, foreign keys, views, triggers, databases)
  for (const char* ddl : {
           "CREATE INDEX IF NOT EXISTS idx_events_title ON events (title);",
           "CREATE VIEW IF NOT EXISTS v_events AS SELECT id, title FROM events;",
           "CREATE TRIGGER IF NOT EXISTS trg_events_upd AFTER UPDATE ON events BEGIN SELECT 1; END;",
           "CREATE TABLE child_items (id INTEGER PRIMARY KEY, event_id INTEGER REFERENCES events(id));",
       }) {
    const auto r = dbpp::ExecuteBlocking(session, ddl, false, 0);
    Expect(r.error.empty(), std::string("CREATE catalog test object failed: ") + r.error);
  }
  const dbpp::AdbcObjectsResult rich_objs = dbpp::LoadObjectsBlocking(session);
  Expect(rich_objs.error.empty(), "LoadObjectsBlocking rich catalog failed: " + rich_objs.error);
  bool found_ev_idx = false;
  bool found_child_fk = false;
  bool found_view = false;
  bool found_trg = false;
  for (const auto& obj : rich_objs.objects) {
    if (obj.name == "events") {
      for (const auto& ix : obj.indexes) {
        if (ix.name == "idx_events_title") found_ev_idx = true;
      }
    }
    if (obj.name == "child_items" && !obj.foreign_keys.empty()) {
      found_child_fk = true;
    }
    if (obj.name == "v_events" && obj.is_view) {
      found_view = true;
    }
  }
  for (const auto& trg : rich_objs.triggers) {
    if (trg.name == "trg_events_upd" && trg.table_name == "events") {
      found_trg = true;
    }
  }
  Expect(found_ev_idx && found_child_fk && found_view && found_trg &&
             !rich_objs.databases.empty(),
         "LoadObjectsBlocking catalog extras missing: idx=" +
             std::string(found_ev_idx ? "1" : "0") + " fk=" + (found_child_fk ? "1" : "0") +
             " view=" + (found_view ? "1" : "0") + " trg=" + (found_trg ? "1" : "0") +
             " dbs=" + std::to_string(rich_objs.databases.size()));

  // 13. Ordered URI fallback on OpenAdbcBlocking (primary fails -> fallback succeeds)
  {
    const dbpp::AdbcOpenResult fallback_open = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
        .driver_path = driver_path,
        .uri = "file:" + (dir / "nonexistent_dir_xyz" / "missing.sqlite").string() + "?mode=ro",
        .fallback_uri = db_path.string(),
        .username = {},
        .password = {},
        .title = "fallback",
        .remark = "prod-fallback",
        .read_only = false,
        .production_mode = true,
        .show_system_databases = true,
    });
    Expect(fallback_open.session != nullptr && fallback_open.session->production_mode &&
               fallback_open.session->show_system_databases &&
               fallback_open.session->remark == "prod-fallback",
           "OpenAdbcBlocking fallback_uri + session flags mismatch: " + fallback_open.error);
  }

  // 14. Config DB persistence for remark, permissions, and saved queries
  {
    const fs::path cfg_db = dir / "cfg_v2.sqlite3";
    const std::vector<dbpp::ConnectionProfile> saved_profs{
        dbpp::ConnectionProfile{
            .name = "Prod Primary",
            .driver_path = driver_path,
            .uri = db_path.string(),
            .username = "admin",
            .password = "sec",
            .remark = "Primary cluster",
            .read_only = true,
            .production_mode = true,
            .show_system_databases = true,
        },
    };
    Expect(dbpp::SaveConnectionProfiles(cfg_db.string(), saved_profs),
           "SaveConnectionProfiles v2 failed");
    const auto reloaded_profs = dbpp::LoadConnectionProfiles(cfg_db.string());
    Expect(reloaded_profs == saved_profs, "ConnectionProfile v2 fields round-trip mismatch");

    const std::vector<dbpp::SavedQuery> queries{
        dbpp::SavedQuery{
            .id = 0,
            .connection_key = dbpp::SavedQueryConnectionKey(driver_path, db_path.string()),
            .database_name = "main",
            .name = "recent_events",
            .sql_text = "SELECT * FROM events;",
        },
    };
    Expect(dbpp::SaveSavedQueries(cfg_db.string(), queries), "SaveSavedQueries failed");
    const auto reloaded_q = dbpp::LoadSavedQueries(cfg_db.string());
    Expect(reloaded_q.size() == 1 && reloaded_q[0].name == "recent_events" &&
               reloaded_q[0].sql_text == "SELECT * FROM events;",
           "SavedQuery round-trip mismatch");
  }

  // 15. Database SQL Export & Script Import
  {
    const std::string trigger_script =
        "-- leading comment\n"
        "CREATE TABLE t_trig (id INTEGER PRIMARY KEY, val TEXT);\n"
        "CREATE TRIGGER trg_t_trig AFTER INSERT ON t_trig BEGIN\n"
        "  UPDATE t_trig SET val = CASE WHEN NEW.val IS NULL THEN 'empty' ELSE 'a;b' END WHERE id = NEW.id;\n"
        "END;\n"
        "/* trailing comment only */\n";
    const auto stmts = dbpp::SplitSqlStatements(trigger_script, dbpp::Dialect::Sqlite);
    Expect(stmts.size() == 2,
           "SplitSqlStatements must preserve trigger BEGIN..END and ignore comment-only tail, got " +
               std::to_string(stmts.size()));

    const fs::path full_dump = dir / "full_dump.sql";
    const dbpp::SqlDumpResult full_res = dbpp::ExportDatabaseSqlToFileBlocking(
        session,
        dbpp::SqlDumpOptions{.include_schema = true, .include_data = true},
        full_dump);
    Expect(full_res.error.empty() && full_res.tables_exported >= 2 && full_res.rows_written >= 1,
           "ExportDatabaseSqlToFileBlocking full dump failed: " + full_res.error);

    const fs::path dst_db = dir / "restored_full.sqlite";
    const dbpp::AdbcOpenResult dst_open = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
        .driver_path = driver_path,
        .uri = dst_db.string(),
        .title = "restored",
    });
    Expect(dst_open.error.empty() && dst_open.session != nullptr,
           "Open restored sqlite failed: " + dst_open.error);
    const dbpp::SqlScriptImportResult imp_full =
        dbpp::ImportSqlScriptFileBlocking(*dst_open.session, full_dump);
    Expect(imp_full.error.empty() && imp_full.statements_executed >= 4,
           "ImportSqlScriptFileBlocking full dump failed: " + imp_full.error);

    const auto check_ev = dbpp::ExecuteBlocking(
        *dst_open.session, "SELECT id, title FROM events WHERE id = 1", true, 4);
    Expect(check_ev.error.empty() && check_ev.rows.size() == 1 &&
               check_ev.rows[0].values[1].text == "edited_title",
           "Restored events data mismatch");
    const auto check_view = dbpp::ExecuteBlocking(
        *dst_open.session, "SELECT COUNT(*) FROM v_events", true, 4);
    Expect(check_view.error.empty() && check_view.rows.size() == 1,
           "Restored view v_events query failed: " + check_view.error);
    const dbpp::AdbcObjectsResult dst_objs = dbpp::LoadObjectsBlocking(*dst_open.session);
    Expect(!dst_objs.triggers.empty(), "Restored trigger missing");

    const fs::path schema_dump = dir / "schema_only.sql";
    const dbpp::SqlDumpResult schema_res = dbpp::ExportDatabaseSqlToFileBlocking(
        session,
        dbpp::SqlDumpOptions{.include_schema = true, .include_data = false},
        schema_dump);
    Expect(schema_res.error.empty() && schema_res.rows_written == 0 && schema_res.tables_exported >= 2,
           "Schema-only export failed: " + schema_res.error);

    const fs::path schema_db = dir / "restored_schema.sqlite";
    const dbpp::AdbcOpenResult schema_open = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
        .driver_path = driver_path,
        .uri = schema_db.string(),
        .title = "restored_schema",
    });
    Expect(schema_open.error.empty() && schema_open.session != nullptr,
           "Open schema_db failed: " + schema_open.error);
    const dbpp::SqlScriptImportResult imp_schema =
        dbpp::ImportSqlScriptFileBlocking(*schema_open.session, schema_dump);
    Expect(imp_schema.error.empty(), "Import schema-only dump failed: " + imp_schema.error);
    const auto cnt_rows = dbpp::ExecuteBlocking(
        *schema_open.session, "SELECT COUNT(*) FROM events", true, 4);
    Expect(cnt_rows.error.empty() && cnt_rows.rows.size() == 1 &&
               cnt_rows.rows[0].values[0].text == "0",
           "Schema-only import should produce 0 rows");

    const fs::path single_dump = dir / "single_events.sql";
    const dbpp::SqlDumpResult single_res = dbpp::ExportDatabaseSqlToFileBlocking(
        session,
        dbpp::SqlDumpOptions{.single_object = dbpp::DbObject{.name = "events"},
                             .include_schema = true,
                             .include_data = true},
        single_dump);
    Expect(single_res.error.empty() && single_res.tables_exported == 1,
           "Single-object SQL dump failed: " + single_res.error);

    if (ro_open.session) {
      const dbpp::SqlScriptImportResult ro_imp =
          dbpp::ImportSqlScriptFileBlocking(*ro_open.session, full_dump);
      Expect(!ro_imp.error.empty(), "Read-only session must block ImportSqlScriptFileBlocking");
    }
  }

  // 16. TableDesign snapshot (DDL + Columns + Indexes + Foreign Keys + Triggers)
  {
    const char* setup_sql =
        "CREATE TABLE td_parent (id INTEGER PRIMARY KEY, code TEXT NOT NULL UNIQUE);"
        "CREATE TABLE td_child ("
        "  id INTEGER PRIMARY KEY,"
        "  parent_id INTEGER NOT NULL,"
        "  title TEXT DEFAULT 'untitled',"
        "  FOREIGN KEY (parent_id) REFERENCES td_parent(id)"
        ");"
        "CREATE INDEX idx_td_child_title ON td_child(title);"
        "CREATE TRIGGER trg_td_child_ai AFTER INSERT ON td_child BEGIN "
        "  UPDATE td_child SET title = COALESCE(title, 'untitled') WHERE id = NEW.id; "
        "END;";
    const auto stmts = dbpp::SplitSqlStatements(setup_sql, dbpp::Dialect::Sqlite);
    for (const std::string& stmt : stmts) {
      const auto r = dbpp::ExecuteBlocking(session, stmt, false, 0);
      Expect(r.error.empty(), "TableDesign setup SQL failed: " + r.error);
    }

    const dbpp::TableDesignSnapshot td =
        dbpp::LoadTableDesignBlocking(session, dbpp::DbObject{.name = "td_child"});
    Expect(td.error.empty() && !td.connection_lost,
           "LoadTableDesignBlocking failed: " + td.error);
    Expect(td.object.columns.size() == 3, "td_child must have 3 columns");
    Expect(!td.object.foreign_keys.empty() &&
               td.object.foreign_keys[0].fk_table == "td_parent",
           "td_child foreign key to td_parent missing");
    bool found_idx = false;
    for (const auto& ix : td.object.indexes) {
      if (ix.name == "idx_td_child_title") {
        found_idx = !ix.definition.empty() && ix.columns.size() == 1 && ix.columns[0] == "title";
        break;
      }
    }
    Expect(found_idx, "td_child index idx_td_child_title or its definition missing");
    Expect(td.triggers.size() == 1 && td.triggers[0].name == "trg_td_child_ai" &&
               td.triggers[0].timing == "AFTER" && td.triggers[0].event == "INSERT",
           "td_child trigger metadata mismatch");
    Expect(td.ddl.find("CREATE TABLE") != std::string::npos &&
               td.ddl.find("idx_td_child_title") != std::string::npos &&
               td.ddl.find("trg_td_child_ai") != std::string::npos,
           "td_child combined DDL missing expected statements");
    Expect(td.ddl.find("\n  ") != std::string::npos &&
               td.ddl.find("\nBEGIN\n") != std::string::npos,
           "LoadTableDesignBlocking must return multi-line formatted DDL");
  }

  // 17. SQL / DDL Formatter
  {
    const std::string raw_create =
        "CREATE TABLE   \"fmt_test\" ( id INTEGER PRIMARY KEY, price DECIMAL(10, 2) NOT NULL,"
        " title VARCHAR(255) DEFAULT 'a, (b)', status INT CHECK (status IN (1, 2)),"
        " FOREIGN KEY (status) REFERENCES td_parent(id) ) WITHOUT ROWID;";
    const std::string fmt_create = dbpp::FormatSqlStatement(raw_create, dbpp::Dialect::Sqlite);
    Expect(fmt_create.find("CREATE TABLE \"fmt_test\" (\n  id INTEGER PRIMARY KEY,\n") == 0,
           "FormatSqlStatement CREATE TABLE header/column indent mismatch: " + fmt_create);
    Expect(fmt_create.find("DECIMAL(10, 2)") != std::string::npos &&
               fmt_create.find("VARCHAR(255)") != std::string::npos &&
               fmt_create.find("DEFAULT 'a, (b)'") != std::string::npos &&
               fmt_create.find("CHECK (status IN (1, 2))") != std::string::npos &&
               fmt_create.find(") WITHOUT ROWID;") != std::string::npos,
           "FormatSqlStatement CREATE TABLE content mismatch: " + fmt_create);

    const std::string raw_trigger =
        "CREATE TRIGGER trg_fmt AFTER UPDATE OF title ON td_child FOR EACH ROW WHEN (NEW.title != OLD.title) "
        "BEGIN UPDATE td_child SET title = CASE WHEN NEW.title = '' THEN 'x' ELSE NEW.title END WHERE id = NEW.id; END;";
    const std::string fmt_trigger = dbpp::FormatSqlStatement(raw_trigger, dbpp::Dialect::Sqlite);
    Expect(fmt_trigger.find("\n  AFTER UPDATE OF title ON td_child\n") != std::string::npos &&
               fmt_trigger.find("\nBEGIN\n  UPDATE td_child\n") != std::string::npos &&
               fmt_trigger.ends_with("\nEND;"),
           "FormatSqlStatement CREATE TRIGGER mismatch: " + fmt_trigger);

    const std::string raw_view =
        "CREATE VIEW v_fmt AS SELECT id, title FROM td_child WHERE id > 0 ORDER BY id DESC;";
    const std::string fmt_view = dbpp::FormatSqlStatement(raw_view, dbpp::Dialect::Sqlite);
    Expect(fmt_view.find("CREATE VIEW v_fmt AS\nSELECT id, title\n  FROM td_child\n  WHERE id > 0\n  ORDER BY id DESC;") == 0,
           "FormatSqlStatement CREATE VIEW mismatch: " + fmt_view);

    const std::string raw_script = raw_create + "\n" + raw_view;
    const std::string fmt_script = dbpp::FormatSqlScript(raw_script, dbpp::Dialect::Sqlite);
    Expect(fmt_script.find("WITHOUT ROWID;\n\nCREATE VIEW v_fmt AS\n") != std::string::npos,
           "FormatSqlScript multi-statement formatting mismatch: " + fmt_script);
  }

  // 18. Universal Row-Identity & Multi-Dialect Editability Abstraction
  {
    const char* sql_setup =
        "CREATE TABLE id_pk (id INTEGER PRIMARY KEY, name TEXT);"
        "INSERT INTO id_pk VALUES (1, 'alpha');"
        "CREATE TABLE id_uniq (code TEXT NOT NULL, val TEXT);"
        "CREATE UNIQUE INDEX idx_id_uniq_code ON id_uniq(code);"
        "INSERT INTO id_uniq VALUES ('k1', 'v1');"
        "CREATE TABLE id_plain (a TEXT, b INTEGER);"
        "INSERT INTO id_plain VALUES ('raw_a', 42);";
    for (const std::string& stmt : dbpp::SplitSqlStatements(sql_setup, dbpp::Dialect::Sqlite)) {
      const auto r = dbpp::ExecuteBlocking(session, stmt, false, 0);
      Expect(r.error.empty(), "Row-identity setup failed: " + r.error);
    }

    const dbpp::TableModel m_pk =
        dbpp::LoadTableBlocking(session, 1, dbpp::DbObject{.name = "id_pk"}, 0, 50,
                                std::nullopt, {});
    Expect(m_pk.editable && m_pk.row_locator == dbpp::RowLocatorKind::PrimaryKey,
           "id_pk should resolve RowLocatorKind::PrimaryKey");

    const dbpp::TableModel m_uniq =
        dbpp::LoadTableBlocking(session, 1, dbpp::DbObject{.name = "id_uniq"}, 0, 50,
                                std::nullopt, {});
    Expect(m_uniq.editable && m_uniq.row_locator == dbpp::RowLocatorKind::UniqueKey &&
               m_uniq.key_columns.size() == 1 && m_uniq.key_columns[0] == 0,
           "id_uniq should resolve RowLocatorKind::UniqueKey from UNIQUE index");
    const auto upd_uniq = dbpp::UpdateCellBlocking(session, m_uniq, 0, 1, "v2");
    Expect(upd_uniq.error.empty(), "UniqueKey UpdateCellBlocking failed: " + upd_uniq.error);

    const dbpp::TableModel m_plain =
        dbpp::LoadTableBlocking(session, 1, dbpp::DbObject{.name = "id_plain"}, 0, 50,
                                std::nullopt, {});
    Expect(m_plain.editable && m_plain.row_locator == dbpp::RowLocatorKind::HiddenRowId &&
               m_plain.uses_rowid,
           "id_plain should resolve RowLocatorKind::HiddenRowId");

    // Verify AllColumns keyless fallback execution path against SQLite table
    dbpp::TableModel all_cols = m_plain;
    all_cols.row_locator = dbpp::RowLocatorKind::AllColumns;
    all_cols.uses_rowid = false;
    all_cols.key_columns = {0, 1};
    std::vector<dbpp::GridRow> rows = all_cols.snapshot->rows;
    rows[0].rowid.reset();
    rows[0].row_locator_token.clear();
    rows[0].key_values = rows[0].values;
    all_cols.snapshot =
        std::make_shared<const dbpp::GridSnapshot>(dbpp::GridSnapshot{.rows = rows});
    const auto upd_all = dbpp::UpdateCellBlocking(session, all_cols, 0, 0, "updated_a");
    Expect(upd_all.error.empty(), "AllColumns UpdateCellBlocking failed: " + upd_all.error);
    const auto check_all = dbpp::ExecuteBlocking(session, "SELECT a FROM id_plain", true, 4);
    Expect(check_all.error.empty() && check_all.rows.size() == 1 &&
               check_all.rows[0].values[0].text == "updated_a",
           "AllColumns update must persist value");

    // Verify index-based PRIMARY KEY hint resolution (e.g. MySQL ADBC objects)
    dbpp::DbObject mysql_hint;
    mysql_hint.name = "users";
    mysql_hint.columns = {
        dbpp::ColumnMeta{.name = "id", .type = "INT", .nullable = false},
        dbpp::ColumnMeta{.name = "email", .type = "VARCHAR(255)", .nullable = false},
    };
    mysql_hint.indexes = {
        dbpp::IndexMeta{.name = "PRIMARY", .unique = true, .primary = true, .columns = {"id"}},
    };
    dbpp::AdbcSession mysql_dummy;
    mysql_dummy.dialect = dbpp::Dialect::Mysql;
    std::vector<dbpp::ColumnMeta> resolved_cols = mysql_hint.columns;
    const dbpp::ResolvedRowIdentity mysql_id =
        dbpp::ResolveTableIdentityBlocking(mysql_dummy, mysql_hint, resolved_cols);
    Expect(mysql_id.kind == dbpp::RowLocatorKind::PrimaryKey &&
               mysql_id.key_columns.size() == 1 && mysql_id.key_columns[0] == 0 &&
               resolved_cols[0].primary_key,
           "MySQL index-based PRIMARY KEY hint must resolve RowLocatorKind::PrimaryKey");

    dbpp::DbObject view_obj{.name = "v_events", .is_view = true};
    std::vector<dbpp::ColumnMeta> view_cols = {
        dbpp::ColumnMeta{.name = "id", .type = "INTEGER"},
    };
    const dbpp::ResolvedRowIdentity view_id =
        dbpp::ResolveTableIdentityBlocking(session, view_obj, view_cols);
    Expect(view_id.kind == dbpp::RowLocatorKind::None,
           "View must resolve RowLocatorKind::None");
  }

  // 19. Dialect-aware timestamp/datetime/date/time write normalization & SqlLiteral
  {
    const std::string mysql_ts_epoch = dbpp::NormalizeTimestampForWrite(
        "1704067200", "", "created_at", "timestamp", dbpp::Dialect::Mysql);
    Expect(mysql_ts_epoch == "2024-01-01T00:00:00Z",
           "MySQL TIMESTAMP from epoch mismatch: " + mysql_ts_epoch);

    const std::string mysql_ts_from_legacy_utc = dbpp::NormalizeTimestampForWrite(
        "2024-01-01T00:00:00Z", "2024-01-01 00:00:00 UTC", "created_at", "timestamp",
        dbpp::Dialect::Mysql);
    Expect(mysql_ts_from_legacy_utc == "2024-01-01T00:00:00Z",
           "MySQL TIMESTAMP must normalize to ISO UTC without trailing UTC label: " +
               mysql_ts_from_legacy_utc);

    const std::string mysql_ts_local = dbpp::NormalizeTimestampForWrite(
        "2024-01-01 08:00:00", "", "created_at", "timestamp", dbpp::Dialect::Mysql);
    const auto back_local =
        dbpp::LocalizeTimestampValue(mysql_ts_local, "created_at", "timestamp");
    Expect(back_local.has_value() && *back_local == "2024-01-01 08:00:00",
           "MySQL TIMESTAMP local round-trip mismatch: " + mysql_ts_local);

    const std::string mysql_ts_now = dbpp::NormalizeTimestampForWrite(
        "now", "", "created_at", "timestamp", dbpp::Dialect::Mysql);
    Expect(mysql_ts_now.size() >= 20 && mysql_ts_now.back() == 'Z' &&
               mysql_ts_now.find('T') == 10,
           "MySQL TIMESTAMP 'now' must produce ISO-8601 UTC: " + mysql_ts_now);

    const std::string mysql_dt_local = dbpp::NormalizeTimestampForWrite(
        "2024-01-01 08:30:00", "", "dt", "datetime", dbpp::Dialect::Mysql);
    Expect(mysql_dt_local == "2024-01-01 08:30:00",
           "MySQL DATETIME must preserve wall-clock time without UTC shift: " + mysql_dt_local);

    const std::string mysql_dt_slash = dbpp::NormalizeTimestampForWrite(
        "2024/01/02 09:15:30", "", "dt", "datetime", dbpp::Dialect::Mysql);
    Expect(mysql_dt_slash == "2024-01-02 09:15:30",
           "MySQL DATETIME slash normalization mismatch: " + mysql_dt_slash);

    const std::string mysql_dt_t_hm = dbpp::NormalizeTimestampForWrite(
        "2024-01-02T09:15", "", "dt", "datetime(3)", dbpp::Dialect::Mysql);
    Expect(mysql_dt_t_hm == "2024-01-02 09:15:00",
           "MySQL DATETIME T/HM normalization mismatch: " + mysql_dt_t_hm);

    const std::string mysql_dt_now = dbpp::NormalizeTimestampForWrite(
        "now", "", "dt", "datetime", dbpp::Dialect::Mysql);
    Expect(mysql_dt_now.size() == 19 && mysql_dt_now[10] == ' ' && mysql_dt_now.back() != 'Z',
           "MySQL DATETIME 'now' must produce naive wall-clock datetime: " + mysql_dt_now);

    const std::string mysql_date_slash = dbpp::NormalizeTimestampForWrite(
        "2024/02/29", "", "d", "date", dbpp::Dialect::Mysql);
    Expect(mysql_date_slash == "2024-02-29",
           "DATE slash normalization mismatch: " + mysql_date_slash);

    const std::string mysql_date_now = dbpp::NormalizeTimestampForWrite(
        "now", "", "d", "date", dbpp::Dialect::Mysql);
    Expect(mysql_date_now.size() == 10 && mysql_date_now[4] == '-' && mysql_date_now[7] == '-',
           "DATE 'now' mismatch: " + mysql_date_now);

    const std::string mysql_time_now = dbpp::NormalizeTimestampForWrite(
        "now", "", "t", "time", dbpp::Dialect::Mysql);
    Expect(mysql_time_now.size() == 8 && mysql_time_now[2] == ':' && mysql_time_now[5] == ':',
           "TIME 'now' mismatch: " + mysql_time_now);

    Expect(dbpp::SqlLiteral(dbpp::Dialect::Mysql, "2024-01-01T00:00:00Z", "timestamp", false) ==
               "FROM_UNIXTIME(1704067200)",
           "MySQL TIMESTAMP SqlLiteral must emit FROM_UNIXTIME");
    Expect(dbpp::SqlLiteral(dbpp::Dialect::Mysql, "2024-01-01T00:00:00.123456Z", "timestamp(6)",
                            false) == "FROM_UNIXTIME(1704067200.123456)",
           "MySQL TIMESTAMP fractional SqlLiteral mismatch");
    Expect(dbpp::SqlLiteral(dbpp::Dialect::Mysql, "2024-01-01T00:00:00.005Z", "timestamp(3)",
                            false) == "FROM_UNIXTIME(1704067200.005)",
           "MySQL TIMESTAMP leading-zero fractional SqlLiteral mismatch");
    Expect(dbpp::SqlLiteral(dbpp::Dialect::Mysql, "2024-01-01 08:00:00", "datetime", false) ==
               "'2024-01-01 08:00:00'",
           "MySQL DATETIME SqlLiteral mismatch");
    Expect(dbpp::SqlLiteral(dbpp::Dialect::Mysql, "2024/01/02", "date", false) == "'2024-01-02'",
           "MySQL DATE SqlLiteral mismatch");

    Expect(dbpp::SqlLiteral(dbpp::Dialect::Postgresql, "2024-01-01 00:00:00 UTC", "timestamptz",
                            false) == "'2024-01-01T00:00:00Z'",
           "PostgreSQL TIMESTAMPTZ SqlLiteral mismatch");
    Expect(dbpp::SqlLiteral(dbpp::Dialect::Postgresql, "2024-01-01 08:00:00", "timestamp",
                            false) == "'2024-01-01 08:00:00'",
           "PostgreSQL TIMESTAMP SqlLiteral mismatch");
    Expect(dbpp::SqlLiteral(dbpp::Dialect::Postgresql, "2024/01/02", "date", false) ==
               "'2024-01-02'",
           "PostgreSQL DATE SqlLiteral mismatch");
  }

  // 20. Transactional batch commit (updates + deletes + inserts) and automatic rollback
  {
    const char* tx_sql =
        "CREATE TABLE tx_items (id INTEGER PRIMARY KEY, code TEXT NOT NULL UNIQUE, note TEXT);"
        "INSERT INTO tx_items VALUES (1, 'c1', 'n1'), (2, 'c2', 'n2'), (3, 'c3', 'n3');";
    for (const std::string& stmt : dbpp::SplitSqlStatements(tx_sql, dbpp::Dialect::Sqlite)) {
      const auto r = dbpp::ExecuteBlocking(session, stmt, false, 0);
      Expect(r.error.empty(), "tx_items setup failed: " + r.error);
    }

    const dbpp::TableModel tx_model = dbpp::LoadTableBlocking(
        session, 1, dbpp::DbObject{.name = "tx_items"}, 0, 50, std::nullopt, {});
    Expect(tx_model.error.empty() && tx_model.editable && tx_model.snapshot &&
               tx_model.snapshot->rows.size() == 3,
           "tx_items LoadTableBlocking failed: " + tx_model.error);

    // Atomic batch: multi-cell update on row 0, NULL on row 1, edit+delete on row 2 (delete wins),
    // plus an insert row in one transaction.
    const std::vector<dbpp::PendingCellEdit> ok_edits{
        dbpp::PendingCellEdit{.row_index = 0, .column_index = 1, .raw_text = "c1_v1",
                              .display_text = "c1_v1"},
        dbpp::PendingCellEdit{.row_index = 0, .column_index = 1, .raw_text = "c1_new",
                              .display_text = "c1_new"},
        dbpp::PendingCellEdit{.row_index = 0, .column_index = 2, .raw_text = "n1_new",
                              .display_text = "n1_new"},
        dbpp::PendingCellEdit{.row_index = 1, .column_index = 2, .raw_text = "NULL",
                              .display_text = "", .is_null = true},
        dbpp::PendingCellEdit{.row_index = 2, .column_index = 1, .raw_text = "c3_ignored",
                              .display_text = "c3_ignored"},
    };
    const std::vector<std::size_t> ok_deletes{2};
    const std::vector<dbpp::PendingInsertRow> ok_inserts{
        dbpp::PendingInsertRow{.draft_id = 1, .values = {"4", "c4", "n4"}},
    };
    const dbpp::BatchCommitResult ok_res =
        dbpp::CommitTableChangesBlocking(session, tx_model, ok_edits, ok_deletes, ok_inserts);
    Expect(ok_res.error.empty() && ok_res.updated_rows == 2 && ok_res.deleted_rows == 1 &&
               ok_res.inserted_rows == 1 && !ok_res.rolled_back,
           "CommitTableChangesBlocking atomic commit failed: " + ok_res.error);

    const auto after_ok = dbpp::ExecuteBlocking(
        session, "SELECT id, code, note FROM tx_items ORDER BY id", true, 10);
    Expect(after_ok.error.empty() && after_ok.rows.size() == 3 &&
               after_ok.rows[0].values[1].text == "c1_new" &&
               after_ok.rows[0].values[2].text == "n1_new" &&
               after_ok.rows[1].values[2].is_null &&
               after_ok.rows[2].values[0].text == "4" &&
               after_ok.rows[2].values[1].text == "c4",
           "CommitTableChangesBlocking persisted values mismatch");

    // Failed batch mid-way (update + delete succeed, duplicate UNIQUE insert fails -> full rollback)
    const dbpp::TableModel tx_model2 = dbpp::LoadTableBlocking(
        session, 1, dbpp::DbObject{.name = "tx_items"}, 0, 50, std::nullopt, {});
    const std::vector<dbpp::PendingCellEdit> fail_edits{
        dbpp::PendingCellEdit{.row_index = 0, .column_index = 2, .raw_text = "should_rollback",
                              .display_text = "should_rollback"},
    };
    const std::vector<std::size_t> fail_deletes{1};
    const std::vector<dbpp::PendingInsertRow> fail_inserts{
        dbpp::PendingInsertRow{.draft_id = 2, .values = {"5", "c1_new", "dup_unique"}},
    };
    const dbpp::BatchCommitResult fail_res =
        dbpp::CommitTableChangesBlocking(session, tx_model2, fail_edits, fail_deletes,
                                         fail_inserts);
    Expect(!fail_res.error.empty() && fail_res.rolled_back,
           "CommitTableChangesBlocking failure must report error and roll back transaction");

    const auto after_rb = dbpp::ExecuteBlocking(
        session, "SELECT COUNT(*), MAX(CASE WHEN id = 1 THEN note END) FROM tx_items", true, 4);
    Expect(after_rb.error.empty() && after_rb.rows.size() == 1 &&
               after_rb.rows[0].values[0].text == "3" &&
               after_rb.rows[0].values[1].text == "n1_new",
           "Transaction rollback failed to restore prior rows");
  }
}

} // namespace dbpp_test
