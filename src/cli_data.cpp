module;

#include <huxerui/datatable.h>

#ifndef DBPP_VERSION
#define DBPP_VERSION "0.4.7"
#endif

module dbpp.cli;

import std;
import dbpp.types;
import dbpp.adbc;
import dbpp.redis;
import dbpp.driver_installer;
import dbpp.db;

namespace dbpp {

namespace detail {

std::vector<ColumnMeta> ResolveColumnsForObject(AdbcSession& session, const DbObject& obj,
                                                std::string& error_out) {
  AdbcSchemaResult schema = LoadSchemaBlocking(session, obj);
  std::vector<ColumnMeta> cols = std::move(schema.columns);
  if (cols.empty()) {
    const std::string qname = QualifiedName(session.dialect, obj);
    const AdbcQueryResult header =
        ExecuteBlocking(session, "SELECT * FROM " + qname + " LIMIT 0", true, 0);
    if (!header.error.empty()) {
      error_out = !schema.error.empty() ? schema.error : header.error;
      return {};
    }
    for (const std::string& col_name : header.columns) {
      cols.push_back(ColumnMeta{.name = col_name, .type = "ANY"});
    }
  }
  return cols;
}

void AppendJsonTableSchemaObject(CliJsonWriter& w, Dialect dialect, const DbObject& obj,
                                 const std::vector<ColumnMeta>& cols) {
  w.BeginObject();
  bool fo = true;
  w.KeyString("table", obj.name, fo);
  w.KeyString("qualified_name", QualifiedName(dialect, obj), fo);
  w.KeyString("schema", obj.db_schema, fo);
  w.KeyBool("is_view", obj.is_view, fo);
  w.Key("primary_key", fo);
  w.BeginArray();
  bool first_pk = true;
  for (const std::string& pk : obj.primary_key) {
    w.Comma(first_pk);
    w.String(pk);
  }
  w.EndArray();
  w.Key("columns", fo);
  w.BeginArray();
  bool first_col = true;
  for (const ColumnMeta& col : cols) {
    w.Comma(first_col);
    w.BeginObject();
    bool fc = true;
    w.KeyString("name", col.name, fc);
    w.KeyString("type", col.type, fc);
    w.KeyBool("nullable", col.nullable, fc);
    w.KeyBool("primary_key", col.primary_key, fc);
    w.KeyInt64("primary_key_order", col.primary_key_order, fc);
    if (!col.default_expr.empty()) {
      w.KeyString("default", col.default_expr, fc);
    }
    if (!col.remarks.empty()) {
      w.KeyString("remarks", col.remarks, fc);
    }
    if (col.unique) {
      w.KeyBool("unique", true, fc);
    }
    if (col.autoincrement) {
      w.KeyBool("autoincrement", true, fc);
    }
    if (col.generated) {
      w.KeyBool("generated", true, fc);
    }
    w.EndObject();
  }
  w.EndArray();
  if (!obj.foreign_keys.empty()) {
    w.Key("foreign_keys", fo);
    w.BeginArray();
    bool first_fk = true;
    for (const ForeignKeyMeta& fk : obj.foreign_keys) {
      w.Comma(first_fk);
      w.BeginObject();
      bool ffk = true;
      w.KeyString("name", fk.constraint_name, ffk);
      w.KeyString("ref_table", fk.fk_table, ffk);
      if (!fk.fk_db_schema.empty()) {
        w.KeyString("ref_schema", fk.fk_db_schema, ffk);
      }
      w.Key("columns", ffk);
      w.BeginArray();
      bool fc1 = true;
      for (const std::string& c : fk.columns) {
        w.Comma(fc1);
        w.String(c);
      }
      w.EndArray();
      w.Key("ref_columns", ffk);
      w.BeginArray();
      bool fc2 = true;
      for (const std::string& c : fk.fk_columns) {
        w.Comma(fc2);
        w.String(c);
      }
      w.EndArray();
      w.EndObject();
    }
    w.EndArray();
  }
  w.EndObject();
}

int CliHandleSchema(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                 bool early_json) {
  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  CliConnOptions conn;
  std::string schema_filter;
  std::optional<std::string> table_arg;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    if (!CliTryConsumeConnFlag(args, i, conn, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    CliSplitFlag(args[i], flag, inline_val);

    if (flag == "--schema") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, schema_filter, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (!args[i].empty() && args[i].front() == '-') {
      return CliEmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
    }
    if (!table_arg.has_value()) {
      table_arg = args[i];
      continue;
    }
    return CliEmitError(err, early_json, "多余的位置参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    CliPrintCommandHelp("schema", out);
    return kCliExitOk;
  }

  const bool json_mode = (format == CliOutputFormat::Json);
  ConnectionRequest req;
  if (const int code = CliResolveConnectionRequest(conn, req, error); code != kCliExitOk) {
    return CliEmitError(err, json_mode, error, code);
  }

  const AdbcOpenResult opened = OpenAdbcBlocking(req);
  if (!opened.error.empty() || !opened.session) {
    return CliEmitError(err, json_mode, opened.error.empty() ? "连接失败" : opened.error,
                     kCliExitError);
  }

  const AdbcObjectsResult loaded = LoadObjectsBlocking(*opened.session);
  if (!loaded.error.empty()) {
    return CliEmitError(err, json_mode, loaded.error, kCliExitError);
  }

  if (table_arg.has_value()) {
    const auto matched = CliFindObjectByName(loaded.objects, *table_arg, schema_filter, error);
    if (!matched) {
      return CliEmitError(err, json_mode, error, kCliExitError);
    }
    const std::vector<ColumnMeta> cols =
        ResolveColumnsForObject(*opened.session, *matched, error);
    if (!error.empty() && cols.empty()) {
      return CliEmitError(err, json_mode, error, kCliExitError);
    }

    if (json_mode) {
      CliJsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeyString("dialect", CliDialectName(opened.session->dialect), first);
      w.KeyString("table", matched->name, first);
      w.KeyString("qualified_name", QualifiedName(opened.session->dialect, *matched), first);
      w.KeyString("schema", matched->db_schema, first);
      w.KeyBool("is_view", matched->is_view, first);
      w.Key("primary_key", first);
      w.BeginArray();
      bool first_pk = true;
      for (const std::string& pk : matched->primary_key) {
        w.Comma(first_pk);
        w.String(pk);
      }
      w.EndArray();
      w.Key("columns", first);
      w.BeginArray();
      bool first_col = true;
      for (const ColumnMeta& col : cols) {
        w.Comma(first_col);
        w.BeginObject();
        bool fc = true;
        w.KeyString("name", col.name, fc);
        w.KeyString("type", col.type, fc);
        w.KeyBool("nullable", col.nullable, fc);
        w.KeyBool("primary_key", col.primary_key, fc);
        w.KeyInt64("primary_key_order", col.primary_key_order, fc);
        w.EndObject();
      }
      w.EndArray();
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }

    std::vector<GridRow> rows;
    rows.reserve(cols.size());
    for (const ColumnMeta& col : cols) {
      rows.push_back(CliMakeTextRow({
          col.name,
          col.type,
          col.nullable ? "true" : "false",
          col.primary_key ? "true" : "false",
          std::to_string(col.primary_key_order),
      }));
    }
    CliWriteTabularText(out, format, {"name", "type", "nullable", "primary_key", "pk_order"}, rows);
    return kCliExitOk;
  }

  std::vector<std::pair<DbObject, std::vector<ColumnMeta>>> all_schemas;
  for (const DbObject& obj : loaded.objects) {
    if (!schema_filter.empty() && !CliEqualsIgnoreCase(obj.db_schema, schema_filter)) {
      continue;
    }
    std::string col_error;
    std::vector<ColumnMeta> cols = ResolveColumnsForObject(*opened.session, obj, col_error);
    if (!col_error.empty() && cols.empty()) {
      return CliEmitError(err, json_mode, col_error, kCliExitError);
    }
    all_schemas.emplace_back(obj, std::move(cols));
  }

  if (json_mode) {
    CliJsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    w.KeyString("dialect", CliDialectName(opened.session->dialect), first);
    w.KeySize("count", all_schemas.size(), first);
    w.Key("tables", first);
    w.BeginArray();
    bool first_tbl = true;
    for (const auto& [obj, cols] : all_schemas) {
      w.Comma(first_tbl);
      AppendJsonTableSchemaObject(w, opened.session->dialect, obj, cols);
    }
    w.EndArray();
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }

  std::vector<GridRow> rows;
  for (const auto& [obj, cols] : all_schemas) {
    for (const ColumnMeta& col : cols) {
      rows.push_back(CliMakeTextRow({
          obj.db_schema,
          obj.name,
          col.name,
          col.type,
          col.nullable ? "true" : "false",
          col.primary_key ? "true" : "false",
      }));
    }
  }
  CliWriteTabularText(out, format, {"schema", "table", "column", "type", "nullable", "primary_key"},
                   rows);
  return kCliExitOk;
}

int CliHandleRows(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
               bool early_json) {
  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  CliConnOptions conn;
  std::string schema_filter;
  std::int64_t limit = kCliDefaultRowsLimit;
  std::int64_t offset = 0;
  std::string where_expr;
  std::string redis_type_filter;
  std::string order_spec;
  std::optional<std::string> table_arg;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    if (!CliTryConsumeConnFlag(args, i, conn, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    CliSplitFlag(args[i], flag, inline_val);

    if (flag == "--schema") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, schema_filter, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--limit") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      const auto parsed = CliParseStrictInt64(val);
      if (!parsed.has_value() || *parsed < 1 || *parsed > kCliMaxRowsLimit) {
        return CliEmitError(err, early_json,
                         "--limit 必须为 1.." + std::to_string(kCliMaxRowsLimit) + " 之间的整数",
                         kCliExitUsage);
      }
      limit = *parsed;
      continue;
    }
    if (flag == "--offset") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      const auto parsed = CliParseStrictInt64(val);
      if (!parsed.has_value() || *parsed < 0) {
        return CliEmitError(err, early_json, "--offset 必须为非负整数", kCliExitUsage);
      }
      offset = *parsed;
      continue;
    }
    if (flag == "--where") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, where_expr, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--type") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, redis_type_filter, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--order") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, order_spec, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (!args[i].empty() && args[i].front() == '-') {
      return CliEmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
    }
    if (!table_arg.has_value()) {
      table_arg = args[i];
      continue;
    }
    return CliEmitError(err, early_json, "多余的位置参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    CliPrintCommandHelp("rows", out);
    return kCliExitOk;
  }

  const bool json_mode = (format == CliOutputFormat::Json);
  if (!table_arg.has_value() || table_arg->empty()) {
    return CliEmitError(err, json_mode, "缺少表名: dbpp rows <conn> <table>", kCliExitUsage);
  }

  ConnectionRequest req;
  if (const int code = CliResolveConnectionRequest(conn, req, error); code != kCliExitOk) {
    return CliEmitError(err, json_mode, error, code);
  }

  const AdbcOpenResult opened = OpenAdbcBlocking(req);
  if (!opened.error.empty() || !opened.session) {
    return CliEmitError(err, json_mode, opened.error.empty() ? "连接失败" : opened.error,
                     kCliExitError);
  }

  const AdbcObjectsResult loaded = LoadObjectsBlocking(*opened.session);
  if (!loaded.error.empty()) {
    return CliEmitError(err, json_mode, loaded.error, kCliExitError);
  }

  const auto matched = CliFindObjectByName(loaded.objects, *table_arg, schema_filter, error);
  if (!matched) {
    return CliEmitError(err, json_mode, error, kCliExitError);
  }

  const Dialect dialect = opened.session->dialect;
  if (!redis_type_filter.empty() && dialect != Dialect::Redis) {
    return CliEmitError(err, json_mode, "--type 仅支持 Redis 数据库", kCliExitUsage);
  }
  const std::string qname =
      dialect == Dialect::Redis ? matched->name : QualifiedName(dialect, *matched);
  const std::vector<ColumnMeta> cols = ResolveColumnsForObject(*opened.session, *matched, error);
  if (!error.empty() && cols.empty()) {
    return CliEmitError(err, json_mode, error, kCliExitError);
  }

  std::optional<huxerui::DataTableSort> redis_sort;
  std::string order_clause;
  if (!order_spec.empty()) {
    std::string_view col_part = order_spec;
    std::string dir = "ASC";
    if (const std::size_t colon = col_part.rfind(':'); colon != std::string_view::npos) {
      const std::string suffix = CliLowerAscii(col_part.substr(colon + 1));
      if (suffix == "asc" || suffix == "desc") {
        dir = (suffix == "desc") ? "DESC" : "ASC";
        col_part = col_part.substr(0, colon);
      } else {
        return CliEmitError(err, json_mode,
                         "无效的排序方向 ':" + suffix + "' (应为 :asc 或 :desc)",
                         kCliExitUsage);
      }
    }
    const ColumnMeta* matched_col = nullptr;
    for (const ColumnMeta& col : cols) {
      if (col.name == col_part) {
        matched_col = &col;
        break;
      }
    }
    if (matched_col == nullptr) {
      for (const ColumnMeta& col : cols) {
        if (CliEqualsIgnoreCase(col.name, col_part)) {
          matched_col = &col;
          break;
        }
      }
    }
    if (matched_col == nullptr) {
      return CliEmitError(err, json_mode, "未知排序列: " + std::string(col_part), kCliExitUsage);
    }
    order_clause = " ORDER BY " + QuoteIdentifier(dialect, matched_col->name) + " " + dir;
    redis_sort = huxerui::DataTableSort{
        .column_id = matched_col->name,
        .direction = dir == "DESC" ? huxerui::DataTableSortDirection::Descending
                                   : huxerui::DataTableSortDirection::Ascending,
    };
  }

  if (dialect == Dialect::Redis && opened.session->redis) {
    const RedisPageResult page_res = LoadRedisKeysPageBlocking(
        *opened.session->redis, *matched, offset, limit, redis_sort,
        std::string(CliTrimAscii(where_expr)), nullptr,
        std::string(CliTrimAscii(redis_type_filter)));
    if (!page_res.error.empty()) {
      return CliEmitError(err, json_mode, page_res.error, kCliExitError);
    }
    std::vector<std::string> col_names;
    col_names.reserve(page_res.columns.size());
    for (const ColumnMeta& c : page_res.columns) {
      col_names.push_back(c.name);
    }
    const std::int64_t total = page_res.total;
    const std::int64_t returned = static_cast<std::int64_t>(page_res.rows.size());
    const bool truncated = (offset + returned < total);

    if (json_mode) {
      CliJsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeyString("table", matched->name, first);
      w.KeyString("qualified_name", qname, first);
      w.KeyInt64("total", total, first);
      w.KeyInt64("limit", limit, first);
      w.KeyInt64("offset", offset, first);
      w.KeyInt64("returned", returned, first);
      w.KeyBool("truncated", truncated, first);
      CliAppendJsonRowsColumns(w, col_names, page_res.rows, first);
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }

    CliWriteTabularText(out, format, col_names, page_res.rows);
    if (format == CliOutputFormat::Table) {
      out << "(" << returned << " of " << total << " rows, offset " << offset << ")\n";
    }
    return kCliExitOk;
  }

  std::string where_clause;
  if (!CliTrimAscii(where_expr).empty()) {
    where_clause = " WHERE (" + std::string(CliTrimAscii(where_expr)) + ")";
  }

  const AdbcQueryResult count_res =
      ExecuteBlocking(*opened.session, "SELECT COUNT(*) FROM " + qname + where_clause, true, 1);
  if (!count_res.error.empty() || count_res.rows.empty() ||
      count_res.rows.front().values.empty()) {
    return CliEmitError(err, json_mode,
                     count_res.error.empty() ? "COUNT 查询未返回结果" : count_res.error,
                     kCliExitError);
  }
  const std::int64_t total = CliParseStrictInt64(count_res.rows.front().values.front().text).value_or(0);

  std::string tie_breaker;
  std::vector<const ColumnMeta*> pk_cols;
  for (const ColumnMeta& col : cols) {
    if (col.primary_key) {
      pk_cols.push_back(&col);
    }
  }
  std::ranges::sort(pk_cols, [](const ColumnMeta* a, const ColumnMeta* b) {
    return a->primary_key_order < b->primary_key_order;
  });
  for (const ColumnMeta* col : pk_cols) {
    if (!tie_breaker.empty()) {
      tie_breaker += ", ";
    }
    tie_breaker += QuoteIdentifier(dialect, col->name);
  }
  if (tie_breaker.empty() && dialect == Dialect::Sqlite && !matched->is_view) {
    const AdbcQueryResult probe =
        ExecuteBlocking(*opened.session, "SELECT rowid FROM " + qname + " LIMIT 0", true, 0);
    if (probe.error.empty()) {
      tie_breaker = "rowid";
    }
  }
  if (!tie_breaker.empty()) {
    order_clause += order_clause.empty() ? " ORDER BY " : ", ";
    order_clause += tie_breaker;
  }

  std::string select_list;
  for (const ColumnMeta& col : cols) {
    if (!select_list.empty()) {
      select_list += ", ";
    }
    select_list += QuoteIdentifier(dialect, col.name);
  }
  if (select_list.empty()) {
    select_list = "*";
  }

  const std::string data_sql = "SELECT " + select_list + " FROM " + qname + where_clause +
                               order_clause + " LIMIT " + std::to_string(limit) + " OFFSET " +
                               std::to_string(offset);
  const AdbcQueryResult data =
      ExecuteBlocking(*opened.session, data_sql, true, static_cast<std::size_t>(limit));
  if (!data.error.empty() && data.rows.empty()) {
    return CliEmitError(err, json_mode, data.error, kCliExitError);
  }

  const std::int64_t returned = static_cast<std::int64_t>(data.rows.size());
  const bool truncated = (offset + returned < total) || data.truncated;

  if (json_mode) {
    CliJsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    w.KeyString("table", matched->name, first);
    w.KeyString("qualified_name", qname, first);
    w.KeyInt64("total", total, first);
    w.KeyInt64("limit", limit, first);
    w.KeyInt64("offset", offset, first);
    w.KeyInt64("returned", returned, first);
    w.KeyBool("truncated", truncated, first);
    CliAppendJsonRowsColumns(w, data.columns, data.rows, first);
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }

  CliWriteTabularText(out, format, data.columns, data.rows);
  if (format == CliOutputFormat::Table) {
    out << "(" << returned << " of " << total << " rows, offset " << offset << ")\n";
  }
  return kCliExitOk;
}

int CliHandleQueryOrExecOrSql(const std::string& mode, const std::vector<std::string>& args,
                           std::ostream& out, std::ostream& err, std::istream& in,
                           bool early_json) {
  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  CliConnOptions conn;
  std::size_t max_rows = kCliDefaultQueryMaxRows;
  bool read_only = false;
  bool script_mode = false;
  bool schema_only = false;
  std::vector<PreparedParamValue> bound_params;
  std::optional<std::string> sql_arg;
  std::optional<std::string> file_arg;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "-") {
      if (sql_arg.has_value()) {
        return CliEmitError(err, early_json, "多余的位置参数: -", kCliExitUsage);
      }
      sql_arg = "-";
      continue;
    }

    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    if (!CliTryConsumeConnFlag(args, i, conn, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }

    std::string_view flag;
    std::optional<std::string_view> inline_val;
    CliSplitFlag(args[i], flag, inline_val);

    if (flag == "--sql") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      if (sql_arg.has_value()) {
        return CliEmitError(err, early_json, "不能多次指定 SQL 语句", kCliExitUsage);
      }
      sql_arg = std::move(val);
      continue;
    }
    if (flag == "--file" || flag == "-F") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      file_arg = std::move(val);
      continue;
    }
    if ((mode == "query" || mode == "sql") && flag == "--max-rows") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      const auto parsed = CliParseStrictInt64(val);
      if (!parsed.has_value() || *parsed < 1 ||
          static_cast<std::size_t>(*parsed) > kCliMaxQueryMaxRows) {
        return CliEmitError(err, early_json,
                         "--max-rows 必须为 1.." + std::to_string(kCliMaxQueryMaxRows) + " 之间的整数",
                         kCliExitUsage);
      }
      max_rows = static_cast<std::size_t>(*parsed);
      continue;
    }
    if (mode == "query" && flag == "--read-only") {
      if (inline_val.has_value()) {
        return CliEmitError(err, early_json, "参数 --read-only 不接受值", kCliExitUsage);
      }
      read_only = true;
      continue;
    }
    if (mode == "query" && flag == "--schema-only") {
      schema_only = true;
      continue;
    }
    if ((mode == "query" || mode == "exec" || mode == "sql") && flag == "--param") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      bound_params.push_back(PreparedParamValue{.text = std::move(val), .is_null = false});
      continue;
    }
    if ((mode == "query" || mode == "exec" || mode == "sql") && flag == "--param-null") {
      bound_params.push_back(PreparedParamValue{.text = {}, .is_null = true});
      continue;
    }
    if (mode == "exec" && flag == "--script") {
      if (inline_val.has_value()) {
        return CliEmitError(err, early_json, "参数 --script 不接受值", kCliExitUsage);
      }
      script_mode = true;
      continue;
    }
    if (!args[i].empty() && args[i].front() == '-') {
      return CliEmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
    }
    if (!sql_arg.has_value()) {
      sql_arg = args[i];
      continue;
    }
    return CliEmitError(err, early_json, "多余的位置参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    CliPrintCommandHelp(mode, out);
    return kCliExitOk;
  }

  const bool json_mode = (format == CliOutputFormat::Json);
  std::string sql_text;
  int read_code = kCliExitOk;
  if (!CliReadSqlInput(err, json_mode, in, sql_arg, file_arg, sql_text, read_code)) {
    return read_code;
  }

  ConnectionRequest req;
  if (const int code = CliResolveConnectionRequest(conn, req, error); code != kCliExitOk) {
    return CliEmitError(err, json_mode, error, code);
  }

  const bool anticipated_redis =
      detail::CliContainsIgnoreCase(req.driver_path, "redis") ||
      CliLowerAscii(req.uri).starts_with("redis://") ||
      CliLowerAscii(req.uri).starts_with("rediss://");
  if (read_only &&
      !IsReadStatement(anticipated_redis ? Dialect::Redis : Dialect::Generic, sql_text)) {
    return CliEmitError(err, json_mode, "使用 --read-only 时仅允许只读查询语句", kCliExitUsage);
  }
  req.read_only = read_only;

  const AdbcOpenResult opened = OpenAdbcBlocking(req);
  if (!opened.error.empty() || !opened.session) {
    return CliEmitError(err, json_mode, opened.error.empty() ? "连接失败" : opened.error,
                     kCliExitError);
  }

  if (schema_only) {
    const AdbcSchemaResult schema_res = InspectQuerySchemaBlocking(*opened.session, sql_text);
    if (!schema_res.error.empty()) {
      return CliEmitError(err, json_mode, schema_res.error, kCliExitError);
    }
    if (json_mode) {
      CliJsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.Key("columns", first);
      w.BeginArray();
      bool fc = true;
      for (const ColumnMeta& c : schema_res.columns) {
        w.Comma(fc);
        w.BeginObject();
        bool fcol = true;
        w.KeyString("name", c.name, fcol);
        w.KeyString("type", c.type, fcol);
        w.KeyBool("nullable", c.nullable, fcol);
        w.EndObject();
      }
      w.EndArray();
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }
    std::vector<GridRow> srows;
    for (const ColumnMeta& c : schema_res.columns) {
      srows.push_back(CliMakeTextRow({c.name, c.type, c.nullable ? "YES" : "NO"}));
    }
    CliWriteTabularText(out, format, {"name", "type", "nullable"}, srows);
    return kCliExitOk;
  }

  if (mode == "exec" && script_mode) {
    const std::vector<std::string> stmts =
        opened.session->dialect == Dialect::Redis
            ? SplitRedisScript(sql_text)
            : CliSplitSqlStatements(sql_text, opened.session->dialect);
    if (stmts.empty()) {
      return CliEmitError(err, json_mode, "脚本中不包含可执行 SQL 语句", kCliExitUsage);
    }
    std::size_t executed = 0;
    std::int64_t total_affected = 0;
    bool any_affected_known = false;
    for (const std::string& stmt : stmts) {
      const AdbcQueryResult res = ExecuteBlocking(*opened.session, stmt, false, 0);
      if (!res.error.empty()) {
        return CliEmitError(err, json_mode,
                         "第 " + std::to_string(executed + 1) + " 条语句执行失败: " + res.error,
                         kCliExitError);
      }
      ++executed;
      if (res.affected >= 0) {
        total_affected += res.affected;
        any_affected_known = true;
      }
    }
    const std::int64_t reported_affected = any_affected_known ? total_affected : -1;
    if (json_mode) {
      CliJsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeySize("statements_executed", executed, first);
      w.KeyInt64("affected_rows", reported_affected, first);
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }
    out << "已执行 " << executed << " 条语句";
    if (reported_affected >= 0) {
      out << " (影响 " << reported_affected << " 行)";
    }
    out << '\n';
    return kCliExitOk;
  }

  const bool want_rows =
      (mode == "query") ||
      (mode == "sql" && IsReadStatement(opened.session->dialect, sql_text));
  const AdbcQueryResult res =
      !bound_params.empty()
          ? ExecutePreparedBlocking(*opened.session, sql_text, bound_params, want_rows,
                                    want_rows ? max_rows : 0)
          : ExecuteBlocking(*opened.session, sql_text, want_rows, want_rows ? max_rows : 0);
  if (!res.error.empty() && (!want_rows || res.rows.empty())) {
    return CliEmitError(err, json_mode, res.error, kCliExitError);
  }

  if (!want_rows) {
    if (json_mode) {
      CliJsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      if (mode == "sql") {
        w.KeyString("mode", "exec", first);
      }
      w.KeyInt64("affected_rows", res.affected, first);
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }
    if (res.affected >= 0) {
      out << "影响 " << res.affected << " 行\n";
    } else {
      out << "执行完成\n";
    }
    return kCliExitOk;
  }

  if (json_mode) {
    CliJsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    if (mode == "sql") {
      w.KeyString("mode", "query", first);
    }
    w.KeySize("row_count", res.rows.size(), first);
    w.KeyBool("truncated", res.truncated, first);
    CliAppendJsonRowsColumns(w, res.columns, res.rows, first);
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }

  CliWriteTabularText(out, format, res.columns, res.rows);
  if (format == CliOutputFormat::Table && res.truncated) {
    out << "(truncated at " << res.rows.size() << " rows)\n";
  }
  return kCliExitOk;
}


} // namespace detail

} // namespace dbpp
