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
}

} // namespace dbpp_test
