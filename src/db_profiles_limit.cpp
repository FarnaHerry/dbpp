module;

#include <huxerui/datatable.h>
#include <huxerui/http.h>
#include <huxerui/huxerui.h>
#include <huxerui/task.h>
#include <sqlite3.h>

module dbpp.db;

import std;
import dbpp.settings;
import dbpp.i18n;
import dbpp.types;
import dbpp.adbc;
import dbpp.redis;

namespace dbpp {

using namespace huxerui;

namespace {

std::string EncodeProfileField(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '%': result += "%25"; break;
      case '\t': result += "%09"; break;
      case '\n': result += "%0A"; break;
      case '\r': result += "%0D"; break;
      default: result.push_back(c); break;
    }
  }
  return result;
}

std::string DecodeProfileField(std::string_view text) {
  const auto hex_value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string result;
  result.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int high = hex_value(text[i + 1]);
      const int low = hex_value(text[i + 2]);
      if (high >= 0 && low >= 0) {
        result.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    result.push_back(text[i]);
  }
  return result;
}

} // namespace

std::vector<ConnectionProfile> LoadConnectionProfilesFromTsv(const std::string& path) {
  std::vector<ConnectionProfile> profiles;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return profiles;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size() && fields.size() < 5) {
      const std::size_t tab = line.find('\t', start);
      if (tab == std::string::npos) {
        fields.emplace_back(line.data() + start, line.size() - start);
        break;
      }
      fields.emplace_back(line.data() + start, tab - start);
      start = tab + 1;
    }
    // Accept both legacy 4-column rows (no password) and 5-column rows (with password).
    if (fields.size() != 4 && fields.size() != 5) {
      continue;
    }
    ConnectionProfile profile;
    profile.name = DecodeProfileField(fields[0]);
    profile.driver_path = DecodeProfileField(fields[1]);
    profile.uri = DecodeProfileField(fields[2]);
    profile.username = DecodeProfileField(fields[3]);
    if (fields.size() == 5) {
      profile.password = DecodeProfileField(fields[4]);
    }
    if (!profile.driver_path.empty() && !profile.uri.empty()) {
      profiles.push_back(std::move(profile));
    }
  }
  return profiles;
}

bool SaveConnectionProfilesToTsv(const std::string& path,
                                 const std::vector<ConnectionProfile>& profiles) {
  const std::filesystem::path file_path(path);
  if (file_path.has_parent_path()) {
    std::error_code error;
    std::filesystem::create_directories(file_path.parent_path(), error);
  }
  const std::filesystem::path temp_path =
      file_path.parent_path() / (file_path.filename().string() + ".tmp");
  {
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      return false;
    }
    output << "# db++ adbc connections v2\n";
    for (const ConnectionProfile& profile : profiles) {
      output << EncodeProfileField(profile.name) << '\t'
             << EncodeProfileField(profile.driver_path) << '\t'
             << EncodeProfileField(profile.uri) << '\t'
             << EncodeProfileField(profile.username) << '\t'
             << EncodeProfileField(profile.password) << '\n';
    }
    output.flush();
    if (!output) {
      std::error_code ec;
      std::filesystem::remove(temp_path, ec);
      return false;
    }
  }
#if !defined(_WIN32)
  {
    std::error_code perm_ec;
    std::filesystem::permissions(
        temp_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, perm_ec);
  }
#endif
  std::error_code error;
  std::filesystem::rename(temp_path, file_path, error);
  if (error) {
    std::error_code remove_ec;
    std::filesystem::remove(file_path, remove_ec);
    error.clear();
    std::filesystem::rename(temp_path, file_path, error);
    if (error) {
      std::error_code cleanup_ec;
      std::filesystem::remove(temp_path, cleanup_ec);
      return false;
    }
  }
  return true;
}

struct SqliteDbGuard {
  sqlite3* db = nullptr;
  SqliteDbGuard() = default;
  SqliteDbGuard(const SqliteDbGuard&) = delete;
  SqliteDbGuard& operator=(const SqliteDbGuard&) = delete;
  ~SqliteDbGuard() {
    if (db != nullptr) {
      sqlite3_close_v2(db);
      db = nullptr;
    }
  }
};

struct SqliteStmtGuard {
  sqlite3_stmt* stmt = nullptr;
  SqliteStmtGuard() = default;
  SqliteStmtGuard(const SqliteStmtGuard&) = delete;
  SqliteStmtGuard& operator=(const SqliteStmtGuard&) = delete;
  ~SqliteStmtGuard() {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
      stmt = nullptr;
    }
  }
};

std::string ColumnTextString(sqlite3_stmt* stmt, int col) {
  const unsigned char* ptr = sqlite3_column_text(stmt, col);
  const int bytes = sqlite3_column_bytes(stmt, col);
  if (ptr == nullptr || bytes <= 0) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(ptr), static_cast<std::size_t>(bytes));
}

std::vector<ConnectionProfile> LoadConnectionProfiles(const std::string& path) {
  if (path.empty()) {
    return {};
  }
  if (detail::IsLegacyTsvPath(path)) {
    return LoadConnectionProfilesFromTsv(path);
  }

  std::error_code ec;
  const std::filesystem::path target(path);
  const bool db_exists = std::filesystem::is_regular_file(target, ec);
  const std::filesystem::path legacy_tsv = target.parent_path() / "connections.tsv";
  const std::filesystem::path legacy_settings_tsv = target.parent_path() / "settings.tsv";
  if (!db_exists) {
    if (std::filesystem::is_regular_file(legacy_tsv, ec) ||
        std::filesystem::is_regular_file(legacy_settings_tsv, ec)) {
      if (!detail::EnsureConfigSqliteSync(path)) {
        return LoadConnectionProfilesFromTsv(legacy_tsv.string());
      }
    } else {
      return {};
    }
  }

  SqliteDbGuard db_guard;
  if (sqlite3_open_v2(path.c_str(), &db_guard.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK ||
      db_guard.db == nullptr) {
    return {};
  }
  sqlite3_busy_timeout(db_guard.db, 5000);

  SqliteStmtGuard stmt_guard;
  std::vector<ConnectionProfile> profiles;
  if (sqlite3_prepare_v2(
          db_guard.db,
          "SELECT name, driver_path, uri, username, password, "
          "remark, read_only, production_mode, show_system_databases "
          "FROM connections ORDER BY sort_order ASC, id ASC;",
          -1, &stmt_guard.stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt_guard.stmt) == SQLITE_ROW) {
      ConnectionProfile profile;
      profile.name = ColumnTextString(stmt_guard.stmt, 0);
      profile.driver_path = ColumnTextString(stmt_guard.stmt, 1);
      profile.uri = ColumnTextString(stmt_guard.stmt, 2);
      profile.username = ColumnTextString(stmt_guard.stmt, 3);
      profile.password = ColumnTextString(stmt_guard.stmt, 4);
      profile.remark = ColumnTextString(stmt_guard.stmt, 5);
      profile.read_only = sqlite3_column_int(stmt_guard.stmt, 6) != 0;
      profile.production_mode = sqlite3_column_int(stmt_guard.stmt, 7) != 0;
      profile.show_system_databases = sqlite3_column_int(stmt_guard.stmt, 8) != 0;
      if (!profile.driver_path.empty() && !profile.uri.empty()) {
        profiles.push_back(std::move(profile));
      }
    }
  }
  return profiles;
}

bool SaveConnectionProfiles(const std::string& path,
                            const std::vector<ConnectionProfile>& profiles) {
  if (path.empty()) {
    return false;
  }
  if (detail::IsLegacyTsvPath(path)) {
    return SaveConnectionProfilesToTsv(path, profiles);
  }
  if (!detail::EnsureConfigSqliteSync(path)) {
    return false;
  }

  bool ok = false;
  {
    SqliteDbGuard db_guard;
    if (sqlite3_open_v2(path.c_str(), &db_guard.db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK ||
        db_guard.db == nullptr) {
      return false;
    }
    sqlite3_busy_timeout(db_guard.db, 5000);

    ok = sqlite3_exec(db_guard.db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
    if (ok) {
      ok = sqlite3_exec(db_guard.db, "DELETE FROM connections;", nullptr, nullptr, nullptr) ==
           SQLITE_OK;
    }
    if (ok && !profiles.empty()) {
      SqliteStmtGuard stmt_guard;
      if (sqlite3_prepare_v2(
              db_guard.db,
              "INSERT INTO connections "
              "(sort_order, name, driver_path, uri, username, password, "
              "remark, read_only, production_mode, show_system_databases) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
              -1, &stmt_guard.stmt, nullptr) != SQLITE_OK) {
        ok = false;
      } else {
        for (std::size_t i = 0; i < profiles.size(); ++i) {
          const auto& p = profiles[i];
          sqlite3_reset(stmt_guard.stmt);
          sqlite3_clear_bindings(stmt_guard.stmt);
          sqlite3_bind_int64(stmt_guard.stmt, 1, static_cast<sqlite3_int64>(i));
          sqlite3_bind_text(stmt_guard.stmt, 2, p.name.data(), static_cast<int>(p.name.size()),
                            SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_guard.stmt, 3, p.driver_path.data(),
                            static_cast<int>(p.driver_path.size()), SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_guard.stmt, 4, p.uri.data(), static_cast<int>(p.uri.size()),
                            SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_guard.stmt, 5, p.username.data(),
                            static_cast<int>(p.username.size()), SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_guard.stmt, 6, p.password.data(),
                            static_cast<int>(p.password.size()), SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_guard.stmt, 7, p.remark.data(),
                            static_cast<int>(p.remark.size()), SQLITE_TRANSIENT);
          sqlite3_bind_int(stmt_guard.stmt, 8, p.read_only ? 1 : 0);
          sqlite3_bind_int(stmt_guard.stmt, 9, p.production_mode ? 1 : 0);
          sqlite3_bind_int(stmt_guard.stmt, 10, p.show_system_databases ? 1 : 0);
          if (sqlite3_step(stmt_guard.stmt) != SQLITE_DONE) {
            ok = false;
            break;
          }
        }
      }
    }
    if (ok) {
      ok = sqlite3_exec(db_guard.db, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    if (!ok) {
      sqlite3_exec(db_guard.db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
  }
  if (ok) {
    detail::RestrictConfigFilePermissions(path);
  }
  return ok;
}

std::string SavedQueryConnectionKey(std::string_view driver_path, std::string_view uri) {
  return ConnectionFailureKey(driver_path, uri);
}

std::vector<SavedQuery> LoadSavedQueries(const std::string& path) {
  if (path.empty() || detail::IsLegacyTsvPath(path)) {
    return {};
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return {};
  }
  SqliteDbGuard db_guard;
  if (sqlite3_open_v2(path.c_str(), &db_guard.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK ||
      db_guard.db == nullptr) {
    return {};
  }
  sqlite3_busy_timeout(db_guard.db, 5000);

  SqliteStmtGuard stmt_guard;
  std::vector<SavedQuery> queries;
  if (sqlite3_prepare_v2(
          db_guard.db,
          "SELECT id, connection_key, database_name, name, sql_text "
          "FROM saved_queries ORDER BY sort_order ASC, id ASC;",
          -1, &stmt_guard.stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt_guard.stmt) == SQLITE_ROW) {
      SavedQuery q;
      q.id = sqlite3_column_int64(stmt_guard.stmt, 0);
      q.connection_key = ColumnTextString(stmt_guard.stmt, 1);
      q.database_name = ColumnTextString(stmt_guard.stmt, 2);
      q.name = ColumnTextString(stmt_guard.stmt, 3);
      q.sql_text = ColumnTextString(stmt_guard.stmt, 4);
      if (!q.name.empty()) {
        queries.push_back(std::move(q));
      }
    }
  }
  return queries;
}

bool SaveSavedQueries(const std::string& path, const std::vector<SavedQuery>& queries) {
  if (path.empty() || detail::IsLegacyTsvPath(path)) {
    return false;
  }
  if (!detail::EnsureConfigSqliteSync(path)) {
    return false;
  }
  bool ok = false;
  {
    SqliteDbGuard db_guard;
    if (sqlite3_open_v2(path.c_str(), &db_guard.db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK ||
        db_guard.db == nullptr) {
      return false;
    }
    sqlite3_busy_timeout(db_guard.db, 5000);
    ok = sqlite3_exec(db_guard.db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
    if (ok) {
      ok = sqlite3_exec(db_guard.db, "DELETE FROM saved_queries;", nullptr, nullptr, nullptr) ==
           SQLITE_OK;
    }
    if (ok && !queries.empty()) {
      SqliteStmtGuard stmt_guard;
      if (sqlite3_prepare_v2(
              db_guard.db,
              "INSERT INTO saved_queries "
              "(sort_order, connection_key, database_name, name, sql_text) "
              "VALUES (?, ?, ?, ?, ?);",
              -1, &stmt_guard.stmt, nullptr) != SQLITE_OK) {
        ok = false;
      } else {
        for (std::size_t i = 0; i < queries.size(); ++i) {
          const SavedQuery& q = queries[i];
          sqlite3_reset(stmt_guard.stmt);
          sqlite3_clear_bindings(stmt_guard.stmt);
          sqlite3_bind_int64(stmt_guard.stmt, 1, static_cast<sqlite3_int64>(i));
          sqlite3_bind_text(stmt_guard.stmt, 2, q.connection_key.data(),
                            static_cast<int>(q.connection_key.size()), SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_guard.stmt, 3, q.database_name.data(),
                            static_cast<int>(q.database_name.size()), SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_guard.stmt, 4, q.name.data(), static_cast<int>(q.name.size()),
                            SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_guard.stmt, 5, q.sql_text.data(),
                            static_cast<int>(q.sql_text.size()), SQLITE_TRANSIENT);
          if (sqlite3_step(stmt_guard.stmt) != SQLITE_DONE) {
            ok = false;
            break;
          }
        }
      }
    }
    if (ok) {
      ok = sqlite3_exec(db_guard.db, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    if (!ok) {
      sqlite3_exec(db_guard.db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
  }
  if (ok) {
    detail::RestrictConfigFilePermissions(path);
  }
  return ok;
}

SqlLimitRewriteResult ApplyImplicitSqlLimit(Dialect dialect, std::string_view sql,
                                            std::int64_t page_size, std::int64_t page) {
  if (dialect == Dialect::Redis || page_size <= 0) {
    return {std::string(sql), false, 0, 0};
  }

  struct Depth0Token {
    bool is_word = false;
    std::string upper;
  };
  std::vector<Depth0Token> depth0_tokens;
  int depth = 0;
  bool seen_semicolon = false;
  std::size_t body_end = 0;
  std::size_t i = 0;
  const std::size_t n = sql.size();

  const auto is_word_start = [](unsigned char ch) {
    return std::isalpha(ch) != 0 || ch == '_';
  };
  const auto is_word_char = [](unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '_';
  };

  while (i < n) {
    const unsigned char ch = static_cast<unsigned char>(sql[i]);
    if (std::isspace(ch) != 0) {
      ++i;
      continue;
    }
    if (ch == '-' && i + 1 < n && sql[i + 1] == '-') {
      i += 2;
      while (i < n && sql[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (ch == '/' && i + 1 < n && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) {
        ++i;
      }
      i = (i + 1 < n) ? (i + 2) : n;
      continue;
    }

    if (depth == 0 && ch == ';') {
      seen_semicolon = true;
      ++i;
      continue;
    }
    if (seen_semicolon) {
      return {std::string(sql), false, 0};
    }

    if (ch == '\'') {
      ++i;
      while (i < n) {
        if (dialect == Dialect::Mysql && sql[i] == '\\' && i + 1 < n) {
          i += 2;
          continue;
        }
        if (sql[i] == '\'') {
          if (i + 1 < n && sql[i + 1] == '\'') {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      body_end = i;
      if (depth == 0) {
        depth0_tokens.push_back({false, {}});
      }
      continue;
    }

    if (ch == '"' || ch == '`') {
      const char quote = static_cast<char>(ch);
      ++i;
      while (i < n) {
        if (sql[i] == quote) {
          if (i + 1 < n && sql[i + 1] == quote) {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      body_end = i;
      if (depth == 0) {
        depth0_tokens.push_back({false, {}});
      }
      continue;
    }

    if (ch == '[') {
      ++i;
      while (i < n) {
        if (sql[i] == ']') {
          if (i + 1 < n && sql[i + 1] == ']') {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      body_end = i;
      if (depth == 0) {
        depth0_tokens.push_back({false, {}});
      }
      continue;
    }

    if (ch == '$') {
      std::size_t tag_end = i + 1;
      while (tag_end < n && is_word_char(static_cast<unsigned char>(sql[tag_end]))) {
        ++tag_end;
      }
      if (tag_end < n && sql[tag_end] == '$') {
        const std::string_view tag = sql.substr(i, tag_end - i + 1);
        const std::size_t close_pos = sql.find(tag, tag_end + 1);
        if (close_pos != std::string_view::npos) {
          i = close_pos + tag.size();
          body_end = i;
          if (depth == 0) {
            depth0_tokens.push_back({false, {}});
          }
          continue;
        }
      }
    }

    if (ch == '(') {
      if (depth == 0) {
        depth0_tokens.push_back({false, {}});
      }
      ++depth;
      ++i;
      body_end = i;
      continue;
    }

    if (ch == ')') {
      if (depth > 0) {
        --depth;
      }
      ++i;
      body_end = i;
      if (depth == 0) {
        depth0_tokens.push_back({false, {}});
      }
      continue;
    }

    if (is_word_start(ch)) {
      const std::size_t start = i;
      ++i;
      while (i < n && is_word_char(static_cast<unsigned char>(sql[i]))) {
        ++i;
      }
      body_end = i;
      if (depth == 0) {
        depth0_tokens.push_back({true, detail::DbUpperAscii(sql.substr(start, i - start))});
      }
      continue;
    }

    ++i;
    body_end = i;
    if (depth == 0) {
      depth0_tokens.push_back({false, {}});
    }
  }

  if (depth0_tokens.empty() || !depth0_tokens.front().is_word || body_end == 0) {
    return {std::string(sql), false, 0};
  }

  const auto is_select_top = [&](std::size_t select_index) {
    if (select_index + 1 >= depth0_tokens.size() || !depth0_tokens[select_index + 1].is_word) {
      return false;
    }
    const std::string& next = depth0_tokens[select_index + 1].upper;
    if (next == "TOP") {
      return true;
    }
    if ((next == "DISTINCT" || next == "ALL") && select_index + 2 < depth0_tokens.size() &&
        depth0_tokens[select_index + 2].is_word &&
        depth0_tokens[select_index + 2].upper == "TOP") {
      return true;
    }
    return false;
  };

  const std::string& first = depth0_tokens.front().upper;
  if (first == "SELECT") {
    if (is_select_top(0)) {
      return {std::string(sql), false, 0};
    }
  } else if (first == "WITH") {
    bool found_read_verb = false;
    for (std::size_t idx = 1; idx < depth0_tokens.size(); ++idx) {
      if (!depth0_tokens[idx].is_word) {
        continue;
      }
      const std::string& word = depth0_tokens[idx].upper;
      if (word == "SELECT" || word == "TABLE" || word == "VALUES") {
        if (word == "SELECT" && is_select_top(idx)) {
          return {std::string(sql), false, 0};
        }
        found_read_verb = true;
        break;
      }
      if (word == "INSERT" || word == "UPDATE" || word == "DELETE" || word == "MERGE") {
        return {std::string(sql), false, 0};
      }
    }
    if (!found_read_verb) {
      return {std::string(sql), false, 0};
    }
  } else if (first != "TABLE") {
    return {std::string(sql), false, 0};
  }

  for (const Depth0Token& token : depth0_tokens) {
    if (!token.is_word) {
      continue;
    }
    if (token.upper == "LIMIT" || token.upper == "FETCH" || token.upper == "OFFSET" ||
        token.upper == "FOR" || token.upper == "INTO" || token.upper == "LOCK") {
      return {std::string(sql), false, 0};
    }
  }

  const std::int64_t limit = std::clamp(page_size, kMinPageSize, kMaxPageSize);
  const std::int64_t clamped_page = std::max<std::int64_t>(0, page);
  const std::int64_t offset =
      (clamped_page > 0 && limit > std::numeric_limits<std::int64_t>::max() / clamped_page)
          ? std::numeric_limits<std::int64_t>::max()
          : clamped_page * limit;
  std::string_view body = sql.substr(0, body_end);
  while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back())) != 0) {
    body.remove_suffix(1);
  }
  if (body.empty()) {
    return {std::string(sql), false, 0, 0};
  }
  std::string rewritten_sql = std::string(body) + "\nLIMIT " + std::to_string(limit + 1);
  if (offset > 0) {
    rewritten_sql += " OFFSET " + std::to_string(offset);
  }
  return SqlLimitRewriteResult{
      .sql = std::move(rewritten_sql),
      .applied = true,
      .effective_limit = limit,
      .effective_offset = offset,
  };
}

std::string FormatExecutionDuration(std::int64_t elapsed_us) {
  if (elapsed_us < 0) {
    return {};
  }
  if (elapsed_us < 10000) {
    const std::int64_t tenths = (elapsed_us + 50) / 100;
    if (tenths >= 100) {
      return std::to_string(tenths / 10) + " ms";
    }
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " ms";
  }
  if (elapsed_us < 1000000) {
    const std::int64_t millis = (elapsed_us + 500) / 1000;
    if (millis >= 1000) {
      return "1.00 s";
    }
    return std::to_string(millis) + " ms";
  }
  const std::int64_t hundredths = (elapsed_us + 5000) / 10000;
  const std::int64_t secs = hundredths / 100;
  const std::int64_t frac = hundredths % 100;
  std::string frac_str = std::to_string(frac);
  if (frac < 10) {
    frac_str = "0" + frac_str;
  }
  return std::to_string(secs) + "." + frac_str + " s";
}

std::string CompactSqlPreview(std::string_view sql, std::size_t max_bytes) {
  std::string out;
  out.reserve(std::min<std::size_t>(sql.size(), max_bytes == 0 ? 0 : max_bytes + 3));
  bool prev_space = true;
  for (const unsigned char ch : sql) {
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f') {
      if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
      continue;
    }
    out.push_back(static_cast<char>(ch));
    prev_space = false;
  }
  if (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  if (max_bytes > 0 && out.size() > max_bytes) {
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0U) == 0x80U) {
      --cut;
    }
    out.resize(cut);
    while (!out.empty() && out.back() == ' ') {
      out.pop_back();
    }
    out += "...";
  }
  return out;
}

} // namespace dbpp
