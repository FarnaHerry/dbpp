module;

#include <huxerui/datatable.h>

module dbpp.redis;

import std;
import dbpp.types;
import dbpp.i18n;

namespace dbpp {

namespace {

constexpr int kMaxTreeDepth = 64;

struct PrefixTreeNode {
  std::string id;
  std::string label;
  std::size_t key_count = 0;
  std::map<std::string, std::unique_ptr<PrefixTreeNode>> children;
  std::vector<std::pair<std::string, std::string>> leaves;
};

void InsertKeyIntoPrefixTree(PrefixTreeNode& node, const std::string& full_key,
                             std::size_t cursor, std::string_view delimiter, int depth) {
  ++node.key_count;
  if (delimiter.empty() || depth >= kMaxTreeDepth) {
    node.leaves.emplace_back(full_key.substr(cursor), full_key);
    return;
  }
  const std::size_t pos = full_key.find(delimiter, cursor);
  if (pos == std::string::npos) {
    node.leaves.emplace_back(full_key.substr(cursor), full_key);
    return;
  }
  const std::size_t next_cursor = pos + delimiter.size();
  // Keep boundary keys ending at a delimiter (or empty segments like a::b) as leaves.
  if (pos == cursor || next_cursor >= full_key.size()) {
    node.leaves.emplace_back(full_key.substr(cursor), full_key);
    return;
  }
  const std::string segment = full_key.substr(cursor, pos - cursor);
  auto& child = node.children[segment];
  if (!child) {
    child = std::make_unique<PrefixTreeNode>();
    child->id = full_key.substr(0, next_cursor);
    child->label = segment;
  }
  InsertKeyIntoPrefixTree(*child, full_key, next_cursor, delimiter, depth + 1);
}

void CollectFolderIds(const PrefixTreeNode& node, std::unordered_set<std::string>& out) {
  for (const auto& [seg, child] : node.children) {
    (void)seg;
    out.insert(child->id);
    CollectFolderIds(*child, out);
  }
}

void EmitPrefixTreeRows(const PrefixTreeNode& node, int depth,
                        const std::unordered_set<std::string>& expanded_folders,
                        std::vector<RedisTreeFlatRow>& out) {
  for (const auto& [seg, child] : node.children) {
    (void)seg;
    const bool expanded = expanded_folders.contains(child->id);
    out.push_back(RedisTreeFlatRow{
        .id = child->id,
        .label = child->label,
        .full_key = {},
        .depth = depth,
        .is_folder = true,
        .expanded = expanded,
        .key_count = child->key_count,
    });
    if (expanded) {
      EmitPrefixTreeRows(*child, depth + 1, expanded_folders, out);
    }
  }
  std::vector<std::pair<std::string, std::string>> sorted_leaves = node.leaves;
  std::ranges::sort(sorted_leaves, [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first < b.first : a.second < b.second;
  });
  for (const auto& [label, full_key] : sorted_leaves) {
    out.push_back(RedisTreeFlatRow{
        .id = "leaf:" + full_key,
        .label = label,
        .full_key = full_key,
        .depth = depth,
        .is_folder = false,
        .expanded = false,
        .key_count = 0,
    });
  }
}

bool EnsureRedisDbSelected(RedisClient& client, const DbObject& object, std::string& error_out,
                           bool* connection_lost_out = nullptr) {
  const std::optional<int> db = ParseRedisDbName(object.name);
  if (!db.has_value()) {
    error_out = TrFmt("Invalid Redis database object name: {0}",
                      "无效的 Redis 数据库对象名: {0}", {object.name});
    return false;
  }
  if (!client.EnsureDb(*db, error_out)) {
    if (connection_lost_out != nullptr) {
      *connection_lost_out = !client.IsConnected();
    }
    return false;
  }
  return true;
}

} // namespace

RedisKeyListResult ScanRedisKeyNamesBlocking(RedisClient& client, const DbObject& object,
                                             std::string filter, std::string type_filter,
                                             const std::atomic<bool>* cancel_flag) {
  RedisKeyListResult result;
  if (!EnsureRedisDbSelected(client, object, result.error)) {
    return result;
  }

  const auto started = std::chrono::steady_clock::now();
  const auto is_cancelled = [&]() -> bool {
    return cancel_flag != nullptr && cancel_flag->load(std::memory_order_acquire);
  };

  const std::string_view trimmed_filter = detail::RedisTrimAscii(filter);
  const std::string pattern = trimmed_filter.empty() ? "*" : std::string(trimmed_filter);
  const std::string normalized_type = detail::RedisNormalizeTypeFilter(type_filter);
  result.executed_sql = "SCAN " + object.name +
                        (trimmed_filter.empty() ? std::string{} : " MATCH " + pattern) +
                        (normalized_type.empty() ? std::string{} : " TYPE " + normalized_type);

  std::unordered_set<std::string> seen;
  std::string cursor = "0";
  std::size_t scan_iterations = 0;
  bool client_side_type_filter = false;

  for (;;) {
    if (is_cancelled()) {
      result.scan_cancelled = true;
      result.error = Tr("Operation cancelled", "操作已取消");
      return result;
    }
    std::vector<std::string> scan_cmd = {"SCAN", cursor, "MATCH", pattern, "COUNT", "500"};
    if (!normalized_type.empty() && !client_side_type_filter) {
      scan_cmd.emplace_back("TYPE");
      scan_cmd.push_back(normalized_type);
    }
    RedisValue scan_reply;
    if (!client.Command(scan_cmd, scan_reply, result.error)) {
      return result;
    }
    if (scan_reply.IsError()) {
      if (!normalized_type.empty() && !client_side_type_filter && scan_iterations == 0 &&
          detail::RedisLowerAscii(scan_reply.str).find("syntax") != std::string::npos) {
        client_side_type_filter = true;
        cursor = "0";
        continue;
      }
      result.error = scan_reply.str;
      return result;
    }
    ++scan_iterations;
    if (scan_reply.kind != RedisValueKind::Array || scan_reply.elements.size() < 2) {
      result.error = Tr("Invalid SCAN response format", "SCAN 返回格式无效");
      return result;
    }
    cursor = scan_reply.elements[0].ToDisplayString();
    const RedisValue& batch = scan_reply.elements[1];
    if (batch.kind == RedisValueKind::Array) {
      if (!client_side_type_filter) {
        for (const RedisValue& item : batch.elements) {
          std::string k = item.ToDisplayString();
          if (seen.insert(k).second) {
            result.keys.push_back(std::move(k));
            if (result.keys.size() >= detail::kRedisMaxScanKeys) {
              result.scan_capped = (cursor != "0");
              break;
            }
          }
        }
      } else {
        std::vector<std::string> candidates;
        candidates.reserve(batch.elements.size());
        for (const RedisValue& item : batch.elements) {
          std::string k = item.ToDisplayString();
          if (seen.insert(k).second) {
            candidates.push_back(std::move(k));
          }
        }
        if (!candidates.empty()) {
          std::vector<std::vector<std::string>> type_cmds;
          type_cmds.reserve(candidates.size());
          for (const std::string& cand : candidates) {
            type_cmds.push_back({"TYPE", cand});
          }
          std::vector<RedisValue> type_replies;
          if (!client.Pipeline(type_cmds, type_replies, result.error)) {
            return result;
          }
          for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (i < type_replies.size() &&
                detail::RedisLowerAscii(type_replies[i].ToDisplayString()) == normalized_type) {
              result.keys.push_back(std::move(candidates[i]));
              if (result.keys.size() >= detail::kRedisMaxScanKeys) {
                result.scan_capped = (cursor != "0");
                break;
              }
            }
          }
        }
      }
    }
    if (cursor == "0" || result.keys.size() >= detail::kRedisMaxScanKeys) {
      break;
    }
    if (scan_iterations >= detail::kRedisMaxScanIterations) {
      result.scan_capped = true;
      result.scan_iterations_capped = true;
      break;
    }
  }

  std::ranges::sort(result.keys);
  result.elapsed_us = std::max<std::int64_t>(
      0, std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - started)
             .count());
  return result;
}

std::vector<RedisTreeFlatRow> BuildRedisTreeFlatRows(
    const std::vector<std::string>& sorted_keys, std::string_view delimiter,
    const std::unordered_set<std::string>& expanded_folders,
    std::unordered_set<std::string>* all_folder_ids_out) {
  if (all_folder_ids_out != nullptr) {
    all_folder_ids_out->clear();
  }
  std::vector<RedisTreeFlatRow> out;
  if (delimiter.empty()) {
    out.reserve(sorted_keys.size());
    for (const std::string& k : sorted_keys) {
      out.push_back(RedisTreeFlatRow{
          .id = "leaf:" + k,
          .label = k,
          .full_key = k,
          .depth = 0,
          .is_folder = false,
          .expanded = false,
          .key_count = 0,
      });
    }
    return out;
  }

  PrefixTreeNode root;
  for (const std::string& k : sorted_keys) {
    InsertKeyIntoPrefixTree(root, k, 0, delimiter, 0);
  }
  if (all_folder_ids_out != nullptr) {
    CollectFolderIds(root, *all_folder_ids_out);
  }
  EmitPrefixTreeRows(root, 0, expanded_folders, out);
  return out;
}

RedisKeyDetail LoadRedisKeyDetailBlocking(RedisClient& client, const DbObject& object,
                                          std::string_view key, bool load_full_string) {
  RedisKeyDetail detail;
  detail.key = std::string(key);
  if (!EnsureRedisDbSelected(client, object, detail.error)) {
    return detail;
  }

  const std::string key_str(key);
  std::vector<RedisValue> meta_replies;
  if (!client.Pipeline({{"TYPE", key_str}, {"TTL", key_str}}, meta_replies, detail.error)) {
    return detail;
  }
  if (!meta_replies.empty()) {
    detail.type = detail::RedisLowerAscii(meta_replies[0].ToDisplayString());
  }
  if (meta_replies.size() >= 2 && meta_replies[1].kind == RedisValueKind::Integer) {
    detail.ttl = meta_replies[1].integer;
  }
  if (detail.type.empty() || detail.type == "none") {
    detail.exists = false;
    return detail;
  }
  detail.exists = true;

  std::string size_cmd_name;
  if (detail.type == "string") size_cmd_name = "STRLEN";
  else if (detail.type == "hash") size_cmd_name = "HLEN";
  else if (detail.type == "list") size_cmd_name = "LLEN";
  else if (detail.type == "set") size_cmd_name = "SCARD";
  else if (detail.type == "zset") size_cmd_name = "ZCARD";
  else if (detail.type == "stream") size_cmd_name = "XLEN";

  if (!size_cmd_name.empty()) {
    RedisValue sz_reply;
    if (!client.Command({size_cmd_name, key_str}, sz_reply, detail.error)) {
      return detail;
    }
    if (sz_reply.kind == RedisValueKind::Integer) {
      detail.size = sz_reply.integer;
    }
  }

  const bool truncated = detail::IsRedisValuePreviewTruncated(detail.type, detail.size);
  if (detail.type == "string") {
    RedisValue val_reply;
    const bool use_range = !load_full_string && truncated;
    std::vector<std::string> cmd =
        use_range
            ? std::vector<std::string>{"GETRANGE", key_str, "0",
                                       std::to_string(detail::kRedisMaxStringPreviewBytes - 1)}
            : std::vector<std::string>{"GET", key_str};
    if (!client.Command(cmd, val_reply, detail.error)) {
      return detail;
    }
    if (val_reply.IsError()) {
      detail.error = val_reply.str;
      return detail;
    }
    if (!val_reply.IsNull()) {
      detail.string_value = val_reply.ToDisplayString();
      if (use_range) {
        detail::RedisTrimIncompleteUtf8Tail(detail.string_value);
        detail.truncated = true;
      }
    }
    return detail;
  }

  detail.truncated = truncated;
  RedisValue val_reply;
  std::vector<std::string> cmd;
  bool is_scan = false;
  if (detail.type == "hash") {
    if (truncated) {
      is_scan = true;
      cmd = {"HSCAN", key_str, "0", "COUNT", std::to_string(detail::kRedisMaxCollectionPreview)};
    } else {
      cmd = {"HGETALL", key_str};
    }
  } else if (detail.type == "list") {
    cmd = {"LRANGE", key_str, "0", std::to_string(detail::kRedisMaxListZsetPreview - 1)};
  } else if (detail.type == "set") {
    if (truncated) {
      is_scan = true;
      cmd = {"SSCAN", key_str, "0", "COUNT", std::to_string(detail::kRedisMaxCollectionPreview)};
    } else {
      cmd = {"SMEMBERS", key_str};
    }
  } else if (detail.type == "zset") {
    cmd = {"ZRANGE", key_str, "0", std::to_string(detail::kRedisMaxListZsetPreview - 1),
           "WITHSCORES"};
  } else if (detail.type == "stream") {
    cmd = {"XRANGE", key_str, "-", "+", "COUNT", std::to_string(detail::kRedisMaxStreamPreview)};
  } else {
    return detail;
  }

  if (!client.Command(cmd, val_reply, detail.error)) {
    return detail;
  }
  if (val_reply.IsError()) {
    detail.error = val_reply.str;
    return detail;
  }

  const std::vector<RedisValue>* elems = &val_reply.elements;
  std::vector<RedisValue> empty;
  if (is_scan) {
    elems = (val_reply.kind == RedisValueKind::Array && val_reply.elements.size() >= 2 &&
             val_reply.elements[1].kind == RedisValueKind::Array)
                ? &val_reply.elements[1].elements
                : &empty;
  }

  if (detail.type == "hash") {
    const std::size_t max_items = detail::kRedisMaxCollectionPreview;
    for (std::size_t i = 0; i + 1 < elems->size() && detail.entries.size() < max_items; i += 2) {
      detail.entries.emplace_back((*elems)[i].ToDisplayString(), (*elems)[i + 1].ToDisplayString());
    }
  } else if (detail.type == "zset") {
    const std::size_t max_items = detail::kRedisMaxListZsetPreview;
    for (std::size_t i = 0; i + 1 < elems->size() && detail.entries.size() < max_items; i += 2) {
      detail.entries.emplace_back((*elems)[i].ToDisplayString(), (*elems)[i + 1].ToDisplayString());
    }
  } else if (detail.type == "stream") {
    for (const RedisValue& entry : *elems) {
      if (entry.kind == RedisValueKind::Array && entry.elements.size() >= 2) {
        const std::string id = entry.elements[0].ToDisplayString();
        const std::string fields =
            detail::RedisFormatJsonHashPairs(entry.elements[1].elements, 64);
        detail.entries.emplace_back(id, fields);
      }
    }
  } else {
    const std::size_t cap = detail.type == "list" ? detail::kRedisMaxListZsetPreview
                                                  : detail::kRedisMaxCollectionPreview;
    for (std::size_t i = 0; i < elems->size() && i < cap; ++i) {
      const std::size_t label_num = detail.type == "list" ? i : (i + 1);
      detail.entries.emplace_back(std::to_string(label_num), (*elems)[i].ToDisplayString());
    }
  }
  return detail;
}

AdbcQueryResult UpdateRedisKeyStringBlocking(RedisClient& client, const DbObject& object,
                                             std::string_view key, const std::string& new_value) {
  AdbcQueryResult result;
  if (!EnsureRedisDbSelected(client, object, result.error, &result.connection_lost)) {
    return result;
  }
  const std::string key_str(key);
  RedisValue pttl_reply;
  std::string pttl_err;
  bool keep_ttl = false;
  if (client.Command({"PTTL", key_str}, pttl_reply, pttl_err) &&
      pttl_reply.kind == RedisValueKind::Integer && pttl_reply.integer > 0) {
    keep_ttl = true;
  } else if (!client.IsConnected()) {
    result.connection_lost = true;
    result.error = pttl_err;
    return result;
  }
  RedisValue reply;
  std::vector<std::string> cmd = {"SET", key_str, new_value};
  if (keep_ttl) {
    cmd.emplace_back("KEEPTTL");
  }
  if (!client.Command(cmd, reply, result.error)) {
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

AdbcQueryResult UpdateRedisKeyTtlBlocking(RedisClient& client, const DbObject& object,
                                          std::string_view key, std::string_view ttl_text) {
  AdbcQueryResult result;
  if (!EnsureRedisDbSelected(client, object, result.error, &result.connection_lost)) {
    return result;
  }
  const std::string key_str(key);
  const std::string_view trimmed = detail::RedisTrimAscii(ttl_text);
  RedisValue reply;
  if (trimmed.empty() || trimmed == "-1" || detail::RedisEqualsIgnoreCase(trimmed, "NULL")) {
    if (!client.Command({"PERSIST", key_str}, reply, result.error)) {
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
  if (!client.Command({"EXPIRE", key_str, std::to_string(*ttl_val)}, reply, result.error)) {
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

AdbcQueryResult RenameRedisKeyBlocking(RedisClient& client, const DbObject& object,
                                       std::string_view old_key, std::string_view new_key) {
  AdbcQueryResult result;
  if (new_key.empty()) {
    result.error = Tr("Key name cannot be empty", "键名不能为空");
    return result;
  }
  if (old_key == new_key) {
    return result;
  }
  if (!EnsureRedisDbSelected(client, object, result.error, &result.connection_lost)) {
    return result;
  }
  RedisValue reply;
  if (!client.Command({"RENAMENX", std::string(old_key), std::string(new_key)}, reply,
                      result.error)) {
    result.connection_lost = !client.IsConnected();
    return result;
  }
  if (reply.IsError()) {
    result.error = reply.str;
    return result;
  }
  if (reply.kind == RedisValueKind::Integer && reply.integer == 0) {
    result.error = TrFmt("Target key already exists: {0}", "目标键已存在，无法覆盖: {0}",
                         {std::string(new_key)});
    return result;
  }
  result.affected = 1;
  return result;
}

AdbcQueryResult DeleteRedisKeyByNameBlocking(RedisClient& client, const DbObject& object,
                                             std::string_view key) {
  AdbcQueryResult result;
  if (!EnsureRedisDbSelected(client, object, result.error, &result.connection_lost)) {
    return result;
  }
  RedisValue reply;
  if (!client.Command({"DEL", std::string(key)}, reply, result.error)) {
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

} // namespace dbpp
