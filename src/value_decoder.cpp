module;

#include <ctime>

module dbpp.decoder;

import std;
import dbpp.i18n;

namespace dbpp {

namespace {

constexpr std::size_t kMaxHexBytes = 16384;

std::string_view TrimAscii(std::string_view text) noexcept {
  return detail::DecoderTrimAscii(text);
}

int HexDigitValue(char c) noexcept { return detail::DecoderHexDigitValue(c); }

void AppendCodepointUtf8(std::uint32_t cp, std::string& out) {
  detail::DecoderAppendCodepointUtf8(cp, out);
}

bool ParseHex4(std::string_view s, std::size_t pos, std::uint16_t& out) noexcept {
  return detail::DecoderParseHex4(s, pos, out);
}

bool FormatJsonString(std::string_view raw, bool pretty, std::string& out, std::string& error) {
  return detail::DecoderFormatJsonString(raw, pretty, out, error);
}

int Base64CharVal(unsigned char c) noexcept {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

bool DecodeBase64Bytes(std::string_view raw, std::string& bytes_out, std::string& error_out) {
  std::string compact;
  compact.reserve(raw.size());
  for (const unsigned char c : raw) {
    if (!std::isspace(c)) {
      compact.push_back(static_cast<char>(c));
    }
  }
  if (compact.empty()) {
    error_out = Tr("Empty Base64 input", "Base64 输入为空");
    return false;
  }
  std::size_t pad = 0;
  while (!compact.empty() && compact.back() == '=') {
    compact.pop_back();
    ++pad;
  }
  if (pad > 2 || (compact.size() % 4) == 1) {
    error_out = Tr("Invalid Base64 length or padding", "无效的 Base64 长度或填充");
    return false;
  }
  if (pad != 0 && ((compact.size() + pad) % 4) != 0) {
    error_out = Tr("Invalid Base64 padding", "无效的 Base64 '=' 填充");
    return false;
  }

  bytes_out.clear();
  bytes_out.reserve((compact.size() * 3) / 4);
  std::uint32_t acc = 0;
  int bits = 0;
  for (const unsigned char c : compact) {
    const int val = Base64CharVal(c);
    if (val < 0) {
      error_out = Tr("Invalid character in Base64 input", "Base64 包含非法字符");
      return false;
    }
    acc = (acc << 6) | static_cast<std::uint32_t>(val);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes_out.push_back(static_cast<char>((acc >> bits) & 0xFFu));
    }
  }
  return true;
}

bool IsValidPrintableUtf8(std::string_view text) noexcept {
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char b0 = static_cast<unsigned char>(text[i]);
    if (b0 < 0x80u) {
      if (b0 < 0x20u && b0 != '\t' && b0 != '\r' && b0 != '\n') {
        return false;
      }
      ++i;
      continue;
    }
    std::size_t len = 0;
    std::uint32_t min_cp = 0;
    std::uint32_t cp = 0;
    if ((b0 & 0xE0u) == 0xC0u) {
      len = 2;
      min_cp = 0x80u;
      cp = b0 & 0x1Fu;
    } else if ((b0 & 0xF0u) == 0xE0u) {
      len = 3;
      min_cp = 0x800u;
      cp = b0 & 0x0Fu;
    } else if ((b0 & 0xF8u) == 0xF0u) {
      len = 4;
      min_cp = 0x10000u;
      cp = b0 & 0x07u;
    } else {
      return false;
    }
    if (i + len > text.size()) {
      return false;
    }
    for (std::size_t j = 1; j < len; ++j) {
      const unsigned char bx = static_cast<unsigned char>(text[i + j]);
      if ((bx & 0xC0u) != 0x80u) {
        return false;
      }
      cp = (cp << 6) | (bx & 0x3Fu);
    }
    if (cp < min_cp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
      return false;
    }
    i += len;
  }
  return true;
}

std::string FormatHexDumpString(std::string_view bytes, bool& truncated_out) {
  static constexpr char kHex[] = "0123456789abcdef";
  const std::size_t total = bytes.size();
  const std::size_t limit = std::min(total, kMaxHexBytes);
  truncated_out = total > limit;

  std::string out;
  out.reserve(limit * 4 + 64);
  for (std::size_t off = 0; off < limit; off += 16) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      out.push_back(kHex[(off >> shift) & 0x0F]);
    }
    out += "  ";
    for (std::size_t j = 0; j < 16; ++j) {
      if (off + j < limit) {
        const unsigned char b = static_cast<unsigned char>(bytes[off + j]);
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
      } else {
        out += "  ";
      }
      out.push_back(j == 7 ? ' ' : '\0');
      if (out.back() == '\0') out.pop_back();
      out.push_back(' ');
    }
    out += " |";
    for (std::size_t j = 0; j < 16 && off + j < limit; ++j) {
      const unsigned char b = static_cast<unsigned char>(bytes[off + j]);
      out.push_back((b >= 0x20u && b <= 0x7Eu) ? static_cast<char>(b) : '.');
    }
    out += "|\n";
  }
  if (!out.empty() && out.back() == '\n') {
    out.pop_back();
  }
  if (truncated_out) {
    out += "\n" + TrFmt("... [truncated: showing first {0} / {1} bytes]",
                        "... [已截断：仅显示前 {0} / 共 {1} 字节]",
                        {std::to_string(limit), std::to_string(total)});
  }
  return out;
}

bool BreakTm(std::time_t sec, bool utc, std::tm& out) noexcept {
#if defined(_WIN32)
  return (utc ? ::gmtime_s(&out, &sec) : ::localtime_s(&out, &sec)) == 0;
#else
  return (utc ? ::gmtime_r(&sec, &out) : ::localtime_r(&sec, &out)) != nullptr;
#endif
}

std::string FormatEpochTime(std::int64_t sec, std::int64_t subsec_digits, int width, bool utc) {
  std::tm tm_val{};
  if (!BreakTm(static_cast<std::time_t>(sec), utc, tm_val)) {
    return {};
  }
  char buf[32]{};
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_val) == 0) {
    return {};
  }
  std::string s(buf);
  if (width > 0 && subsec_digits >= 0) {
    std::string frac = std::to_string(subsec_digits);
    if (frac.size() < static_cast<std::size_t>(width)) {
      frac.insert(0, static_cast<std::size_t>(width) - frac.size(), '0');
    }
    s.push_back('.');
    s += frac;
  }
  if (utc) {
    s += " UTC";
  }
  return s;
}

struct ParsedTimestamp {
  std::int64_t sec = 0;
  std::int64_t subsec = 0;
  int frac_width = 0;
  std::string_view unit_en;
  std::string_view unit_zh;
};

bool ParseUnixTimestamp(std::string_view raw, bool strict_auto_range, ParsedTimestamp& out) noexcept {
  const std::string_view t = TrimAscii(raw);
  if (t.empty()) return false;
  bool neg = false;
  std::string_view digits = t;
  if (digits.front() == '-') {
    neg = true;
    digits.remove_prefix(1);
  }
  if (digits.empty()) return false;
  for (const char c : digits) {
    if (c < '0' || c > '9') return false;
  }
  std::int64_t val = 0;
  const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), val);
  if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
    return false;
  }
  if (neg) val = -val;

  const std::size_t n = digits.size();
  std::int64_t denom = 1;
  if (n == 10 || (!strict_auto_range && n == 9)) {
    denom = 1;
    out.frac_width = 0;
    out.unit_en = "seconds (s)";
    out.unit_zh = "秒 (s)";
  } else if (n == 13) {
    denom = 1000;
    out.frac_width = 3;
    out.unit_en = "milliseconds (ms)";
    out.unit_zh = "毫秒 (ms)";
  } else if (n == 16) {
    denom = 1000000;
    out.frac_width = 6;
    out.unit_en = "microseconds (us)";
    out.unit_zh = "微秒 (us)";
  } else if (!strict_auto_range && n == 19) {
    denom = 1000000000;
    out.frac_width = 9;
    out.unit_en = "nanoseconds (ns)";
    out.unit_zh = "纳秒 (ns)";
  } else {
    return false;
  }

  out.sec = val / denom;
  out.subsec = val % denom;
  if (out.subsec < 0) {
    out.subsec += denom;
    --out.sec;
  }
  if (strict_auto_range) {
    // 2001-09-09T01:46:40Z .. 2100-01-01T00:00:00Z
    return out.sec >= 1000000000LL && out.sec <= 4102444800LL;
  }
  return out.sec >= -62135596800LL && out.sec <= 253402300799LL;
}

bool DecodeUrlPercent(std::string_view raw, std::string& out, std::string& error_out) {
  out.clear();
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '%') {
      if (i + 2 >= raw.size()) {
        error_out = Tr("Incomplete %XX sequence in URL string", "URL 编码中存在不完整的 %XX");
        return false;
      }
      const int hi = HexDigitValue(raw[i + 1]);
      const int lo = HexDigitValue(raw[i + 2]);
      if (hi < 0 || lo < 0) {
        error_out = Tr("Invalid %XX hexadecimal escape in URL string", "URL 编码中包含无效的 %XX");
        return false;
      }
      out.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
    } else {
      out.push_back(raw[i]);
    }
  }
  return true;
}

bool DecodeUnicodeEscapes(std::string_view raw, std::string& out, bool& changed_out) {
  out.clear();
  out.reserve(raw.size());
  changed_out = false;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      const char next = raw[i + 1];
      if (next == 'u') {
        std::uint16_t u1 = 0;
        if (ParseHex4(raw, i + 2, u1)) {
          i += 5;
          changed_out = true;
          if (u1 >= 0xD800u && u1 <= 0xDBFFu && i + 6 < raw.size() && raw[i + 1] == '\\' &&
              raw[i + 2] == 'u') {
            std::uint16_t u2 = 0;
            if (ParseHex4(raw, i + 3, u2) && u2 >= 0xDC00u && u2 <= 0xDFFFu) {
              i += 6;
              const std::uint32_t cp =
                  0x10000u + (((static_cast<std::uint32_t>(u1 - 0xD800u)) << 10) |
                              static_cast<std::uint32_t>(u2 - 0xDC00u));
              AppendCodepointUtf8(cp, out);
              continue;
            }
          }
          AppendCodepointUtf8(u1, out);
          continue;
        }
      } else if (next == 'n') {
        out.push_back('\n');
        ++i;
        changed_out = true;
        continue;
      } else if (next == 'r') {
        out.push_back('\r');
        ++i;
        changed_out = true;
        continue;
      } else if (next == 't') {
        out.push_back('\t');
        ++i;
        changed_out = true;
        continue;
      } else if (next == '\\') {
        out.push_back('\\');
        ++i;
        changed_out = true;
        continue;
      }
    }
    out.push_back(raw[i]);
  }
  return true;
}

void AppendJwtTimestampClaims(std::string_view payload_pretty, std::string& out) {
  static constexpr std::array<std::string_view, 3> kClaims = {"iat", "nbf", "exp"};
  std::vector<std::string> lines;
  for (const std::string_view claim : kClaims) {
    const std::string needle = "  \"" + std::string(claim) + "\": ";
    std::size_t search_pos = 0;
    while (search_pos < payload_pretty.size()) {
      const std::size_t hit = payload_pretty.find(needle, search_pos);
      if (hit == std::string_view::npos) break;
      if (hit == 0 || payload_pretty[hit - 1] == '\n') {
        std::size_t val_start = hit + needle.size();
        std::size_t val_end = val_start;
        if (val_end < payload_pretty.size() && payload_pretty[val_end] == '-') ++val_end;
        while (val_end < payload_pretty.size() &&
               payload_pretty[val_end] >= '0' && payload_pretty[val_end] <= '9') {
          ++val_end;
        }
        std::int64_t ts = 0;
        const std::string_view num_sv = payload_pretty.substr(val_start, val_end - val_start);
        const auto [ptr, ec] = std::from_chars(num_sv.data(), num_sv.data() + num_sv.size(), ts);
        if (ec == std::errc{} && ptr == num_sv.data() + num_sv.size()) {
          const std::string local = FormatEpochTime(ts, 0, 0, false);
          const std::string utc = FormatEpochTime(ts, 0, 0, true);
          if (!local.empty()) {
            lines.push_back("  " + std::string(claim) + ": " + std::string(num_sv) + " -> " +
                            local + " (" + utc + ")");
          }
        }
        break;
      }
      search_pos = hit + needle.size();
    }
  }
  if (!lines.empty()) {
    out += "\n\n// Claims Timestamps\n";
    for (const std::string& l : lines) {
      out += l + "\n";
    }
    if (!out.empty() && out.back() == '\n') {
      out.pop_back();
    }
  }
}

bool FormatInflatedOrDecodedBytes(std::string_view prefix_en, std::string_view size_info,
                                  std::string bytes, DecodedValueResult& res) {
  if (IsValidPrintableUtf8(bytes)) {
    const std::string_view trimmed = TrimAscii(bytes);
    if (!trimmed.empty() && ((trimmed.front() == '{' && trimmed.back() == '}') ||
                             (trimmed.front() == '[' && trimmed.back() == ']'))) {
      std::string pretty_json;
      std::string json_err;
      if (FormatJsonString(trimmed, true, pretty_json, json_err)) {
        res.ok = true;
        res.text = std::move(pretty_json);
        res.subtitle = TrFmt("{0} → JSON ({1})", "{0} → JSON（{1}）",
                             {std::string(prefix_en), std::string(size_info)});
        return true;
      }
    }
    res.ok = true;
    res.text = std::move(bytes);
    res.subtitle = TrFmt("{0} → UTF-8 ({1})", "{0} → UTF-8（{1}）",
                         {std::string(prefix_en), std::string(size_info)});
    return true;
  }
  bool hex_trunc = false;
  res.ok = true;
  res.text = FormatHexDumpString(bytes, hex_trunc);
  res.subtitle = TrFmt("{0} → Binary Hex ({1})", "{0} → 二进制十六进制（{1}）",
                       {std::string(prefix_en), std::string(size_info)});
  return true;
}

bool TryDecodeJwt(std::string_view raw, DecodedValueResult& out) {
  const std::string_view t = TrimAscii(raw);
  const std::size_t dot1 = t.find('.');
  if (dot1 == std::string_view::npos || dot1 == 0) return false;
  const std::size_t dot2 = t.find('.', dot1 + 1);
  if (dot2 == std::string_view::npos || dot2 == dot1 + 1) return false;
  if (t.find('.', dot2 + 1) != std::string_view::npos) return false;

  const std::string_view hdr_b64 = t.substr(0, dot1);
  const std::string_view pay_b64 = t.substr(dot1 + 1, dot2 - dot1 - 1);
  const std::string_view sig_b64 = t.substr(dot2 + 1);

  std::string hdr_raw;
  std::string pay_raw;
  std::string err;
  if (!DecodeBase64Bytes(hdr_b64, hdr_raw, err) || !DecodeBase64Bytes(pay_b64, pay_raw, err)) {
    out.ok = false;
    out.error = Tr("Invalid JWT Base64URL segment", "无效的 JWT Base64URL 分段");
    return false;
  }
  std::string hdr_pretty;
  std::string pay_pretty;
  if (!FormatJsonString(hdr_raw, true, hdr_pretty, err) ||
      !FormatJsonString(pay_raw, true, pay_pretty, err)) {
    out.ok = false;
    out.error = Tr("JWT header or payload is not valid JSON", "JWT Header 或 Payload 不是有效 JSON");
    return false;
  }
  const std::string_view hdr_trim = TrimAscii(hdr_pretty);
  const std::string_view pay_trim = TrimAscii(pay_pretty);
  if (hdr_trim.empty() || hdr_trim.front() != '{' || pay_trim.empty() || pay_trim.front() != '{') {
    out.ok = false;
    out.error = Tr("JWT header and payload must be JSON objects", "JWT Header 和 Payload 必须为 JSON 对象");
    return false;
  }

  std::size_t sig_bytes_len = 0;
  if (!sig_b64.empty()) {
    std::string sig_raw;
    std::string sig_err;
    if (DecodeBase64Bytes(sig_b64, sig_raw, sig_err)) {
      sig_bytes_len = sig_raw.size();
    }
  }

  out.ok = true;
  out.text = "// Header\n" + hdr_pretty + "\n\n// Payload\n" + pay_pretty;
  AppendJwtTimestampClaims(pay_pretty, out.text);
  out.subtitle = TrFmt("JWT · signature {0} bytes", "JWT · 签名 {0} 字节",
                       {std::to_string(sig_bytes_len)});
  return true;
}

} // namespace

std::string_view DecoderDisplayName(ValueDecoderKind kind) noexcept {
  switch (kind) {
    case ValueDecoderKind::Raw:
      return "Raw";
    case ValueDecoderKind::Json:
      return "JSON";
    case ValueDecoderKind::Base64:
      return "Base64";
    case ValueDecoderKind::Gzip:
      return "Gzip / Zlib";
    case ValueDecoderKind::Jwt:
      return "JWT";
    case ValueDecoderKind::Url:
      return "URL";
    case ValueDecoderKind::Hex:
      return "Hex";
    case ValueDecoderKind::Unicode:
      return "Unicode";
    case ValueDecoderKind::Timestamp:
      return "Timestamp";
  }
  return "Raw";
}

ValueDecoderKind DetectSuggestedDecoder(std::string_view raw) noexcept {
  if (raw.empty()) {
    return ValueDecoderKind::Raw;
  }
  const std::string_view trimmed = TrimAscii(raw);
  if (trimmed.empty()) {
    return ValueDecoderKind::Raw;
  }

  // 0. Raw Gzip / Zlib check (verify actual inflation for Zlib to avoid ASCII 'x^' false hits).
  if (const auto raw_comp = detail::DetectCompressedContainer(raw);
      raw_comp != detail::InflateContainerKind::None) {
    detail::InflateContainerKind probed_kind = detail::InflateContainerKind::None;
    std::string dummy_out;
    std::string dummy_err;
    if (raw_comp == detail::InflateContainerKind::Gzip ||
        detail::InflateGzipOrZlib(raw, probed_kind, dummy_out, dummy_err)) {
      return ValueDecoderKind::Gzip;
    }
  }

  // 1. JWT check first (before URL/Base64).
  {
    DecodedValueResult jwt_probe;
    if (TryDecodeJwt(trimmed, jwt_probe) && jwt_probe.ok) {
      return ValueDecoderKind::Jwt;
    }
  }

  // 2. JSON object/array check.
  if ((trimmed.front() == '{' && trimmed.back() == '}') ||
      (trimmed.front() == '[' && trimmed.back() == ']')) {
    std::string dummy_out;
    std::string dummy_err;
    if (FormatJsonString(trimmed, false, dummy_out, dummy_err)) {
      return ValueDecoderKind::Json;
    }
  }

  // 3. Unix timestamp within [2001, 2100] range.
  ParsedTimestamp ts_probe;
  if (ParseUnixTimestamp(trimmed, true, ts_probe)) {
    return ValueDecoderKind::Timestamp;
  }

  // 4. Unicode escape sequence (\uXXXX).
  if (trimmed.find("\\u") != std::string_view::npos) {
    for (std::size_t i = 0; i + 5 < trimmed.size(); ++i) {
      if (trimmed[i] == '\\' && trimmed[i + 1] == 'u') {
        std::uint16_t dummy_u = 0;
        if (ParseHex4(trimmed, i + 2, dummy_u)) {
          return ValueDecoderKind::Unicode;
        }
      }
    }
  }

  // 5. URL-encoded (%XX).
  if (trimmed.find('%') != std::string_view::npos) {
    std::string url_out;
    std::string url_err;
    if (DecodeUrlPercent(trimmed, url_out, url_err) && url_out != trimmed &&
        IsValidPrintableUtf8(url_out)) {
      return ValueDecoderKind::Url;
    }
  }

  // 6. Conservative Base64 check (avoid false positives on normal words/identifiers).
  if (trimmed.size() >= 8) {
    bool has_ws = false;
    bool has_b64_punct = false;
    bool has_upper = false;
    bool has_lower = false;
    bool has_digit = false;
    for (const unsigned char c : trimmed) {
      if (std::isspace(c)) {
        has_ws = true;
        break;
      }
      if (c == '+' || c == '/' || c == '=' || c == '-') {
        has_b64_punct = true;
      } else if (c >= 'A' && c <= 'Z') {
        has_upper = true;
      } else if (c >= 'a' && c <= 'z') {
        has_lower = true;
      } else if (c >= '0' && c <= '9') {
        has_digit = true;
      } else if (c != '_') {
        has_ws = true;
        break;
      }
    }
    const bool looks_like_token =
        !has_ws && (has_b64_punct || (trimmed.size() >= 12 && has_upper && has_lower && has_digit));
    if (looks_like_token && trimmed.find('_') == std::string_view::npos) {
      std::string b64_bytes;
      std::string b64_err;
      if (DecodeBase64Bytes(trimmed, b64_bytes, b64_err)) {
        if (detail::DetectCompressedContainer(b64_bytes) != detail::InflateContainerKind::None) {
          detail::InflateContainerKind probed_kind = detail::InflateContainerKind::None;
          std::string inf_out;
          std::string inf_err;
          if (detail::InflateGzipOrZlib(b64_bytes, probed_kind, inf_out, inf_err)) {
            return ValueDecoderKind::Gzip;
          }
        }
        if (b64_bytes.size() >= 4 && IsValidPrintableUtf8(b64_bytes)) {
          return ValueDecoderKind::Base64;
        }
      }
    }
  }

  // 7. Binary / non-UTF-8 control bytes -> Hex.
  if (!IsValidPrintableUtf8(raw)) {
    return ValueDecoderKind::Hex;
  }

  return ValueDecoderKind::Raw;
}

DecodedValueResult DecodeValue(std::string_view raw, ValueDecoderKind kind) {
  DecodedValueResult res;
  res.kind = kind;
  switch (kind) {
    case ValueDecoderKind::Raw:
      res.ok = true;
      res.text = std::string(raw);
      res.subtitle = TrFmt("Raw ({0} bytes)", "原始文本（{0} 字节）",
                           {std::to_string(raw.size())});
      return res;

    case ValueDecoderKind::Json: {
      res.ok = FormatJsonString(raw, true, res.text, res.error);
      if (res.ok) {
        res.subtitle = Tr("Valid JSON (formatted)", "有效 JSON（已格式化）");
      }
      return res;
    }

    case ValueDecoderKind::Base64: {
      std::string bytes;
      if (!DecodeBase64Bytes(raw, bytes, res.error)) {
        res.ok = false;
        return res;
      }
      if (detail::DetectCompressedContainer(bytes) != detail::InflateContainerKind::None) {
        detail::InflateContainerKind comp_kind = detail::InflateContainerKind::None;
        std::string inflated;
        std::string inf_err;
        if (detail::InflateGzipOrZlib(bytes, comp_kind, inflated, inf_err)) {
          const std::string kind_tag =
              (comp_kind == detail::InflateContainerKind::Gzip) ? "Base64 → Gzip" : "Base64 → Zlib";
          const std::string sz_info = TrFmt("{0} → {1} bytes", "{0} → {1} 字节",
                                            {std::to_string(bytes.size()),
                                             std::to_string(inflated.size())});
          FormatInflatedOrDecodedBytes(kind_tag, sz_info, std::move(inflated), res);
          return res;
        }
      }
      const std::string sz_info =
          TrFmt("{0} bytes", "{0} 字节", {std::to_string(bytes.size())});
      FormatInflatedOrDecodedBytes("Base64", sz_info, std::move(bytes), res);
      return res;
    }

    case ValueDecoderKind::Gzip: {
      bool via_base64 = false;
      std::string compressed;
      if (detail::DetectCompressedContainer(raw) != detail::InflateContainerKind::None) {
        compressed = std::string(raw);
      } else {
        std::string b64_err;
        if (!DecodeBase64Bytes(raw, compressed, b64_err) ||
            detail::DetectCompressedContainer(compressed) == detail::InflateContainerKind::None) {
          res.ok = false;
          res.error = Tr("Input is neither Gzip/Zlib nor Base64-encoded Gzip/Zlib",
                         "输入既不是 Gzip/Zlib 压缩流，也不是 Base64 编码的压缩数据");
          return res;
        }
        via_base64 = true;
      }
      detail::InflateContainerKind comp_kind = detail::InflateContainerKind::None;
      std::string inflated;
      if (!detail::InflateGzipOrZlib(compressed, comp_kind, inflated, res.error)) {
        res.ok = false;
        return res;
      }
      const std::string prefix =
          std::string(via_base64 ? "Base64 → " : "") +
          (comp_kind == detail::InflateContainerKind::Gzip ? "Gzip" : "Zlib");
      const std::string sz_info =
          TrFmt("{0} → {1} bytes", "{0} → {1} 字节",
                {std::to_string(compressed.size()), std::to_string(inflated.size())});
      FormatInflatedOrDecodedBytes(prefix, sz_info, std::move(inflated), res);
      return res;
    }

    case ValueDecoderKind::Jwt: {
      if (!TryDecodeJwt(raw, res)) {
        res.ok = false;
        if (res.error.empty()) {
          res.error = Tr("Not a valid 3-segment JWT (header.payload.signature)",
                         "不是有效的三段式 JWT（header.payload.signature）");
        }
      }
      return res;
    }

    case ValueDecoderKind::Url: {
      std::string decoded;
      if (!DecodeUrlPercent(raw, decoded, res.error)) {
        res.ok = false;
        return res;
      }
      res.ok = true;
      res.text = std::move(decoded);
      res.subtitle = TrFmt("URL Decoded ({0} bytes)", "URL 解码（{0} 字节）",
                           {std::to_string(res.text.size())});
      return res;
    }

    case ValueDecoderKind::Hex: {
      bool hex_trunc = false;
      res.ok = true;
      res.text = FormatHexDumpString(raw, hex_trunc);
      res.subtitle = TrFmt("Hex Dump ({0} bytes)", "十六进制视图（{0} 字节）",
                           {std::to_string(raw.size())});
      return res;
    }

    case ValueDecoderKind::Unicode: {
      bool changed = false;
      DecodeUnicodeEscapes(raw, res.text, changed);
      res.ok = true;
      res.subtitle = Tr("Unicode Unescaped", "Unicode 转义还原");
      return res;
    }

    case ValueDecoderKind::Timestamp: {
      ParsedTimestamp parsed;
      if (!ParseUnixTimestamp(raw, false, parsed)) {
        res.ok = false;
        res.error = Tr("Expected a Unix timestamp with 9, 10 (s), 13 (ms), 16 (us), or 19 (ns) digits",
                       "请输入 9 / 10（秒）、13（毫秒）、16（微秒）或 19（纳秒）位 Unix 时间戳");
        return res;
      }
      const std::string local = FormatEpochTime(parsed.sec, parsed.subsec, parsed.frac_width, false);
      const std::string utc = FormatEpochTime(parsed.sec, parsed.subsec, parsed.frac_width, true);
      if (local.empty() || utc.empty()) {
        res.ok = false;
        res.error = Tr("Timestamp out of representable range", "时间戳超出可格式化范围");
        return res;
      }
      res.ok = true;
      res.subtitle = std::string(Tr(parsed.unit_en, parsed.unit_zh));
      res.text = "Local: " + local + "\nUTC:   " + utc +
                 "\nEpoch: " + std::string(TrimAscii(raw));
      return res;
    }
  }
  return res;
}

DecodedValueResult MinifyJsonValue(std::string_view raw) {
  DecodedValueResult res;
  res.kind = ValueDecoderKind::Json;
  res.ok = FormatJsonString(raw, false, res.text, res.error);
  return res;
}

} // namespace dbpp
