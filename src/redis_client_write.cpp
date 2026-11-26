module;

#include <huxerui/datatable.h>

#include <cerrno>
#include <cstring>

module dbpp.redis;

import std;
import dbpp.types;
import dbpp.i18n;

namespace dbpp {

namespace {

bool ValidateWritableKeyType(std::string_view type_text, std::string& normalized_out,
                             std::string& error_out) {
  const std::string lower = detail::RedisLowerAscii(detail::RedisTrimAscii(type_text));
  if (lower.empty() || lower == "string") {
    normalized_out = "string";
    return true;
  }
  if (lower == "hash" || lower == "list" || lower == "set" || lower == "zset") {
    normalized_out = lower;
    return true;
  }
  error_out = TrFmt("Unsupported Redis type '{0}' (valid: string, hash, list, set, zset)",
                    "不支持的 Redis 类型 '{0}'（可选: string, hash, list, set, zset）",
                    {type_text});
  return false;
}

struct ParsedWriteSpec {
  std::string key;
  std::string type;
  std::optional<std::int64_t> ttl;
  std::vector<std::string> args;
};

bool WriteTypedRedisKey(RedisClient& client, std::string_view key, std::string_view type,
                        const std::vector<std::string>& args, std::optional<std::int64_t> ttl_sec,
                        bool replace_existing, std::optional<std::int64_t> preserve_pttl_ms,
                        std::string& error_out) {
  if (type == "string") {
    const std::string& val = args.empty() ? std::string{} : args[0];
    std::vector<std::string> cmd = {"SET", std::string(key), val};
    if (preserve_pttl_ms.has_value() && *preserve_pttl_ms > 0) {
      cmd.emplace_back("KEEPTTL");
    }
    RedisValue reply;
    if (!client.Command(cmd, reply, error_out)) {
      return false;
    }
    if (reply.IsError()) {
      if (cmd.size() > 3 && detail::RedisLowerAscii(reply.str).find("syntax") != std::string::npos) {
        // Fallback for very old Redis versions without KEEPTTL.
        RedisValue r2;
        if (!client.Command({"SET", std::string(key), val}, r2, error_out)) {
          return false;
        }
        if (r2.IsError()) {
          error_out = r2.str;
          return false;
        }
        RedisValue r3;
        (void)client.Command({"PEXPIRE", std::string(key), std::to_string(*preserve_pttl_ms)},
                             r3, error_out);
      } else {
        error_out = reply.str;
        return false;
      }
    }
    if (ttl_sec.has_value() && *ttl_sec > 0) {
      RedisValue exp_reply;
      if (!client.Command({"EXPIRE", std::string(key), std::to_string(*ttl_sec)}, exp_reply,
                          error_out)) {
        return false;
      }
      if (exp_reply.IsError()) {
        error_out = exp_reply.str;
        return false;
      }
    }
    return true;
  }

  std::vector<std::vector<std::string>> batch;
  if (replace_existing) {
    batch.push_back({"DEL", std::string(key)});
  }
  std::vector<std::string> write_cmd;
  if (type == "hash") {
    write_cmd.emplace_back("HSET");
  } else if (type == "list") {
    write_cmd.emplace_back("RPUSH");
  } else if (type == "set") {
    write_cmd.emplace_back("SADD");
  } else if (type == "zset") {
    write_cmd.emplace_back("ZADD");
  } else {
    error_out = TrFmt("Unsupported type: {0}", "不支持的类型: {0}", {type});
    return false;
  }
  write_cmd.emplace_back(key);
  write_cmd.insert(write_cmd.end(), args.begin(), args.end());
  batch.push_back(std::move(write_cmd));

  if (ttl_sec.has_value() && *ttl_sec > 0) {
    batch.push_back({"EXPIRE", std::string(key), std::to_string(*ttl_sec)});
  } else if (preserve_pttl_ms.has_value() && *preserve_pttl_ms > 0) {
    batch.push_back({"PEXPIRE", std::string(key), std::to_string(*preserve_pttl_ms)});
  }

  std::vector<RedisValue> replies;
  if (!client.Pipeline(batch, replies, error_out)) {
    return false;
  }
  for (const RedisValue& r : replies) {
    if (r.IsError()) {
      error_out = r.str;
      return false;
    }
  }
  return true;
}

bool ParseInfoIntoRows(std::string_view info_text, std::vector<GridRow>& rows_out) {
  std::string section = "general";
  std::size_t pos = 0;
  while (pos < info_text.size()) {
    const std::size_t nl = info_text.find('\n', pos);
    const std::string_view raw_line =
        nl == std::string_view::npos ? info_text.substr(pos) : info_text.substr(pos, nl - pos);
    pos = nl == std::string_view::npos ? info_text.size() : nl + 1;

    const std::string_view line = detail::RedisTrimAscii(raw_line);
    if (line.empty()) {
      continue;
    }
    if (line.front() == '#') {
      section = std::string(detail::RedisTrimAscii(line.substr(1)));
      continue;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    GridRow row;
    row.values = {
        detail::RedisTextCell(section),
        detail::RedisTextCell(std::string(line.substr(0, colon))),
        detail::RedisTextCell(std::string(line.substr(colon + 1))),
    };
    rows_out.push_back(std::move(row));
  }
  return !rows_out.empty();
}

} // namespace

namespace {

AdbcQueryResult ExecuteSingleRedisCommandBlocking(RedisClient& client,
                                                  std::string_view command_text,
                                                  std::size_t max_rows) {
  AdbcQueryResult result;
  const RedisCommandParseResult parsed = ParseRedisCommandLine(command_text);
  if (!parsed.error.empty()) {
    result.error = parsed.error;
    return result;
  }
  if (parsed.args.empty()) {
    result.error = Tr("Redis command cannot be empty", "Redis 命令不能为空");
    return result;
  }

  const std::string cmd = detail::RedisUpperAscii(parsed.args[0]);
  const std::string sub = parsed.args.size() > 1 ? detail::RedisUpperAscii(parsed.args[1]) : std::string{};
  const std::size_t row_limit = max_rows == 0 ? kMaxQueryRows : max_rows;

  if (cmd == "SELECT" && parsed.args.size() == 2) {
    const auto db_num = detail::RedisParseStrictInt64(parsed.args[1]);
    if (!db_num.has_value() || *db_num < 0 || *db_num > 1000000) {
      result.error = TrFmt("Invalid database index: {0}", "无效的数据库编号: {0}",
                           {parsed.args[1]});
      return result;
    }
    if (!client.EnsureDb(static_cast<int>(*db_num), result.error)) {
      result.connection_lost = !client.IsConnected();
      return result;
    }
    result.columns = {"result"};
    result.rows.push_back(GridRow{.values = {detail::RedisTextCell("OK (db" + std::to_string(*db_num) + ")")},
                                  .key_values = {},
                                  .rowid = std::nullopt});
    result.affected = 0;
    return result;
  }

  RedisValue reply;
  if (!client.Command(parsed.args, reply, result.error)) {
    result.connection_lost = !client.IsConnected();
    return result;
  }
  if (reply.IsError()) {
    result.error = reply.str;
    return result;
  }

  if (cmd == "INFO" &&
      (reply.kind == RedisValueKind::BulkString || reply.kind == RedisValueKind::SimpleString)) {
    std::vector<GridRow> info_rows;
    if (ParseInfoIntoRows(reply.str, info_rows)) {
      result.columns = {"section", "property", "value"};
      if (info_rows.size() > row_limit) {
        result.truncated = true;
        info_rows.resize(row_limit);
      }
      result.rows = std::move(info_rows);
      result.affected = static_cast<std::int64_t>(result.rows.size());
      return result;
    }
  }

  if (reply.kind == RedisValueKind::Array) {
    const bool withscores = std::ranges::any_of(parsed.args, [](const std::string& a) {
      return detail::RedisEqualsIgnoreCase(a, "WITHSCORES");
    });
    const bool is_pair_cmd = reply.is_map || cmd == "HGETALL" ||
                             (cmd == "CONFIG" && sub == "GET") || withscores;

    if (is_pair_cmd) {
      result.columns = withscores ? std::vector<std::string>{"member", "score"}
                                  : std::vector<std::string>{"field", "value"};
      for (std::size_t i = 0; i + 1 < reply.elements.size(); i += 2) {
        if (result.rows.size() >= row_limit) {
          result.truncated = true;
          break;
        }
        result.rows.push_back(GridRow{
            .values = {detail::RedisReplyToCell(reply.elements[i]), detail::RedisReplyToCell(reply.elements[i + 1])},
            .key_values = {},
            .rowid = std::nullopt,
        });
      }
      result.affected = static_cast<std::int64_t>(result.rows.size());
      return result;
    }

    // Two-element cursor array from SCAN / HSCAN / SSCAN / ZSCAN -> unwrap or show elements.
    if ((cmd == "SCAN" || cmd == "HSCAN" || cmd == "SSCAN" || cmd == "ZSCAN") &&
        reply.elements.size() >= 2 && reply.elements[1].kind == RedisValueKind::Array) {
      const std::string cursor = reply.elements[0].ToDisplayString();
      result.columns = {"cursor", "item"};
      for (const RedisValue& elem : reply.elements[1].elements) {
        if (result.rows.size() >= row_limit) {
          result.truncated = true;
          break;
        }
        result.rows.push_back(GridRow{.values = {detail::RedisTextCell(cursor), detail::RedisReplyToCell(elem)},
                                      .key_values = {},
                                      .rowid = std::nullopt});
      }
      if (result.rows.empty()) {
        result.rows.push_back(
            GridRow{.values = {detail::RedisTextCell(cursor), detail::RedisNullCell()}, .key_values = {}, .rowid = std::nullopt});
      }
      result.affected = static_cast<std::int64_t>(result.rows.size());
      return result;
    }

    result.columns = {"index", "value"};
    for (std::size_t i = 0; i < reply.elements.size(); ++i) {
      if (result.rows.size() >= row_limit) {
        result.truncated = true;
        break;
      }
      result.rows.push_back(GridRow{
          .values = {detail::RedisTextCell(std::to_string(i)), detail::RedisReplyToCell(reply.elements[i])},
          .key_values = {},
          .rowid = std::nullopt,
      });
    }
    result.affected = static_cast<std::int64_t>(result.rows.size());
    return result;
  }

  result.columns = {"result"};
  result.rows.push_back(GridRow{.values = {detail::RedisReplyToCell(reply)}, .key_values = {}, .rowid = std::nullopt});
  result.affected = reply.kind == RedisValueKind::Integer ? reply.integer : 1;
  return result;
}

} // namespace

AdbcQueryResult ExecuteRedisCommandBlocking(RedisClient& client, std::string_view command_text,
                                            std::size_t max_rows) {
  const std::vector<std::string> lines = SplitRedisScript(command_text);
  if (lines.empty()) {
    AdbcQueryResult result;
    const RedisCommandParseResult syntax_check = ParseRedisCommandLine(command_text);
    result.error = !syntax_check.error.empty()
                       ? syntax_check.error
                       : std::string(Tr("Redis command cannot be empty", "Redis 命令不能为空"));
    return result;
  }
  if (lines.size() == 1) {
    return ExecuteSingleRedisCommandBlocking(client, lines.front(), max_rows);
  }
  AdbcQueryResult last_result;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    AdbcQueryResult step = ExecuteSingleRedisCommandBlocking(client, lines[i], max_rows);
    if (!step.error.empty()) {
      step.error = TrFmt("Command {0} failed: {1}", "第 {0} 条命令失败: {1}",
                         {std::to_string(i + 1), step.error});
      return step;
    }
    last_result = std::move(step);
  }
  return last_result;
}

AdbcQueryResult UpdateRedisCellBlocking(RedisClient& client, const TableModel& model,
                                        std::size_t row_index, std::size_t column_index,
                                        const std::string& text) {
  AdbcQueryResult result;
  if (!model.snapshot || row_index >= model.snapshot->rows.size() || column_index >= 5) {
    result.error = Tr("Target row or column is invalid", "目标行或列已失效");
    return result;
  }
  const std::optional<int> db = ParseRedisDbName(model.object);
  if (!db.has_value()) {
    result.error = TrFmt("Invalid Redis database: {0}", "无效的 Redis 数据库: {0}",
                         {model.object});
    return result;
  }
  if (!client.EnsureDb(*db, result.error)) {
    result.connection_lost = !client.IsConnected();
    return result;
  }

  const GridRow& row = model.snapshot->rows[row_index];
  if (row.values.size() < 5) {
    result.error = Tr("Incomplete row data", "行数据不完整");
    return result;
  }
  const std::string& key = row.values[0].text;
  const std::string type = detail::RedisLowerAscii(row.values[1].text);

  if (column_index == 0) {
    if (text.empty()) {
      result.error = Tr("Key name cannot be empty", "键名不能为空");
      return result;
    }
    if (text == key) {
      return result;
    }
    RedisValue reply;
    if (!client.Command({"RENAMENX", key, text}, reply, result.error)) {
      result.connection_lost = !client.IsConnected();
      return result;
    }
    if (reply.IsError()) {
      result.error = reply.str;
      return result;
    }
    if (reply.kind == RedisValueKind::Integer && reply.integer == 0) {
      result.error = TrFmt("Target key already exists: {0}", "目标键已存在，无法覆盖: {0}",
                           {text});
      return result;
    }
    result.affected = 1;
    return result;
  }

  if (column_index == 1) {
    result.error = Tr("Changing key type in place is not supported; delete and re-add instead",
                      "不支持直接修改已有键的类型，请删除后新增");
    return result;
  }

  if (column_index == 2) {
    const std::string_view trimmed = detail::RedisTrimAscii(text);
    if (trimmed.empty() || trimmed == "-1" || detail::RedisEqualsIgnoreCase(trimmed, "NULL")) {
      RedisValue reply;
      if (!client.Command({"PERSIST", key}, reply, result.error)) {
        result.connection_lost = !client.IsConnected();
        return result;
      }
      if (reply.IsError()) {
        result.error = reply.str;
        return result;
      }
      result.affected = 1;
      return result;
    }
    const auto ttl_val = detail::RedisParseStrictInt64(trimmed);
    if (!ttl_val.has_value() || *ttl_val <= 0) {
      result.error = Tr("TTL must be a positive integer (seconds) or -1 (no expiry)",
                        "TTL 必须为正整数（秒）或 -1（永不过期）");
      return result;
    }
    RedisValue reply;
    if (!client.Command({"EXPIRE", key, std::to_string(*ttl_val)}, reply, result.error)) {
      result.connection_lost = !client.IsConnected();
      return result;
    }
    if (reply.IsError()) {
      result.error = reply.str;
      return result;
    }
    result.affected = reply.integer;
    return result;
  }

  if (column_index == 3) {
    result.error = Tr("size is a read-only computed column", "size 为只读统计列");
    return result;
  }

  // column_index == 4: update value.
  std::vector<std::string> write_args;
  if (!detail::RedisBuildWriteCommandArgs(type, text, write_args, result.error)) {
    return result;
  }
  RedisValue pttl_reply;
  std::optional<std::int64_t> preserve_pttl;
  if (client.Command({"PTTL", key}, pttl_reply, result.error) &&
      pttl_reply.kind == RedisValueKind::Integer && pttl_reply.integer > 0) {
    preserve_pttl = pttl_reply.integer;
  } else {
    if (!client.IsConnected()) {
      result.connection_lost = true;
      return result;
    }
    result.error.clear();
  }
  if (!WriteTypedRedisKey(client, key, type, write_args, std::nullopt, type != "string",
                          preserve_pttl, result.error)) {
    result.connection_lost = !client.IsConnected();
    return result;
  }
  result.affected = 1;
  return result;
}

AdbcQueryResult DeleteRedisKeyBlocking(RedisClient& client, const TableModel& model,
                                       std::size_t row_index) {
  AdbcQueryResult result;
  if (!model.snapshot || row_index >= model.snapshot->rows.size() ||
      model.snapshot->rows[row_index].values.empty()) {
    result.error = Tr("Target row is invalid", "目标行已失效");
    return result;
  }
  const std::optional<int> db = ParseRedisDbName(model.object);
  if (!db.has_value()) {
    result.error = TrFmt("Invalid Redis database: {0}", "无效的 Redis 数据库: {0}",
                         {model.object});
    return result;
  }
  if (!client.EnsureDb(*db, result.error)) {
    result.connection_lost = !client.IsConnected();
    return result;
  }
  const std::string& key = model.snapshot->rows[row_index].values[0].text;
  RedisValue reply;
  if (!client.Command({"DEL", key}, reply, result.error)) {
    result.connection_lost = !client.IsConnected();
    return result;
  }
  if (reply.IsError()) {
    result.error = reply.str;
    return result;
  }
  result.affected = reply.kind == RedisValueKind::Integer ? reply.integer : 1;
  return result;
}

BatchInsertResult InsertRedisRowsBlocking(RedisClient& client, const TableModel& model,
                                          const std::vector<PendingInsertRow>& rows) {
  if (rows.empty()) {
    return {};
  }
  const std::optional<int> db = ParseRedisDbName(model.object);
  if (!db.has_value()) {
    return BatchInsertResult{.inserted = 0, .rolled_back = true,
                             .error = TrFmt("Invalid Redis database: {0}",
                                            "无效的 Redis 数据库: {0}", {model.object})};
  }

  std::vector<ParsedWriteSpec> specs;
  specs.reserve(rows.size());
  std::unordered_set<std::string> batch_keys;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto cell_at = [&](std::size_t col) -> std::string_view {
      return col < rows[i].values.size() ? std::string_view(rows[i].values[col])
                                         : std::string_view{};
    };
    const std::string row_num = std::to_string(i + 1);
    const std::string prefix = TrFmt("Row {0}", "第 {0} 行", {row_num});
    const std::string key(cell_at(0));
    if (key.empty()) {
      return BatchInsertResult{.inserted = 0, .rolled_back = true,
                               .error = TrFmt("Row {0}: key name cannot be empty",
                                              "第 {0} 行键名不能为空", {row_num})};
    }
    if (!batch_keys.insert(key).second) {
      return BatchInsertResult{.inserted = 0, .rolled_back = true,
                               .error = TrFmt("Row {0}: duplicate key {1}",
                                              "第 {0} 行键名与前面重复: {1}", {row_num, key})};
    }

    std::string norm_type;
    std::string err;
    if (!ValidateWritableKeyType(cell_at(1), norm_type, err)) {
      return BatchInsertResult{.inserted = 0, .rolled_back = true, .error = prefix + ": " + err};
    }

    std::optional<std::int64_t> ttl;
    const std::string_view ttl_sv = detail::RedisTrimAscii(cell_at(2));
    if (!ttl_sv.empty() && ttl_sv != "-1" && !detail::RedisEqualsIgnoreCase(ttl_sv, "NULL")) {
      const auto parsed = detail::RedisParseStrictInt64(ttl_sv);
      if (!parsed.has_value() || *parsed <= 0) {
        return BatchInsertResult{
            .inserted = 0, .rolled_back = true,
            .error = TrFmt("Row {0}: TTL must be a positive integer or -1",
                           "第 {0} 行: TTL 必须为正整数或 -1", {row_num})};
      }
      ttl = *parsed;
    }

    std::vector<std::string> args;
    if (!detail::RedisBuildWriteCommandArgs(norm_type, cell_at(4), args, err)) {
      return BatchInsertResult{.inserted = 0, .rolled_back = true, .error = prefix + ": " + err};
    }

    specs.push_back(ParsedWriteSpec{
        .key = key,
        .type = std::move(norm_type),
        .ttl = ttl,
        .args = std::move(args),
    });
  }

  std::string err;
  if (!client.EnsureDb(*db, err)) {
    return BatchInsertResult{.inserted = 0,
                             .rolled_back = true,
                             .connection_lost = !client.IsConnected(),
                             .error = err};
  }

  std::vector<std::vector<std::string>> exists_cmds;
  exists_cmds.reserve(specs.size());
  for (const ParsedWriteSpec& spec : specs) {
    exists_cmds.push_back({"EXISTS", spec.key});
  }
  std::vector<RedisValue> exists_replies;
  if (!client.Pipeline(exists_cmds, exists_replies, err)) {
    return BatchInsertResult{.inserted = 0,
                             .rolled_back = true,
                             .connection_lost = !client.IsConnected(),
                             .error = err};
  }
  for (std::size_t i = 0; i < specs.size() && i < exists_replies.size(); ++i) {
    if (exists_replies[i].kind == RedisValueKind::Integer && exists_replies[i].integer > 0) {
      return BatchInsertResult{.inserted = 0, .rolled_back = true,
                               .error = TrFmt("Row {0}: key already exists: {1}",
                                              "第 {0} 行键已存在: {1}",
                                              {std::to_string(i + 1), specs[i].key})};
    }
  }

  std::size_t inserted = 0;
  for (const ParsedWriteSpec& spec : specs) {
    if (!WriteTypedRedisKey(client, spec.key, spec.type, spec.args, spec.ttl, false,
                            std::nullopt, err)) {
      return BatchInsertResult{.inserted = inserted,
                               .rolled_back = (inserted == 0),
                               .connection_lost = !client.IsConnected(),
                               .error = TrFmt("Failed to write {0}: {1}", "写入 {0} 失败: {1}",
                                              {spec.key, err})};
    }
    ++inserted;
  }

  return BatchInsertResult{.inserted = inserted, .rolled_back = false, .error = {}};
}

} // namespace dbpp
