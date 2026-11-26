module;

#include <huxerui/state.h>

module dbpp.mcp;

import std;
import dbpp.settings;
import dbpp.types;
import dbpp.adbc;
import dbpp.db;

namespace dbpp::detail {

namespace {

void AppendJsonString(std::string& out, std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (const unsigned char uc : text) {
    switch (uc) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (uc < 0x20U) {
          out += "\\u00";
          out.push_back(kHex[(uc >> 4) & 0x0FU]);
          out.push_back(kHex[uc & 0x0FU]);
        } else {
          out.push_back(static_cast<char>(uc));
        }
        break;
    }
  }
  out.push_back('"');
}

void AppendBoolField(std::string& out, bool& first, std::string_view key, bool val) {
  if (!first) out += ", ";
  first = false;
  AppendJsonString(out, key);
  out += ": ";
  out += (val ? "true" : "false");
}

void AppendStringField(std::string& out, bool& first, std::string_view key, std::string_view val) {
  if (!first) out += ", ";
  first = false;
  AppendJsonString(out, key);
  out += ": ";
  AppendJsonString(out, val);
}

void AppendIntField(std::string& out, bool& first, std::string_view key, std::int64_t val) {
  if (!first) out += ", ";
  first = false;
  AppendJsonString(out, key);
  out += ": ";
  out += std::to_string(val);
}

const DbObject* FindMatchedObject(const std::vector<DbObject>& objects,
                                    std::string_view table_name,
                                    std::string_view schema_name) {
  for (const DbObject& o : objects) {
    if (o.name == table_name && (schema_name.empty() || o.db_schema == schema_name)) {
      return &o;
    }
  }
  for (const DbObject& o : objects) {
    if (o.name == table_name) {
      return &o;
    }
  }
  return nullptr;
}

} // namespace

McpExtendedToolOutput McpHandleExtendedToolCall(
    std::string_view tool_name, AdbcSession& session, std::string_view table_name,
    std::string_view schema_name, std::string_view sql, bool approximate,
    bool read_only, std::int64_t max_rows) {
  McpExtendedToolOutput res;

  if (tool_name == "list_tables") {
    res.handled = true;
    const AdbcObjectsResult objs = LoadObjectsBlocking(session);
    if (!objs.error.empty() && objs.objects.empty()) {
      res.is_error = true;
      res.payload_json = "Failed to list tables: " + objs.error;
      return res;
    }
    std::string out = "{\n  \"count\": " + std::to_string(objs.objects.size()) +
                      ",\n  \"objects\": [";
    for (std::size_t i = 0; i < objs.objects.size(); ++i) {
      const DbObject& o = objs.objects[i];
      if (i > 0) out += ",";
      out += "\n    {";
      bool f = true;
      AppendStringField(out, f, "name", o.name);
      AppendStringField(out, f, "schema", o.db_schema);
      AppendStringField(out, f, "catalog", o.catalog);
      AppendStringField(out, f, "table_type", o.table_type);
      AppendBoolField(out, f, "is_view", o.is_view);
      out += "}";
    }
    out += "\n  ]\n}";
    res.payload_json = std::move(out);
    return res;
  }

  if (tool_name == "run_query") {
    res.handled = true;
    if (McpTrimView(sql).empty()) {
      res.is_error = true;
      res.payload_json = "Missing required argument `sql`";
      return res;
    }
    const bool is_read = IsReadStatement(session.dialect, sql);
    if (read_only && !is_read) {
      res.is_error = true;
      res.payload_json =
          "Read-only mode is enabled in db++ MCP settings; write/DDL statements are blocked.";
      return res;
    }
    const std::int64_t clamped_rows = std::clamp<std::int64_t>(max_rows, 1, 1000);
    const AdbcQueryResult qres =
        ExecuteBlocking(session, std::string(sql), is_read, static_cast<std::size_t>(clamped_rows));
    if (!qres.error.empty() && qres.rows.empty()) {
      res.is_error = true;
      res.payload_json = "Query failed: " + qres.error;
      return res;
    }
    std::string out = "{\n  \"columns\": [";
    for (std::size_t i = 0; i < qres.columns.size(); ++i) {
      if (i > 0) out += ", ";
      AppendJsonString(out, qres.columns[i]);
    }
    out += "],\n  \"rows\": [";
    for (std::size_t r = 0; r < qres.rows.size(); ++r) {
      if (r > 0) out += ",";
      out += "\n    [";
      for (std::size_t c = 0; c < qres.rows[r].values.size(); ++c) {
        if (c > 0) out += ", ";
        const CellValue& cell = qres.rows[r].values[c];
        if (cell.is_null) {
          out += "null";
        } else {
          AppendJsonString(out, cell.text);
        }
      }
      out += "]";
    }
    out += "\n  ]";
    bool f = false;
    AppendIntField(out, f, "row_count", static_cast<std::int64_t>(qres.rows.size()));
    AppendBoolField(out, f, "truncated", qres.truncated);
    AppendIntField(out, f, "affected", qres.affected);
    out += "\n}";
    res.payload_json = std::move(out);
    return res;
  }

  if (tool_name == "describe_table") {
    res.handled = true;
    if (table_name.empty()) {
      res.is_error = true;
      res.payload_json = "Missing required argument `table`";
      return res;
    }
    const AdbcObjectsResult objs = LoadObjectsBlocking(session);
    const DbObject* matched = FindMatchedObject(objs.objects, table_name, schema_name);
    DbObject target = matched != nullptr
                          ? *matched
                          : DbObject{.name = std::string(table_name),
                                     .db_schema = std::string(schema_name)};
    const AdbcSchemaResult schema = LoadSchemaBlocking(session, target);
    if (!schema.error.empty() && schema.columns.empty()) {
      res.is_error = true;
      res.payload_json = "Failed to describe table: " + schema.error;
      return res;
    }

    std::string out = "{\n  \"table\": ";
    AppendJsonString(out, table_name);
    out += ",\n  \"schema\": ";
    AppendJsonString(out, schema_name);
    out += ",\n  \"columns\": [";
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
      const ColumnMeta& col = schema.columns[i];
      if (i > 0) out += ",";
      out += "\n    {";
      bool f = true;
      AppendStringField(out, f, "name", col.name);
      AppendStringField(out, f, "type", col.type);
      AppendBoolField(out, f, "nullable", col.nullable);
      AppendBoolField(out, f, "primary_key", col.primary_key);
      if (!col.default_expr.empty()) AppendStringField(out, f, "default", col.default_expr);
      if (!col.remarks.empty()) AppendStringField(out, f, "remarks", col.remarks);
      if (col.unique) AppendBoolField(out, f, "unique", true);
      if (col.autoincrement) AppendBoolField(out, f, "autoincrement", true);
      if (col.generated) AppendBoolField(out, f, "generated", true);
      out += "}";
    }
    out += "\n  ]";
    if (matched != nullptr && !matched->foreign_keys.empty()) {
      out += ",\n  \"foreign_keys\": [";
      for (std::size_t i = 0; i < matched->foreign_keys.size(); ++i) {
        const ForeignKeyMeta& fk = matched->foreign_keys[i];
        if (i > 0) out += ",";
        out += "\n    {";
        bool f = true;
        AppendStringField(out, f, "name", fk.constraint_name);
        AppendStringField(out, f, "ref_table", fk.fk_table);
        if (!fk.fk_db_schema.empty()) AppendStringField(out, f, "ref_schema", fk.fk_db_schema);
        out += ", \"columns\": [";
        for (std::size_t j = 0; j < fk.columns.size(); ++j) {
          if (j > 0) out += ", ";
          AppendJsonString(out, fk.columns[j]);
        }
        out += "], \"ref_columns\": [";
        for (std::size_t j = 0; j < fk.fk_columns.size(); ++j) {
          if (j > 0) out += ", ";
          AppendJsonString(out, fk.fk_columns[j]);
        }
        out += "]}";
      }
      out += "\n  ]";
    }
    out += "\n}";
    res.payload_json = std::move(out);
    return res;
  }

  if (tool_name == "get_connection_info") {
    res.handled = true;
    const AdbcConnectionInfo info = LoadConnectionInfoBlocking(session);
    std::string types_err;
    const std::vector<std::string> table_types = LoadTableTypesBlocking(session, types_err);
    std::string out = "{\n  ";
    bool f = true;
    AppendBoolField(out, f, "supported", info.supported);
    AppendStringField(out, f, "vendor_name", info.vendor_name);
    AppendStringField(out, f, "vendor_version", info.vendor_version);
    AppendStringField(out, f, "driver_name", info.driver_name);
    AppendStringField(out, f, "driver_version", info.driver_version);
    AppendStringField(out, f, "driver_arrow_version", info.driver_arrow_version);
    if (info.driver_adbc_version.has_value()) {
      AppendIntField(out, f, "driver_adbc_version", *info.driver_adbc_version);
    }
    if (info.vendor_sql.has_value()) {
      AppendBoolField(out, f, "vendor_sql", *info.vendor_sql);
    }
    if (info.vendor_substrait.has_value()) {
      AppendBoolField(out, f, "vendor_substrait", *info.vendor_substrait);
    }
    out += ", \"table_types\": [";
    for (std::size_t i = 0; i < table_types.size(); ++i) {
      if (i > 0) out += ", ";
      AppendJsonString(out, table_types[i]);
    }
    out += "]\n}";
    res.payload_json = std::move(out);
    return res;
  }

  if (tool_name == "get_table_stats") {
    res.handled = true;
    if (table_name.empty()) {
      res.is_error = true;
      res.payload_json = "Missing required argument `table`";
      return res;
    }
    DbObject target;
    target.name = std::string(table_name);
    target.db_schema = std::string(schema_name);
    const TableStatisticsResult stats = LoadTableStatisticsBlocking(session, target, approximate);
    if (!stats.error.empty()) {
      res.is_error = true;
      res.payload_json = "Failed to get table stats: " + stats.error;
      return res;
    }
    std::string out = "{\n  ";
    bool f = true;
    AppendStringField(out, f, "table", table_name);
    AppendBoolField(out, f, "supported", stats.supported);
    if (stats.row_count.has_value()) {
      AppendIntField(out, f, "row_count", *stats.row_count);
      AppendBoolField(out, f, "row_count_approximate", stats.row_count_approximate);
    }
    out += ",\n  \"statistics\": [";
    for (std::size_t i = 0; i < stats.statistics.size(); ++i) {
      const ColumnStatistic& s = stats.statistics[i];
      if (i > 0) out += ",";
      out += "\n    {";
      bool fs = true;
      AppendStringField(out, fs, "column", s.column_name);
      AppendStringField(out, fs, "statistic", s.statistic_name);
      AppendStringField(out, fs, "value", s.value_text);
      AppendBoolField(out, fs, "approximate", s.approximate);
      out += "}";
    }
    out += "\n  ]\n}";
    res.payload_json = std::move(out);
    return res;
  }

  if (tool_name == "inspect_query_schema") {
    res.handled = true;
    if (McpTrimView(sql).empty()) {
      res.is_error = true;
      res.payload_json = "Missing required argument `sql`";
      return res;
    }
    const AdbcSchemaResult schema = InspectQuerySchemaBlocking(session, std::string(sql));
    if (!schema.error.empty()) {
      res.is_error = true;
      res.payload_json = "Failed to inspect query schema: " + schema.error;
      return res;
    }
    std::string out = "{\n  \"columns\": [";
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
      const ColumnMeta& c = schema.columns[i];
      if (i > 0) out += ",";
      out += "\n    {";
      bool fc = true;
      AppendStringField(out, fc, "name", c.name);
      AppendStringField(out, fc, "type", c.type);
      AppendBoolField(out, fc, "nullable", c.nullable);
      out += "}";
    }
    out += "\n  ]\n}";
    res.payload_json = std::move(out);
    return res;
  }

  return res;
}

} // namespace dbpp::detail
