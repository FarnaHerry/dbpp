module;

#include <huxerui/datatable.h>

module dbpp.cli;

import std;
import dbpp.types;
import dbpp.adbc;
import dbpp.redis;
import dbpp.db;

namespace dbpp::detail {

namespace {

bool ParseIngestModeArg(std::string_view val, IngestMode& out) {
  const std::string lower = CliLowerAscii(val);
  if (lower == "create") {
    out = IngestMode::Create;
    return true;
  }
  if (lower == "append") {
    out = IngestMode::Append;
    return true;
  }
  if (lower == "replace") {
    out = IngestMode::Replace;
    return true;
  }
  if (lower == "create_append" || lower == "create-append") {
    out = IngestMode::CreateAppend;
    return true;
  }
  return false;
}

bool ParseExportFormatArg(std::string_view val, ExportFormat& out) {
  const std::string lower = CliLowerAscii(val);
  if (lower == "csv") {
    out = ExportFormat::Csv;
    return true;
  }
  if (lower == "jsonl" || lower == "ndjson") {
    out = ExportFormat::Jsonl;
    return true;
  }
  return false;
}

} // namespace

int CliHandleTables(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                    bool early_json) {
  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  CliConnOptions conn;
  std::string schema_filter;
  CliObjectTypeFilter type_filter = CliObjectTypeFilter::All;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) continue;
    if (!CliTryConsumeConnFlag(args, i, conn, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) continue;

    std::string_view flag;
    std::optional<std::string_view> inline_val;
    CliSplitFlag(args[i], flag, inline_val);

    if (flag == "--schema") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, schema_filter, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--type") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      const std::string lower = CliLowerAscii(val);
      if (lower == "all") {
        type_filter = CliObjectTypeFilter::All;
      } else if (lower == "table") {
        type_filter = CliObjectTypeFilter::Table;
      } else if (lower == "view") {
        type_filter = CliObjectTypeFilter::View;
      } else {
        return CliEmitError(err, early_json,
                            "无效的 --type 值: " + val + " (可选: all, table, view)",
                            kCliExitUsage);
      }
      continue;
    }
    return CliEmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    CliPrintCommandHelp("tables", out);
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

  std::vector<DbObject> filtered;
  for (const DbObject& obj : loaded.objects) {
    if (!schema_filter.empty() && !CliEqualsIgnoreCase(obj.db_schema, schema_filter)) {
      continue;
    }
    if (type_filter == CliObjectTypeFilter::Table && obj.is_view) {
      continue;
    }
    if (type_filter == CliObjectTypeFilter::View && !obj.is_view) {
      continue;
    }
    filtered.push_back(obj);
  }

  if (json_mode) {
    CliJsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    w.KeyString("dialect", CliDialectName(opened.session->dialect), first);
    w.KeySize("count", filtered.size(), first);
    w.Key("objects", first);
    w.BeginArray();
    bool first_obj = true;
    for (const DbObject& obj : filtered) {
      w.Comma(first_obj);
      w.BeginObject();
      bool fo = true;
      w.KeyString("name", obj.name, fo);
      w.KeyString("qualified_name", QualifiedName(opened.session->dialect, obj), fo);
      w.KeyString("catalog", obj.catalog, fo);
      w.KeyString("schema", obj.db_schema, fo);
      w.KeyString("table_type", obj.table_type, fo);
      w.KeyBool("is_view", obj.is_view, fo);
      w.Key("primary_key", fo);
      w.BeginArray();
      bool first_pk = true;
      for (const std::string& pk : obj.primary_key) {
        w.Comma(first_pk);
        w.String(pk);
      }
      w.EndArray();
      w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }

  std::vector<GridRow> rows;
  rows.reserve(filtered.size());
  for (const DbObject& obj : filtered) {
    std::string pks;
    for (std::size_t k = 0; k < obj.primary_key.size(); ++k) {
      if (k > 0) pks += ", ";
      pks += obj.primary_key[k];
    }
    rows.push_back(CliMakeTextRow(
        {obj.name, obj.db_schema, obj.table_type, obj.is_view ? "view" : "table", pks}));
  }
  CliWriteTabularText(out, format, {"name", "schema", "table_type", "kind", "primary_key"}, rows);
  return kCliExitOk;
}

int CliHandleInfo(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                  bool early_json) {
  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  CliConnOptions conn;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) continue;
    if (!CliTryConsumeConnFlag(args, i, conn, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) continue;
    return CliEmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
  }
  if (help) {
    CliPrintCommandHelp("info", out);
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

  const AdbcConnectionInfo info = LoadConnectionInfoBlocking(*opened.session);
  std::string types_err;
  const std::vector<std::string> table_types = LoadTableTypesBlocking(*opened.session, types_err);

  if (json_mode) {
    CliJsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    w.KeyString("dialect", CliDialectName(opened.session->dialect), first);
    w.KeyBool("supported", info.supported, first);
    w.KeyString("vendor_name", info.vendor_name, first);
    w.KeyString("vendor_version", info.vendor_version, first);
    w.KeyString("driver_name", info.driver_name, first);
    w.KeyString("driver_version", info.driver_version, first);
    w.KeyString("driver_arrow_version", info.driver_arrow_version, first);
    if (info.driver_adbc_version.has_value()) {
      w.KeyInt64("driver_adbc_version", *info.driver_adbc_version, first);
    }
    if (info.vendor_sql.has_value()) {
      w.KeyBool("vendor_sql", *info.vendor_sql, first);
    }
    if (info.vendor_substrait.has_value()) {
      w.KeyBool("vendor_substrait", *info.vendor_substrait, first);
    }
    w.Key("table_types", first);
    w.BeginArray();
    bool ft = true;
    for (const std::string& tt : table_types) {
      w.Comma(ft);
      w.String(tt);
    }
    w.EndArray();
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }

  std::vector<GridRow> rows;
  rows.push_back(CliMakeTextRow({"dialect", std::string(CliDialectName(opened.session->dialect))}));
  rows.push_back(CliMakeTextRow({"vendor_name", info.vendor_name}));
  rows.push_back(CliMakeTextRow({"vendor_version", info.vendor_version}));
  rows.push_back(CliMakeTextRow({"driver_name", info.driver_name}));
  rows.push_back(CliMakeTextRow({"driver_version", info.driver_version}));
  rows.push_back(CliMakeTextRow({"driver_arrow_version", info.driver_arrow_version}));
  if (info.driver_adbc_version.has_value()) {
    rows.push_back(CliMakeTextRow({"driver_adbc_version", std::to_string(*info.driver_adbc_version)}));
  }
  CliWriteTabularText(out, format, {"property", "value"}, rows);
  return kCliExitOk;
}

int CliHandleStats(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                   bool early_json) {
  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  CliConnOptions conn;
  std::string schema_filter;
  std::optional<std::string> table_arg;
  bool approximate = true;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) continue;
    if (!CliTryConsumeConnFlag(args, i, conn, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) continue;

    std::string_view flag;
    std::optional<std::string_view> inline_val;
    CliSplitFlag(args[i], flag, inline_val);
    if (flag == "--schema") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, schema_filter, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--exact") {
      approximate = false;
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
    CliPrintCommandHelp("stats", out);
    return kCliExitOk;
  }
  const bool json_mode = (format == CliOutputFormat::Json);
  if (!table_arg.has_value()) {
    return CliEmitError(err, json_mode, "缺少表名参数", kCliExitUsage);
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
  const TableStatisticsResult stats =
      LoadTableStatisticsBlocking(*opened.session, *matched, approximate);
  if (!stats.error.empty()) {
    return CliEmitError(err, json_mode, stats.error, kCliExitError);
  }

  if (json_mode) {
    CliJsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    w.KeyString("table", matched->name, first);
    w.KeyBool("supported", stats.supported, first);
    if (stats.row_count.has_value()) {
      w.KeyInt64("row_count", *stats.row_count, first);
      w.KeyBool("row_count_approximate", stats.row_count_approximate, first);
    }
    w.Key("statistics", first);
    w.BeginArray();
    bool fs = true;
    for (const ColumnStatistic& st : stats.statistics) {
      w.Comma(fs);
      w.BeginObject();
      bool fi = true;
      w.KeyString("column", st.column_name, fi);
      w.KeyString("statistic", st.statistic_name, fi);
      w.KeyInt64("key", st.statistic_key, fi);
      w.KeyString("value", st.value_text, fi);
      w.KeyBool("approximate", st.approximate, fi);
      w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }

  if (!stats.supported) {
    out << "当前 ADBC 驱动未实现 AdbcConnectionGetStatistics\n";
    return kCliExitOk;
  }
  std::vector<GridRow> rows;
  for (const ColumnStatistic& st : stats.statistics) {
    rows.push_back(CliMakeTextRow({st.column_name.empty() ? "(table)" : st.column_name,
                                   st.statistic_name, st.value_text,
                                   st.approximate ? "yes" : "no"}));
  }
  CliWriteTabularText(out, format, {"column", "statistic", "value", "approximate"}, rows);
  return kCliExitOk;
}

int CliHandleImport(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                    bool early_json) {
  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  CliConnOptions conn;
  std::optional<std::string> table_arg;
  std::optional<std::string> file_arg;
  std::string file_type;
  IngestMode mode = IngestMode::CreateAppend;
  std::string target_catalog;
  std::string target_schema;
  char delimiter = ',';
  bool has_header = true;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) continue;
    if (!CliTryConsumeConnFlag(args, i, conn, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) continue;

    std::string_view flag;
    std::optional<std::string_view> inline_val;
    CliSplitFlag(args[i], flag, inline_val);
    if (flag == "--type") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, file_type, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--mode") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error) ||
          !ParseIngestModeArg(val, mode)) {
        return CliEmitError(err, early_json, error.empty() ? "无效的 --mode" : error,
                            kCliExitUsage);
      }
      continue;
    }
    if (flag == "--catalog") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, target_catalog, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--schema") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, target_schema, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--delimiter") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error) || val.empty()) {
        return CliEmitError(err, early_json, error.empty() ? "分隔符不能为空" : error,
                            kCliExitUsage);
      }
      delimiter = (val == "\\t" || val == "tab") ? '\t' : val[0];
      continue;
    }
    if (flag == "--no-header") {
      has_header = false;
      continue;
    }
    if (!args[i].empty() && args[i].front() == '-') {
      return CliEmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
    }
    if (!table_arg.has_value()) {
      table_arg = args[i];
    } else if (!file_arg.has_value()) {
      file_arg = args[i];
    } else {
      return CliEmitError(err, early_json, "多余的位置参数: " + args[i], kCliExitUsage);
    }
  }

  if (help) {
    CliPrintCommandHelp("import", out);
    return kCliExitOk;
  }
  const bool json_mode = (format == CliOutputFormat::Json);
  if (!table_arg.has_value() || !file_arg.has_value()) {
    return CliEmitError(err, json_mode, "用法: dbpp import <conn> <table> <file>", kCliExitUsage);
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

  const std::filesystem::path file_path(*file_arg);
  const std::string ext = CliLowerAscii(file_path.extension().string());
  const std::string kind = file_type.empty()
                               ? ((ext == ".jsonl" || ext == ".ndjson") ? "jsonl" : "csv")
                               : CliLowerAscii(file_type);
  const BulkIngestResult res =
      (kind == "jsonl" || kind == "ndjson")
          ? IngestJsonlFileBlocking(*opened.session, file_path, *table_arg, mode,
                                    target_catalog, target_schema)
          : IngestCsvFileBlocking(*opened.session, file_path, *table_arg, mode,
                                  target_catalog, target_schema, delimiter, has_header);
  if (!res.error.empty()) {
    return CliEmitError(err, json_mode, res.error, kCliExitError);
  }

  if (json_mode) {
    CliJsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    w.KeyString("table", *table_arg, first);
    w.KeyInt64("rows_affected", res.rows_affected, first);
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }
  out << "导入完成: " << *table_arg;
  if (res.rows_affected >= 0) {
    out << " (" << res.rows_affected << " 行)";
  }
  out << '\n';
  return kCliExitOk;
}

int CliHandleExport(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                    std::istream& in, bool early_json) {
  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  CliConnOptions conn;
  std::optional<std::string> sql_arg;
  std::optional<std::string> file_arg;
  std::optional<std::string> out_arg;
  std::string type_str;
  std::size_t max_rows = 0;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "-") {
      sql_arg = "-";
      continue;
    }
    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) continue;
    if (!CliTryConsumeConnFlag(args, i, conn, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) continue;

    std::string_view flag;
    std::optional<std::string_view> inline_val;
    CliSplitFlag(args[i], flag, inline_val);
    if (flag == "--sql") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
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
    if (flag == "--out" || flag == "-o") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      out_arg = std::move(val);
      continue;
    }
    if (flag == "--type") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, type_str, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--max-rows") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      const auto n = CliParseStrictInt64(val);
      if (!n.has_value() || *n < 0) {
        return CliEmitError(err, early_json, "--max-rows 必须为非负整数", kCliExitUsage);
      }
      max_rows = static_cast<std::size_t>(*n);
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
    CliPrintCommandHelp("export", out);
    return kCliExitOk;
  }
  const bool json_mode = (format == CliOutputFormat::Json);
  std::string sql_text;
  int read_code = kCliExitOk;
  if (!CliReadSqlInput(err, json_mode, in, sql_arg, file_arg, sql_text, read_code)) {
    return read_code;
  }

  ExportFormat export_fmt = ExportFormat::Csv;
  if (!type_str.empty()) {
    if (!ParseExportFormatArg(type_str, export_fmt)) {
      return CliEmitError(err, json_mode, "无效的 --type (可选: csv, jsonl)", kCliExitUsage);
    }
  } else if (out_arg.has_value()) {
    const std::string ext = CliLowerAscii(std::filesystem::path(*out_arg).extension().string());
    if (ext == ".jsonl" || ext == ".ndjson") {
      export_fmt = ExportFormat::Jsonl;
    }
  }

  ConnectionRequest req;
  req.read_only = true;
  if (const int code = CliResolveConnectionRequest(conn, req, error); code != kCliExitOk) {
    return CliEmitError(err, json_mode, error, code);
  }
  const AdbcOpenResult opened = OpenAdbcBlocking(req);
  if (!opened.error.empty() || !opened.session) {
    return CliEmitError(err, json_mode, opened.error.empty() ? "连接失败" : opened.error,
                        kCliExitError);
  }

  ExportStreamResult res;
  if (out_arg.has_value()) {
    res = ExportQueryToFileBlocking(*opened.session, sql_text, export_fmt, *out_arg, max_rows);
  } else {
    res = ExportQueryStreamBlocking(*opened.session, sql_text, export_fmt, out, max_rows);
  }
  if (!res.error.empty()) {
    return CliEmitError(err, json_mode, res.error, kCliExitError);
  }

  if (out_arg.has_value()) {
    if (json_mode) {
      CliJsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeyString("output", *out_arg, first);
      w.KeySize("rows_written", res.rows_written, first);
      w.KeyBool("truncated", res.truncated, first);
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
    } else {
      out << "导出完成: " << res.rows_written << " 行 -> " << *out_arg << '\n';
    }
  }
  return kCliExitOk;
}

int CliHandleCopy(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                  bool early_json) {
  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  CliConnOptions src_conn;
  CliConnOptions dst_conn;
  std::string sql_text;
  std::string target_table;
  std::string target_catalog;
  std::string target_schema;
  IngestMode mode = IngestMode::CreateAppend;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) continue;

    std::string_view flag;
    std::optional<std::string_view> inline_val;
    CliSplitFlag(args[i], flag, inline_val);

    if (flag == "--src-sqlite") {
      src_conn.has_sqlite = true;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, src_conn.sqlite_path, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--src-driver") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, src_conn.driver, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--src-uri") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, src_conn.uri, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--src-profile") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, src_conn.profile_name, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--dst-sqlite") {
      dst_conn.has_sqlite = true;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, dst_conn.sqlite_path, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--dst-driver") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, dst_conn.driver, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--dst-uri") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, dst_conn.uri, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--dst-profile") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, dst_conn.profile_name, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--sql") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, sql_text, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--table") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, target_table, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--catalog") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, target_catalog, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--schema") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, target_schema, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--mode") {
      std::string val;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, val, error) ||
          !ParseIngestModeArg(val, mode)) {
        return CliEmitError(err, early_json, error.empty() ? "无效的 --mode" : error,
                            kCliExitUsage);
      }
      continue;
    }
    return CliEmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    CliPrintCommandHelp("copy", out);
    return kCliExitOk;
  }
  const bool json_mode = (format == CliOutputFormat::Json);
  if (target_table.empty()) {
    return CliEmitError(err, json_mode, "缺少 --table 参数", kCliExitUsage);
  }
  ConnectionRequest src_req;
  if (const int code = CliResolveConnectionRequest(src_conn, src_req, error); code != kCliExitOk) {
    return CliEmitError(err, json_mode, error, code);
  }
  ConnectionRequest dst_req;
  if (const int code = CliResolveConnectionRequest(dst_conn, dst_req, error); code != kCliExitOk) {
    return CliEmitError(err, json_mode, error, code);
  }

  src_req.read_only = true;
  const AdbcOpenResult src_open = OpenAdbcBlocking(src_req);
  if (!src_open.error.empty() || !src_open.session) {
    return CliEmitError(err, json_mode, src_open.error.empty() ? "源连接失败" : src_open.error,
                        kCliExitError);
  }
  const AdbcOpenResult dst_open = OpenAdbcBlocking(dst_req);
  if (!dst_open.error.empty() || !dst_open.session) {
    return CliEmitError(err, json_mode, dst_open.error.empty() ? "目标连接失败" : dst_open.error,
                        kCliExitError);
  }

  if (sql_text.empty()) {
    sql_text = "SELECT * FROM " + QuoteIdentifier(src_open.session->dialect, target_table);
  }
  const BulkIngestResult res =
      CopyQueryToTableBlocking(*src_open.session, sql_text, *dst_open.session, target_table,
                               mode, target_catalog, target_schema);
  if (!res.error.empty()) {
    return CliEmitError(err, json_mode, res.error, kCliExitError);
  }

  if (json_mode) {
    CliJsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    w.KeyString("table", target_table, first);
    w.KeyInt64("rows_affected", res.rows_affected, first);
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }
  out << "跨库复制完成: " << target_table;
  if (res.rows_affected >= 0) {
    out << " (" << res.rows_affected << " 行)";
  }
  out << '\n';
  return kCliExitOk;
}

} // namespace dbpp::detail
