module;

#include <arrow-adbc/adbc.h>
#include <nanoarrow/nanoarrow.h>

module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;

namespace dbpp {

namespace {

constexpr std::array<ExportFormatInfo, 9> kExportFormatTable{{
    {ExportFormat::Csv, "csv", "csv", "CSV", "text/csv"},
    {ExportFormat::Tsv, "tsv", "tsv", "TSV", "text/tab-separated-values"},
    {ExportFormat::Json, "json", "json", "JSON", "application/json"},
    {ExportFormat::Jsonl, "jsonl", "jsonl", "JSONL", "application/x-ndjson"},
    {ExportFormat::SqlInsert, "sql", "sql", "SQL", "application/sql"},
    {ExportFormat::Markdown, "markdown", "md", "Markdown", "text/markdown"},
    {ExportFormat::Html, "html", "html", "HTML", "text/html"},
    {ExportFormat::Xml, "xml", "xml", "XML", "application/xml"},
    {ExportFormat::Xlsx, "xlsx", "xlsx", "Excel (XLSX)",
     "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
}};

void WriteDelimitedField(std::ostream& out, std::string_view value, char delimiter) {
  bool needs_quotes = false;
  for (const char ch : value) {
    if (ch == delimiter || ch == '"' || ch == '\n' || ch == '\r') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
    return;
  }
  out.put('"');
  for (const char ch : value) {
    if (ch == '"') {
      out.put('"');
    }
    out.put(ch);
  }
  out.put('"');
}

void AppendHtmlEscaped(std::string& out, std::string_view text) {
  for (const char ch : text) {
    switch (ch) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(ch); break;
    }
  }
}

void AppendXmlEscaped(std::string& out, std::string_view text, bool escape_quotes) {
  for (const unsigned char uc : text) {
    if (uc < 0x20U && uc != '\t' && uc != '\n' && uc != '\r') {
      continue;
    }
    switch (uc) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += (escape_quotes ? "&quot;" : "\""); break;
      case '\'': out += (escape_quotes ? "&apos;" : "'"); break;
      default: out.push_back(static_cast<char>(uc)); break;
    }
  }
}

void AppendMarkdownCell(std::string& out, std::string_view text) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
      out += "<br>";
    } else if (ch == '\n') {
      out += "<br>";
    } else if (ch == '|') {
      out += "\\|";
    } else {
      out.push_back(ch);
    }
  }
}

bool IsValidXmlTagName(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  const unsigned char first = static_cast<unsigned char>(name.front());
  if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
    return false;
  }
  if (name.size() >= 3 && detail::AdbcEqualsIgnoreCase(name.substr(0, 3), "xml")) {
    return false;
  }
  for (const unsigned char uc : name) {
    const bool ok = (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') ||
                    (uc >= '0' && uc <= '9') || uc == '_' || uc == '-' || uc == '.';
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string FormatSqlInsertTarget(Dialect dialect, std::string_view sql_table_name) {
  const std::string_view trimmed = detail::AdbcTrimAscii(sql_table_name);
  if (trimmed.empty()) {
    return QuoteIdentifier(dialect, "query_result");
  }
  if (trimmed.front() == '"' || trimmed.front() == '`' || trimmed.front() == '[') {
    return std::string(trimmed);
  }
  std::string out;
  std::size_t pos = 0;
  while (pos < trimmed.size()) {
    const std::size_t dot = trimmed.find('.', pos);
    const std::string_view part = detail::AdbcTrimAscii(
        dot == std::string_view::npos ? trimmed.substr(pos) : trimmed.substr(pos, dot - pos));
    if (!part.empty()) {
      if (!out.empty()) {
        out.push_back('.');
      }
      out += QuoteIdentifier(dialect, part);
    }
    if (dot == std::string_view::npos) {
      break;
    }
    pos = dot + 1;
  }
  return out.empty() ? QuoteIdentifier(dialect, "query_result") : out;
}

void WriteHtmlDocumentHeader(std::ostream& out, const std::vector<std::string>& column_names) {
  std::string buf =
      "<!DOCTYPE html>\n"
      "<html lang=\"en\">\n"
      "<head>\n"
      "<meta charset=\"UTF-8\">\n"
      "<title>db++ Export</title>\n"
      "<style>\n"
      "body { font-family: system-ui, -apple-system, sans-serif; margin: 20px; color: #1f2328; }\n"
      "table { border-collapse: collapse; width: 100%; font-size: 13px; }\n"
      "th, td { border: 1px solid #d0d7de; padding: 6px 10px; text-align: left; white-space: pre-wrap; }\n"
      "th { background: #f6f8fa; font-weight: 600; }\n"
      "tr:nth-child(even) td { background: #fbfcfd; }\n"
      "td.null { color: #8c959f; font-style: italic; }\n"
      "</style>\n"
      "</head>\n"
      "<body>\n"
      "<table>\n"
      "<thead><tr>";
  for (const std::string& col : column_names) {
    buf += "<th>";
    AppendHtmlEscaped(buf, col);
    buf += "</th>";
  }
  buf += "</tr></thead>\n<tbody>\n";
  out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
}

void WriteDelimitedHeader(std::ostream& out, const std::vector<std::string>& column_names,
                          char delimiter) {
  if (column_names.empty()) {
    return;
  }
  for (std::size_t c = 0; c < column_names.size(); ++c) {
    if (c > 0) {
      out.put(delimiter);
    }
    WriteDelimitedField(out, column_names[c], delimiter);
  }
  out.put('\n');
}

void WriteMarkdownHeader(std::ostream& out, const std::vector<std::string>& column_names) {
  if (column_names.empty()) {
    return;
  }
  std::string line = "|";
  std::string sep = "|";
  for (const std::string& col : column_names) {
    line += ' ';
    AppendMarkdownCell(line, col);
    line += " |";
    sep += " --- |";
  }
  line.push_back('\n');
  sep.push_back('\n');
  out.write(line.data(), static_cast<std::streamsize>(line.size()));
  out.write(sep.data(), static_cast<std::streamsize>(sep.size()));
}

} // namespace

ExportFormatInfo GetExportFormatInfo(ExportFormat format) {
  for (const ExportFormatInfo& info : kExportFormatTable) {
    if (info.format == format) {
      return info;
    }
  }
  return kExportFormatTable[0];
}

std::optional<ExportFormat> ParseExportFormatName(std::string_view text) {
  const std::string lower = detail::AdbcLowerAscii(detail::AdbcTrimAscii(text));
  if (lower == "csv") return ExportFormat::Csv;
  if (lower == "tsv") return ExportFormat::Tsv;
  if (lower == "json") return ExportFormat::Json;
  if (lower == "jsonl" || lower == "ndjson") return ExportFormat::Jsonl;
  if (lower == "sql" || lower == "sql-insert" || lower == "insert") return ExportFormat::SqlInsert;
  if (lower == "markdown" || lower == "md") return ExportFormat::Markdown;
  if (lower == "html" || lower == "htm") return ExportFormat::Html;
  if (lower == "xml") return ExportFormat::Xml;
  if (lower == "xlsx" || lower == "excel") return ExportFormat::Xlsx;
  return std::nullopt;
}

std::optional<ExportFormat> InferExportFormatFromExtension(std::string_view ext) {
  std::string_view clean = detail::AdbcTrimAscii(ext);
  if (!clean.empty() && clean.front() == '.') {
    clean.remove_prefix(1);
  }
  return ParseExportFormatName(clean);
}

ExportStreamResult ExportQueryStreamBlocking(AdbcSession& session, std::string sql,
                                             ExportFormat format, std::ostream& out,
                                             std::size_t max_rows,
                                             std::string_view sql_table_name) {
  ExportStreamResult result;
  if (session.IsClosed()) {
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (!session.connection_created) {
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }

  session.cancel_requested.store(false, std::memory_order_release);
  detail::AdbcErrorGuard error;
  detail::AdbcStatementGuard statement_guard;
  AdbcStatusCode status =
      AdbcStatementNew(&session.connection, &statement_guard.statement, error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }
  statement_guard.created = true;

  status = AdbcStatementSetSqlQuery(&statement_guard.statement, sql.c_str(), error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }

  detail::AdbcStreamHolder stream;
  detail::AdbcRegisterActiveStatement(session, &statement_guard.statement);
  status = AdbcStatementExecuteQuery(&statement_guard.statement, &stream.stream, nullptr,
                                     error.Ptr());
  detail::AdbcClearActiveStatement(session, &statement_guard.statement);
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.cancelled = (status == ADBC_STATUS_CANCELLED) ||
                       session.cancel_requested.load(std::memory_order_acquire);
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }

  detail::AdbcSchemaHolder schema;
  if (stream.stream.get_schema(&stream.stream, &schema.schema) != 0) {
    AdbcStatusCode stream_status = ADBC_STATUS_OK;
    const auto cap = detail::AdbcCaptureStreamError(stream.stream, &stream_status);
    result.error = cap.message;
    result.cancelled = (stream_status == ADBC_STATUS_CANCELLED);
    result.connection_lost = IsConnectionLostError(stream_status, cap.message, cap.sqlstate);
    return result;
  }

  const std::int64_t field_count = schema.schema.n_children;
  std::vector<std::string> column_names;
  column_names.reserve(static_cast<std::size_t>(field_count));
  for (std::int64_t c = 0; c < field_count; ++c) {
    const ArrowSchema* field = schema.schema.children[c];
    column_names.push_back(field != nullptr && field->name != nullptr
                               ? std::string(field->name)
                               : ("column_" + std::to_string(c + 1)));
  }

  if (format == ExportFormat::Xlsx) {
    return detail::AdbcExportArrowStreamAsXlsx(session, stream.stream, schema.schema, column_names,
                                               out, max_rows, sql_table_name);
  }

  std::string sql_insert_prefix;
  std::vector<bool> xml_valid_tags;
  std::vector<std::string> xml_escaped_col_names;

  if (format == ExportFormat::Csv) {
    WriteDelimitedHeader(out, column_names, ',');
  } else if (format == ExportFormat::Tsv) {
    WriteDelimitedHeader(out, column_names, '\t');
  } else if (format == ExportFormat::Markdown) {
    WriteMarkdownHeader(out, column_names);
  } else if (format == ExportFormat::Html) {
    WriteHtmlDocumentHeader(out, column_names);
  } else if (format == ExportFormat::Xml) {
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rows>\n";
    xml_valid_tags.reserve(column_names.size());
    xml_escaped_col_names.reserve(column_names.size());
    for (const std::string& col : column_names) {
      xml_valid_tags.push_back(IsValidXmlTagName(col));
      std::string esc;
      AppendXmlEscaped(esc, col, true);
      xml_escaped_col_names.push_back(std::move(esc));
    }
  } else if (format == ExportFormat::SqlInsert && field_count > 0) {
    sql_insert_prefix = "INSERT INTO " + FormatSqlInsertTarget(session.dialect, sql_table_name) + " (";
    for (std::int64_t c = 0; c < field_count; ++c) {
      if (c > 0) {
        sql_insert_prefix += ", ";
      }
      sql_insert_prefix += QuoteIdentifier(session.dialect, column_names[static_cast<std::size_t>(c)]);
    }
    sql_insert_prefix += ") VALUES (";
  }

  bool stop = false;
  while (!stop) {
    if (session.cancel_requested.load(std::memory_order_acquire)) {
      result.cancelled = true;
      result.error = Tr("Export cancelled", "导出已取消");
      break;
    }
    detail::AdbcArrayHolder array;
    if (stream.stream.get_next(&stream.stream, &array.array) != 0) {
      AdbcStatusCode stream_status = ADBC_STATUS_OK;
      const auto cap = detail::AdbcCaptureStreamError(stream.stream, &stream_status);
      result.error = cap.message;
      result.cancelled = (stream_status == ADBC_STATUS_CANCELLED);
      result.connection_lost = IsConnectionLostError(stream_status, cap.message, cap.sqlstate);
      break;
    }
    if (array.array.release == nullptr) {
      break;
    }

    ArrowArrayView root{};
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&root, &schema.schema, &arrow_error) != NANOARROW_OK) {
      result.error = detail::AdbcDecodeSystemTextToUtf8(arrow_error.message);
      break;
    }
    if (ArrowArrayViewSetArray(&root, &array.array, &arrow_error) != NANOARROW_OK) {
      ArrowArrayViewReset(&root);
      result.error = detail::AdbcDecodeSystemTextToUtf8(arrow_error.message);
      break;
    }

    for (std::int64_t row = 0; row < array.array.length; ++row) {
      if (max_rows > 0 && result.rows_written >= max_rows) {
        result.truncated = true;
        stop = true;
        break;
      }

      if (format == ExportFormat::Csv || format == ExportFormat::Tsv) {
        const char delim = (format == ExportFormat::Csv) ? ',' : '\t';
        for (std::int64_t c = 0; c < field_count; ++c) {
          if (c > 0) {
            out.put(delim);
          }
          const CellValue cell = detail::AdbcExtractArrowCell(
              *root.children[c], row, schema.schema.children[c], 0, true);
          if (!cell.is_null) {
            WriteDelimitedField(out, cell.text, delim);
          }
        }
        out.put('\n');
      } else if (format == ExportFormat::Json || format == ExportFormat::Jsonl) {
        std::string line;
        if (format == ExportFormat::Json) {
          line += (result.rows_written == 0) ? "[\n  {" : ",\n  {";
        } else {
          line.push_back('{');
        }
        for (std::int64_t c = 0; c < field_count; ++c) {
          if (c > 0) {
            line += ", ";
          }
          detail::AdbcAppendJsonEscapedString(line, column_names[static_cast<std::size_t>(c)]);
          line += ": ";
          detail::AdbcAppendJsonValue(line, *root.children[c], row, schema.schema.children[c], 0);
        }
        line += (format == ExportFormat::Json) ? "}" : "}\n";
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
      } else if (format == ExportFormat::SqlInsert) {
        if (field_count > 0) {
          out << sql_insert_prefix;
          for (std::int64_t c = 0; c < field_count; ++c) {
            if (c > 0) {
              out << ", ";
            }
            out << detail::AdbcFormatArrowSqlLiteral(
                *root.children[c], row, schema.schema.children[c], session.dialect, {});
          }
          out << ");\n";
        }
      } else if (format == ExportFormat::Markdown) {
        if (field_count > 0) {
          std::string line = "|";
          for (std::int64_t c = 0; c < field_count; ++c) {
            line.push_back(' ');
            const CellValue cell = detail::AdbcExtractArrowCell(
                *root.children[c], row, schema.schema.children[c], 0, true);
            if (!cell.is_null) {
              AppendMarkdownCell(line, cell.text);
            }
            line += " |";
          }
          line.push_back('\n');
          out.write(line.data(), static_cast<std::streamsize>(line.size()));
        }
      } else if (format == ExportFormat::Html) {
        std::string line = "<tr>";
        for (std::int64_t c = 0; c < field_count; ++c) {
          const CellValue cell = detail::AdbcExtractArrowCell(
              *root.children[c], row, schema.schema.children[c], 0, true);
          if (cell.is_null) {
            line += "<td class=\"null\">NULL</td>";
          } else {
            line += "<td>";
            AppendHtmlEscaped(line, cell.text);
            line += "</td>";
          }
        }
        line += "</tr>\n";
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
      } else if (format == ExportFormat::Xml) {
        std::string line = "  <row>\n";
        for (std::int64_t c = 0; c < field_count; ++c) {
          const std::size_t idx = static_cast<std::size_t>(c);
          const CellValue cell = detail::AdbcExtractArrowCell(
              *root.children[c], row, schema.schema.children[c], 0, true);
          line += "    ";
          if (xml_valid_tags[idx]) {
            const std::string& tag = column_names[idx];
            if (cell.is_null) {
              line += "<" + tag + " null=\"true\"/>\n";
            } else {
              line += "<" + tag + ">";
              AppendXmlEscaped(line, cell.text, false);
              line += "</" + tag + ">\n";
            }
          } else {
            const std::string& attr_name = xml_escaped_col_names[idx];
            if (cell.is_null) {
              line += "<column name=\"" + attr_name + "\" null=\"true\"/>\n";
            } else {
              line += "<column name=\"" + attr_name + "\">";
              AppendXmlEscaped(line, cell.text, false);
              line += "</column>\n";
            }
          }
        }
        line += "  </row>\n";
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
      }

      if (!out.good()) {
        result.error = Tr("Failed while writing export stream", "写入导出流失败");
        stop = true;
        break;
      }
      ++result.rows_written;
    }
    ArrowArrayViewReset(&root);
  }

  if (result.error.empty()) {
    if (format == ExportFormat::Json) {
      out << (result.rows_written == 0 ? "[]\n" : "\n]\n");
    } else if (format == ExportFormat::Html) {
      out << "</tbody>\n</table>\n</body>\n</html>\n";
    } else if (format == ExportFormat::Xml) {
      out << "</rows>\n";
    }
    if (!out.good()) {
      result.error = Tr("Failed while writing export stream", "写入导出流失败");
    }
  }
  out.flush();
  return result;
}

ExportStreamResult ExportQueryToFileBlocking(AdbcSession& session, std::string sql,
                                             ExportFormat format,
                                             const std::filesystem::path& path,
                                             std::size_t max_rows,
                                             std::string_view sql_table_name) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    ExportStreamResult res;
    res.error = TrFmt("Failed to open output file: {0}", "无法打开导出文件: {0}", {path.string()});
    return res;
  }
  return ExportQueryStreamBlocking(session, std::move(sql), format, file, max_rows, sql_table_name);
}

std::string MakeTempExportPath(ExportFormat format) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const ExportFormatInfo info = GetExportFormatInfo(format);
  return (std::filesystem::temp_directory_path() /
          ("dbpp_export_" + std::to_string(stamp) + "." + std::string(info.extension)))
      .string();
}

void RemoveFileQuietly(std::string_view path) {
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(path), ec);
}

ExportStreamResult ExportQueryToFilePathBlocking(AdbcSession& session, std::string sql,
                                                 ExportFormat format, std::string_view path,
                                                 std::size_t max_rows,
                                                 std::string_view sql_table_name) {
  return ExportQueryToFileBlocking(session, std::move(sql), format, std::filesystem::path(path),
                                   max_rows, sql_table_name);
}

} // namespace dbpp
