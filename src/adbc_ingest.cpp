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

const char* IngestModeOptionValue(IngestMode mode) {
  switch (mode) {
    case IngestMode::Create: return ADBC_INGEST_OPTION_MODE_CREATE;
    case IngestMode::Append: return ADBC_INGEST_OPTION_MODE_APPEND;
    case IngestMode::Replace: return ADBC_INGEST_OPTION_MODE_REPLACE;
    case IngestMode::CreateAppend: return ADBC_INGEST_OPTION_MODE_CREATE_APPEND;
  }
  return ADBC_INGEST_OPTION_MODE_CREATE;
}

ArrowType InferColumnTypeFromSamples(const std::vector<std::vector<std::optional<std::string>>>& rows,
                                     std::size_t col_idx) {
  bool saw_value = false;
  bool all_bool = true;
  bool all_int = true;
  bool all_double = true;
  const std::size_t limit = std::min<std::size_t>(rows.size(), 512);
  for (std::size_t r = 0; r < limit; ++r) {
    if (col_idx >= rows[r].size() || !rows[r][col_idx].has_value()) {
      continue;
    }
    const std::string& val = *rows[r][col_idx];
    if (val.empty()) {
      continue;
    }
    saw_value = true;
    const std::string lower = detail::AdbcLowerAscii(val);
    if (lower != "true" && lower != "false") {
      all_bool = false;
    }
    if (!detail::AdbcParseInt64(val).has_value()) {
      all_int = false;
    }
    if (!detail::AdbcParseDouble(val).has_value()) {
      all_double = false;
    }
    if (!all_bool && !all_int && !all_double) {
      return NANOARROW_TYPE_STRING;
    }
  }
  if (!saw_value) {
    return NANOARROW_TYPE_STRING;
  }
  if (all_bool) {
    return NANOARROW_TYPE_BOOL;
  }
  if (all_int) {
    return NANOARROW_TYPE_INT64;
  }
  if (all_double) {
    return NANOARROW_TYPE_DOUBLE;
  }
  return NANOARROW_TYPE_STRING;
}

bool ParseCsvFile(const std::filesystem::path& path, char delimiter, bool has_header,
                  detail::AdbcTabularDataset& out, std::string& error_out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    error_out = TrFmt("Failed to open delimited file: {0}", "无法打开分隔符文件: {0}",
                      {path.string()});
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
      static_cast<unsigned char>(content[1]) == 0xBB &&
      static_cast<unsigned char>(content[2]) == 0xBF) {
    content.erase(0, 3);
  }

  std::vector<std::vector<std::optional<std::string>>> parsed_rows;
  std::vector<std::optional<std::string>> cur_row;
  std::string field;
  bool in_quotes = false;
  bool field_quoted = false;

  const auto push_field = [&] {
    if (!field_quoted && field.empty()) {
      cur_row.push_back(std::nullopt);
    } else {
      cur_row.push_back(std::move(field));
    }
    field.clear();
    field_quoted = false;
  };
  const auto push_row = [&] {
    if (!cur_row.empty() && !(cur_row.size() == 1 && !cur_row[0].has_value())) {
      parsed_rows.push_back(std::move(cur_row));
    }
    cur_row.clear();
  };

  for (std::size_t i = 0; i < content.size(); ++i) {
    const char ch = content[i];
    if (in_quotes) {
      if (ch == '"') {
        if (i + 1 < content.size() && content[i + 1] == '"') {
          field.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        field.push_back(ch);
      }
      continue;
    }
    if (ch == '"' && field.empty() && !field_quoted) {
      in_quotes = true;
      field_quoted = true;
    } else if (ch == delimiter) {
      push_field();
    } else if (ch == '\r') {
      if (i + 1 < content.size() && content[i + 1] == '\n') {
        ++i;
      }
      push_field();
      push_row();
    } else if (ch == '\n') {
      push_field();
      push_row();
    } else {
      field.push_back(ch);
    }
  }
  if (in_quotes) {
    error_out = Tr("Unterminated quoted field in CSV", "CSV 中存在未闭合的引号字段");
    return false;
  }
  if (!field.empty() || field_quoted || !cur_row.empty()) {
    push_field();
    push_row();
  }

  if (parsed_rows.empty()) {
    error_out = Tr("CSV file contains no rows", "CSV 文件没有数据行");
    return false;
  }

  std::size_t num_cols = 0;
  for (const auto& r : parsed_rows) {
    num_cols = std::max(num_cols, r.size());
  }
  if (num_cols == 0) {
    error_out = Tr("CSV file has no columns", "CSV 文件没有列");
    return false;
  }

  std::size_t data_start = 0;
  if (has_header) {
    out.columns.reserve(num_cols);
    for (std::size_t c = 0; c < num_cols; ++c) {
      std::string name =
          (c < parsed_rows[0].size() && parsed_rows[0][c].has_value() && !parsed_rows[0][c]->empty())
              ? *parsed_rows[0][c]
              : ("col_" + std::to_string(c + 1));
      out.columns.push_back(std::move(name));
    }
    data_start = 1;
  } else {
    for (std::size_t c = 0; c < num_cols; ++c) {
      out.columns.push_back("col_" + std::to_string(c + 1));
    }
  }

  out.rows.reserve(parsed_rows.size() - data_start);
  for (std::size_t r = data_start; r < parsed_rows.size(); ++r) {
    auto row = std::move(parsed_rows[r]);
    row.resize(num_cols);
    out.rows.push_back(std::move(row));
  }
  detail::AdbcFinalizeTabularDatasetTypes(out);
  return true;
}

bool BuildDatasetStream(const detail::AdbcTabularDataset& dataset, ArrowArrayStream& stream_out,
                        std::string& error_out) {
  stream_out = ArrowArrayStream{};
  detail::AdbcSchemaHolder schema;
  ArrowSchemaInit(&schema.schema);
  if (ArrowSchemaSetTypeStruct(&schema.schema,
                               static_cast<std::int64_t>(dataset.columns.size())) != NANOARROW_OK) {
    error_out = "Failed to initialize Arrow schema for bulk ingest";
    return false;
  }
  for (std::size_t c = 0; c < dataset.columns.size(); ++c) {
    ArrowSchema* child = schema.schema.children[c];
    if (ArrowSchemaSetType(child, dataset.types[c]) != NANOARROW_OK ||
        ArrowSchemaSetName(child, dataset.columns[c].c_str()) != NANOARROW_OK) {
      error_out = "Failed to set Arrow column schema for bulk ingest";
      return false;
    }
    child->flags |= ARROW_FLAG_NULLABLE;
  }

  constexpr std::size_t kBatchSize = 1024;
  const std::size_t total_rows = dataset.rows.size();
  const std::size_t num_batches = std::max<std::size_t>(1, (total_rows + kBatchSize - 1) / kBatchSize);

  detail::AdbcSchemaHolder schema_copy;
  if (ArrowSchemaDeepCopy(&schema.schema, &schema_copy.schema) != NANOARROW_OK) {
    error_out = "Failed to copy Arrow schema";
    return false;
  }
  if (ArrowBasicArrayStreamInit(&stream_out, &schema.schema,
                                static_cast<std::int64_t>(num_batches)) != NANOARROW_OK) {
    error_out = "Failed to initialize ArrowBasicArrayStream";
    return false;
  }

  for (std::size_t b = 0; b < num_batches; ++b) {
    const std::size_t start = b * kBatchSize;
    const std::size_t end = std::min(total_rows, start + kBatchSize);
    detail::AdbcArrayHolder batch;
    ArrowError ae;
    ArrowErrorInit(&ae);
    if (ArrowArrayInitFromSchema(&batch.array, &schema_copy.schema, &ae) != NANOARROW_OK ||
        ArrowArrayStartAppending(&batch.array) != NANOARROW_OK) {
      error_out = detail::AdbcDecodeSystemTextToUtf8(ae.message);
      stream_out.release(&stream_out);
      stream_out = ArrowArrayStream{};
      return false;
    }

    for (std::size_t r = start; r < end; ++r) {
      const auto& row = dataset.rows[r];
      for (std::size_t c = 0; c < dataset.columns.size(); ++c) {
        ArrowArray* child = batch.array.children[c];
        const auto& val_opt = c < row.size() ? row[c] : std::optional<std::string>{};
        if (!val_opt.has_value() || val_opt->empty()) {
          if (!val_opt.has_value() || dataset.types[c] != NANOARROW_TYPE_STRING) {
            (void)ArrowArrayAppendNull(child, 1);
            continue;
          }
        }
        const std::string& v = *val_opt;
        ArrowErrorCode rc = NANOARROW_OK;
        switch (dataset.types[c]) {
          case NANOARROW_TYPE_BOOL:
            rc = ArrowArrayAppendInt(child, detail::AdbcParseBool(v).value_or(false) ? 1 : 0);
            break;
          case NANOARROW_TYPE_INT64:
            rc = ArrowArrayAppendInt(child, detail::AdbcParseInt64(v).value_or(0));
            break;
          case NANOARROW_TYPE_DOUBLE:
            rc = ArrowArrayAppendDouble(child, detail::AdbcParseDouble(v).value_or(0.0));
            break;
          default:
            rc = ArrowArrayAppendString(
                child, ArrowStringView{v.data(), static_cast<std::int64_t>(v.size())});
            break;
        }
        if (rc != NANOARROW_OK) {
          error_out = "Failed to append value during bulk ingest";
          stream_out.release(&stream_out);
          stream_out = ArrowArrayStream{};
          return false;
        }
      }
      if (ArrowArrayFinishElement(&batch.array) != NANOARROW_OK) {
        error_out = "Failed to finish row during bulk ingest";
        stream_out.release(&stream_out);
        stream_out = ArrowArrayStream{};
        return false;
      }
    }

    ArrowErrorInit(&ae);
    if (ArrowArrayFinishBuildingDefault(&batch.array, &ae) != NANOARROW_OK) {
      error_out = detail::AdbcDecodeSystemTextToUtf8(ae.message);
      stream_out.release(&stream_out);
      stream_out = ArrowArrayStream{};
      return false;
    }
    ArrowBasicArrayStreamSetArray(&stream_out, static_cast<std::int64_t>(b), &batch.array);
  }
  return true;
}

BulkIngestResult ExecuteBulkIngestStream(AdbcSession& session, ArrowArrayStream& stream,
                                         std::string_view target_table, IngestMode mode,
                                         std::string_view target_catalog,
                                         std::string_view target_schema) {
  BulkIngestResult result;
  if (session.IsClosed() || !session.connection_created) {
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    if (stream.release != nullptr) {
      stream.release(&stream);
      stream = ArrowArrayStream{};
    }
    return result;
  }
  if (session.read_only) {
    result.error = ReadOnlyWriteBlockedMessage();
    if (stream.release != nullptr) {
      stream.release(&stream);
      stream = ArrowArrayStream{};
    }
    return result;
  }

  detail::AdbcErrorGuard error;
  detail::AdbcStatementGuard stmt;
  AdbcStatusCode status = AdbcStatementNew(&session.connection, &stmt.statement, error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    if (stream.release != nullptr) {
      stream.release(&stream);
      stream = ArrowArrayStream{};
    }
    return result;
  }
  stmt.created = true;

  const std::string table_str(target_table);
  status = AdbcStatementSetOption(&stmt.statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                                  table_str.c_str(), error.Ptr());
  if (status == ADBC_STATUS_OK) {
    status = AdbcStatementSetOption(&stmt.statement, ADBC_INGEST_OPTION_MODE,
                                    IngestModeOptionValue(mode), error.Ptr());
  }
  if (status == ADBC_STATUS_OK && !target_catalog.empty()) {
    const std::string cat_str(target_catalog);
    status = AdbcStatementSetOption(&stmt.statement, ADBC_INGEST_OPTION_TARGET_CATALOG,
                                    cat_str.c_str(), error.Ptr());
  }
  if (status == ADBC_STATUS_OK && !target_schema.empty()) {
    const std::string sch_str(target_schema);
    status = AdbcStatementSetOption(&stmt.statement, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA,
                                    sch_str.c_str(), error.Ptr());
  }
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    if (stream.release != nullptr) {
      stream.release(&stream);
      stream = ArrowArrayStream{};
    }
    return result;
  }

  status = AdbcStatementBindStream(&stmt.statement, &stream, error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    if (stream.release != nullptr) {
      stream.release(&stream);
      stream = ArrowArrayStream{};
    }
    return result;
  }

  std::int64_t affected = -1;
  detail::AdbcRegisterActiveStatement(session, &stmt.statement);
  status = AdbcStatementExecuteQuery(&stmt.statement, nullptr, &affected, error.Ptr());
  detail::AdbcClearActiveStatement(session, &stmt.statement);
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }
  result.rows_affected = affected;
  return result;
}

} // namespace

namespace detail {

void AdbcFinalizeTabularDatasetTypes(AdbcTabularDataset& dataset) {
  dataset.types.resize(dataset.columns.size(), NANOARROW_TYPE_STRING);
  for (std::size_t c = 0; c < dataset.columns.size(); ++c) {
    dataset.types[c] = InferColumnTypeFromSamples(dataset.rows, c);
  }
}

} // namespace detail

BulkIngestResult IngestFileBlocking(AdbcSession& session, const std::filesystem::path& path,
                                    ExportFormat format, std::string_view target_table,
                                    IngestMode mode, std::string_view target_catalog,
                                    std::string_view target_schema, char delimiter,
                                    bool has_header) {
  if (format == ExportFormat::SqlInsert) {
    const SqlScriptImportResult sql_res = ImportSqlScriptFileBlocking(session, path, target_schema);
    BulkIngestResult out;
    out.connection_lost = sql_res.connection_lost;
    out.error = sql_res.error;
    out.rows_affected = sql_res.rows_affected >= 0
                            ? sql_res.rows_affected
                            : static_cast<std::int64_t>(sql_res.statements_executed);
    return out;
  }

  detail::AdbcTabularDataset dataset;
  std::string err;
  bool parsed = false;
  switch (format) {
    case ExportFormat::Csv:
      parsed = ParseCsvFile(path, delimiter == '\0' ? ',' : delimiter, has_header, dataset, err);
      break;
    case ExportFormat::Tsv:
      parsed = ParseCsvFile(path, delimiter == '\0' ? '\t' : delimiter, has_header, dataset, err);
      break;
    case ExportFormat::Json:
      parsed = detail::AdbcParseJsonFile(path, dataset, err);
      break;
    case ExportFormat::Jsonl:
      parsed = detail::AdbcParseJsonlFile(path, dataset, err);
      break;
    case ExportFormat::Markdown:
      parsed = detail::AdbcParseMarkdownTableFile(path, dataset, err);
      break;
    case ExportFormat::Html:
      parsed = detail::AdbcParseHtmlTableFile(path, has_header, dataset, err);
      break;
    case ExportFormat::Xml:
      parsed = detail::AdbcParseXmlRowsFile(path, dataset, err);
      break;
    case ExportFormat::Xlsx:
      parsed = detail::AdbcParseXlsxFile(path, has_header, dataset, err);
      break;
    case ExportFormat::SqlInsert:
      break;
  }
  if (!parsed) {
    return BulkIngestResult{.error = std::move(err)};
  }
  ArrowArrayStream stream{};
  if (!BuildDatasetStream(dataset, stream, err)) {
    return BulkIngestResult{.error = std::move(err)};
  }
  return ExecuteBulkIngestStream(session, stream, target_table, mode, target_catalog,
                                 target_schema);
}

BulkIngestResult IngestPathBlocking(AdbcSession& session, std::string_view path,
                                    ExportFormat format, std::string_view target_table,
                                    IngestMode mode, std::string_view target_catalog,
                                    std::string_view target_schema, char delimiter,
                                    bool has_header) {
  return IngestFileBlocking(session, std::filesystem::path(path), format, target_table, mode,
                            target_catalog, target_schema, delimiter, has_header);
}

BulkIngestResult IngestCsvFileBlocking(AdbcSession& session, const std::filesystem::path& path,
                                       std::string_view target_table, IngestMode mode,
                                       std::string_view target_catalog,
                                       std::string_view target_schema, char delimiter,
                                       bool has_header) {
  return IngestFileBlocking(session, path, ExportFormat::Csv, target_table, mode, target_catalog,
                            target_schema, delimiter, has_header);
}

BulkIngestResult IngestCsvPathBlocking(AdbcSession& session, std::string_view path,
                                       std::string_view target_table, IngestMode mode,
                                       std::string_view target_catalog,
                                       std::string_view target_schema, char delimiter,
                                       bool has_header) {
  return IngestCsvFileBlocking(session, std::filesystem::path(path), target_table, mode,
                               target_catalog, target_schema, delimiter, has_header);
}

BulkIngestResult IngestJsonlFileBlocking(AdbcSession& session, const std::filesystem::path& path,
                                         std::string_view target_table, IngestMode mode,
                                         std::string_view target_catalog,
                                         std::string_view target_schema) {
  return IngestFileBlocking(session, path, ExportFormat::Jsonl, target_table, mode, target_catalog,
                            target_schema);
}

BulkIngestResult IngestJsonlPathBlocking(AdbcSession& session, std::string_view path,
                                         std::string_view target_table, IngestMode mode,
                                         std::string_view target_catalog,
                                         std::string_view target_schema) {
  return IngestJsonlFileBlocking(session, std::filesystem::path(path), target_table, mode,
                                 target_catalog, target_schema);
}

BulkIngestResult CopyQueryToTableBlocking(AdbcSession& source, std::string source_sql,
                                          AdbcSession& target, std::string_view target_table,
                                          IngestMode mode, std::string_view target_catalog,
                                          std::string_view target_schema) {
  BulkIngestResult result;
  if (source.IsClosed() || !source.connection_created) {
    result.connection_lost = true;
    result.error = Tr("Source connection is closed", "源连接已关闭");
    return result;
  }

  detail::AdbcErrorGuard error;
  detail::AdbcStatementGuard src_stmt;
  AdbcStatusCode status = AdbcStatementNew(&source.connection, &src_stmt.statement, error.Ptr());
  if (status != ADBC_STATUS_OK) {
    result.error = error.Take();
    return result;
  }
  src_stmt.created = true;

  status = AdbcStatementSetSqlQuery(&src_stmt.statement, source_sql.c_str(), error.Ptr());
  if (status != ADBC_STATUS_OK) {
    result.error = error.Take();
    return result;
  }

  ArrowArrayStream stream{};
  status = AdbcStatementExecuteQuery(&src_stmt.statement, &stream, nullptr, error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }

  return ExecuteBulkIngestStream(target, stream, target_table, mode, target_catalog,
                                 target_schema);
}

} // namespace dbpp
