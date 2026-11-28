module;

#include <arrow-adbc/adbc.h>
#include <nanoarrow/nanoarrow.h>

module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;

namespace dbpp::detail {

namespace {

constexpr std::int64_t kMaxJsSafeInt = 9007199254740991LL;
constexpr std::int64_t kExcelMaxRows = 1048576;
constexpr std::int64_t kExcelMaxCols = 16384;
constexpr std::size_t kExcelMaxCellBytes = 32767;

std::uint32_t UpdateCrc32(std::uint32_t crc, const std::uint8_t* data, std::size_t len) noexcept {
  crc = ~crc;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
    }
  }
  return ~crc;
}

std::uint32_t ComputeCrc32(std::string_view sv) noexcept {
  return UpdateCrc32(0U, reinterpret_cast<const std::uint8_t*>(sv.data()), sv.size());
}

void AppendLe16(std::string& out, std::uint16_t v) {
  out.push_back(static_cast<char>(v & 0xFFU));
  out.push_back(static_cast<char>((v >> 8) & 0xFFU));
}

void AppendLe32(std::string& out, std::uint32_t v) {
  out.push_back(static_cast<char>(v & 0xFFU));
  out.push_back(static_cast<char>((v >> 8) & 0xFFU));
  out.push_back(static_cast<char>((v >> 16) & 0xFFU));
  out.push_back(static_cast<char>((v >> 24) & 0xFFU));
}

struct ZipEntryMeta {
  std::string name;
  std::uint32_t crc32 = 0;
  std::uint32_t size = 0;
  std::uint32_t local_header_offset = 0;
};

void WriteLocalHeader(std::ostream& out, std::string_view name, std::uint32_t crc32,
                      std::uint32_t size) {
  std::string hdr;
  hdr.reserve(30 + name.size());
  AppendLe32(hdr, 0x04034B50U);
  AppendLe16(hdr, 20);      // version needed
  AppendLe16(hdr, 0x0800U); // UTF-8 filename flag
  AppendLe16(hdr, 0);       // compression: STORE
  AppendLe16(hdr, 0);       // mod time
  AppendLe16(hdr, 0x5421U); // mod date (2022-01-01)
  AppendLe32(hdr, crc32);
  AppendLe32(hdr, size);
  AppendLe32(hdr, size);
  AppendLe16(hdr, static_cast<std::uint16_t>(name.size()));
  AppendLe16(hdr, 0); // extra len
  hdr.append(name);
  out.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
}

void WriteCentralDirectoryAndEocd(std::ostream& out, const std::vector<ZipEntryMeta>& entries,
                                  std::uint32_t cd_offset) {
  std::string cd;
  for (const ZipEntryMeta& entry : entries) {
    AppendLe32(cd, 0x02014B50U);
    AppendLe16(cd, 20);      // version made by
    AppendLe16(cd, 20);      // version needed
    AppendLe16(cd, 0x0800U); // UTF-8 flag
    AppendLe16(cd, 0);       // STORE
    AppendLe16(cd, 0);       // mod time
    AppendLe16(cd, 0x5421U); // mod date
    AppendLe32(cd, entry.crc32);
    AppendLe32(cd, entry.size);
    AppendLe32(cd, entry.size);
    AppendLe16(cd, static_cast<std::uint16_t>(entry.name.size()));
    AppendLe16(cd, 0); // extra
    AppendLe16(cd, 0); // comment
    AppendLe16(cd, 0); // disk start
    AppendLe16(cd, 0); // internal attrs
    AppendLe32(cd, 0); // external attrs
    AppendLe32(cd, entry.local_header_offset);
    cd.append(entry.name);
  }
  const std::uint32_t cd_size = static_cast<std::uint32_t>(cd.size());
  out.write(cd.data(), static_cast<std::streamsize>(cd.size()));

  std::string eocd;
  eocd.reserve(22);
  AppendLe32(eocd, 0x06054B50U);
  AppendLe16(eocd, 0);
  AppendLe16(eocd, 0);
  AppendLe16(eocd, static_cast<std::uint16_t>(entries.size()));
  AppendLe16(eocd, static_cast<std::uint16_t>(entries.size()));
  AppendLe32(eocd, cd_size);
  AppendLe32(eocd, cd_offset);
  AppendLe16(eocd, 0);
  out.write(eocd.data(), static_cast<std::streamsize>(eocd.size()));
}

bool WriteZipStringEntry(std::ostream& out, std::string name, std::string_view content,
                         std::vector<ZipEntryMeta>& entries, std::uint32_t& cursor) {
  if (content.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  ZipEntryMeta meta;
  meta.name = std::move(name);
  meta.crc32 = ComputeCrc32(content);
  meta.size = static_cast<std::uint32_t>(content.size());
  meta.local_header_offset = cursor;
  WriteLocalHeader(out, meta.name, meta.crc32, meta.size);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  cursor += 30U + static_cast<std::uint32_t>(meta.name.size()) + meta.size;
  entries.push_back(std::move(meta));
  return out.good();
}

void AppendXmlTextSafe(std::string& out, std::string_view text, bool escape_quotes,
                       std::size_t max_raw_bytes) {
  std::size_t len = std::min(text.size(), max_raw_bytes);
  // Avoid cutting in the middle of a UTF-8 continuation sequence.
  while (len > 0 && len < text.size() &&
         (static_cast<unsigned char>(text[len]) & 0xC0U) == 0x80U) {
    --len;
  }
  for (std::size_t i = 0; i < len; ++i) {
    const unsigned char uc = static_cast<unsigned char>(text[i]);
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

std::string SanitizeSheetName(std::string_view raw) {
  std::string_view trimmed = AdbcTrimAscii(raw);
  const std::size_t dot = trimmed.rfind('.');
  if (dot != std::string_view::npos && dot + 1 < trimmed.size()) {
    trimmed = trimmed.substr(dot + 1);
  }
  if (trimmed.size() >= 2 &&
      ((trimmed.front() == '"' && trimmed.back() == '"') ||
       (trimmed.front() == '`' && trimmed.back() == '`') ||
       (trimmed.front() == '[' && trimmed.back() == ']'))) {
    trimmed = trimmed.substr(1, trimmed.size() - 2);
  }
  std::string cleaned;
  for (const unsigned char uc : trimmed) {
    if (uc < 0x20U || uc == '\\' || uc == '/' || uc == '?' || uc == '*' ||
        uc == '[' || uc == ']' || uc == ':') {
      continue;
    }
    if (cleaned.size() >= 31) {
      break;
    }
    cleaned.push_back(static_cast<char>(uc));
  }
  while (!cleaned.empty() && (static_cast<unsigned char>(cleaned.back()) & 0xC0U) == 0x80U) {
    cleaned.pop_back();
  }
  while (!cleaned.empty() && cleaned.front() == '\'') cleaned.erase(cleaned.begin());
  while (!cleaned.empty() && cleaned.back() == '\'') cleaned.pop_back();
  return cleaned.empty() ? std::string("Sheet1") : cleaned;
}

void AppendExcelColumnRef(std::string& out, std::int64_t col_zero_based) {
  char rev[8];
  int count = 0;
  std::int64_t n = col_zero_based + 1;
  while (n > 0 && count < 8) {
    --n;
    rev[count++] = static_cast<char>('A' + (n % 26));
    n /= 26;
  }
  for (int i = count - 1; i >= 0; --i) {
    out.push_back(rev[i]);
  }
}

void AppendXlsxCell(std::string& out, std::int64_t col_idx, std::int64_t row_one_based,
                    const ArrowArrayView& view, std::int64_t row, const ArrowSchema* schema,
                    int depth) {
  if (depth > 16 || ArrowArrayViewIsNull(&view, row) != 0 ||
      view.storage_type == NANOARROW_TYPE_NA) {
    return;
  }

  ArrowSchemaView schema_view{};
  ArrowError arrow_error;
  ArrowErrorInit(&arrow_error);
  const bool has_schema =
      schema != nullptr && ArrowSchemaViewInit(&schema_view, schema, &arrow_error) == NANOARROW_OK;

  if (has_schema && schema_view.type == NANOARROW_TYPE_DICTIONARY &&
      view.dictionary != nullptr && schema->dictionary != nullptr) {
    const std::int64_t dict_idx = ArrowArrayViewGetIntUnsafe(&view, row);
    if (dict_idx >= 0 && dict_idx < view.dictionary->length) {
      AppendXlsxCell(out, col_idx, row_one_based, *view.dictionary, dict_idx, schema->dictionary,
                     depth + 1);
    }
    return;
  }

  std::string ref;
  AppendExcelColumnRef(ref, col_idx);
  ref += std::to_string(row_one_based);

  const bool same_storage = !has_schema || schema_view.type == view.storage_type;
  switch (view.storage_type) {
    case NANOARROW_TYPE_BOOL:
      if (same_storage) {
        out += "<c r=\"" + ref + "\" t=\"b\"><v>" +
               (ArrowArrayViewGetIntUnsafe(&view, row) != 0 ? "1" : "0") + "</v></c>";
        return;
      }
      break;
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_INT64:
      if (same_storage) {
        const std::int64_t v = ArrowArrayViewGetIntUnsafe(&view, row);
        if (v >= -kMaxJsSafeInt && v <= kMaxJsSafeInt) {
          out += "<c r=\"" + ref + "\" t=\"n\"><v>" + std::to_string(v) + "</v></c>";
          return;
        }
      }
      break;
    case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_UINT64:
      if (same_storage) {
        const std::uint64_t v = ArrowArrayViewGetUIntUnsafe(&view, row);
        if (v <= static_cast<std::uint64_t>(kMaxJsSafeInt)) {
          out += "<c r=\"" + ref + "\" t=\"n\"><v>" + std::to_string(v) + "</v></c>";
          return;
        }
      }
      break;
    case NANOARROW_TYPE_HALF_FLOAT:
    case NANOARROW_TYPE_FLOAT:
    case NANOARROW_TYPE_DOUBLE:
      if (same_storage) {
        const double d = ArrowArrayViewGetDoubleUnsafe(&view, row);
        if (std::isfinite(d)) {
          out += "<c r=\"" + ref + "\" t=\"n\"><v>" + AdbcFormatDouble(d) + "</v></c>";
          return;
        }
      }
      break;
    default:
      break;
  }

  const CellValue cell = AdbcExtractArrowCell(view, row, schema, depth, true);
  if (cell.is_null) {
    return;
  }
  out += "<c r=\"" + ref + "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">";
  AppendXmlTextSafe(out, cell.text, false, kExcelMaxCellBytes);
  out += "</t></is></c>";
}

struct TempFileGuard {
  std::filesystem::path path;
  ~TempFileGuard() {
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }
};

constexpr std::string_view kContentTypesXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
    "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
    "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n"
    "  <Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>\n"
    "  <Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>\n"
    "  <Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>\n"
    "</Types>\n";

constexpr std::string_view kRootRelsXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
    "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>\n"
    "</Relationships>\n";

constexpr std::string_view kWorkbookRelsXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
    "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>\n"
    "  <Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>\n"
    "</Relationships>\n";

constexpr std::string_view kStylesXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">\n"
    "  <fonts count=\"2\">\n"
    "    <font><sz val=\"11\"/><name val=\"Calibri\"/></font>\n"
    "    <font><b/><sz val=\"11\"/><name val=\"Calibri\"/></font>\n"
    "  </fonts>\n"
    "  <fills count=\"3\">\n"
    "    <fill><patternFill patternType=\"none\"/></fill>\n"
    "    <fill><patternFill patternType=\"gray125\"/></fill>\n"
    "    <fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFF2F2F2\"/><bgColor indexed=\"64\"/></patternFill></fill>\n"
    "  </fills>\n"
    "  <borders count=\"2\">\n"
    "    <border><left/><right/><top/><bottom/><diagonal/></border>\n"
    "    <border><left/><right/><top/><bottom style=\"thin\"><color rgb=\"FFD0D7DE\"/></bottom><diagonal/></border>\n"
    "  </borders>\n"
    "  <cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>\n"
    "  <cellXfs count=\"2\">\n"
    "    <xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>\n"
    "    <xf numFmtId=\"0\" fontId=\"1\" fillId=\"2\" borderId=\"1\" xfId=\"0\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>\n"
    "  </cellXfs>\n"
    "</styleSheet>\n";

} // namespace

ExportStreamResult AdbcExportArrowStreamAsXlsx(AdbcSession& session, ArrowArrayStream& stream,
                                               const ArrowSchema& schema,
                                               const std::vector<std::string>& column_names,
                                               std::ostream& out, std::size_t max_rows,
                                               std::string_view sheet_name) {
  ExportStreamResult result;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  TempFileGuard sheet_tmp{
      std::filesystem::temp_directory_path() / ("dbpp_xlsx_sheet_" + std::to_string(stamp) + ".xml")};

  std::ofstream sheet_out(sheet_tmp.path, std::ios::binary | std::ios::trunc);
  if (!sheet_out.is_open()) {
    result.error = Tr("Failed to create temporary worksheet file", "无法创建临时工作表文件");
    return result;
  }

  std::uint32_t sheet_crc = 0;
  std::uint64_t sheet_size = 0;
  const auto emit_sheet = [&](std::string_view chunk) -> bool {
    if (chunk.empty()) return true;
    sheet_crc = UpdateCrc32(sheet_crc, reinterpret_cast<const std::uint8_t*>(chunk.data()), chunk.size());
    sheet_size += chunk.size();
    sheet_out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    return sheet_out.good();
  };

  const std::int64_t col_count = std::min<std::int64_t>(
      static_cast<std::int64_t>(column_names.size()), kExcelMaxCols);

  std::string hdr =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">\n"
      "  <sheetViews><sheetView workbookViewId=\"0\">"
      "<pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/>"
      "</sheetView></sheetViews>\n"
      "  <sheetData>\n";
  if (col_count > 0) {
    hdr += "    <row r=\"1\">";
    for (std::int64_t c = 0; c < col_count; ++c) {
      std::string ref;
      AppendExcelColumnRef(ref, c);
      ref += '1';
      hdr += "<c r=\"" + ref + "\" s=\"1\" t=\"inlineStr\"><is><t xml:space=\"preserve\">";
      AppendXmlTextSafe(hdr, column_names[static_cast<std::size_t>(c)], false, kExcelMaxCellBytes);
      hdr += "</t></is></c>";
    }
    hdr += "</row>\n";
  }
  if (!emit_sheet(hdr)) {
    result.error = Tr("Failed while writing export stream", "写入导出流失败");
    return result;
  }

  std::int64_t excel_row = 2;
  bool stop = false;
  while (!stop) {
    if (session.cancel_requested.load(std::memory_order_acquire)) {
      result.cancelled = true;
      result.error = Tr("Export cancelled", "导出已取消");
      return result;
    }
    AdbcArrayHolder array;
    if (stream.get_next(&stream, &array.array) != 0) {
      AdbcStatusCode stream_status = ADBC_STATUS_OK;
      const auto cap = AdbcCaptureStreamError(stream, &stream_status);
      result.error = cap.message;
      result.cancelled = (stream_status == ADBC_STATUS_CANCELLED);
      result.connection_lost = IsConnectionLostError(stream_status, cap.message, cap.sqlstate);
      return result;
    }
    if (array.array.release == nullptr) {
      break;
    }

    ArrowArrayView root{};
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&root, &schema, &arrow_error) != NANOARROW_OK) {
      result.error = AdbcDecodeSystemTextToUtf8(arrow_error.message);
      return result;
    }
    if (ArrowArrayViewSetArray(&root, &array.array, &arrow_error) != NANOARROW_OK) {
      ArrowArrayViewReset(&root);
      result.error = AdbcDecodeSystemTextToUtf8(arrow_error.message);
      return result;
    }

    for (std::int64_t row = 0; row < array.array.length; ++row) {
      if ((max_rows > 0 && result.rows_written >= max_rows) || excel_row > kExcelMaxRows) {
        result.truncated = true;
        stop = true;
        break;
      }
      std::string line = "    <row r=\"" + std::to_string(excel_row) + "\">";
      for (std::int64_t c = 0; c < col_count; ++c) {
        AppendXlsxCell(line, c, excel_row, *root.children[c], row, schema.children[c], 0);
      }
      line += "</row>\n";
      if (!emit_sheet(line)) {
        ArrowArrayViewReset(&root);
        result.error = Tr("Failed while writing export stream", "写入导出流失败");
        return result;
      }
      ++excel_row;
      ++result.rows_written;
    }
    ArrowArrayViewReset(&root);
  }

  if (!emit_sheet("  </sheetData>\n</worksheet>\n")) {
    result.error = Tr("Failed while writing export stream", "写入导出流失败");
    return result;
  }
  sheet_out.close();

  if (sheet_size > std::numeric_limits<std::uint32_t>::max()) {
    result.error = Tr("XLSX worksheet exceeds 4 GiB ZIP32 limit", "XLSX 工作表超出 4 GiB 限制");
    return result;
  }

  std::string safe_sheet_escaped;
  AppendXmlTextSafe(safe_sheet_escaped, SanitizeSheetName(sheet_name), true, 64);
  const std::string workbook_xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">\n"
      "  <sheets><sheet name=\"" + safe_sheet_escaped + "\" sheetId=\"1\" r:id=\"rId1\"/></sheets>\n"
      "</workbook>\n";

  std::vector<ZipEntryMeta> entries;
  std::uint32_t cursor = 0;
  if (!WriteZipStringEntry(out, "[Content_Types].xml", kContentTypesXml, entries, cursor) ||
      !WriteZipStringEntry(out, "_rels/.rels", kRootRelsXml, entries, cursor) ||
      !WriteZipStringEntry(out, "xl/workbook.xml", workbook_xml, entries, cursor) ||
      !WriteZipStringEntry(out, "xl/_rels/workbook.xml.rels", kWorkbookRelsXml, entries, cursor) ||
      !WriteZipStringEntry(out, "xl/styles.xml", kStylesXml, entries, cursor)) {
    result.error = Tr("Failed while writing XLSX archive", "写入 XLSX 压缩包失败");
    return result;
  }

  ZipEntryMeta sheet_meta;
  sheet_meta.name = "xl/worksheets/sheet1.xml";
  sheet_meta.crc32 = sheet_crc;
  sheet_meta.size = static_cast<std::uint32_t>(sheet_size);
  sheet_meta.local_header_offset = cursor;
  WriteLocalHeader(out, sheet_meta.name, sheet_meta.crc32, sheet_meta.size);

  std::ifstream sheet_in(sheet_tmp.path, std::ios::binary);
  if (!sheet_in.is_open()) {
    result.error = Tr("Failed to read temporary worksheet file", "无法读取临时工作表文件");
    return result;
  }
  std::array<char, 65536> copy_buf{};
  while (sheet_in.good()) {
    sheet_in.read(copy_buf.data(), static_cast<std::streamsize>(copy_buf.size()));
    const std::streamsize got = sheet_in.gcount();
    if (got > 0) {
      out.write(copy_buf.data(), got);
    }
  }
  cursor += 30U + static_cast<std::uint32_t>(sheet_meta.name.size()) + sheet_meta.size;
  entries.push_back(std::move(sheet_meta));

  WriteCentralDirectoryAndEocd(out, entries, cursor);
  out.flush();
  if (!out.good()) {
    result.error = Tr("Failed while writing XLSX archive", "写入 XLSX 压缩包失败");
  }
  return result;
}

} // namespace dbpp::detail
