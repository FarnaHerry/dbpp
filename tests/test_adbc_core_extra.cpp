#include "test_adbc_core_helpers.h"

import std;
import dbpp.settings;
import dbpp.i18n;
import dbpp.types;
import dbpp.adbc;
import dbpp.db;

namespace fs = std::filesystem;

namespace {

using dbpp_test::Expect;

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

  Expect(dbpp::ConnectionFailureKey("/tmp/libpg.so", "postgresql://user:secret@127.0.0.1:5432/app") ==
             dbpp::ConnectionFailureKey("/tmp/libpg.so", "postgresql://127.0.0.1:5432/app"),
         "ConnectionFailureKey must ignore embedded credentials in URI");
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
  const dbpp::AdbcObjectsResult listed = dbpp::LoadObjectsBlocking(*opened.session);
  Expect(listed.error.empty(), "LoadObjectsBlocking failed: " + listed.error);
  for (const dbpp::DbObject& candidate : listed.objects) {
    if (candidate.name == "products") {
      obj = candidate;
      break;
    }
  }

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
             page0.executed_sql.starts_with("SELECT * FROM ") &&
             page0.executed_sql.find("LIMIT 2 OFFSET 0") != std::string::npos,
         "Page 0 with page_size=2 should return 2 rows using SELECT * and populate audit SQL/duration");
  const dbpp::TableModel page1 =
      dbpp::LoadTableBlocking(*opened.session, 1, obj, 1, 2, std::nullopt, {});
  Expect(page1.error.empty() && page1.total == 3 && page1.page == 1 && page1.page_size == 2 &&
             page1.snapshot && page1.snapshot->rows.size() == 1 &&
             page1.elapsed_us >= 0 &&
             page1.executed_sql.find("LIMIT 2 OFFSET 2") != std::string::npos,
         "Page 1 with page_size=2 should return 1 row and populate audit SQL/duration");

  Expect(dbpp::ExecuteBlocking(
             *opened.session, "CREATE TABLE logs_no_pk (msg TEXT NOT NULL);", false, 0)
             .error.empty(),
         "Create PK-less SQLite table failed");
  Expect(dbpp::ExecuteBlocking(
             *opened.session, "INSERT INTO logs_no_pk VALUES ('boot'), ('ready');", false, 0)
             .error.empty(),
         "Seed PK-less SQLite table failed");
  dbpp::DbObject logs_obj;
  logs_obj.name = "logs_no_pk";
  const dbpp::TableModel logs_page =
      dbpp::LoadTableBlocking(*opened.session, 1, logs_obj, 0, 50, std::nullopt, {});
  Expect(logs_page.error.empty() && logs_page.editable && logs_page.uses_rowid &&
             logs_page.executed_sql.starts_with("SELECT rowid AS \"__dbpp_rowid\", * FROM ") &&
             logs_page.snapshot && logs_page.snapshot->rows.size() == 2 &&
             logs_page.snapshot->rows[0].rowid.has_value(),
         "PK-less SQLite table should query rowid, * and stay editable: " +
             logs_page.executed_sql);
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

namespace dbpp_test {

void RunAdbcCoreExtraTests(const fs::path& dir, const std::string& driver_path) {
  TestConnectionLostAndSessionLifecycle(dir, driver_path);
  TestSqlFilterNormalizationAndFiltering(dir, driver_path);
  TestEditTableDdlGenerationAndExecution(dir, driver_path);
  TestMcpSettingsPersistence(dir);
  TestSqlDatabaseContextHelpers();
  TestImplicitSqlLimitAndExecution(dir, driver_path);
  RunAdbcCapabilitiesTests(dir, driver_path);
}

} // namespace dbpp_test
