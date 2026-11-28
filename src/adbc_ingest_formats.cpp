module;

#include <nanoarrow/nanoarrow.h>

module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;

namespace dbpp::detail {

namespace {

void StripUtf8Bom(std::string& content) {
  if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEFU &&
      static_cast<unsigned char>(content[1]) == 0xBBU &&
      static_cast<unsigned char>(content[2]) == 0xBFU) {
    content.erase(0, 3);
  }
}

bool ReadFileToUtf8String(const std::filesystem::path& path, std::string_view label,
                          std::string& out, std::string& error_out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    error_out = TrFmt("Failed to open {0} file: {1}", "无法打开 {0} 文件: {1}",
                      {std::string(label), path.string()});
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  StripUtf8Bom(out);
  return true;
}

void AppendCodepointUtf8(std::uint32_t cp, std::string& out) {
  if (cp <= 0x7FU) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFU) {
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

bool ParseHexUint(std::string_view sv, int base, std::uint32_t& out) noexcept {
  if (sv.empty()) return false;
  std::uint32_t val = 0;
  for (const char ch : sv) {
    int digit = -1;
    if (ch >= '0' && ch <= '9') digit = ch - '0';
    else if (base == 16 && ch >= 'a' && ch <= 'f') digit = 10 + (ch - 'a');
    else if (base == 16 && ch >= 'A' && ch <= 'F') digit = 10 + (ch - 'A');
    if (digit < 0 || digit >= base) return false;
    val = val * static_cast<std::uint32_t>(base) + static_cast<std::uint32_t>(digit);
  }
  out = val;
  return true;
}

using ObjectFields = std::vector<std::pair<std::string, std::optional<std::string>>>;

void SkipJsonWs(std::string_view text, std::size_t& pos) noexcept {
  while (pos < text.size() &&
         (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n')) {
    ++pos;
  }
}

bool ParseJsonStringAt(std::string_view text, std::size_t& pos, std::string& out) {
  if (pos >= text.size() || text[pos] != '"') return false;
  ++pos;
  out.clear();
  while (pos < text.size()) {
    const char c = text[pos++];
    if (c == '"') return true;
    if (c == '\\') {
      if (pos >= text.size()) return false;
      const char e = text[pos++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t cp = 0;
          if (pos + 4 > text.size() || !ParseHexUint(text.substr(pos, 4), 16, cp)) {
            return false;
          }
          pos += 4;
          if (cp >= 0xD800U && cp <= 0xDBFFU && pos + 6 <= text.size() &&
              text[pos] == '\\' && text[pos + 1] == 'u') {
            std::uint32_t lo = 0;
            if (ParseHexUint(text.substr(pos + 2, 4), 16, lo) && lo >= 0xDC00U && lo <= 0xDFFFU) {
              pos += 6;
              cp = 0x10000U + ((cp - 0xD800U) << 10) + (lo - 0xDC00U);
            }
          }
          AppendCodepointUtf8(cp, out);
          break;
        }
        default: out.push_back(e); break;
      }
    } else {
      out.push_back(c);
    }
  }
  return false;
}

bool ParseJsonObjectAt(std::string_view text, std::size_t& pos, ObjectFields& fields,
                       std::string& error_out) {
  fields.clear();
  SkipJsonWs(text, pos);
  if (pos >= text.size() || text[pos] != '{') {
    error_out = Tr("Expected JSON object '{'", "期望 JSON 对象 '{'");
    return false;
  }
  ++pos;
  SkipJsonWs(text, pos);
  if (pos < text.size() && text[pos] == '}') {
    ++pos;
    return true;
  }
  while (pos < text.size()) {
    SkipJsonWs(text, pos);
    std::string key;
    if (!ParseJsonStringAt(text, pos, key)) {
      error_out = Tr("Invalid JSON object key", "JSON 对象键格式无效");
      return false;
    }
    SkipJsonWs(text, pos);
    if (pos >= text.size() || text[pos] != ':') {
      error_out = Tr("Expected ':' in JSON object", "JSON 对象缺少冒号");
      return false;
    }
    ++pos;
    SkipJsonWs(text, pos);
    if (pos >= text.size()) {
      error_out = Tr("Unexpected end of JSON object", "JSON 对象意外结束");
      return false;
    }
    std::optional<std::string> val;
    if (text[pos] == '"') {
      std::string sv;
      if (!ParseJsonStringAt(text, pos, sv)) {
        error_out = Tr("Invalid JSON string value", "JSON 字符串值格式无效");
        return false;
      }
      val = std::move(sv);
    } else if (text.substr(pos, 4) == "null") {
      pos += 4;
      val = std::nullopt;
    } else if (text.substr(pos, 4) == "true") {
      pos += 4;
      val = "true";
    } else if (text.substr(pos, 5) == "false") {
      pos += 5;
      val = "false";
    } else if (text[pos] == '{' || text[pos] == '[') {
      const std::size_t start = pos;
      int depth = 0;
      bool in_q = false;
      while (pos < text.size()) {
        const char ch = text[pos++];
        if (in_q) {
          if (ch == '\\' && pos < text.size()) ++pos;
          else if (ch == '"') in_q = false;
        } else {
          if (ch == '"') in_q = true;
          else if (ch == '{' || ch == '[') ++depth;
          else if (ch == '}' || ch == ']') {
            --depth;
            if (depth == 0) break;
          }
        }
      }
      if (depth != 0) {
        error_out = Tr("Unterminated nested JSON value", "JSON 嵌套值未闭合");
        return false;
      }
      val = std::string(text.substr(start, pos - start));
    } else {
      const std::size_t start = pos;
      while (pos < text.size() && text[pos] != ',' && text[pos] != '}' &&
             text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\r' && text[pos] != '\n') {
        ++pos;
      }
      if (pos == start) {
        error_out = Tr("Invalid JSON value", "JSON 值无效");
        return false;
      }
      val = std::string(text.substr(start, pos - start));
    }
    fields.emplace_back(std::move(key), std::move(val));
    SkipJsonWs(text, pos);
    if (pos < text.size() && text[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < text.size() && text[pos] == '}') {
      ++pos;
      return true;
    }
    error_out = Tr("Malformed JSON object", "JSON 对象格式错误");
    return false;
  }
  error_out = Tr("Unexpected end of JSON object", "JSON 对象意外结束");
  return false;
}

void FinalizeRecordRows(const std::vector<ObjectFields>& records, AdbcTabularDataset& out) {
  std::unordered_map<std::string, std::size_t> col_index;
  for (const auto& rec : records) {
    for (const auto& [k, _] : rec) {
      if (!col_index.contains(k)) {
        const std::size_t idx = out.columns.size();
        col_index.emplace(k, idx);
        out.columns.push_back(k);
      }
    }
  }
  out.rows.resize(records.size());
  for (std::size_t r = 0; r < records.size(); ++r) {
    out.rows[r].resize(out.columns.size());
    for (const auto& [k, v] : records[r]) {
      out.rows[r][col_index[k]] = v;
    }
  }
  AdbcFinalizeTabularDatasetTypes(out);
}

std::string DecodeEntities(std::string_view text, bool html_nbsp) {
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
    else if (html_nbsp && ent == "nbsp") out.push_back(' ');
    else if (ent.size() >= 2 && ent[0] == '#') {
      std::uint32_t cp = 0;
      const bool hex = (ent[1] == 'x' || ent[1] == 'X');
      if (ParseHexUint(hex ? ent.substr(2) : ent.substr(1), hex ? 16 : 10, cp) && cp > 0) {
        AppendCodepointUtf8(cp, out);
      } else {
        out.append(text.substr(i, semi - i + 1));
      }
    } else {
      out.append(text.substr(i, semi - i + 1));
    }
    i = semi;
  }
  return out;
}

std::size_t FindTagIgnoreCase(std::string_view hay, std::string_view needle, std::size_t pos) {
  const std::string h_low = AdbcLowerAscii(hay.substr(pos));
  const std::string n_low = AdbcLowerAscii(needle);
  const std::size_t found = h_low.find(n_low);
  return found == std::string::npos ? std::string::npos : pos + found;
}

bool HasAttrValueIgnoreCase(std::string_view attrs, std::string_view attr_name,
                            std::string_view expected) {
  const std::string low_attrs = AdbcLowerAscii(attrs);
  const std::string low_name = AdbcLowerAscii(attr_name);
  const std::string low_exp = AdbcLowerAscii(expected);
  std::size_t pos = 0;
  while ((pos = low_attrs.find(low_name, pos)) != std::string::npos) {
    if (pos > 0 && std::isalnum(static_cast<unsigned char>(low_attrs[pos - 1])) != 0) {
      ++pos;
      continue;
    }
    std::size_t p = pos + low_name.size();
    while (p < low_attrs.size() && std::isspace(static_cast<unsigned char>(low_attrs[p])) != 0) ++p;
    if (p >= low_attrs.size() || low_attrs[p] != '=') {
      pos = p;
      continue;
    }
    ++p;
    while (p < low_attrs.size() && std::isspace(static_cast<unsigned char>(low_attrs[p])) != 0) ++p;
    if (p < low_attrs.size() && (low_attrs[p] == '"' || low_attrs[p] == '\'')) {
      const char q = low_attrs[p++];
      const std::size_t end = low_attrs.find(q, p);
      if (end != std::string::npos && AdbcTrimAscii(std::string_view(low_attrs).substr(p, end - p)) == low_exp) {
        return true;
      }
    }
    pos = p;
  }
  return false;
}

std::optional<std::string> ExtractAttrRaw(std::string_view attrs, std::string_view attr_name) {
  const std::string low_attrs = AdbcLowerAscii(attrs);
  const std::string low_name = AdbcLowerAscii(attr_name);
  std::size_t pos = 0;
  while ((pos = low_attrs.find(low_name, pos)) != std::string::npos) {
    if (pos > 0 && std::isalnum(static_cast<unsigned char>(low_attrs[pos - 1])) != 0) {
      ++pos;
      continue;
    }
    std::size_t p = pos + low_name.size();
    while (p < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[p])) != 0) ++p;
    if (p >= attrs.size() || attrs[p] != '=') {
      pos = p;
      continue;
    }
    ++p;
    while (p < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[p])) != 0) ++p;
    if (p < attrs.size() && (attrs[p] == '"' || attrs[p] == '\'')) {
      const char q = attrs[p++];
      const std::size_t end = attrs.find(q, p);
      if (end != std::string::npos) {
        return DecodeEntities(attrs.substr(p, end - p), false);
      }
    }
    pos = p;
  }
  return std::nullopt;
}

std::vector<std::string> SplitMarkdownRow(std::string_view line) {
  std::vector<std::string> raw;
  std::string cur;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == '|') {
      cur.push_back('|');
      ++i;
    } else if (line[i] == '|') {
      raw.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(line[i]);
    }
  }
  raw.push_back(std::move(cur));
  if (!raw.empty() && AdbcTrimAscii(raw.front()).empty()) raw.erase(raw.begin());
  if (!raw.empty() && AdbcTrimAscii(raw.back()).empty()) raw.pop_back();
  return raw;
}

bool IsMarkdownSeparatorRow(const std::vector<std::string>& cells) {
  if (cells.empty()) return false;
  for (const std::string& cell : cells) {
    std::string_view s = AdbcTrimAscii(cell);
    if (s.empty()) return false;
    if (s.front() == ':') s.remove_prefix(1);
    if (!s.empty() && s.back() == ':') s.remove_suffix(1);
    if (s.empty()) return false;
    for (const char ch : s) {
      if (ch != '-') return false;
    }
  }
  return true;
}

std::string UnescapeMarkdownCell(std::string_view raw) {
  const std::string_view trimmed = AdbcTrimAscii(raw);
  std::string out;
  out.reserve(trimmed.size());
  for (std::size_t i = 0; i < trimmed.size(); ++i) {
    if (trimmed[i] == '<') {
      const std::string low = AdbcLowerAscii(trimmed.substr(i, 6));
      if (low.starts_with("<br/>")) {
        out.push_back('\n');
        i += 4;
        continue;
      }
      if (low.starts_with("<br />")) {
        out.push_back('\n');
        i += 5;
        continue;
      }
      if (low.starts_with("<br>")) {
        out.push_back('\n');
        i += 3;
        continue;
      }
    }
    out.push_back(trimmed[i]);
  }
  return out;
}

std::string ExtractHtmlCellText(std::string_view raw) {
  std::string plain;
  plain.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '<') {
      const std::size_t close = raw.find('>', i + 1);
      if (close == std::string_view::npos) break;
      const std::string tag_low = AdbcLowerAscii(AdbcTrimAscii(raw.substr(i + 1, close - i - 1)));
      if (tag_low == "br" || tag_low == "br/" || tag_low == "br /") {
        plain.push_back('\n');
      }
      i = close;
    } else {
      plain.push_back(raw[i]);
    }
  }
  return DecodeEntities(plain, true);
}

void SkipXmlMisc(std::string_view xml, std::size_t& pos) {
  while (pos < xml.size()) {
    while (pos < xml.size() && std::isspace(static_cast<unsigned char>(xml[pos])) != 0) ++pos;
    if (xml.substr(pos, 2) == "<?") {
      const std::size_t end = xml.find("?>", pos + 2);
      pos = (end == std::string_view::npos) ? xml.size() : end + 2;
      continue;
    }
    if (xml.substr(pos, 4) == "<!--") {
      const std::size_t end = xml.find("-->", pos + 4);
      pos = (end == std::string_view::npos) ? xml.size() : end + 3;
      continue;
    }
    if (xml.substr(pos, 2) == "<!" && xml.substr(pos, 9) != "<![CDATA[") {
      const std::size_t end = xml.find('>', pos + 2);
      pos = (end == std::string_view::npos) ? xml.size() : end + 1;
      continue;
    }
    break;
  }
}

std::size_t FindXmlTagEnd(std::string_view xml, std::size_t lt_pos) {
  bool in_q = false;
  char q = '\0';
  for (std::size_t i = lt_pos + 1; i < xml.size(); ++i) {
    const char c = xml[i];
    if (in_q) {
      if (c == q) in_q = false;
    } else {
      if (c == '"' || c == '\'') {
        in_q = true;
        q = c;
      } else if (c == '>') {
        return i;
      }
    }
  }
  return std::string_view::npos;
}

bool ReadXmlElement(std::string_view xml, std::size_t& pos, std::string& tag_out,
                    std::string& attrs_out, std::string_view& body_out, bool& self_closing) {
  SkipXmlMisc(xml, pos);
  if (pos >= xml.size() || xml[pos] != '<') return false;
  if (pos + 1 < xml.size() && xml[pos + 1] == '/') return false;

  const std::size_t gt = FindXmlTagEnd(xml, pos);
  if (gt == std::string_view::npos) return false;
  std::string_view inside = xml.substr(pos + 1, gt - pos - 1);
  self_closing = !inside.empty() && inside.back() == '/';
  if (self_closing) inside.remove_suffix(1);
  inside = AdbcTrimAscii(inside);
  if (inside.empty()) return false;

  std::size_t sp = 0;
  while (sp < inside.size() && std::isspace(static_cast<unsigned char>(inside[sp])) == 0) ++sp;
  tag_out = std::string(inside.substr(0, sp));
  attrs_out = std::string(inside.substr(sp));
  pos = gt + 1;
  if (self_closing) {
    body_out = {};
    return true;
  }

  const std::size_t body_start = pos;
  int depth = 1;
  while (pos < xml.size() && depth > 0) {
    if (xml.substr(pos, 9) == "<![CDATA[") {
      const std::size_t cend = xml.find("]]>", pos + 9);
      pos = (cend == std::string_view::npos) ? xml.size() : cend + 3;
      continue;
    }
    if (xml.substr(pos, 4) == "<!--") {
      const std::size_t cend = xml.find("-->", pos + 4);
      pos = (cend == std::string_view::npos) ? xml.size() : cend + 3;
      continue;
    }
    if (xml[pos] != '<') {
      ++pos;
      continue;
    }
    const std::size_t next_gt = FindXmlTagEnd(xml, pos);
    if (next_gt == std::string_view::npos) return false;
    const std::string_view piece = AdbcTrimAscii(xml.substr(pos + 1, next_gt - pos - 1));
    if (piece.starts_with('/')) {
      --depth;
      if (depth == 0) {
        body_out = xml.substr(body_start, pos - body_start);
        pos = next_gt + 1;
        return true;
      }
    } else if (!piece.ends_with('/') && !piece.starts_with('?') && !piece.starts_with('!')) {
      ++depth;
    }
    pos = next_gt + 1;
  }
  return false;
}

std::string DecodeXmlBody(std::string_view body) {
  std::string out;
  std::size_t pos = 0;
  while (pos < body.size()) {
    if (body.substr(pos, 9) == "<![CDATA[") {
      const std::size_t cend = body.find("]]>", pos + 9);
      if (cend == std::string_view::npos) {
        out.append(body.substr(pos + 9));
        break;
      }
      out.append(body.substr(pos + 9, cend - (pos + 9)));
      pos = cend + 3;
      continue;
    }
    const std::size_t next = body.find("<![CDATA[", pos);
    const std::string_view chunk =
        (next == std::string_view::npos) ? body.substr(pos) : body.substr(pos, next - pos);
    out.append(DecodeEntities(chunk, false));
    if (next == std::string_view::npos) break;
    pos = next;
  }
  return out;
}

} // namespace

bool AdbcParseJsonFile(const std::filesystem::path& path, AdbcTabularDataset& out,
                       std::string& error_out) {
  std::string content;
  if (!ReadFileToUtf8String(path, "JSON", content, error_out)) return false;

  std::size_t pos = 0;
  SkipJsonWs(content, pos);
  if (pos >= content.size()) {
    error_out = Tr("JSON file is empty", "JSON 文件为空");
    return false;
  }

  std::vector<ObjectFields> records;
  if (content[pos] == '[') {
    ++pos;
    SkipJsonWs(content, pos);
    if (pos < content.size() && content[pos] == ']') {
      error_out = Tr("JSON array contains no records", "JSON 数组不包含任何记录");
      return false;
    }
    while (pos < content.size()) {
      ObjectFields rec;
      if (!ParseJsonObjectAt(content, pos, rec, error_out)) return false;
      records.push_back(std::move(rec));
      SkipJsonWs(content, pos);
      if (pos < content.size() && content[pos] == ',') {
        ++pos;
        SkipJsonWs(content, pos);
        continue;
      }
      if (pos < content.size() && content[pos] == ']') {
        ++pos;
        break;
      }
      error_out = Tr("Expected ',' or ']' in JSON array", "JSON 数组缺少逗号或结束括号");
      return false;
    }
  } else if (content[pos] == '{') {
    ObjectFields rec;
    if (!ParseJsonObjectAt(content, pos, rec, error_out)) return false;
    records.push_back(std::move(rec));
  } else {
    error_out = Tr("JSON root must be an array of objects or an object",
                   "JSON 根节点必须是对象数组或单个对象");
    return false;
  }

  SkipJsonWs(content, pos);
  if (pos != content.size()) {
    error_out = Tr("Unexpected trailing characters in JSON", "JSON 文件末尾包含多余字符");
    return false;
  }
  if (records.empty()) {
    error_out = Tr("JSON file contains no records", "JSON 文件没有有效记录");
    return false;
  }
  FinalizeRecordRows(records, out);
  if (out.columns.empty()) {
    error_out = Tr("JSON records have no columns", "JSON 记录不包含任何字段");
    return false;
  }
  return true;
}

bool AdbcParseJsonlFile(const std::filesystem::path& path, AdbcTabularDataset& out,
                        std::string& error_out) {
  std::string content;
  if (!ReadFileToUtf8String(path, "JSONL", content, error_out)) return false;

  std::vector<ObjectFields> records;
  std::istringstream iss(content);
  std::string line;
  std::size_t line_no = 0;
  while (std::getline(iss, line)) {
    ++line_no;
    const std::string_view trimmed = AdbcTrimAscii(line);
    if (trimmed.empty()) continue;
    ObjectFields rec;
    std::size_t pos = 0;
    std::string line_err;
    if (!ParseJsonObjectAt(trimmed, pos, rec, line_err)) {
      error_out = "Line " + std::to_string(line_no) + ": " + line_err;
      return false;
    }
    SkipJsonWs(trimmed, pos);
    if (pos != trimmed.size()) {
      error_out = "Line " + std::to_string(line_no) + ": " +
                  std::string(Tr("Unexpected trailing characters in JSONL", "JSONL 行末包含多余字符"));
      return false;
    }
    records.push_back(std::move(rec));
  }
  if (records.empty()) {
    error_out = Tr("JSONL file contains no records", "JSONL 文件没有有效记录");
    return false;
  }
  FinalizeRecordRows(records, out);
  if (out.columns.empty()) {
    error_out = Tr("JSONL file contains no columns", "JSONL 文件没有字段");
    return false;
  }
  return true;
}

bool AdbcParseMarkdownTableFile(const std::filesystem::path& path, AdbcTabularDataset& out,
                                std::string& error_out) {
  std::string content;
  if (!ReadFileToUtf8String(path, "Markdown", content, error_out)) return false;

  std::vector<std::string> lines;
  std::istringstream iss(content);
  for (std::string l; std::getline(iss, l);) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(std::move(l));
  }

  std::size_t header_idx = lines.size();
  for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
    if (lines[i].find('|') == std::string::npos || lines[i + 1].find('|') == std::string::npos) {
      continue;
    }
    const auto sep = SplitMarkdownRow(lines[i + 1]);
    if (IsMarkdownSeparatorRow(sep)) {
      header_idx = i;
      break;
    }
  }
  if (header_idx >= lines.size()) {
    error_out = Tr("No Markdown table found", "未找到有效的 Markdown 表格");
    return false;
  }

  const auto raw_headers = SplitMarkdownRow(lines[header_idx]);
  if (raw_headers.empty()) {
    error_out = Tr("Markdown table has no columns", "Markdown 表格没有列");
    return false;
  }
  for (std::size_t c = 0; c < raw_headers.size(); ++c) {
    std::string name = UnescapeMarkdownCell(raw_headers[c]);
    out.columns.push_back(name.empty() ? ("col_" + std::to_string(c + 1)) : std::move(name));
  }

  for (std::size_t r = header_idx + 2; r < lines.size(); ++r) {
    if (AdbcTrimAscii(lines[r]).empty() || lines[r].find('|') == std::string::npos) break;
    const auto cells = SplitMarkdownRow(lines[r]);
    std::vector<std::optional<std::string>> row(out.columns.size());
    for (std::size_t c = 0; c < out.columns.size() && c < cells.size(); ++c) {
      std::string text = UnescapeMarkdownCell(cells[c]);
      if (!text.empty()) {
        row[c] = std::move(text);
      }
    }
    out.rows.push_back(std::move(row));
  }
  if (out.rows.empty()) {
    error_out = Tr("Markdown table contains no data rows", "Markdown 表格没有数据行");
    return false;
  }
  AdbcFinalizeTabularDatasetTypes(out);
  return true;
}

bool AdbcParseHtmlTableFile(const std::filesystem::path& path, bool has_header,
                            AdbcTabularDataset& out, std::string& error_out) {
  std::string content;
  if (!ReadFileToUtf8String(path, "HTML", content, error_out)) return false;

  std::string_view scope(content);
  if (const std::size_t tbl_start = FindTagIgnoreCase(scope, "<table", 0);
      tbl_start != std::string::npos) {
    const std::size_t tbl_end = FindTagIgnoreCase(scope, "</table>", tbl_start);
    scope = (tbl_end == std::string::npos) ? scope.substr(tbl_start)
                                           : scope.substr(tbl_start, tbl_end - tbl_start);
  }

  std::vector<std::vector<std::optional<std::string>>> parsed_rows;
  std::size_t pos = 0;
  while (true) {
    const std::size_t tr_start = FindTagIgnoreCase(scope, "<tr", pos);
    if (tr_start == std::string::npos) break;
    const std::size_t tr_gt = scope.find('>', tr_start);
    if (tr_gt == std::string::npos) break;
    const std::size_t tr_end = FindTagIgnoreCase(scope, "</tr>", tr_gt + 1);
    const std::string_view tr_body = (tr_end == std::string::npos)
                                         ? scope.substr(tr_gt + 1)
                                         : scope.substr(tr_gt + 1, tr_end - (tr_gt + 1));

    std::vector<std::optional<std::string>> row;
    std::size_t cpos = 0;
    while (cpos < tr_body.size()) {
      const std::size_t td_pos = FindTagIgnoreCase(tr_body, "<td", cpos);
      const std::size_t th_pos = FindTagIgnoreCase(tr_body, "<th", cpos);
      const std::size_t cell_start = std::min(td_pos, th_pos);
      if (cell_start == std::string::npos) break;
      const bool is_th = (th_pos < td_pos);
      const std::size_t cell_gt = tr_body.find('>', cell_start);
      if (cell_gt == std::string::npos) break;
      const std::string_view attrs = tr_body.substr(cell_start + 3, cell_gt - (cell_start + 3));
      const std::size_t cell_end =
          FindTagIgnoreCase(tr_body, is_th ? "</th>" : "</td>", cell_gt + 1);
      const std::string_view inner = (cell_end == std::string::npos)
                                         ? tr_body.substr(cell_gt + 1)
                                         : tr_body.substr(cell_gt + 1, cell_end - (cell_gt + 1));
      std::string text = ExtractHtmlCellText(inner);
      const bool marked_null =
          HasAttrValueIgnoreCase(attrs, "class", "null") &&
          (text.empty() || AdbcEqualsIgnoreCase(AdbcTrimAscii(text), "NULL"));
      if (marked_null) {
        row.push_back(std::nullopt);
      } else {
        row.push_back(std::move(text));
      }
      cpos = (cell_end == std::string::npos) ? tr_body.size() : cell_end + 5;
    }
    if (!row.empty()) {
      parsed_rows.push_back(std::move(row));
    }
    pos = (tr_end == std::string::npos) ? scope.size() : tr_end + 5;
  }

  if (parsed_rows.empty()) {
    error_out = Tr("No HTML table rows found", "未找到 HTML 表格数据行");
    return false;
  }
  std::size_t num_cols = 0;
  for (const auto& r : parsed_rows) num_cols = std::max(num_cols, r.size());
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
    error_out = Tr("HTML table contains no data rows", "HTML 表格没有数据行");
    return false;
  }
  AdbcFinalizeTabularDatasetTypes(out);
  return true;
}

bool AdbcParseXmlRowsFile(const std::filesystem::path& path, AdbcTabularDataset& out,
                          std::string& error_out) {
  std::string content;
  if (!ReadFileToUtf8String(path, "XML", content, error_out)) return false;

  std::size_t pos = 0;
  std::string root_tag, root_attrs;
  std::string_view root_body;
  bool root_self = false;
  if (!ReadXmlElement(content, pos, root_tag, root_attrs, root_body, root_self)) {
    error_out = Tr("Invalid XML document", "无效的 XML 文档");
    return false;
  }
  if (root_self) {
    error_out = Tr("XML document contains no rows", "XML 文档没有数据行");
    return false;
  }

  std::vector<ObjectFields> records;
  std::size_t rpos = 0;
  while (rpos < root_body.size()) {
    std::string row_tag, row_attrs;
    std::string_view row_body;
    bool row_self = false;
    if (!ReadXmlElement(root_body, rpos, row_tag, row_attrs, row_body, row_self)) break;

    ObjectFields rec;
    std::size_t cpos = 0;
    while (!row_self && cpos < row_body.size()) {
      std::string col_tag, col_attrs;
      std::string_view col_body;
      bool col_self = false;
      if (!ReadXmlElement(row_body, cpos, col_tag, col_attrs, col_body, col_self)) break;

      std::string col_name = col_tag;
      if (AdbcEqualsIgnoreCase(col_tag, "column") || AdbcEqualsIgnoreCase(col_tag, "field") ||
          AdbcEqualsIgnoreCase(col_tag, "col")) {
        if (const auto named = ExtractAttrRaw(col_attrs, "name")) {
          if (!named->empty()) col_name = *named;
        }
      }
      const bool is_null = col_self || HasAttrValueIgnoreCase(col_attrs, "null", "true") ||
                           HasAttrValueIgnoreCase(col_attrs, "xsi:nil", "true");
      if (is_null) {
        rec.emplace_back(std::move(col_name), std::nullopt);
      } else {
        rec.emplace_back(std::move(col_name), DecodeXmlBody(col_body));
      }
    }
    if (!rec.empty()) {
      records.push_back(std::move(rec));
    }
  }
  if (records.empty()) {
    error_out = Tr("XML document contains no valid row elements", "XML 文档不包含有效行元素");
    return false;
  }
  FinalizeRecordRows(records, out);
  return true;
}

} // namespace dbpp::detail
