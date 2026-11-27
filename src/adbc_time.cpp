module;

#include <cstdio>
#include <ctime>

module dbpp.adbc;

import std;
import dbpp.types;

namespace dbpp {

namespace {

std::int64_t ScaleForPrecision(int precision) {
  switch (precision) {
    case 3: return 1'000;
    case 6: return 1'000'000;
    case 9: return 1'000'000'000;
    default: return 1;
  }
}

int PrecisionForTimeUnit(int time_unit) {
  switch (time_unit) {
    case 1: return 3;
    case 2: return 6;
    case 3: return 9;
    default: return 0;
  }
}

bool ParseFixedDigits(std::string_view text, std::size_t pos, std::size_t count, int& out) {
  if (pos + count > text.size()) {
    return false;
  }
  int val = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[pos + i]);
    if (c < '0' || c > '9') {
      return false;
    }
    val = val * 10 + (c - '0');
  }
  out = val;
  return true;
}

bool LocalTimeFromEpoch(std::time_t sec, std::tm& tm_out) {
#ifdef _WIN32
  return localtime_s(&tm_out, &sec) == 0;
#else
  return localtime_r(&sec, &tm_out) != nullptr;
#endif
}

bool UtcTimeFromEpoch(std::time_t sec, std::tm& tm_out) {
#ifdef _WIN32
  return gmtime_s(&tm_out, &sec) == 0;
#else
  return gmtime_r(&sec, &tm_out) != nullptr;
#endif
}

std::optional<std::int64_t> CivilToUtcEpochSeconds(int year, int month, int day, int hour, int min,
                                                   int sec) {
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || min < 0 ||
      min > 59 || sec < 0 || sec > 60) {
    return std::nullopt;
  }
  const auto ymd = std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} /
                   std::chrono::day{static_cast<unsigned>(day)};
  if (!ymd.ok()) {
    return std::nullopt;
  }
  const auto sys_d = std::chrono::sys_days{ymd};
  const auto secs = sys_d.time_since_epoch() + std::chrono::hours{hour} +
                    std::chrono::minutes{min} + std::chrono::seconds{sec};
  return std::chrono::duration_cast<std::chrono::seconds>(secs).count();
}

std::string FormatTmDateTime(const std::tm& tm_val, std::int64_t sub_val, int precision,
                             char sep = ' ') {
  char base[96];
  std::snprintf(base, sizeof(base), "%04d-%02d-%02d%c%02d:%02d:%02d", tm_val.tm_year + 1900,
                tm_val.tm_mon + 1, tm_val.tm_mday, sep, tm_val.tm_hour, tm_val.tm_min,
                tm_val.tm_sec);
  std::string out(base);
  if (precision > 0 && sub_val > 0) {
    const int w = std::clamp(precision, 1, 9);
    char frac_buf[32];
    std::snprintf(frac_buf, sizeof(frac_buf), "%0*lld", w,
                  static_cast<long long>(sub_val));
    std::string_view frac(frac_buf);
    while (!frac.empty() && frac.back() == '0') {
      frac.remove_suffix(1);
    }
    if (!frac.empty()) {
      out.push_back('.');
      out.append(frac);
    }
  }
  return out;
}

std::string FormatLocalEpoch(std::int64_t raw_value, int precision) {
  const std::int64_t scale = ScaleForPrecision(precision);
  std::int64_t sec = raw_value / scale;
  std::int64_t rem = raw_value % scale;
  if (rem < 0) {
    --sec;
    rem += scale;
  }
  std::tm local_tm{};
  if (!LocalTimeFromEpoch(static_cast<std::time_t>(sec), local_tm)) {
    return {};
  }
  return FormatTmDateTime(local_tm, rem, precision, ' ');
}

struct ParsedDateTime {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  std::int64_t sub_val = 0;
  int sub_precision = 0;
  bool has_tz = false;
  int tz_offset_seconds = 0; // offset east of UTC (+08:00 => +28800)
};

bool ParseDateTimeInput(std::string_view raw, ParsedDateTime& out) {
  out = ParsedDateTime{};
  const std::string_view s = detail::AdbcTrimAscii(raw);
  if (s.size() < 16) {
    return false;
  }
  const char date_sep = s[4];
  if ((date_sep != '-' && date_sep != '/') || s[7] != date_sep ||
      !ParseFixedDigits(s, 0, 4, out.year) ||
      !ParseFixedDigits(s, 5, 2, out.month) ||
      !ParseFixedDigits(s, 8, 2, out.day)) {
    return false;
  }
  if (s[10] != ' ' && s[10] != 'T' && s[10] != 't') {
    return false;
  }
  if (!ParseFixedDigits(s, 11, 2, out.hour) || s[13] != ':' ||
      !ParseFixedDigits(s, 14, 2, out.minute)) {
    return false;
  }
  std::size_t pos = 16;
  if (pos < s.size() && s[pos] == ':') {
    ++pos;
    if (!ParseFixedDigits(s, pos, 2, out.second)) {
      return false;
    }
    pos += 2;
  }
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    std::size_t digits = 0;
    std::int64_t frac = 0;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
      if (digits < 9) {
        frac = frac * 10 + (s[pos] - '0');
        ++digits;
      }
      ++pos;
    }
    if (digits == 0) {
      return false;
    }
    while (digits < 9 && digits > 6 && digits != 9) {
      frac *= 10;
      ++digits;
    }
    if (digits > 3 && digits < 6) {
      while (digits < 6) {
        frac *= 10;
        ++digits;
      }
    } else if (digits < 3) {
      while (digits < 3) {
        frac *= 10;
        ++digits;
      }
    }
    out.sub_val = frac;
    out.sub_precision = static_cast<int>(digits);
  }

  std::string_view tz = detail::AdbcTrimAscii(s.substr(pos));
  if (tz.empty()) {
    out.has_tz = false;
    return CivilToUtcEpochSeconds(out.year, out.month, out.day, out.hour, out.minute,
                                  out.second)
        .has_value();
  }
  if (detail::AdbcEqualsIgnoreCase(tz, "z") || detail::AdbcEqualsIgnoreCase(tz, "utc") ||
      detail::AdbcEqualsIgnoreCase(tz, "gmt") || detail::AdbcEqualsIgnoreCase(tz, "etc/utc")) {
    out.has_tz = true;
    out.tz_offset_seconds = 0;
    return CivilToUtcEpochSeconds(out.year, out.month, out.day, out.hour, out.minute,
                                  out.second)
        .has_value();
  }
  if (tz.front() == '+' || tz.front() == '-') {
    const int sign = tz.front() == '-' ? -1 : 1;
    tz.remove_prefix(1);
    int oh = 0;
    int om = 0;
    if (!ParseFixedDigits(tz, 0, 2, oh)) {
      return false;
    }
    std::size_t off_pos = 2;
    if (off_pos < tz.size()) {
      if (tz[off_pos] == ':') {
        ++off_pos;
      }
      if (!ParseFixedDigits(tz, off_pos, 2, om)) {
        return false;
      }
      off_pos += 2;
    }
    if (off_pos != tz.size() || oh > 23 || om > 59) {
      return false;
    }
    out.has_tz = true;
    out.tz_offset_seconds = sign * (oh * 3600 + om * 60);
    return CivilToUtcEpochSeconds(out.year, out.month, out.day, out.hour, out.minute,
                                  out.second)
        .has_value();
  }
  return false;
}

bool IsStrictDigits(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (const unsigned char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

// Returns precision (0, 3, 6, 9) when raw_value is a plausible Unix epoch timestamp.
std::optional<int> DetectPlausibleUnixEpochPrecision(std::string_view raw_value) {
  const std::string_view s = detail::AdbcTrimAscii(raw_value);
  if (!IsStrictDigits(s)) {
    return std::nullopt;
  }
  const auto parsed = detail::AdbcParseInt64(s);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  const std::int64_t v = *parsed;
  // Plausible range: 1980-01-01 .. 2100-01-01
  constexpr std::int64_t kMinSec = 315'532'800LL;
  constexpr std::int64_t kMaxSec = 4'102'444'800LL;
  if (v >= kMinSec && v <= kMaxSec && (s.size() == 9 || s.size() == 10)) {
    return 0;
  }
  if (v >= kMinSec * 1'000LL && v <= kMaxSec * 1'000LL && s.size() >= 12 && s.size() <= 13) {
    return 3;
  }
  if (v >= kMinSec * 1'000'000LL && v <= kMaxSec * 1'000'000LL &&
      s.size() >= 15 && s.size() <= 16) {
    return 6;
  }
  if (v >= kMinSec * 1'000'000'000LL && v <= kMaxSec * 1'000'000'000LL &&
      s.size() >= 18 && s.size() <= 19) {
    return 9;
  }
  return std::nullopt;
}

std::int64_t RescaleSubSeconds(std::int64_t sub_val, int from_prec, int to_prec) {
  if (to_prec <= 0 || sub_val <= 0 || from_prec <= 0) {
    return 0;
  }
  while (from_prec < to_prec) {
    sub_val *= 10;
    ++from_prec;
  }
  while (from_prec > to_prec) {
    sub_val /= 10;
    --from_prec;
  }
  return sub_val;
}

std::optional<std::int64_t> LocalDateTimeToUtcEpochSeconds(const ParsedDateTime& dt) {
  std::tm tm_val{};
  tm_val.tm_year = dt.year - 1900;
  tm_val.tm_mon = dt.month - 1;
  tm_val.tm_mday = dt.day;
  tm_val.tm_hour = dt.hour;
  tm_val.tm_min = dt.minute;
  tm_val.tm_sec = dt.second;
  tm_val.tm_isdst = -1;
  const std::time_t epoch = std::mktime(&tm_val);
  if (epoch == static_cast<std::time_t>(-1)) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(epoch);
}

bool IsIsoZonedColumnType(std::string_view col_type) {
  const std::string lower = detail::AdbcLowerAscii(col_type);
  return lower.find("timestamptz") != std::string::npos ||
         lower.find("with time zone") != std::string::npos ||
         (lower.find("timestamp[") != std::string::npos &&
          lower.find("tz=") != std::string::npos);
}

bool IsIntegerColumnType(std::string_view col_type) {
  const std::string lower = detail::AdbcLowerAscii(col_type);
  return lower.find("int") != std::string::npos;
}

std::string FormatUtcIso(std::int64_t utc_sec, std::int64_t sub_val, int sub_prec) {
  std::tm utc_tm{};
  if (!UtcTimeFromEpoch(static_cast<std::time_t>(utc_sec), utc_tm)) {
    return {};
  }
  std::string out = FormatTmDateTime(utc_tm, sub_val, sub_prec, 'T');
  out.push_back('Z');
  return out;
}

std::string FormatUtcLikeOriginal(std::int64_t utc_sec, std::int64_t sub_val, int sub_prec,
                                  std::string_view raw_original) {
  std::tm utc_tm{};
  if (!UtcTimeFromEpoch(static_cast<std::time_t>(utc_sec), utc_tm)) {
    return {};
  }
  const std::string_view orig = detail::AdbcTrimAscii(raw_original);
  const bool uses_t = orig.find('T') != std::string_view::npos;
  const bool ends_with_utc = orig.ends_with(" UTC") || orig.ends_with(" utc");
  const bool ends_with_z = !orig.empty() && (orig.back() == 'Z' || orig.back() == 'z');
  const bool has_plus_offset = orig.find("+00") != std::string_view::npos;
  std::string out = FormatTmDateTime(utc_tm, sub_val, sub_prec, uses_t ? 'T' : ' ');
  if (ends_with_utc) {
    out += " UTC";
  } else if (has_plus_offset) {
    out += "+00:00";
  } else if (ends_with_z || !uses_t) {
    out.push_back('Z');
  } else {
    out.push_back('Z');
  }
  return out;
}

} // namespace

namespace detail {

std::string AdbcFormatLocalTimestamp(std::int64_t value, int time_unit) {
  const int precision = PrecisionForTimeUnit(time_unit);
  const std::string formatted = FormatLocalEpoch(value, precision);
  if (!formatted.empty()) {
    return formatted;
  }
  return AdbcFormatTimestampTz(value, time_unit, {});
}

bool AdbcParseTimestampInstant(std::string_view text, bool assume_local_if_naive,
                               std::int64_t& utc_sec_out, std::int64_t& sub_ns_out) {
  const std::string_view s = AdbcTrimAscii(text);
  if (s.empty()) {
    return false;
  }
  ParsedDateTime dt;
  if (ParseDateTimeInput(s, dt)) {
    sub_ns_out = RescaleSubSeconds(dt.sub_val, dt.sub_precision, 9);
    if (dt.has_tz) {
      if (const auto civil = CivilToUtcEpochSeconds(dt.year, dt.month, dt.day, dt.hour, dt.minute,
                                                    dt.second)) {
        utc_sec_out = *civil - dt.tz_offset_seconds;
        return true;
      }
      return false;
    }
    if (assume_local_if_naive) {
      if (const auto local_sec = LocalDateTimeToUtcEpochSeconds(dt)) {
        utc_sec_out = *local_sec;
        return true;
      }
      return false;
    }
    if (const auto civil = CivilToUtcEpochSeconds(dt.year, dt.month, dt.day, dt.hour, dt.minute,
                                                  dt.second)) {
      utc_sec_out = *civil;
      return true;
    }
    return false;
  }
  if (const auto prec = DetectPlausibleUnixEpochPrecision(s)) {
    if (const auto iv = AdbcParseInt64(s)) {
      const std::int64_t scale = ScaleForPrecision(*prec);
      std::int64_t sec = *iv / scale;
      std::int64_t rem = *iv % scale;
      if (rem < 0) {
        --sec;
        rem += scale;
      }
      utc_sec_out = sec;
      sub_ns_out = RescaleSubSeconds(rem, *prec, 9);
      return true;
    }
  }
  const std::size_t dot = s.find('.');
  if (dot != std::string_view::npos && dot > 0 && dot + 1 < s.size()) {
    const std::string_view whole = s.substr(0, dot);
    const std::string_view frac = s.substr(dot + 1);
    if (IsStrictDigits(frac)) {
      if (const auto prec = DetectPlausibleUnixEpochPrecision(whole); prec == 0) {
        if (const auto sec_val = AdbcParseInt64(whole)) {
          std::int64_t ns = 0;
          std::size_t digits = 0;
          for (const char c : frac) {
            if (digits < 9) {
              ns = ns * 10 + (c - '0');
              ++digits;
            }
          }
          while (digits < 9) {
            ns *= 10;
            ++digits;
          }
          utc_sec_out = *sec_val;
          sub_ns_out = ns;
          return true;
        }
      }
    }
  }
  return false;
}

bool AdbcParseNaiveDateTime(std::string_view text, std::string& formatted_out) {
  const std::string_view s = AdbcTrimAscii(text);
  if (s.empty()) {
    return false;
  }
  ParsedDateTime dt;
  if (ParseDateTimeInput(s, dt)) {
    if (dt.has_tz) {
      const auto civil = CivilToUtcEpochSeconds(dt.year, dt.month, dt.day, dt.hour, dt.minute,
                                                dt.second);
      if (!civil.has_value()) {
        return false;
      }
      const std::int64_t instant_sec = *civil - dt.tz_offset_seconds;
      std::tm local_tm{};
      if (!LocalTimeFromEpoch(static_cast<std::time_t>(instant_sec), local_tm)) {
        return false;
      }
      formatted_out = FormatTmDateTime(local_tm, dt.sub_val, dt.sub_precision, ' ');
      return true;
    }
    std::tm tm_val{};
    tm_val.tm_year = dt.year - 1900;
    tm_val.tm_mon = dt.month - 1;
    tm_val.tm_mday = dt.day;
    tm_val.tm_hour = dt.hour;
    tm_val.tm_min = dt.minute;
    tm_val.tm_sec = dt.second;
    formatted_out = FormatTmDateTime(tm_val, dt.sub_val, dt.sub_precision, ' ');
    return true;
  }
  if (const auto prec = DetectPlausibleUnixEpochPrecision(s)) {
    if (const auto iv = AdbcParseInt64(s)) {
      formatted_out = FormatLocalEpoch(*iv, *prec);
      return !formatted_out.empty();
    }
  }
  return false;
}

bool AdbcParseDateDays(std::string_view text, std::int32_t& days_since_epoch_out) {
  const std::string_view s = AdbcTrimAscii(text);
  if (s.size() < 10) {
    return false;
  }
  const char sep = s[4];
  int year = 0;
  int month = 0;
  int day = 0;
  if ((sep != '-' && sep != '/') || s[7] != sep ||
      !ParseFixedDigits(s, 0, 4, year) ||
      !ParseFixedDigits(s, 5, 2, month) ||
      !ParseFixedDigits(s, 8, 2, day)) {
    return false;
  }
  if (s.size() > 10 && s[10] != ' ' && s[10] != 'T' && s[10] != 't') {
    return false;
  }
  const auto ymd = std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} /
                   std::chrono::day{static_cast<unsigned>(day)};
  if (!ymd.ok()) {
    return false;
  }
  days_since_epoch_out =
      static_cast<std::int32_t>(std::chrono::sys_days{ymd}.time_since_epoch().count());
  return true;
}

} // namespace detail

bool IsTimestampColumnHint(std::string_view column_name, std::string_view column_type) {
  const std::string t_lower = detail::AdbcLowerAscii(column_type);
  if (t_lower.find("timestamp") != std::string::npos ||
      t_lower.find("datetime") != std::string::npos) {
    return true;
  }
  const std::string n_lower = detail::AdbcLowerAscii(column_name);
  if (n_lower.empty()) {
    return false;
  }
  return n_lower == "timestamp" || n_lower == "ts" || n_lower == "ctime" || n_lower == "mtime" ||
         n_lower == "atime" || n_lower == "created" || n_lower == "updated" ||
         n_lower == "deleted" || n_lower.ends_with("_at") || n_lower.ends_with("_ts") ||
         n_lower.ends_with("_time") || n_lower.ends_with("timestamp");
}

std::optional<std::string> LocalizeTimestampValue(std::string_view raw_value,
                                                  std::string_view column_name,
                                                  std::string_view column_type) {
  const std::string_view s = detail::AdbcTrimAscii(raw_value);
  if (s.empty()) {
    return std::nullopt;
  }
  const bool hint = IsTimestampColumnHint(column_name, column_type);
  if (hint) {
    if (const auto prec = DetectPlausibleUnixEpochPrecision(s)) {
      if (const auto iv = detail::AdbcParseInt64(s)) {
        const std::string localized = FormatLocalEpoch(*iv, *prec);
        if (!localized.empty()) {
          return localized;
        }
      }
    }
  }

  ParsedDateTime parsed;
  if (ParseDateTimeInput(s, parsed) && parsed.has_tz) {
    const auto utc_sec = CivilToUtcEpochSeconds(parsed.year, parsed.month, parsed.day, parsed.hour,
                                                parsed.minute, parsed.second);
    if (utc_sec.has_value()) {
      const std::int64_t instant_sec = *utc_sec - parsed.tz_offset_seconds;
      std::tm local_tm{};
      if (LocalTimeFromEpoch(static_cast<std::time_t>(instant_sec), local_tm)) {
        return FormatTmDateTime(local_tm, parsed.sub_val, parsed.sub_precision, ' ');
      }
    }
  }
  return std::nullopt;
}

std::string NormalizeTimestampForWrite(std::string_view input, std::string_view raw_original,
                                         std::string_view column_name,
                                         std::string_view column_type, Dialect dialect) {
  const std::string_view trimmed = detail::AdbcTrimAscii(input);
  if (trimmed.empty() || detail::AdbcEqualsIgnoreCase(trimmed, "NULL")) {
    return std::string(trimmed);
  }

  const std::string t_lower = detail::AdbcLowerAscii(column_type);
  const bool is_int_type = IsIntegerColumnType(column_type);
  const bool is_date_only = !is_int_type && t_lower.find("date") != std::string::npos &&
                            t_lower.find("datetime") == std::string::npos &&
                            t_lower.find("timestamp") == std::string::npos;
  const bool is_time_only = !is_int_type && t_lower.find("time") != std::string::npos &&
                            t_lower.find("timestamp") == std::string::npos &&
                            t_lower.find("datetime") == std::string::npos &&
                            t_lower.find("date") == std::string::npos;

  if (is_date_only) {
    if (detail::AdbcEqualsIgnoreCase(trimmed, "now")) {
      const auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
      std::tm local_tm{};
      if (LocalTimeFromEpoch(static_cast<std::time_t>(now_sec), local_tm)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", local_tm.tm_year + 1900,
                      local_tm.tm_mon + 1, local_tm.tm_mday);
        return std::string(buf);
      }
    }
    std::int32_t days = 0;
    if (detail::AdbcParseDateDays(trimmed, days)) {
      return detail::AdbcFormatDate(std::chrono::sys_days{std::chrono::days{days}});
    }
    return std::string(trimmed);
  }

  if (is_time_only) {
    if (detail::AdbcEqualsIgnoreCase(trimmed, "now")) {
      const auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
      std::tm local_tm{};
      if (LocalTimeFromEpoch(static_cast<std::time_t>(now_sec), local_tm)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", local_tm.tm_hour, local_tm.tm_min,
                      local_tm.tm_sec);
        return std::string(buf);
      }
    }
    return std::string(trimmed);
  }

  const bool hint = IsTimestampColumnHint(column_name, column_type);
  const auto orig_epoch_prec = DetectPlausibleUnixEpochPrecision(raw_original);
  const std::optional<int> target_epoch_prec =
      orig_epoch_prec.has_value()
          ? orig_epoch_prec
          : ((hint && is_int_type) ? std::optional<int>{0} : std::nullopt);

  if (target_epoch_prec.has_value()) {
    if (detail::AdbcEqualsIgnoreCase(trimmed, "now")) {
      const auto now = std::chrono::system_clock::now();
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch());
      const std::int64_t sec = std::chrono::duration_cast<std::chrono::seconds>(ns).count();
      const std::int64_t sub =
          RescaleSubSeconds((ns % std::chrono::seconds{1}).count(), 9, *target_epoch_prec);
      return std::to_string(sec * ScaleForPrecision(*target_epoch_prec) + sub);
    }
    if (IsStrictDigits(trimmed)) {
      return std::string(trimmed);
    }
    std::int64_t utc_sec = 0;
    std::int64_t sub_ns = 0;
    if (detail::AdbcParseTimestampInstant(trimmed, /*assume_local_if_naive=*/true, utc_sec,
                                          sub_ns)) {
      const std::int64_t sub = RescaleSubSeconds(sub_ns, 9, *target_epoch_prec);
      return std::to_string(utc_sec * ScaleForPrecision(*target_epoch_prec) + sub);
    }
    return std::string(input);
  }

  const bool is_mysql_datetime =
      (dialect == Dialect::Mysql) && t_lower.find("datetime") != std::string::npos;
  const bool is_mysql_timestamp = (dialect == Dialect::Mysql) &&
                                  t_lower.find("timestamp") != std::string::npos &&
                                  t_lower.find("datetime") == std::string::npos;
  const bool is_zoned_sql_type = is_mysql_timestamp || IsIsoZonedColumnType(column_type);
  const bool is_naive_sql_datetime =
      is_mysql_datetime ||
      (!is_zoned_sql_type && !is_int_type &&
       (t_lower.find("datetime") != std::string::npos ||
        t_lower.find("timestamp") != std::string::npos));

  if (is_mysql_datetime) {
    if (detail::AdbcEqualsIgnoreCase(trimmed, "now")) {
      const auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
      std::tm local_tm{};
      if (LocalTimeFromEpoch(static_cast<std::time_t>(now_sec), local_tm)) {
        return FormatTmDateTime(local_tm, 0, 0, ' ');
      }
    }
    std::string formatted;
    if (detail::AdbcParseNaiveDateTime(trimmed, formatted)) {
      return formatted;
    }
    return std::string(trimmed);
  }

  if (is_zoned_sql_type) {
    if (detail::AdbcEqualsIgnoreCase(trimmed, "now")) {
      const auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
      const std::string iso = FormatUtcIso(now_sec, 0, 0);
      if (!iso.empty()) {
        return iso;
      }
    }
    std::int64_t utc_sec = 0;
    std::int64_t sub_ns = 0;
    if (detail::AdbcParseTimestampInstant(trimmed, /*assume_local_if_naive=*/true, utc_sec,
                                          sub_ns)) {
      const int frac_prec = sub_ns > 0 ? 9 : 0;
      const std::string iso = FormatUtcIso(utc_sec, sub_ns, frac_prec);
      if (!iso.empty()) {
        return iso;
      }
    }
    return std::string(trimmed);
  }

  ParsedDateTime orig_dt;
  const bool orig_is_zoned_iso =
      ParseDateTimeInput(raw_original, orig_dt) && orig_dt.has_tz;
  if (orig_is_zoned_iso && (dialect == Dialect::Sqlite || dialect == Dialect::Generic)) {
    if (detail::AdbcEqualsIgnoreCase(trimmed, "now")) {
      const auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
      return FormatUtcLikeOriginal(now_sec, 0, 0, raw_original);
    }
    ParsedDateTime dt;
    if (ParseDateTimeInput(trimmed, dt)) {
      if (dt.has_tz) {
        return std::string(trimmed);
      }
      if (const auto utc_sec = LocalDateTimeToUtcEpochSeconds(dt)) {
        const std::string converted =
            FormatUtcLikeOriginal(*utc_sec, dt.sub_val, dt.sub_precision, raw_original);
        if (!converted.empty()) {
          return converted;
        }
      }
    }
  }

  if (is_naive_sql_datetime) {
    if (detail::AdbcEqualsIgnoreCase(trimmed, "now")) {
      const auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
      std::tm local_tm{};
      if (LocalTimeFromEpoch(static_cast<std::time_t>(now_sec), local_tm)) {
        return FormatTmDateTime(local_tm, 0, 0, ' ');
      }
    }
    std::string formatted;
    if (detail::AdbcParseNaiveDateTime(trimmed, formatted)) {
      return formatted;
    }
  }

  return std::string(input);
}

void ApplyResultTimestampLocalization(const std::vector<std::string>& columns,
                                      const std::vector<ColumnMeta>* meta,
                                      std::vector<GridRow>& rows) {
  for (std::size_t col = 0; col < columns.size(); ++col) {
    const std::string_view col_name = columns[col];
    const std::string_view col_type =
        (meta != nullptr && col < meta->size()) ? std::string_view((*meta)[col].type)
                                                : std::string_view{};
    for (GridRow& row : rows) {
      if (col >= row.values.size()) {
        continue;
      }
      CellValue& cell = row.values[col];
      if (cell.is_null || !cell.display_text.empty()) {
        continue;
      }
      if (auto localized = LocalizeTimestampValue(cell.text, col_name, col_type)) {
        cell.display_text = std::move(*localized);
      }
    }
  }
}

} // namespace dbpp
