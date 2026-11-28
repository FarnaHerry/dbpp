module;

#include <nanoarrow/nanoarrow.h>

module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.decoder;

namespace dbpp::detail {

namespace {

struct ZipCentralEntry {
  std::uint16_t method = 0;
  std::uint32_t comp_size = 0;
  std::uint32_t uncomp_size = 0;
  std::uint32_t local_offset = 0;
};

std::uint16_t ReadZipLe16(const std::string& buf, std::size_t pos) noexcept {
  return static_cast<std::uint16_t>(static_cast<unsigned char>(buf[pos])) |
         (static_cast<std::uint16_t>(static_cast<unsigned char>(buf[pos + 1])) << 8);
}

std::uint32_t ReadZipLe32(const std::string& buf, std::size_t pos) noexcept {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(buf[pos])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[pos + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[pos + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[pos + 3])) << 24);
}

bool ParseZipIndex(const std::string& archive,
                   std::unordered_map<std::string, ZipCentralEntry>& out,
                   std::string& error_out) {
  if (archive.size() < 22) {
    error_out = Tr("Invalid XLSX/ZIP file (too small)", "无效的 XLSX/ZIP 文件（体积过小）");
    return false;
  }
  const std::size_t min_pos = archive.size() > 65557 ? archive.size() - 65557 : 0;
  std::size_t eocd_pos = std::string::npos;
  for (std::size_t p = archive.size() - 22;; --p) {
    if (ReadZipLe32(archive, p) == 0x06054B50U) {
      eocd_pos = p;
      break;
    }
    if (p == min_pos) break;
  }
  if (eocd_pos == std::string::npos) {
    error_out = Tr("Invalid XLSX archive: ZIP directory not found", "无效的 XLSX 归档：未找到 ZIP 目录");
    return false;
  }

  const std::uint16_t entry_count = ReadZipLe16(archive, eocd_pos + 10);
  const std::uint32_t cd_size = ReadZipLe32(archive, eocd_pos + 12);
  const std::uint32_t cd_offset = ReadZipLe32(archive, eocd_pos + 16);
  if (static_cast<std::size_t>(cd_offset) + cd_size > archive.size()) {
    error_out = Tr("Corrupted XLSX ZIP central directory", "XLSX ZIP 中央目录损坏");
    return false;
  }

  std::size_t cur = cd_offset;
  for (std::uint16_t i = 0; i < entry_count; ++i) {
    if (cur + 46 > archive.size() || ReadZipLe32(archive, cur) != 0x02014B50U) {
      error_out = Tr("Corrupted XLSX ZIP central directory entry", "XLSX ZIP 目录项损坏");
      return false;
    }
    ZipCentralEntry entry;
    entry.method = ReadZipLe16(archive, cur + 10);
    entry.comp_size = ReadZipLe32(archive, cur + 20);
    entry.uncomp_size = ReadZipLe32(archive, cur + 24);
    const std::uint16_t name_len = ReadZipLe16(archive, cur + 28);
    const std::uint16_t extra_len = ReadZipLe16(archive, cur + 30);
    const std::uint16_t comment_len = ReadZipLe16(archive, cur + 32);
    entry.local_offset = ReadZipLe32(archive, cur + 42);
    cur += 46;
    if (cur + name_len + extra_len + comment_len > archive.size()) {
      error_out = Tr("Truncated XLSX ZIP central directory entry", "XLSX ZIP 目录项截断");
      return false;
    }
    std::string name = archive.substr(cur, name_len);
    cur += name_len + extra_len + comment_len;
    out.emplace(std::move(name), entry);
  }
  return true;
}

bool ExtractZipEntry(const std::string& archive, const ZipCentralEntry& entry, std::string& out,
                     std::string& error_out) {
  if (static_cast<std::size_t>(entry.local_offset) + 30 > archive.size() ||
      ReadZipLe32(archive, entry.local_offset) != 0x04034B50U) {
    error_out = Tr("Corrupted XLSX local file header", "XLSX 本地文件头损坏");
    return false;
  }
  const std::uint16_t name_len = ReadZipLe16(archive, entry.local_offset + 26);
  const std::uint16_t extra_len = ReadZipLe16(archive, entry.local_offset + 28);
  const std::size_t data_pos = static_cast<std::size_t>(entry.local_offset) + 30 + name_len + extra_len;
  if (data_pos + entry.comp_size > archive.size()) {
    error_out = Tr("Truncated XLSX ZIP entry payload", "XLSX ZIP 条目数据不完整");
    return false;
  }
  const std::string_view payload(archive.data() + data_pos, entry.comp_size);
  if (entry.method == 0) {
    out.assign(payload);
    return true;
  }
  if (entry.method == 8) {
    return InflateRawDeflateBytes(payload, out, error_out);
  }
  error_out = Tr("Unsupported XLSX ZIP compression method", "不支持的 XLSX ZIP 压缩方法");
  return false;
}

void AppendUtf8Cp(std::uint32_t cp, std::string& out) {
  if (cp <= 0x7FU) out.push_back(static_cast<char>(cp));
  else if (cp <= 0x7FFU) {
    out.push_back(static_cast<char>(0xC0U | (cp >> 6)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  } else if (cp <= 0xFFFFU) {
    out.push_back(static_cast<char>(0xE0U | (cp >> 12)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  } else if (cp <= 0x10FFFFU) {
    out.push_back(static_cast<char>(0xF0U | (cp >> 18)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 12) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  }
}

std::string UnescapeXml(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out.push_back(text[i]);
      continue;
    }
    const std::size_t semi = text.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 16) {
      out.push_back('&');
      continue;
    }
    const std::string_view ent = text.substr(i + 1, semi - i - 1);
    if (ent == "amp") out.push_back('&');
    else if (ent == "lt") out.push_back('<');
    else if (ent == "gt") out.push_back('>');
    else if (ent == "quot") out.push_back('"');
    else if (ent == "apos" || ent == "#39") out.push_back('\'');
    else if (ent.size() >= 2 && ent[0] == '#') {
      const bool hex = (ent[1] == 'x' || ent[1] == 'X');
      const std::string_view digits = hex ? ent.substr(2) : ent.substr(1);
      std::uint32_t cp = 0;
      bool ok = !digits.empty();
      for (const char ch : digits) {
        int d = -1;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (hex && ch >= 'a' && ch <= 'f') d = 10 + (ch - 'a');
        else if (hex && ch >= 'A' && ch <= 'F') d = 10 + (ch - 'A');
        if (d < 0) { ok = false; break; }
        cp = cp * (hex ? 16U : 10U) + static_cast<std::uint32_t>(d);
      }
      if (ok && cp > 0) AppendUtf8Cp(cp, out);
      else out.append(text.substr(i, semi - i + 1));
    } else {
      out.append(text.substr(i, semi - i + 1));
    }
    i = semi;
  }
  return out;
}

std::optional<std::string> ReadXmlAttr(std::string_view attrs, std::string_view name) {
  std::size_t pos = 0;
  while ((pos = attrs.find(name, pos)) != std::string_view::npos) {
    if (pos > 0 && std::isalnum(static_cast<unsigned char>(attrs[pos - 1])) != 0) {
      ++pos;
      continue;
    }
    std::size_t p = pos + name.size();
    while (p < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[p])) != 0) ++p;
    if (p >= attrs.size() || attrs[p] != '=') { pos = p; continue; }
    ++p;
    while (p < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[p])) != 0) ++p;
    if (p < attrs.size() && (attrs[p] == '"' || attrs[p] == '\'')) {
      const char q = attrs[p++];
      const std::size_t end = attrs.find(q, p);
      if (end != std::string_view::npos) return UnescapeXml(attrs.substr(p, end - p));
    }
    pos = p;
  }
  return std::nullopt;
}

std::string ExtractConcatenatedT(std::string_view xml) {
  std::string out;
  std::size_t pos = 0;
  while (pos < xml.size()) {
    const std::size_t start = xml.find("<t", pos);
    if (start == std::string_view::npos) break;
    if (start + 2 < xml.size() && xml[start + 2] != '>' &&
        std::isspace(static_cast<unsigned char>(xml[start + 2])) == 0) {
      pos = start + 2;
      continue;
    }
    const std::size_t gt = xml.find('>', start + 2);
    if (gt == std::string_view::npos) break;
    if (gt > start && xml[gt - 1] == '/') {
      pos = gt + 1;
      continue;
    }
    const std::size_t end = xml.find("</t>", gt + 1);
    if (end == std::string_view::npos) break;
    out.append(UnescapeXml(xml.substr(gt + 1, end - (gt + 1))));
    pos = end + 4;
  }
  return out;
}

std::vector<std::string> ParseSharedStrings(std::string_view xml) {
  std::vector<std::string> items;
  std::size_t pos = 0;
  while (pos < xml.size()) {
    const std::size_t si_start = xml.find("<si", pos);
    if (si_start == std::string_view::npos) break;
    const std::size_t gt = xml.find('>', si_start + 3);
    if (gt == std::string_view::npos) break;
    if (gt > si_start && xml[gt - 1] == '/') {
      items.emplace_back();
      pos = gt + 1;
      continue;
    }
    const std::size_t si_end = xml.find("</si>", gt + 1);
    if (si_end == std::string_view::npos) break;
    items.push_back(ExtractConcatenatedT(xml.substr(gt + 1, si_end - (gt + 1))));
    pos = si_end + 5;
  }
  return items;
}

std::string ResolveFirstWorksheetPath(
    const std::string& archive,
    const std::unordered_map<std::string, ZipCentralEntry>& entries) {
  const auto wb_it = entries.find("xl/workbook.xml");
  const auto rels_it = entries.find("xl/_rels/workbook.xml.rels");
  if (wb_it != entries.end() && rels_it != entries.end()) {
    std::string wb_xml, rels_xml, dummy_err;
    if (ExtractZipEntry(archive, wb_it->second, wb_xml, dummy_err) &&
        ExtractZipEntry(archive, rels_it->second, rels_xml, dummy_err)) {
      const std::size_t sh = wb_xml.find("<sheet");
      const std::size_t sh_gt = (sh != std::string::npos) ? wb_xml.find('>', sh) : std::string::npos;
      if (sh != std::string::npos && sh_gt != std::string::npos) {
        const auto rid = ReadXmlAttr(std::string_view(wb_xml).substr(sh, sh_gt - sh), "r:id");
        if (rid.has_value()) {
          std::size_t pos = 0;
          while ((pos = rels_xml.find("<Relationship", pos)) != std::string::npos) {
            const std::size_t gt = rels_xml.find('>', pos);
            if (gt == std::string::npos) break;
            const std::string_view attrs = std::string_view(rels_xml).substr(pos, gt - pos);
            if (ReadXmlAttr(attrs, "Id") == rid) {
              if (auto target = ReadXmlAttr(attrs, "Target")) {
                if (!target->empty() && target->front() == '/') {
                  target->erase(target->begin());
                  return *target;
                }
                if (target->starts_with("xl/")) return *target;
                return "xl/" + *target;
              }
            }
            pos = gt + 1;
          }
        }
      }
    }
  }
  return "xl/worksheets/sheet1.xml";
}

std::optional<std::size_t> ParseCellColumnIndex(std::string_view ref) {
  std::size_t col = 0;
  bool any = false;
  for (const char ch : ref) {
    if (ch >= 'A' && ch <= 'Z') {
      col = col * 26 + static_cast<std::size_t>(ch - 'A' + 1);
      any = true;
    } else if (ch >= 'a' && ch <= 'z') {
      col = col * 26 + static_cast<std::size_t>(ch - 'a' + 1);
      any = true;
    } else {
      break;
    }
  }
  return any ? std::optional<std::size_t>(col - 1) : std::nullopt;
}

std::string ExtractVTag(std::string_view body) {
  const std::size_t v_start = body.find("<v");
  if (v_start == std::string_view::npos) return {};
  const std::size_t gt = body.find('>', v_start + 2);
  if (gt == std::string_view::npos || body[gt - 1] == '/') return {};
  const std::size_t v_end = body.find("</v>", gt + 1);
  if (v_end == std::string_view::npos) return {};
  return UnescapeXml(body.substr(gt + 1, v_end - (gt + 1)));
}

} // namespace

bool AdbcParseXlsxFile(const std::filesystem::path& path, bool has_header, AdbcTabularDataset& out,
                       std::string& error_out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    error_out = TrFmt("Failed to open XLSX file: {0}", "无法打开 XLSX 文件: {0}", {path.string()});
    return false;
  }
  const std::string archive((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  std::unordered_map<std::string, ZipCentralEntry> entries;
  if (!ParseZipIndex(archive, entries, error_out)) return false;

  std::vector<std::string> shared_strings;
  if (const auto ss_it = entries.find("xl/sharedStrings.xml"); ss_it != entries.end()) {
    std::string ss_xml;
    if (!ExtractZipEntry(archive, ss_it->second, ss_xml, error_out)) return false;
    shared_strings = ParseSharedStrings(ss_xml);
  }

  const std::string sheet_path = ResolveFirstWorksheetPath(archive, entries);
  const auto sh_it = entries.find(sheet_path);
  if (sh_it == entries.end()) {
    error_out = TrFmt("Worksheet not found in XLSX: {0}", "XLSX 中未找到工作表: {0}", {sheet_path});
    return false;
  }
  std::string sheet_xml;
  if (!ExtractZipEntry(archive, sh_it->second, sheet_xml, error_out)) return false;

  std::vector<std::vector<std::optional<std::string>>> parsed_rows;
  std::size_t pos = 0;
  while (pos < sheet_xml.size()) {
    const std::size_t row_start = sheet_xml.find("<row", pos);
    if (row_start == std::string_view::npos) break;
    if (row_start + 4 < sheet_xml.size() && sheet_xml[row_start + 4] != '>' &&
        std::isspace(static_cast<unsigned char>(sheet_xml[row_start + 4])) == 0) {
      pos = row_start + 4;
      continue;
    }
    const std::size_t row_gt = sheet_xml.find('>', row_start + 4);
    if (row_gt == std::string_view::npos) break;
    if (sheet_xml[row_gt - 1] == '/') {
      pos = row_gt + 1;
      continue;
    }
    const std::size_t row_end = sheet_xml.find("</row>", row_gt + 1);
    if (row_end == std::string_view::npos) break;
    const std::string_view row_body =
        std::string_view(sheet_xml).substr(row_gt + 1, row_end - (row_gt + 1));

    std::vector<std::optional<std::string>> row_cells;
    std::size_t cpos = 0;
    std::size_t seq_col = 0;
    bool any_non_null = false;
    while (cpos < row_body.size()) {
      const std::size_t c_start = row_body.find("<c", cpos);
      if (c_start == std::string_view::npos) break;
      if (c_start + 2 < row_body.size() && row_body[c_start + 2] != '>' &&
          std::isspace(static_cast<unsigned char>(row_body[c_start + 2])) == 0) {
        cpos = c_start + 2;
        continue;
      }
      const std::size_t c_gt = row_body.find('>', c_start + 2);
      if (c_gt == std::string_view::npos) break;
      const std::string_view c_attrs = row_body.substr(c_start + 2, c_gt - (c_start + 2));
      const bool self_close = row_body[c_gt - 1] == '/';
      std::string_view c_body;
      if (self_close) {
        cpos = c_gt + 1;
      } else {
        const std::size_t c_end = row_body.find("</c>", c_gt + 1);
        if (c_end == std::string_view::npos) break;
        c_body = row_body.substr(c_gt + 1, c_end - (c_gt + 1));
        cpos = c_end + 4;
      }

      const std::size_t col_idx =
          ReadXmlAttr(c_attrs, "r")
              .and_then([](const std::string& ref) { return ParseCellColumnIndex(ref); })
              .value_or(seq_col);
      seq_col = col_idx + 1;
      if (self_close) continue;

      const std::string type = ReadXmlAttr(c_attrs, "t").value_or("n");
      std::optional<std::string> cell_val;
      if (type == "inlineStr") {
        cell_val = ExtractConcatenatedT(c_body);
      } else if (type == "s") {
        const std::string idx_str = ExtractVTag(c_body);
        if (const auto idx = AdbcParseInt64(idx_str)) {
          if (*idx >= 0 && static_cast<std::size_t>(*idx) < shared_strings.size()) {
            cell_val = shared_strings[static_cast<std::size_t>(*idx)];
          }
        }
      } else if (type == "b") {
        const std::string v = ExtractVTag(c_body);
        if (!v.empty()) cell_val = (v == "1" || AdbcEqualsIgnoreCase(v, "true")) ? "true" : "false";
      } else if (type == "str" || type == "e") {
        cell_val = ExtractVTag(c_body);
      } else {
        const std::string v = ExtractVTag(c_body);
        if (!v.empty()) cell_val = v;
      }
      if (cell_val.has_value()) {
        if (row_cells.size() <= col_idx) row_cells.resize(col_idx + 1);
        row_cells[col_idx] = std::move(cell_val);
        any_non_null = true;
      }
    }
    if (any_non_null) {
      parsed_rows.push_back(std::move(row_cells));
    }
    pos = row_end + 6;
  }

  if (parsed_rows.empty()) {
    error_out = Tr("XLSX worksheet contains no rows", "XLSX 工作表没有数据行");
    return false;
  }
  std::size_t num_cols = 0;
  for (const auto& r : parsed_rows) num_cols = std::max(num_cols, r.size());
  if (num_cols == 0) {
    error_out = Tr("XLSX worksheet has no columns", "XLSX 工作表没有列");
    return false;
  }
  std::size_t data_start = 0;
  if (has_header) {
    for (std::size_t c = 0; c < num_cols; ++c) {
      const auto& hdr = c < parsed_rows[0].size() ? parsed_rows[0][c] : std::optional<std::string>{};
      out.columns.push_back((hdr.has_value() && !hdr->empty()) ? *hdr
                                                               : ("col_" + std::to_string(c + 1)));
    }
    data_start = 1;
  } else {
    for (std::size_t c = 0; c < num_cols; ++c) out.columns.push_back("col_" + std::to_string(c + 1));
  }
  for (std::size_t r = data_start; r < parsed_rows.size(); ++r) {
    auto row = std::move(parsed_rows[r]);
    row.resize(num_cols);
    out.rows.push_back(std::move(row));
  }
  if (out.rows.empty()) {
    error_out = Tr("XLSX worksheet contains no data rows", "XLSX 工作表没有数据行");
    return false;
  }
  AdbcFinalizeTabularDatasetTypes(out);
  return true;
}

} // namespace dbpp::detail
