module;

#include <huxerui/datatable.h>

module dbpp.adbc;

import std;
import dbpp.types;

namespace dbpp {

namespace {

enum class SqlTokKind {
  Word,
  Number,
  String,
  QuotedIdent,
  DollarString,
  LineComment,
  BlockComment,
  LParen,
  RParen,
  Comma,
  Semicolon,
  Dot,
  Symbol,
};

struct SqlToken {
  SqlTokKind kind = SqlTokKind::Word;
  std::string text;
  std::string upper;
};

bool IsIdentStartChar(unsigned char c) noexcept {
  return std::isalpha(c) != 0 || c == '_' || c >= 0x80U;
}

bool IsIdentContinueChar(unsigned char c) noexcept {
  return std::isalnum(c) != 0 || c == '_' || c == '$' || c >= 0x80U;
}

std::string ToAsciiUpper(std::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return out;
}

std::string MatchDollarTag(std::string_view sql, std::size_t pos) {
  if (pos >= sql.size() || sql[pos] != '$') {
    return {};
  }
  std::size_t j = pos + 1;
  while (j < sql.size()) {
    const unsigned char c = static_cast<unsigned char>(sql[j]);
    if (sql[j] == '$') {
      return std::string(sql.substr(pos, j - pos + 1));
    }
    if (std::isalnum(c) == 0 && sql[j] != '_') {
      return {};
    }
    ++j;
  }
  return {};
}

std::vector<SqlToken> TokenizeSql(std::string_view sql, Dialect dialect) {
  std::vector<SqlToken> tokens;
  std::size_t i = 0;
  while (i < sql.size()) {
    const unsigned char c = static_cast<unsigned char>(sql[i]);
    if (std::isspace(c) != 0) {
      ++i;
      continue;
    }

    // Line comment: -- ...
    if (sql[i] == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      const std::size_t start = i;
      i += 2;
      while (i < sql.size() && sql[i] != '\n' && sql[i] != '\r') {
        ++i;
      }
      tokens.push_back(SqlToken{
          .kind = SqlTokKind::LineComment,
          .text = std::string(sql.substr(start, i - start)),
          .upper = {},
      });
      continue;
    }

    // Block comment: /* ... */
    if (sql[i] == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      const std::size_t start = i;
      i += 2;
      while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) {
        ++i;
      }
      i = (i + 1 < sql.size()) ? i + 2 : sql.size();
      tokens.push_back(SqlToken{
          .kind = SqlTokKind::BlockComment,
          .text = std::string(sql.substr(start, i - start)),
          .upper = {},
      });
      continue;
    }

    // Dollar-quoted literal (PostgreSQL)
    if (sql[i] == '$') {
      const std::string tag = MatchDollarTag(sql, i);
      if (!tag.empty()) {
        const std::size_t start = i;
        const std::size_t close = sql.find(tag, i + tag.size());
        i = (close == std::string_view::npos) ? sql.size() : (close + tag.size());
        tokens.push_back(SqlToken{
            .kind = SqlTokKind::DollarString,
            .text = std::string(sql.substr(start, i - start)),
            .upper = {},
        });
        continue;
      }
    }

    // Single-quoted literal
    if (sql[i] == '\'') {
      const std::size_t start = i++;
      while (i < sql.size()) {
        if (dialect == Dialect::Mysql && sql[i] == '\\' && i + 1 < sql.size()) {
          i += 2;
          continue;
        }
        if (sql[i] == '\'') {
          if (i + 1 < sql.size() && sql[i + 1] == '\'') {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      tokens.push_back(SqlToken{
          .kind = SqlTokKind::String,
          .text = std::string(sql.substr(start, i - start)),
          .upper = {},
      });
      continue;
    }

    // Quoted identifiers: "..." or `...` or [...]
    if (sql[i] == '"' || sql[i] == '`' || (sql[i] == '[' && dialect != Dialect::Postgresql)) {
      const char close = (sql[i] == '[') ? ']' : sql[i];
      const std::size_t start = i++;
      while (i < sql.size()) {
        if (sql[i] == close) {
          if (close != ']' && i + 1 < sql.size() && sql[i + 1] == close) {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      tokens.push_back(SqlToken{
          .kind = SqlTokKind::QuotedIdent,
          .text = std::string(sql.substr(start, i - start)),
          .upper = {},
      });
      continue;
    }

    // Punctuation
    if (sql[i] == '(') {
      tokens.push_back(SqlToken{.kind = SqlTokKind::LParen, .text = "(", .upper = {}});
      ++i;
      continue;
    }
    if (sql[i] == ')') {
      tokens.push_back(SqlToken{.kind = SqlTokKind::RParen, .text = ")", .upper = {}});
      ++i;
      continue;
    }
    if (sql[i] == ',') {
      tokens.push_back(SqlToken{.kind = SqlTokKind::Comma, .text = ",", .upper = {}});
      ++i;
      continue;
    }
    if (sql[i] == ';') {
      tokens.push_back(SqlToken{.kind = SqlTokKind::Semicolon, .text = ";", .upper = {}});
      ++i;
      continue;
    }
    if (sql[i] == '.' && !(i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1])))) {
      tokens.push_back(SqlToken{.kind = SqlTokKind::Dot, .text = ".", .upper = {}});
      ++i;
      continue;
    }

    // Numbers
    if (std::isdigit(c) != 0 ||
        (sql[i] == '.' && i + 1 < sql.size() &&
         std::isdigit(static_cast<unsigned char>(sql[i + 1])) != 0)) {
      const std::size_t start = i;
      if (sql[i] == '0' && i + 1 < sql.size() && (sql[i + 1] == 'x' || sql[i + 1] == 'X')) {
        i += 2;
        while (i < sql.size() && std::isxdigit(static_cast<unsigned char>(sql[i])) != 0) {
          ++i;
        }
      } else {
        bool seen_dot = (sql[i] == '.');
        ++i;
        while (i < sql.size()) {
          if (std::isdigit(static_cast<unsigned char>(sql[i])) != 0) {
            ++i;
          } else if (sql[i] == '.' && !seen_dot) {
            seen_dot = true;
            ++i;
          } else if ((sql[i] == 'e' || sql[i] == 'E') && i + 1 < sql.size()) {
            ++i;
            if (sql[i] == '+' || sql[i] == '-') {
              ++i;
            }
          } else {
            break;
          }
        }
      }
      tokens.push_back(SqlToken{
          .kind = SqlTokKind::Number,
          .text = std::string(sql.substr(start, i - start)),
          .upper = {},
      });
      continue;
    }

    // Words (identifiers / keywords)
    if (IsIdentStartChar(c)) {
      const std::size_t start = i++;
      while (i < sql.size() && IsIdentContinueChar(static_cast<unsigned char>(sql[i]))) {
        ++i;
      }
      std::string raw(sql.substr(start, i - start));
      std::string upper = ToAsciiUpper(raw);
      tokens.push_back(SqlToken{
          .kind = SqlTokKind::Word,
          .text = std::move(raw),
          .upper = std::move(upper),
      });
      continue;
    }

    // Multi-char operators
    if (i + 2 < sql.size() && sql.substr(i, 3) == "->>") {
      tokens.push_back(SqlToken{.kind = SqlTokKind::Symbol, .text = "->>", .upper = {}});
      i += 3;
      continue;
    }
    if (i + 1 < sql.size()) {
      const std::string_view two = sql.substr(i, 2);
      if (two == "::" || two == "<>" || two == "!=" || two == "<=" || two == ">=" || two == "||" ||
          two == "->") {
        tokens.push_back(
            SqlToken{.kind = SqlTokKind::Symbol, .text = std::string(two), .upper = {}});
        i += 2;
        continue;
      }
    }

    tokens.push_back(
        SqlToken{.kind = SqlTokKind::Symbol, .text = std::string(1, sql[i]), .upper = {}});
    ++i;
  }
  return tokens;
}

bool IsWord(const SqlToken& tok, std::string_view upper) noexcept {
  return tok.kind == SqlTokKind::Word && tok.upper == upper;
}

bool KeywordExpectsSpaceBeforeLParen(std::string_view upper) noexcept {
  static constexpr std::array<std::string_view, 25> kKeywords = {
      "IN",      "NOT",    "AND",    "OR",     "ON",     "AS",      "CHECK",
      "KEY",     "UNIQUE", "USING",  "WHERE",  "WHEN",   "SET",     "SELECT",
      "FROM",    "JOIN",   "EXISTS", "CASE",   "BY",     "VALUES",  "TABLE",
      "INTO",    "CONSTRAINT", "OVER", "FILTER",
  };
  return std::ranges::find(kKeywords, upper) != kKeywords.end();
}

bool NeedsSpaceBetween(const SqlToken& prev, const SqlToken& cur,
                       const SqlToken* prev_prev) noexcept {
  if (prev.kind == SqlTokKind::LineComment) {
    return false;
  }
  if (prev.kind == SqlTokKind::LParen || cur.kind == SqlTokKind::RParen ||
      cur.kind == SqlTokKind::Comma || cur.kind == SqlTokKind::Semicolon ||
      prev.kind == SqlTokKind::Dot || cur.kind == SqlTokKind::Dot) {
    return false;
  }
  if ((prev.kind == SqlTokKind::Symbol && prev.text == "::") ||
      (cur.kind == SqlTokKind::Symbol && cur.text == "::")) {
    return false;
  }
  if (cur.kind == SqlTokKind::LParen) {
    return prev.kind == SqlTokKind::Word && KeywordExpectsSpaceBeforeLParen(prev.upper);
  }
  // Unary + or - attached to a number
  if (prev.kind == SqlTokKind::Symbol && (prev.text == "-" || prev.text == "+") &&
      cur.kind == SqlTokKind::Number) {
    if (prev_prev == nullptr || prev_prev->kind == SqlTokKind::LParen ||
        prev_prev->kind == SqlTokKind::Comma || prev_prev->kind == SqlTokKind::Symbol ||
        IsWord(*prev_prev, "DEFAULT") || IsWord(*prev_prev, "VALUES") ||
        IsWord(*prev_prev, "RETURN")) {
      return false;
    }
  }
  return true;
}

std::string JoinTokenSlice(const std::vector<SqlToken>& tokens, std::size_t begin, std::size_t end,
                           std::size_t indent_spaces) {
  std::string out;
  const SqlToken* prev = nullptr;
  const SqlToken* prev_prev = nullptr;
  bool at_line_start = false;
  for (std::size_t i = begin; i < end; ++i) {
    const SqlToken& cur = tokens[i];
    if (at_line_start) {
      out.append(indent_spaces, ' ');
      at_line_start = false;
      prev = nullptr;
      prev_prev = nullptr;
    } else if (prev != nullptr && NeedsSpaceBetween(*prev, cur, prev_prev)) {
      out.push_back(' ');
    }
    out += cur.text;
    if (cur.kind == SqlTokKind::LineComment) {
      out.push_back('\n');
      at_line_start = true;
    }
    prev_prev = prev;
    prev = &cur;
  }
  return out;
}

std::optional<std::size_t> MatchClauseBreak(const std::vector<SqlToken>& tokens, std::size_t idx,
                                            std::size_t end) {
  if (idx >= end || tokens[idx].kind != SqlTokKind::Word) {
    return std::nullopt;
  }
  const std::string& w = tokens[idx].upper;
  if (w == "SELECT" || w == "FROM" || w == "WHERE" || w == "HAVING" || w == "LIMIT" ||
      w == "OFFSET" || w == "VALUES" || w == "SET" || w == "RETURNING" || w == "INTERSECT" ||
      w == "EXCEPT" || w == "JOIN") {
    return 1;
  }
  if (idx + 1 < end && tokens[idx + 1].kind == SqlTokKind::Word) {
    const std::string& w2 = tokens[idx + 1].upper;
    if ((w == "GROUP" || w == "ORDER" || w == "PARTITION") && w2 == "BY") {
      return 2;
    }
    if (w == "UNION" && w2 == "ALL") {
      return 2;
    }
    if (w == "ON" && w2 == "CONFLICT") {
      return 2;
    }
    if ((w == "LEFT" || w == "RIGHT" || w == "INNER" || w == "FULL" || w == "CROSS" ||
         w == "NATURAL") &&
        w2 == "JOIN") {
      return 2;
    }
    if (idx + 2 < end && tokens[idx + 2].kind == SqlTokKind::Word &&
        (w == "LEFT" || w == "RIGHT" || w == "FULL") && w2 == "OUTER" &&
        tokens[idx + 2].upper == "JOIN") {
      return 3;
    }
    if (idx + 3 < end && w == "ON" && w2 == "DUPLICATE" &&
        IsWord(tokens[idx + 2], "KEY") && IsWord(tokens[idx + 3], "UPDATE")) {
      return 4;
    }
  }
  if (w == "UNION") {
    return 1;
  }
  return std::nullopt;
}

std::string FormatClauseRange(const std::vector<SqlToken>& tokens, std::size_t begin,
                              std::size_t end, std::size_t base_indent) {
  if (begin >= end) {
    return {};
  }
  std::string out;
  int paren_depth = 0;
  std::size_t line_start = begin;
  bool first_line = true;

  const auto flush_line = [&](std::size_t line_end, bool is_first) {
    if (line_start >= line_end) {
      return;
    }
    const std::size_t indent = is_first ? base_indent : (base_indent + 2);
    if (!first_line) {
      out.push_back('\n');
    }
    out.append(indent, ' ');
    out += JoinTokenSlice(tokens, line_start, line_end, indent);
    first_line = false;
  };

  for (std::size_t i = begin; i < end; ++i) {
    if (tokens[i].kind == SqlTokKind::LParen) {
      ++paren_depth;
    } else if (tokens[i].kind == SqlTokKind::RParen) {
      if (paren_depth > 0) {
        --paren_depth;
      }
    } else if (paren_depth == 0 && i > line_start) {
      if (const auto len = MatchClauseBreak(tokens, i, end)) {
        flush_line(i, first_line);
        line_start = i;
        i += (*len - 1);
      }
    }
  }
  flush_line(end, first_line);
  return out;
}

std::size_t FindMatchingRParen(const std::vector<SqlToken>& tokens, std::size_t lparen_idx,
                               std::size_t end) {
  int depth = 0;
  for (std::size_t i = lparen_idx; i < end; ++i) {
    if (tokens[i].kind == SqlTokKind::LParen) {
      ++depth;
    } else if (tokens[i].kind == SqlTokKind::RParen) {
      --depth;
      if (depth == 0) {
        return i;
      }
    }
  }
  return end;
}

std::string FormatCreateTable(const std::vector<SqlToken>& tokens, std::size_t end,
                              bool had_semicolon) {
  std::optional<std::size_t> lparen_pos;
  std::optional<std::size_t> as_pos;
  int depth = 0;
  for (std::size_t i = 0; i < end; ++i) {
    if (tokens[i].kind == SqlTokKind::LParen) {
      if (depth == 0 && !lparen_pos.has_value()) {
        lparen_pos = i;
        break;
      }
      ++depth;
    } else if (tokens[i].kind == SqlTokKind::RParen) {
      if (depth > 0) --depth;
    } else if (depth == 0 && IsWord(tokens[i], "AS")) {
      as_pos = i;
      break;
    }
  }

  if (as_pos.has_value()) {
    std::string out = JoinTokenSlice(tokens, 0, *as_pos + 1, 0);
    if (*as_pos + 1 < end) {
      out += "\n";
      out += FormatClauseRange(tokens, *as_pos + 1, end, 0);
    }
    if (had_semicolon) {
      out.push_back(';');
    }
    return out;
  }

  if (!lparen_pos.has_value()) {
    std::string out = JoinTokenSlice(tokens, 0, end, 0);
    if (had_semicolon) {
      out.push_back(';');
    }
    return out;
  }

  const std::size_t rparen_pos = FindMatchingRParen(tokens, *lparen_pos, end);
  if (rparen_pos >= end) {
    std::string out = JoinTokenSlice(tokens, 0, end, 0);
    if (had_semicolon) {
      out.push_back(';');
    }
    return out;
  }

  std::string out = JoinTokenSlice(tokens, 0, *lparen_pos, 0);
  out += " (\n";

  // Split top-level items inside CREATE TABLE (...)
  std::size_t item_start = *lparen_pos + 1;
  std::vector<std::string> items;
  int inner_depth = 0;
  for (std::size_t i = *lparen_pos + 1; i < rparen_pos; ++i) {
    if (tokens[i].kind == SqlTokKind::LParen) {
      ++inner_depth;
    } else if (tokens[i].kind == SqlTokKind::RParen) {
      if (inner_depth > 0) --inner_depth;
    } else if (tokens[i].kind == SqlTokKind::Comma && inner_depth == 0) {
      std::string piece = JoinTokenSlice(tokens, item_start, i, 2);
      if (!piece.empty()) {
        items.push_back(std::move(piece));
      }
      item_start = i + 1;
    }
  }
  if (item_start < rparen_pos) {
    std::string tail = JoinTokenSlice(tokens, item_start, rparen_pos, 2);
    if (!tail.empty()) {
      items.push_back(std::move(tail));
    }
  }

  for (std::size_t i = 0; i < items.size(); ++i) {
    out += "  ";
    out += items[i];
    if (i + 1 < items.size()) {
      out.push_back(',');
    }
    out.push_back('\n');
  }
  out.push_back(')');
  if (rparen_pos + 1 < end) {
    out.push_back(' ');
    out += JoinTokenSlice(tokens, rparen_pos + 1, end, 0);
  }
  if (had_semicolon) {
    out.push_back(';');
  }
  return out;
}

std::string FormatCreateTrigger(const std::vector<SqlToken>& tokens, std::size_t end,
                                bool had_semicolon) {
  std::optional<std::size_t> begin_idx;
  int depth = 0;
  for (std::size_t i = 0; i < end; ++i) {
    if (tokens[i].kind == SqlTokKind::LParen) {
      ++depth;
    } else if (tokens[i].kind == SqlTokKind::RParen) {
      if (depth > 0) --depth;
    } else if (depth == 0 && IsWord(tokens[i], "BEGIN")) {
      begin_idx = i;
      break;
    }
  }

  if (!begin_idx.has_value()) {
    std::string out = JoinTokenSlice(tokens, 0, end, 0);
    if (had_semicolon) out.push_back(';');
    return out;
  }

  // Find matching END outside CASE ... END
  std::optional<std::size_t> end_idx;
  int case_depth = 0;
  depth = 0;
  for (std::size_t i = *begin_idx + 1; i < end; ++i) {
    if (tokens[i].kind == SqlTokKind::LParen) {
      ++depth;
    } else if (tokens[i].kind == SqlTokKind::RParen) {
      if (depth > 0) --depth;
    } else if (depth == 0) {
      if (IsWord(tokens[i], "CASE")) {
        ++case_depth;
      } else if (IsWord(tokens[i], "END")) {
        if (case_depth > 0) {
          --case_depth;
        } else {
          end_idx = i;
          break;
        }
      }
    }
  }
  if (!end_idx.has_value()) {
    std::string out = JoinTokenSlice(tokens, 0, end, 0);
    if (had_semicolon) out.push_back(';');
    return out;
  }

  // Format header lines before BEGIN
  std::string out;
  std::size_t seg_start = 0;
  bool first_hdr = true;
  depth = 0;
  for (std::size_t i = 1; i < *begin_idx; ++i) {
    if (tokens[i].kind == SqlTokKind::LParen) {
      ++depth;
    } else if (tokens[i].kind == SqlTokKind::RParen) {
      if (depth > 0) --depth;
    } else if (depth == 0) {
      const bool break_kw =
          IsWord(tokens[i], "BEFORE") || IsWord(tokens[i], "AFTER") ||
          IsWord(tokens[i], "INSTEAD") || IsWord(tokens[i], "WHEN") ||
          (i + 1 < *begin_idx && IsWord(tokens[i], "FOR") && IsWord(tokens[i + 1], "EACH"));
      if (break_kw && i > seg_start) {
        if (!first_hdr) out.push_back('\n');
        const std::size_t indent = first_hdr ? 0 : 2;
        out.append(indent, ' ');
        out += JoinTokenSlice(tokens, seg_start, i, indent);
        first_hdr = false;
        seg_start = i;
      }
    }
  }
  if (seg_start < *begin_idx) {
    if (!first_hdr) out.push_back('\n');
    const std::size_t indent = first_hdr ? 0 : 2;
    out.append(indent, ' ');
    out += JoinTokenSlice(tokens, seg_start, *begin_idx, indent);
  }

  out += "\nBEGIN\n";

  // Split statements inside BEGIN ... END by top-level ';'
  std::size_t stmt_start = *begin_idx + 1;
  int body_paren = 0;
  for (std::size_t i = *begin_idx + 1; i < *end_idx; ++i) {
    if (tokens[i].kind == SqlTokKind::LParen) {
      ++body_paren;
    } else if (tokens[i].kind == SqlTokKind::RParen) {
      if (body_paren > 0) --body_paren;
    } else if (body_paren == 0 && tokens[i].kind == SqlTokKind::Semicolon) {
      if (stmt_start < i) {
        out += FormatClauseRange(tokens, stmt_start, i, 2);
        out += ";\n";
      }
      stmt_start = i + 1;
    }
  }
  if (stmt_start < *end_idx) {
    out += FormatClauseRange(tokens, stmt_start, *end_idx, 2);
    out.push_back('\n');
  }

  out += "END";
  if (*end_idx + 1 < end) {
    out.push_back(' ');
    out += JoinTokenSlice(tokens, *end_idx + 1, end, 0);
  }
  if (had_semicolon) {
    out.push_back(';');
  }
  return out;
}

std::string FormatCreateView(const std::vector<SqlToken>& tokens, std::size_t end,
                             bool had_semicolon) {
  int depth = 0;
  for (std::size_t i = 0; i < end; ++i) {
    if (tokens[i].kind == SqlTokKind::LParen) {
      ++depth;
    } else if (tokens[i].kind == SqlTokKind::RParen) {
      if (depth > 0) --depth;
    } else if (depth == 0 && IsWord(tokens[i], "AS") && i + 1 < end) {
      std::string out = JoinTokenSlice(tokens, 0, i + 1, 0);
      out.push_back('\n');
      out += FormatClauseRange(tokens, i + 1, end, 0);
      if (had_semicolon) out.push_back(';');
      return out;
    }
  }
  std::string out = JoinTokenSlice(tokens, 0, end, 0);
  if (had_semicolon) out.push_back(';');
  return out;
}

std::string FormatCreateIndex(const std::vector<SqlToken>& tokens, std::size_t end,
                              bool had_semicolon) {
  int depth = 0;
  for (std::size_t i = 0; i < end; ++i) {
    if (tokens[i].kind == SqlTokKind::LParen) {
      ++depth;
    } else if (tokens[i].kind == SqlTokKind::RParen) {
      if (depth > 0) --depth;
    } else if (depth == 0 && IsWord(tokens[i], "WHERE") && i > 0) {
      std::string out = JoinTokenSlice(tokens, 0, i, 0);
      out += "\n  ";
      out += JoinTokenSlice(tokens, i, end, 2);
      if (had_semicolon) out.push_back(';');
      return out;
    }
  }
  std::string out = JoinTokenSlice(tokens, 0, end, 0);
  if (had_semicolon) out.push_back(';');
  return out;
}

} // namespace

std::string FormatSqlStatement(std::string_view sql, Dialect dialect) {
  const std::string_view trimmed = detail::AdbcTrimAscii(sql);
  if (trimmed.empty() || dialect == Dialect::Redis) {
    return std::string(trimmed);
  }
  std::vector<SqlToken> tokens = TokenizeSql(trimmed, dialect);
  if (tokens.empty()) {
    return std::string(trimmed);
  }

  bool had_semicolon = false;
  std::size_t end = tokens.size();
  while (end > 0 && tokens[end - 1].kind == SqlTokKind::Semicolon) {
    had_semicolon = true;
    --end;
  }
  if (end == 0) {
    return {};
  }

  // Skip leading comments when inspecting statement kind
  std::size_t lead = 0;
  while (lead < end &&
         (tokens[lead].kind == SqlTokKind::LineComment ||
          tokens[lead].kind == SqlTokKind::BlockComment)) {
    ++lead;
  }
  if (lead >= end) {
    return std::string(trimmed);
  }

  if (IsWord(tokens[lead], "CREATE")) {
    for (std::size_t i = lead + 1; i < std::min(end, lead + 5); ++i) {
      if (tokens[i].kind != SqlTokKind::Word) {
        continue;
      }
      if (IsWord(tokens[i], "TABLE")) {
        return FormatCreateTable(tokens, end, had_semicolon);
      }
      if (IsWord(tokens[i], "TRIGGER")) {
        return FormatCreateTrigger(tokens, end, had_semicolon);
      }
      if (IsWord(tokens[i], "VIEW")) {
        return FormatCreateView(tokens, end, had_semicolon);
      }
      if (IsWord(tokens[i], "INDEX")) {
        return FormatCreateIndex(tokens, end, had_semicolon);
      }
    }
  }

  if (IsWord(tokens[lead], "SELECT") || IsWord(tokens[lead], "WITH") ||
      IsWord(tokens[lead], "INSERT") || IsWord(tokens[lead], "UPDATE") ||
      IsWord(tokens[lead], "DELETE") || IsWord(tokens[lead], "REPLACE")) {
    std::string out = FormatClauseRange(tokens, 0, end, 0);
    if (had_semicolon) {
      out.push_back(';');
    }
    return out;
  }

  std::string out = JoinTokenSlice(tokens, 0, end, 0);
  if (had_semicolon) {
    out.push_back(';');
  }
  return out;
}

std::string FormatSqlScript(std::string_view script, Dialect dialect) {
  const std::string_view trimmed = detail::AdbcTrimAscii(script);
  if (trimmed.empty() || dialect == Dialect::Redis) {
    return std::string(trimmed);
  }
  const std::vector<std::string> statements = SplitSqlStatements(trimmed, dialect);
  if (statements.empty()) {
    return FormatSqlStatement(trimmed, dialect);
  }
  std::string out;
  for (std::size_t i = 0; i < statements.size(); ++i) {
    std::string formatted = FormatSqlStatement(statements[i], dialect);
    if (formatted.empty()) {
      continue;
    }
    if (!formatted.ends_with(';')) {
      formatted.push_back(';');
    }
    if (!out.empty()) {
      out += "\n\n";
    }
    out += formatted;
  }
  return out;
}

} // namespace dbpp
