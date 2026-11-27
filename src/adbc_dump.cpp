module;

#include <arrow-adbc/adbc.h>
#include <nanoarrow/nanoarrow.h>

module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.redis;

namespace dbpp {

namespace {

bool HasExecutableSql(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (std::isspace(c) != 0) {
      ++i;
      continue;
    }
    if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
      i += 2;
      while (i < text.size() && text[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
        ++i;
      }
      i = (i + 1 < text.size()) ? i + 2 : text.size();
      continue;
    }
    return true;
  }
  return false;
}

std::string EnsureSemicolonTerminated(std::string sql) {
  const std::string_view trimmed = detail::AdbcTrimAscii(sql);
  std::string out(trimmed);
  if (!out.empty() && out.back() != ';') {
    out.push_back(';');
  }
  return out;
}

std::string DialectLabel(Dialect dialect) {
  switch (dialect) {
    case Dialect::Sqlite: return "SQLite";
    case Dialect::Postgresql: return "PostgreSQL";
    case Dialect::Mysql: return "MySQL";
    case Dialect::Duckdb: return "DuckDB";
    case Dialect::Redis: return "Redis";
    case Dialect::Generic: return "SQL";
  }
  return "SQL";
}

bool IsInternalOrNonRelationalObject(Dialect dialect, const DbObject& obj) {
  if (detail::AdbcContainsLower(obj.table_type, "index") ||
      detail::AdbcContainsLower(obj.table_type, "trigger") ||
      detail::AdbcContainsLower(obj.table_type, "sequence") ||
      detail::AdbcContainsLower(obj.table_type, "foreign")) {
    return true;
  }
  if (dialect == Dialect::Sqlite && obj.name.starts_with("sqlite_")) {
    return true;
  }
  return false;
}

bool MatchesTargetDatabase(Dialect dialect, const DbObject& obj, std::string_view target_db) {
  const std::string trimmed(detail::AdbcTrimAscii(target_db));
  if (trimmed.empty()) {
    const std::string db = ObjectDatabaseName(dialect, obj);
    return db.empty() || !IsSystemDatabaseOrSchema(db);
  }
  const std::string obj_db = ObjectDatabaseName(dialect, obj);
  if (dialect == Dialect::Sqlite && detail::AdbcEqualsIgnoreCase(trimmed, "main") &&
      obj_db.empty()) {
    return true;
  }
  return detail::AdbcEqualsIgnoreCase(obj_db, trimmed);
}

std::string QuerySqliteMasterSql(AdbcSession& session, std::string_view schema_name,
                                 std::string_view obj_type, std::string_view name_col,
                                 std::string_view name_val) {
  const std::string master =
      schema_name.empty()
          ? std::string("sqlite_master")
          : (QuoteIdentifier(Dialect::Sqlite, schema_name) + ".sqlite_master");
  const std::string sql =
      "SELECT sql FROM " + master + " WHERE type = " +
      SqlLiteral(Dialect::Sqlite, obj_type, "TEXT", false) + " AND " +
      std::string(name_col) + " = " + SqlLiteral(Dialect::Sqlite, name_val, "TEXT", false) +
      " AND sql IS NOT NULL";
  const AdbcQueryResult res = detail::AdbcExecuteBlockingRaw(session, sql, true, 16);
  if (!res.error.empty() || res.rows.empty() || res.rows[0].values.empty() ||
      res.rows[0].values[0].is_null) {
    return {};
  }
  return res.rows[0].values[0].text;
}

std::vector<std::string> QuerySqliteMasterMultiSql(AdbcSession& session,
                                                   std::string_view schema_name,
                                                   std::string_view obj_type,
                                                   std::string_view tbl_name) {
  std::vector<std::string> out;
  const std::string master =
      schema_name.empty()
          ? std::string("sqlite_master")
          : (QuoteIdentifier(Dialect::Sqlite, schema_name) + ".sqlite_master");
  const std::string sql =
      "SELECT sql FROM " + master + " WHERE type = " +
      SqlLiteral(Dialect::Sqlite, obj_type, "TEXT", false) + " AND tbl_name = " +
      SqlLiteral(Dialect::Sqlite, tbl_name, "TEXT", false) + " AND sql IS NOT NULL ORDER BY rowid";
  const AdbcQueryResult res = detail::AdbcExecuteBlockingRaw(session, sql, true, 512);
  if (!res.error.empty()) {
    return out;
  }
  for (const GridRow& row : res.rows) {
    if (!row.values.empty() && !row.values[0].is_null && !row.values[0].text.empty()) {
      out.push_back(row.values[0].text);
    }
  }
  return out;
}

std::string QueryMysqlCreateObject(AdbcSession& session, std::string_view kind,
                                   const std::string& qualified) {
  const std::string sql = "SHOW CREATE " + std::string(kind) + " " + qualified;
  const AdbcQueryResult res = detail::AdbcExecuteBlockingRaw(session, sql, true, 4);
  if (!res.error.empty() || res.rows.empty() || res.rows[0].values.size() < 2 ||
      res.rows[0].values[1].is_null) {
    return {};
  }
  return res.rows[0].values[1].text;
}

std::string SynthesizeCreateTableSql(Dialect dialect, const DbObject& obj,
                                     const std::vector<ColumnMeta>& cols) {
  if (cols.empty()) {
    return {};
  }
  std::string out = "CREATE TABLE " + QualifiedName(dialect, obj) + " (\n";
  std::vector<std::pair<int, std::string>> pk_cols;
  for (std::size_t i = 0; i < cols.size(); ++i) {
    const ColumnMeta& col = cols[i];
    if (i > 0) {
      out += ",\n";
    }
    out += "  " + QuoteIdentifier(dialect, col.name) + " " +
           (col.type.empty() ? std::string("TEXT") : col.type);
    if (!col.nullable) {
      out += " NOT NULL";
    }
    if (!col.default_expr.empty()) {
      out += " DEFAULT " + col.default_expr;
    }
    if (col.primary_key) {
      pk_cols.emplace_back(col.primary_key_order, col.name);
    }
  }
  if (!pk_cols.empty()) {
    std::ranges::sort(pk_cols, [](const auto& a, const auto& b) {
      return a.first < b.first;
    });
    out += ",\n  PRIMARY KEY (";
    for (std::size_t i = 0; i < pk_cols.size(); ++i) {
      if (i > 0) {
        out += ", ";
      }
      out += QuoteIdentifier(dialect, pk_cols[i].second);
    }
    out += ")";
  }
  for (const ForeignKeyMeta& fk : obj.foreign_keys) {
    if (fk.columns.empty() || fk.fk_table.empty()) {
      continue;
    }
    out += ",\n  FOREIGN KEY (";
    for (std::size_t i = 0; i < fk.columns.size(); ++i) {
      if (i > 0) out += ", ";
      out += QuoteIdentifier(dialect, fk.columns[i]);
    }
    out += ") REFERENCES " + QuoteIdentifier(dialect, fk.fk_table);
    if (!fk.fk_columns.empty()) {
      out += " (";
      for (std::size_t i = 0; i < fk.fk_columns.size(); ++i) {
        if (i > 0) out += ", ";
        out += QuoteIdentifier(dialect, fk.fk_columns[i]);
      }
      out += ")";
    }
  }
  out += "\n);";
  return out;
}

std::string BuildSwitchDatabaseSqlForAdbc(Dialect dialect, std::string_view target_database) {
  const std::string trimmed(detail::AdbcTrimAscii(target_database));
  if (trimmed.empty() || dialect == Dialect::Sqlite || dialect == Dialect::Redis) {
    return {};
  }
  if (dialect == Dialect::Mysql) {
    return "USE " + QuoteIdentifier(Dialect::Mysql, trimmed);
  }
  if (dialect == Dialect::Postgresql) {
    const std::size_t dot = trimmed.rfind('.');
    const std::string_view schema =
        (dot != std::string_view::npos && dot + 1 < trimmed.size())
            ? std::string_view(trimmed).substr(dot + 1)
            : std::string_view(trimmed);
    return "SET search_path TO " + QuoteIdentifier(Dialect::Postgresql, schema);
  }
  return "USE " + QuoteIdentifier(dialect, trimmed);
}

bool DumpTableDataSql(AdbcSession& session, const DbObject& obj,
                      const std::vector<ColumnMeta>& meta_cols, std::ostream& out,
                      SqlDumpResult& result) {
  const std::string qualified = QualifiedName(session.dialect, obj);
  const std::string select_sql = "SELECT * FROM " + qualified;

  session.cancel_requested.store(false, std::memory_order_release);
  detail::AdbcErrorGuard error;
  detail::AdbcStatementGuard statement_guard;
  AdbcStatusCode status =
      AdbcStatementNew(&session.connection, &statement_guard.statement, error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return false;
  }
  statement_guard.created = true;

  status = AdbcStatementSetSqlQuery(&statement_guard.statement, select_sql.c_str(), error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return false;
  }

  detail::AdbcStreamHolder stream;
  detail::AdbcRegisterActiveStatement(session, &statement_guard.statement);
  status = AdbcStatementExecuteQuery(&statement_guard.statement, &stream.stream, nullptr,
                                     error.Ptr());
  detail::AdbcClearActiveStatement(session, &statement_guard.statement);
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return false;
  }

  detail::AdbcSchemaHolder schema;
  if (stream.stream.get_schema(&stream.stream, &schema.schema) != 0) {
    AdbcStatusCode stream_status = ADBC_STATUS_OK;
    const auto cap = detail::AdbcCaptureStreamError(stream.stream, &stream_status);
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(stream_status, cap.message, cap.sqlstate);
    return false;
  }

  const std::int64_t field_count = schema.schema.n_children;
  if (field_count <= 0) {
    return true;
  }

  std::vector<std::string> col_types(static_cast<std::size_t>(field_count));
  std::string insert_prefix = "INSERT INTO " + qualified + " (";
  for (std::int64_t c = 0; c < field_count; ++c) {
    if (c > 0) {
      insert_prefix += ", ";
    }
    const ArrowSchema* field = schema.schema.children[c];
    const std::string col_name = (field != nullptr && field->name != nullptr)
                                     ? std::string(field->name)
                                     : ("column_" + std::to_string(c + 1));
    insert_prefix += QuoteIdentifier(session.dialect, col_name);
    for (const ColumnMeta& meta : meta_cols) {
      if (detail::AdbcEqualsIgnoreCase(meta.name, col_name)) {
        col_types[static_cast<std::size_t>(c)] = meta.type;
        break;
      }
    }
  }
  insert_prefix += ") VALUES (";

  while (true) {
    detail::AdbcArrayHolder array;
    if (stream.stream.get_next(&stream.stream, &array.array) != 0) {
      AdbcStatusCode stream_status = ADBC_STATUS_OK;
      const auto cap = detail::AdbcCaptureStreamError(stream.stream, &stream_status);
      result.error = cap.message;
      result.connection_lost = IsConnectionLostError(stream_status, cap.message, cap.sqlstate);
      return false;
    }
    if (array.array.release == nullptr) {
      break;
    }

    ArrowArrayView root{};
    ArrowError arrow_err;
    ArrowErrorInit(&arrow_err);
    if (ArrowArrayViewInitFromSchema(&root, &schema.schema, &arrow_err) != NANOARROW_OK) {
      result.error = arrow_err.message;
      return false;
    }
    if (ArrowArrayViewSetArray(&root, &array.array, &arrow_err) != NANOARROW_OK) {
      ArrowArrayViewReset(&root);
      result.error = arrow_err.message;
      return false;
    }

    for (std::int64_t row = 0; row < array.array.length; ++row) {
      if (session.cancel_requested.load(std::memory_order_acquire)) {
        ArrowArrayViewReset(&root);
        result.error = Tr("Export cancelled", "导出已取消");
        return false;
      }
      out << insert_prefix;
      for (std::int64_t c = 0; c < field_count; ++c) {
        if (c > 0) {
          out << ", ";
        }
        out << detail::AdbcFormatArrowSqlLiteral(
            *root.children[c], row, schema.schema.children[c], session.dialect,
            col_types[static_cast<std::size_t>(c)]);
      }
      out << ");\n";
      if (!out.good()) {
        ArrowArrayViewReset(&root);
        result.error = Tr("Failed while writing SQL dump file", "写入 SQL 导出文件失败");
        return false;
      }
      ++result.rows_written;
    }
    ArrowArrayViewReset(&root);
  }
  return true;
}

} // namespace

std::vector<std::string> SplitSqlStatements(std::string_view script, Dialect dialect) {
  if (script.size() >= 3 && static_cast<unsigned char>(script[0]) == 0xEF &&
      static_cast<unsigned char>(script[1]) == 0xBB &&
      static_cast<unsigned char>(script[2]) == 0xBF) {
    script.remove_prefix(3);
  }

  std::vector<std::string> statements;
  std::size_t stmt_start = 0;
  std::size_t i = 0;
  int head_words = 0;
  bool seen_create = false;
  bool in_trigger = false;
  int begin_depth = 0;
  int case_depth = 0;

  const auto reset_stmt_state = [&] {
    head_words = 0;
    seen_create = false;
    in_trigger = false;
    begin_depth = 0;
    case_depth = 0;
  };

  while (i < script.size()) {
    const char c = script[i];
    if (c == '-' && i + 1 < script.size() && script[i + 1] == '-') {
      i += 2;
      while (i < script.size() && script[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < script.size() && script[i + 1] == '*') {
      i += 2;
      while (i + 1 < script.size() && !(script[i] == '*' && script[i + 1] == '/')) {
        ++i;
      }
      i = (i + 1 < script.size()) ? i + 2 : script.size();
      continue;
    }
    if (c == '\'') {
      ++i;
      while (i < script.size()) {
        if (dialect == Dialect::Mysql && script[i] == '\\' && i + 1 < script.size()) {
          i += 2;
          continue;
        }
        if (script[i] == '\'') {
          if (i + 1 < script.size() && script[i + 1] == '\'') {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (c == '"' || c == '`') {
      const char quote = c;
      ++i;
      while (i < script.size()) {
        if (script[i] == quote) {
          if (i + 1 < script.size() && script[i + 1] == quote) {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (c == '$') {
      std::size_t j = i + 1;
      while (j < script.size() &&
             (std::isalnum(static_cast<unsigned char>(script[j])) || script[j] == '_')) {
        ++j;
      }
      if (j < script.size() && script[j] == '$') {
        const std::string_view tag = script.substr(i, j - i + 1);
        const std::size_t close = script.find(tag, j + 1);
        i = close == std::string_view::npos ? script.size() : close + tag.size();
        continue;
      }
    }
    if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
      std::size_t j = i + 1;
      while (j < script.size() &&
             (std::isalnum(static_cast<unsigned char>(script[j])) || script[j] == '_')) {
        ++j;
      }
      const std::string_view word = script.substr(i, j - i);
      if (head_words == 0) {
        seen_create = detail::AdbcEqualsIgnoreCase(word, "CREATE");
        head_words = 1;
      } else if (head_words == 1) {
        if (seen_create &&
            !detail::AdbcEqualsIgnoreCase(word, "TEMP") &&
            !detail::AdbcEqualsIgnoreCase(word, "TEMPORARY") &&
            !detail::AdbcEqualsIgnoreCase(word, "OR") &&
            !detail::AdbcEqualsIgnoreCase(word, "REPLACE")) {
          if (detail::AdbcEqualsIgnoreCase(word, "TRIGGER")) {
            in_trigger = true;
          }
          head_words = 2;
        }
      }
      if (in_trigger) {
        if (detail::AdbcEqualsIgnoreCase(word, "BEGIN")) {
          ++begin_depth;
        } else if (detail::AdbcEqualsIgnoreCase(word, "CASE")) {
          ++case_depth;
        } else if (detail::AdbcEqualsIgnoreCase(word, "END")) {
          if (case_depth > 0) {
            --case_depth;
          } else if (begin_depth > 0) {
            --begin_depth;
          }
        }
      }
      i = j;
      continue;
    }
    if (c == ';' && begin_depth == 0) {
      const std::string_view piece = detail::AdbcTrimAscii(script.substr(stmt_start, i - stmt_start));
      if (HasExecutableSql(piece)) {
        statements.emplace_back(piece);
      }
      stmt_start = i + 1;
      reset_stmt_state();
    }
    ++i;
  }

  const std::string_view tail = detail::AdbcTrimAscii(script.substr(stmt_start));
  if (HasExecutableSql(tail)) {
    statements.emplace_back(tail);
  }
  return statements;
}

std::string MakeTempSqlExportPath() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return (std::filesystem::temp_directory_path() /
          ("dbpp_dump_" + std::to_string(stamp) + ".sql"))
      .string();
}

SqlDumpResult ExportDatabaseSqlToFileBlocking(AdbcSession& session, const SqlDumpOptions& options,
                                              const std::filesystem::path& path) {
  SqlDumpResult result;
  if (session.IsClosed() || !session.connection_created) {
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (session.dialect == Dialect::Redis) {
    result.error = Tr("SQL export is not supported for Redis", "Redis 不支持导出 SQL");
    return result;
  }

  std::vector<DbObject> tables;
  std::vector<DbObject> views;
  std::vector<TriggerMeta> triggers;
  if (options.single_object.has_value()) {
    if (options.single_object->is_view) {
      views.push_back(*options.single_object);
    } else {
      tables.push_back(*options.single_object);
    }
    const AdbcObjectsResult objs = LoadObjectsBlocking(session);
    if (objs.error.empty()) {
      triggers = objs.triggers;
    }
  } else {
    const AdbcObjectsResult objs = LoadObjectsBlocking(session);
    if (objs.connection_lost) {
      result.connection_lost = true;
      result.error = objs.error;
      return result;
    }
    if (!objs.error.empty()) {
      result.error = objs.error;
      return result;
    }
    for (const DbObject& obj : objs.objects) {
      if (!MatchesTargetDatabase(session.dialect, obj, options.target_database) ||
          IsInternalOrNonRelationalObject(session.dialect, obj)) {
        continue;
      }
      if (obj.is_view) {
        views.push_back(obj);
      } else {
        tables.push_back(obj);
      }
    }
    triggers = objs.triggers;
  }

  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    result.error = TrFmt("Failed to open output file: {0}", "无法打开导出文件: {0}", {path.string()});
    return result;
  }

  out << "-- db++ SQL Export\n";
  out << "-- Dialect: " << DialectLabel(session.dialect) << "\n";
  if (!options.target_database.empty()) {
    out << "-- Database: " << options.target_database << "\n";
  }
  out << "\n";

  std::vector<std::string> deferred_indexes;
  std::vector<std::string> deferred_triggers;

  for (const DbObject& table : tables) {
    const AdbcSchemaResult schema_res = LoadSchemaBlocking(session, table);
    const std::vector<ColumnMeta>& cols =
        !schema_res.columns.empty() ? schema_res.columns : table.columns;

    if (options.include_schema) {
      std::string ddl;
      if (session.dialect == Dialect::Sqlite) {
        ddl = QuerySqliteMasterSql(session, table.db_schema, "table", "name", table.name);
        for (const std::string& ix_sql :
             QuerySqliteMasterMultiSql(session, table.db_schema, "index", table.name)) {
          deferred_indexes.push_back(
              EnsureSemicolonTerminated(FormatSqlStatement(ix_sql, session.dialect)));
        }
        for (const std::string& trg_sql :
             QuerySqliteMasterMultiSql(session, table.db_schema, "trigger", table.name)) {
          deferred_triggers.push_back(
              EnsureSemicolonTerminated(FormatSqlStatement(trg_sql, session.dialect)));
        }
      } else if (session.dialect == Dialect::Mysql) {
        ddl = QueryMysqlCreateObject(session, "TABLE", QualifiedName(session.dialect, table));
        for (const TriggerMeta& trg : triggers) {
          if (detail::AdbcEqualsIgnoreCase(trg.table_name, table.name) && !trg.definition.empty()) {
            deferred_triggers.push_back(EnsureSemicolonTerminated(
                FormatSqlStatement(trg.definition, session.dialect)));
          }
        }
      } else {
        for (const IndexMeta& ix : table.indexes) {
          if (!ix.primary && !ix.definition.empty()) {
            deferred_indexes.push_back(
                EnsureSemicolonTerminated(FormatSqlStatement(ix.definition, session.dialect)));
          }
        }
        for (const TriggerMeta& trg : triggers) {
          if (detail::AdbcEqualsIgnoreCase(trg.table_name, table.name) && !trg.definition.empty()) {
            deferred_triggers.push_back(EnsureSemicolonTerminated(
                FormatSqlStatement(trg.definition, session.dialect)));
          }
        }
      }
      if (ddl.empty()) {
        ddl = SynthesizeCreateTableSql(session.dialect, table, cols);
      } else {
        ddl = FormatSqlStatement(ddl, session.dialect);
      }
      if (!ddl.empty()) {
        out << EnsureSemicolonTerminated(ddl) << "\n\n";
      }
    }

    if (options.include_data) {
      if (!DumpTableDataSql(session, table, cols, out, result)) {
        return result;
      }
      out << "\n";
    }
    ++result.tables_exported;
  }

  if (options.include_schema) {
    for (const std::string& ix_sql : deferred_indexes) {
      out << ix_sql << "\n\n";
    }
    for (const DbObject& view : views) {
      std::string view_ddl;
      if (session.dialect == Dialect::Sqlite) {
        view_ddl = QuerySqliteMasterSql(session, view.db_schema, "view", "name", view.name);
      } else if (session.dialect == Dialect::Mysql) {
        view_ddl = QueryMysqlCreateObject(session, "VIEW", QualifiedName(session.dialect, view));
      }
      if (!view_ddl.empty()) {
        out << EnsureSemicolonTerminated(FormatSqlStatement(view_ddl, session.dialect)) << "\n\n";
      }
    }
    for (const std::string& trg_sql : deferred_triggers) {
      out << trg_sql << "\n\n";
    }
  }

  out.flush();
  if (!out.good()) {
    result.error = Tr("Failed while writing SQL dump file", "写入 SQL 导出文件失败");
  }
  return result;
}

SqlDumpResult ExportDatabaseSqlToFilePathBlocking(AdbcSession& session,
                                                  const SqlDumpOptions& options,
                                                  std::string_view path) {
  return ExportDatabaseSqlToFileBlocking(session, options, std::filesystem::path(path));
}

SqlScriptImportResult ImportSqlScriptFileBlocking(AdbcSession& session,
                                                  const std::filesystem::path& path,
                                                  std::string_view target_database) {
  SqlScriptImportResult result;
  if (session.IsClosed() || !session.connection_created) {
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (session.read_only) {
    result.error = ReadOnlyWriteBlockedMessage();
    return result;
  }
  if (session.dialect == Dialect::Redis) {
    result.error = Tr("SQL import is not supported for Redis", "Redis 不支持导入 SQL 脚本");
    return result;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    result.error = TrFmt("Failed to open SQL file: {0}", "无法打开 SQL 文件: {0}", {path.string()});
    return result;
  }
  const std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  const std::vector<std::string> statements = SplitSqlStatements(content, session.dialect);
  if (statements.empty()) {
    result.error = Tr("SQL script contains no executable statements", "SQL 脚本中不包含可执行语句");
    return result;
  }

  const std::string trimmed_db(detail::AdbcTrimAscii(target_database));
  if (!trimmed_db.empty() &&
      !(session.dialect == Dialect::Sqlite && detail::AdbcEqualsIgnoreCase(trimmed_db, "main"))) {
    std::string ns_err;
    if (!SwitchSessionNamespaceBlocking(session, {}, trimmed_db, ns_err)) {
      const std::string switch_sql = BuildSwitchDatabaseSqlForAdbc(session.dialect, trimmed_db);
      if (!switch_sql.empty()) {
        const AdbcQueryResult sw = ExecuteSwitchDatabaseSqlBlocking(session, switch_sql);
        if (sw.connection_lost) {
          result.connection_lost = true;
          result.error = sw.error;
          return result;
        }
        if (!sw.error.empty()) {
          result.error = sw.error;
          return result;
        }
      }
    }
  }

  std::int64_t total_affected = 0;
  bool any_affected = false;
  for (const std::string& stmt : statements) {
    const AdbcQueryResult res = ExecuteBlocking(session, stmt, false, 0);
    if (!res.error.empty()) {
      result.connection_lost = res.connection_lost;
      result.error = TrFmt("Statement {0} failed: {1}", "第 {0} 条语句执行失败: {1}",
                           {std::to_string(result.statements_executed + 1), res.error});
      return result;
    }
    ++result.statements_executed;
    if (res.affected >= 0) {
      total_affected += res.affected;
      any_affected = true;
    }
  }
  result.rows_affected = any_affected ? total_affected : -1;
  return result;
}

SqlScriptImportResult ImportSqlScriptPathBlocking(AdbcSession& session, std::string_view path,
                                                  std::string_view target_database) {
  return ImportSqlScriptFileBlocking(session, std::filesystem::path(path), target_database);
}

TableDesignSnapshot LoadTableDesignBlocking(AdbcSession& session, const DbObject& object) {
  TableDesignSnapshot result;
  result.object = object;
  if (session.IsClosed() || !session.connection_created) {
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (session.dialect == Dialect::Redis) {
    return result;
  }

  const AdbcSchemaResult schema_res = LoadSchemaBlocking(session, object);
  if (schema_res.connection_lost) {
    result.connection_lost = true;
    result.error = schema_res.error;
    return result;
  }
  if (!schema_res.error.empty() && schema_res.columns.empty()) {
    result.error = schema_res.error;
  }
  if (!schema_res.columns.empty()) {
    result.object.columns = schema_res.columns;
  }

  const AdbcObjectsResult objs = LoadObjectsBlocking(session);
  if (objs.connection_lost) {
    result.connection_lost = true;
    result.error = objs.error;
    return result;
  }
  if (objs.error.empty()) {
    const DbObject* matched = nullptr;
    for (const DbObject& candidate : objs.objects) {
      if (!detail::AdbcEqualsIgnoreCase(candidate.name, object.name)) {
        continue;
      }
      const bool schema_match =
          object.db_schema.empty() || candidate.db_schema.empty() ||
          detail::AdbcEqualsIgnoreCase(candidate.db_schema, object.db_schema);
      const bool catalog_match =
          object.catalog.empty() || candidate.catalog.empty() ||
          detail::AdbcEqualsIgnoreCase(candidate.catalog, object.catalog);
      if (schema_match && catalog_match) {
        matched = &candidate;
        break;
      }
    }
    if (matched != nullptr) {
      if (result.object.columns.empty()) {
        result.object.columns = matched->columns;
      }
      result.object.foreign_keys = matched->foreign_keys;
      result.object.constraints = matched->constraints;
      result.object.indexes = matched->indexes;
      result.object.primary_key = matched->primary_key;
      result.object.table_type = matched->table_type;
      result.object.is_view = matched->is_view;
      if (result.object.db_schema.empty()) {
        result.object.db_schema = matched->db_schema;
      }
      if (result.object.catalog.empty()) {
        result.object.catalog = matched->catalog;
      }
    }

    for (const TriggerMeta& trg : objs.triggers) {
      if (!detail::AdbcEqualsIgnoreCase(trg.table_name, result.object.name)) {
        continue;
      }
      if (!result.object.db_schema.empty() && !trg.db_schema.empty() &&
          !detail::AdbcEqualsIgnoreCase(trg.db_schema, result.object.db_schema)) {
        continue;
      }
      result.triggers.push_back(trg);
    }
  }

  if (session.dialect == Dialect::Sqlite) {
    for (IndexMeta& ix : result.object.indexes) {
      if (ix.definition.empty() && !ix.name.empty()) {
        ix.definition = QuerySqliteMasterSql(session, result.object.db_schema, "index", "name",
                                             ix.name);
      }
      if (!ix.definition.empty()) {
        ix.definition = FormatSqlStatement(ix.definition, session.dialect);
      }
    }
  } else {
    for (IndexMeta& ix : result.object.indexes) {
      if (!ix.definition.empty()) {
        ix.definition = FormatSqlStatement(ix.definition, session.dialect);
      }
    }
  }
  for (TriggerMeta& trg : result.triggers) {
    if (!trg.definition.empty()) {
      trg.definition = FormatSqlStatement(trg.definition, session.dialect);
    }
  }

  std::vector<std::string> ddl_parts;
  auto append_ddl = [&ddl_parts, &session](std::string s) {
    const std::string formatted = FormatSqlStatement(s, session.dialect);
    const std::string trimmed(detail::AdbcTrimAscii(formatted));
    if (!trimmed.empty()) {
      ddl_parts.push_back(EnsureSemicolonTerminated(trimmed));
    }
  };

  if (session.dialect == Dialect::Sqlite) {
    const std::string kind = result.object.is_view ? "view" : "table";
    std::string main_ddl =
        QuerySqliteMasterSql(session, result.object.db_schema, kind, "name", result.object.name);
    if (main_ddl.empty() && !result.object.is_view) {
      main_ddl = SynthesizeCreateTableSql(session.dialect, result.object, result.object.columns);
    }
    append_ddl(std::move(main_ddl));
    if (!result.object.is_view) {
      for (const std::string& ix_sql : QuerySqliteMasterMultiSql(
               session, result.object.db_schema, "index", result.object.name)) {
        append_ddl(ix_sql);
      }
      for (const std::string& trg_sql : QuerySqliteMasterMultiSql(
               session, result.object.db_schema, "trigger", result.object.name)) {
        append_ddl(trg_sql);
      }
    }
  } else if (session.dialect == Dialect::Mysql) {
    const std::string kind = result.object.is_view ? "VIEW" : "TABLE";
    std::string main_ddl =
        QueryMysqlCreateObject(session, kind, QualifiedName(session.dialect, result.object));
    if (main_ddl.empty() && !result.object.is_view) {
      main_ddl = SynthesizeCreateTableSql(session.dialect, result.object, result.object.columns);
    }
    append_ddl(std::move(main_ddl));
    for (const TriggerMeta& trg : result.triggers) {
      append_ddl(trg.definition);
    }
  } else {
    if (!result.object.is_view) {
      append_ddl(SynthesizeCreateTableSql(session.dialect, result.object, result.object.columns));
    }
    for (const IndexMeta& ix : result.object.indexes) {
      if (!ix.primary && !ix.definition.empty()) {
        append_ddl(ix.definition);
      }
    }
    for (const TriggerMeta& trg : result.triggers) {
      append_ddl(trg.definition);
    }
  }

  for (std::size_t i = 0; i < ddl_parts.size(); ++i) {
    if (i > 0) {
      result.ddl += "\n\n";
    }
    result.ddl += ddl_parts[i];
  }
  return result;
}

} // namespace dbpp
