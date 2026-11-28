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

std::map<int, std::int64_t> ParseInfoKeyspace(std::string_view info_text) {
  std::map<int, std::int64_t> counts;
  std::size_t pos = 0;
  while (pos < info_text.size()) {
    const std::size_t nl = info_text.find('\n', pos);
    const std::string_view raw_line =
        nl == std::string_view::npos ? info_text.substr(pos) : info_text.substr(pos, nl - pos);
    pos = nl == std::string_view::npos ? info_text.size() : nl + 1;

    const std::string_view line = detail::RedisTrimAscii(raw_line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    const std::optional<int> db_idx = ParseRedisDbName(line.substr(0, colon));
    if (!db_idx.has_value()) {
      continue;
    }
    const std::string_view metrics = line.substr(colon + 1);
    const std::size_t keys_pos = metrics.find("keys=");
    if (keys_pos == std::string_view::npos) {
      continue;
    }
    const std::size_t val_start = keys_pos + 5;
    const std::size_t comma = metrics.find(',', val_start);
    const std::string_view count_sv = comma == std::string_view::npos
                                          ? metrics.substr(val_start)
                                          : metrics.substr(val_start, comma - val_start);
    if (const auto parsed = detail::RedisParseStrictInt64(count_sv)) {
      counts[*db_idx] = *parsed;
    }
  }
  return counts;
}

CellValue TextCell(std::string text) {
  return CellValue{.text = std::move(text), .is_null = false, .display_text = {}};
}

CellValue NullCell() {
  return CellValue{.text = "NULL", .is_null = true, .display_text = {}};
}

CellValue ReplyToCell(const RedisValue& val) {
  if (val.IsNull()) {
    return NullCell();
  }
  return TextCell(val.ToDisplayString());
}

void TrimIncompleteUtf8Tail(std::string& s) {
  if (s.empty()) {
    return;
  }
  std::size_t idx = s.size();
  std::size_t cont = 0;
  while (idx > 0 && cont < 3 && (static_cast<unsigned char>(s[idx - 1]) & 0xC0u) == 0x80u) {
    --idx;
    ++cont;
  }
  if (idx == 0) {
    return;
  }
  const unsigned char lead = static_cast<unsigned char>(s[idx - 1]);
  std::size_t expected_len = 0;
  if ((lead & 0x80u) == 0u) {
    return;
  } else if ((lead & 0xE0u) == 0xC0u) {
    expected_len = 2;
  } else if ((lead & 0xF0u) == 0xE0u) {
    expected_len = 3;
  } else if ((lead & 0xF8u) == 0xF0u) {
    expected_len = 4;
  } else {
    return;
  }
  const std::size_t actual_len = s.size() - (idx - 1);
  if (actual_len < expected_len) {
    s.erase(idx - 1);
  }
}

std::string NormalizeRedisTypeFilter(std::string_view raw) {
  std::string lowered = detail::RedisLowerAscii(detail::RedisTrimAscii(raw));
  if (lowered == "all") {
    lowered.clear();
  }
  return lowered;
}

} // namespace

// ---- db++ Domain Adapters --------------------------------------------------

std::vector<ColumnMeta> RedisKeySchema() {
  return {
      ColumnMeta{.name = "key",
                 .type = "TEXT",
                 .nullable = false,
                 .primary_key = true,
                 .primary_key_order = 1},
      ColumnMeta{.name = "value", .type = "TEXT", .nullable = true},
      ColumnMeta{.name = "type", .type = "TEXT", .nullable = false},
      ColumnMeta{.name = "ttl", .type = "INTEGER", .nullable = false},
      ColumnMeta{.name = "size", .type = "INTEGER", .nullable = false},
  };
}

AdbcObjectsResult LoadRedisObjectsBlocking(RedisClient& client) {
  AdbcObjectsResult result;
  RedisValue info_reply;
  std::string info_err;
  const bool info_ok = client.Command({"INFO", "keyspace"}, info_reply, info_err);

  std::map<int, std::int64_t> counts;
  bool has_keyspace = false;
  if (info_ok && !info_reply.IsError() && !info_reply.IsNull()) {
    counts = ParseInfoKeyspace(info_reply.ToDisplayString());
    has_keyspace = true;
  }

  int db_limit = 16;
  RedisValue cfg_reply;
  std::string cfg_err;
  const bool cfg_ok = client.Command({"CONFIG", "GET", "databases"}, cfg_reply, cfg_err);
  if (cfg_ok && cfg_reply.kind == RedisValueKind::Array && cfg_reply.elements.size() >= 2) {
    if (const auto parsed = detail::RedisParseStrictInt64(cfg_reply.elements[1].ToDisplayString())) {
      db_limit = std::clamp<int>(static_cast<int>(*parsed), 1, 16);
    }
  }

  bool dbsize_ok = false;
  if (!has_keyspace) {
    RedisValue dbsize_reply;
    std::string dbsize_err;
    dbsize_ok = client.Command({"DBSIZE"}, dbsize_reply, dbsize_err);
    if (dbsize_ok && dbsize_reply.kind == RedisValueKind::Integer) {
      counts[client.CurrentDb()] = dbsize_reply.integer;
      has_keyspace = true;
    }
  }

  const bool any_cmd_succeeded = info_ok || cfg_ok || dbsize_ok;
  if (!any_cmd_succeeded && !client.IsConnected()) {
    RedisValue ping_reply;
    std::string ping_err;
    if (!client.Command({"PING"}, ping_reply, ping_err) || ping_reply.IsError()) {
      result.error =
          !ping_err.empty()
              ? ping_err
              : (!ping_reply.str.empty()
                     ? ping_reply.str
                     : std::string(Tr("Unable to connect to Redis server",
                                      "无法连接到 Redis 服务器")));
      return result;
    }
  }

  std::set<int> db_indices;
  for (int i = 0; i < db_limit; ++i) {
    db_indices.insert(i);
  }
  for (const auto& [k, count] : counts) {
    (void)count;
    db_indices.insert(k);
  }
  db_indices.insert(client.CurrentDb());

  const std::vector<ColumnMeta> schema = RedisKeySchema();
  for (const int idx : db_indices) {
    DbObject obj;
    obj.name = "db" + std::to_string(idx);
    obj.table_type = "REDIS DB";
    obj.is_view = false;
    obj.primary_key = {"key"};
    obj.columns = schema;
    if (const auto it = counts.find(idx); it != counts.end()) {
      obj.item_count = it->second;
    } else if (has_keyspace) {
      obj.item_count = 0;
    }
    result.objects.push_back(std::move(obj));
  }
  return result;
}

RedisPageResult LoadRedisKeysPageBlocking(RedisClient& client, const DbObject& object,
                                          std::int64_t offset, std::int64_t limit,
                                          std::optional<huxerui::DataTableSort> sort,
                                          std::string filter,
                                          const std::atomic<bool>* cancel_flag,
                                          std::string type_filter) {
  RedisPageResult result;
  result.columns = RedisKeySchema();

  const std::optional<int> target_db = ParseRedisDbName(object.name);
  if (!target_db.has_value()) {
    result.error = TrFmt("Invalid Redis database object name: {0}",
                         "无效的 Redis 数据库对象名: {0}", {object.name});
    return result;
  }
  if (!client.EnsureDb(*target_db, result.error)) {
    return result;
  }

  const auto is_cancelled = [&]() -> bool {
    return cancel_flag != nullptr && cancel_flag->load(std::memory_order_acquire);
  };

  const std::string_view trimmed_filter = detail::RedisTrimAscii(filter);
  const std::string pattern = trimmed_filter.empty() ? "*" : std::string(trimmed_filter);
  const std::string normalized_type = NormalizeRedisTypeFilter(type_filter);

  std::vector<std::string> keys;
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
      // Redis < 6.0 does not support SCAN ... TYPE; fall back to client-side TYPE pipeline.
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
            keys.push_back(std::move(k));
            if (keys.size() >= detail::kRedisMaxScanKeys) {
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
              keys.push_back(std::move(candidates[i]));
              if (keys.size() >= detail::kRedisMaxScanKeys) {
                result.scan_capped = (cursor != "0");
                break;
              }
            }
          }
        }
      }
    }
    if (cursor == "0" || keys.size() >= detail::kRedisMaxScanKeys) {
      break;
    }
    if (scan_iterations >= detail::kRedisMaxScanIterations) {
      result.scan_capped = true;
      result.scan_iterations_capped = true;
      break;
    }
  }

  if (is_cancelled()) {
    result.scan_cancelled = true;
    result.error = Tr("Operation cancelled", "操作已取消");
    return result;
  }

  result.total = static_cast<std::int64_t>(keys.size());
  const std::int64_t safe_offset = std::max<std::int64_t>(0, offset);
  const std::int64_t safe_limit = std::max<std::int64_t>(1, limit);

  const auto fetch_rows_for_keys = [&](const std::vector<std::string>& page_keys,
                                       std::vector<GridRow>& rows_out,
                                       std::string& err_out) -> bool {
    rows_out.clear();
    if (page_keys.empty()) {
      return true;
    }

    // Pipeline 1: fetch type and TTL in O(1) per key.
    std::vector<std::vector<std::string>> meta_cmds;
    meta_cmds.reserve(page_keys.size() * 2);
    for (const std::string& k : page_keys) {
      meta_cmds.push_back({"TYPE", k});
      meta_cmds.push_back({"TTL", k});
    }
    std::vector<RedisValue> meta_replies;
    if (!client.Pipeline(meta_cmds, meta_replies, err_out)) {
      return false;
    }

    std::vector<std::string> types(page_keys.size(), "none");
    std::vector<std::int64_t> ttls(page_keys.size(), -1);
    for (std::size_t i = 0; i < page_keys.size(); ++i) {
      if (i * 2 < meta_replies.size()) {
        types[i] = detail::RedisLowerAscii(meta_replies[i * 2].ToDisplayString());
      }
      if (!normalized_type.empty() && types[i] != normalized_type) {
        types[i] = "none";
      }
      if (i * 2 + 1 < meta_replies.size() &&
          meta_replies[i * 2 + 1].kind == RedisValueKind::Integer) {
        ttls[i] = meta_replies[i * 2 + 1].integer;
      }
    }

    // Pipeline 2: fetch O(1) size per key before deciding bounded preview commands.
    std::vector<std::vector<std::string>> size_cmds;
    std::vector<std::size_t> size_cmd_indices(page_keys.size(), static_cast<std::size_t>(-1));
    size_cmds.reserve(page_keys.size());
    for (std::size_t i = 0; i < page_keys.size(); ++i) {
      const std::string& k = page_keys[i];
      const std::string& t = types[i];
      if (t == "string") {
        size_cmd_indices[i] = size_cmds.size();
        size_cmds.push_back({"STRLEN", k});
      } else if (t == "hash") {
        size_cmd_indices[i] = size_cmds.size();
        size_cmds.push_back({"HLEN", k});
      } else if (t == "list") {
        size_cmd_indices[i] = size_cmds.size();
        size_cmds.push_back({"LLEN", k});
      } else if (t == "set") {
        size_cmd_indices[i] = size_cmds.size();
        size_cmds.push_back({"SCARD", k});
      } else if (t == "zset") {
        size_cmd_indices[i] = size_cmds.size();
        size_cmds.push_back({"ZCARD", k});
      } else if (t == "stream") {
        size_cmd_indices[i] = size_cmds.size();
        size_cmds.push_back({"XLEN", k});
      }
    }
    std::vector<RedisValue> size_replies;
    if (!size_cmds.empty() && !client.Pipeline(size_cmds, size_replies, err_out)) {
      return false;
    }

    std::vector<std::int64_t> sizes(page_keys.size(), 0);
    for (std::size_t i = 0; i < page_keys.size(); ++i) {
      const std::size_t idx = size_cmd_indices[i];
      if (idx < size_replies.size() && size_replies[idx].kind == RedisValueKind::Integer) {
        sizes[i] = size_replies[idx].integer;
      }
    }

    // Pipeline 3: fetch full value for small keys and safe bounded preview for large keys.
    struct FetchMapping {
      std::size_t key_idx = 0;
      std::size_t val_cmd_idx = static_cast<std::size_t>(-1);
      bool truncated = false;
      bool is_scan_reply = false;
      std::size_t preview_cap = 0;
    };
    std::vector<std::vector<std::string>> val_cmds;
    std::vector<FetchMapping> mappings;
    mappings.reserve(page_keys.size());

    for (std::size_t i = 0; i < page_keys.size(); ++i) {
      const std::string& k = page_keys[i];
      const std::string& t = types[i];
      if (t == "none") {
        continue;
      }
      const std::int64_t sz = sizes[i];
      const bool truncated = detail::IsRedisValuePreviewTruncated(t, sz);
      FetchMapping map{.key_idx = i, .truncated = truncated};
      if (t == "string") {
        map.val_cmd_idx = val_cmds.size();
        map.preview_cap = detail::kRedisMaxStringPreviewBytes;
        if (truncated) {
          val_cmds.push_back(
              {"GETRANGE", k, "0", std::to_string(detail::kRedisMaxStringPreviewBytes - 1)});
        } else {
          val_cmds.push_back({"GET", k});
        }
      } else if (t == "hash") {
        map.val_cmd_idx = val_cmds.size();
        map.preview_cap = detail::kRedisMaxCollectionPreview;
        if (truncated) {
          map.is_scan_reply = true;
          val_cmds.push_back(
              {"HSCAN", k, "0", "COUNT", std::to_string(detail::kRedisMaxCollectionPreview)});
        } else {
          val_cmds.push_back({"HGETALL", k});
        }
      } else if (t == "list") {
        map.val_cmd_idx = val_cmds.size();
        map.preview_cap = detail::kRedisMaxListZsetPreview;
        val_cmds.push_back(
            {"LRANGE", k, "0", std::to_string(detail::kRedisMaxListZsetPreview - 1)});
      } else if (t == "set") {
        map.val_cmd_idx = val_cmds.size();
        map.preview_cap = detail::kRedisMaxCollectionPreview;
        if (truncated) {
          map.is_scan_reply = true;
          val_cmds.push_back(
              {"SSCAN", k, "0", "COUNT", std::to_string(detail::kRedisMaxCollectionPreview)});
        } else {
          val_cmds.push_back({"SMEMBERS", k});
        }
      } else if (t == "zset") {
        map.val_cmd_idx = val_cmds.size();
        map.preview_cap = detail::kRedisMaxListZsetPreview;
        val_cmds.push_back({"ZRANGE", k, "0", std::to_string(detail::kRedisMaxListZsetPreview - 1),
                            "WITHSCORES"});
      } else if (t == "stream") {
        map.val_cmd_idx = val_cmds.size();
        map.preview_cap = detail::kRedisMaxStreamPreview;
        val_cmds.push_back(
            {"XRANGE", k, "-", "+", "COUNT", std::to_string(detail::kRedisMaxStreamPreview)});
      }
      mappings.push_back(map);
    }

    std::vector<RedisValue> val_replies;
    if (!val_cmds.empty() && !client.Pipeline(val_cmds, val_replies, err_out)) {
      return false;
    }

    rows_out.reserve(mappings.size());
    for (const FetchMapping& m : mappings) {
      const std::string& k = page_keys[m.key_idx];
      const std::string& t = types[m.key_idx];
      const std::int64_t sz = sizes[m.key_idx];
      const std::string size_str = std::to_string(sz);

      CellValue val_cell = NullCell();
      if (m.val_cmd_idx < val_replies.size()) {
        const RedisValue& vr = val_replies[m.val_cmd_idx];
        if (!vr.IsNull() && !vr.IsError()) {
          const std::vector<RedisValue>* elems = &vr.elements;
          std::vector<RedisValue> empty_elems;
          if (m.is_scan_reply) {
            if (vr.kind == RedisValueKind::Array && vr.elements.size() >= 2 &&
                vr.elements[1].kind == RedisValueKind::Array) {
              elems = &vr.elements[1].elements;
            } else {
              elems = &empty_elems;
            }
          }

          if (t == "string") {
            std::string s = vr.ToDisplayString();
            if (m.truncated) {
              detail::RedisTrimIncompleteUtf8Tail(s);
              s += TrFmt(" … [truncated: {0} / {1} bytes]",
                         " … [已截断：仅预览 {0} / {1} 字节]",
                         {std::to_string(s.size()), size_str});
            }
            val_cell = TextCell(std::move(s));
          } else {
            std::string formatted;
            if (t == "hash") {
              formatted =
                  detail::RedisFormatJsonHashPairs(*elems, detail::kRedisMaxCollectionPreview);
            } else if (t == "list" || t == "set") {
              formatted =
                  detail::RedisFormatJsonArrayOfStrings(*elems, detail::kRedisMaxCollectionPreview);
            } else if (t == "zset") {
              formatted =
                  detail::RedisFormatJsonZsetPairs(*elems, detail::kRedisMaxListZsetPreview);
            } else if (t == "stream") {
              formatted = detail::RedisFormatJsonStreamEntries(*elems);
            }
            if (m.truncated) {
              formatted += TrFmt(" … [truncated: previewing up to {0} / {1}]",
                                 " … [已截断：仅预览前 {0} / 共 {1} 项]",
                                 {std::to_string(m.preview_cap), size_str});
            }
            val_cell = TextCell(std::move(formatted));
          }
        }
      } else {
        val_cell = TextCell("<" + t + ">");
      }

      GridRow row;
      row.values = {
          TextCell(k),
          std::move(val_cell),
          TextCell(t),
          TextCell(std::to_string(ttls[m.key_idx])),
          TextCell(size_str),
      };
      row.key_values = {row.values[detail::kRedisColKey]};
      rows_out.push_back(std::move(row));
    }
    return true;
  };

  const bool non_key_sort =
      sort.has_value() && !sort->column_id.empty() && sort->column_id != "key";
  const bool descending =
      sort.has_value() && sort->direction == huxerui::DataTableSortDirection::Descending;

  if (non_key_sort && keys.size() <= 1000) {
    std::vector<GridRow> all_rows;
    if (!fetch_rows_for_keys(keys, all_rows, result.error)) {
      return result;
    }
    std::size_t sort_col = 0;
    for (std::size_t c = 0; c < result.columns.size(); ++c) {
      if (result.columns[c].name == sort->column_id) {
        sort_col = c;
        break;
      }
    }
    const bool numeric_col =
        (sort_col == detail::kRedisColTtl || sort_col == detail::kRedisColSize);
    std::ranges::sort(all_rows, [&](const GridRow& a, const GridRow& b) {
      const CellValue& ca = a.values[sort_col];
      const CellValue& cb = b.values[sort_col];
      int cmp = 0;
      if (numeric_col) {
        const std::int64_t na = detail::RedisParseStrictInt64(ca.text).value_or(0);
        const std::int64_t nb = detail::RedisParseStrictInt64(cb.text).value_or(0);
        cmp = (na < nb) ? -1 : (na > nb ? 1 : 0);
      } else {
        cmp = (ca.text < cb.text) ? -1 : (ca.text > cb.text ? 1 : 0);
      }
      if (cmp == 0) {
        cmp = (a.values[0].text < b.values[0].text)
                  ? -1
                  : (a.values[0].text > b.values[0].text ? 1 : 0);
      }
      return descending ? (cmp > 0) : (cmp < 0);
    });
    result.total = static_cast<std::int64_t>(all_rows.size());
    const std::size_t start =
        std::min(static_cast<std::size_t>(safe_offset), all_rows.size());
    const std::size_t end =
        std::min(start + static_cast<std::size_t>(safe_limit), all_rows.size());
    result.rows.assign(std::make_move_iterator(all_rows.begin() + static_cast<std::ptrdiff_t>(start)),
                       std::make_move_iterator(all_rows.begin() + static_cast<std::ptrdiff_t>(end)));
    return result;
  }

  std::ranges::sort(keys, [&](const std::string& a, const std::string& b) {
    return (sort.has_value() && sort->column_id == "key" && descending) ? (a > b) : (a < b);
  });
  const std::size_t start = std::min(static_cast<std::size_t>(safe_offset), keys.size());
  const std::size_t end =
      std::min(start + static_cast<std::size_t>(safe_limit), keys.size());
  std::vector<std::string> slice(keys.begin() + static_cast<std::ptrdiff_t>(start),
                                 keys.begin() + static_cast<std::ptrdiff_t>(end));
  if (!fetch_rows_for_keys(slice, result.rows, result.error)) {
    return result;
  }
  return result;
}

TableModel LoadRedisTableBlocking(RedisClient& client, std::uint64_t session_id,
                                  const DbObject& object, std::int64_t page,
                                  std::int64_t page_size,
                                  std::optional<huxerui::DataTableSort> sort, std::string filter,
                                  const std::atomic<bool>* cancel_flag,
                                  std::string type_filter) {
  const std::string normalized_type = NormalizeRedisTypeFilter(type_filter);
  TableModel model;
  model.session_id = session_id;
  model.object = object.name;
  model.qualified_name = object.name;
  model.is_view = false;
  model.read_only = false;
  model.editable = true;
  model.columns = RedisKeySchema();
  model.key_columns = {0};
  model.page_size = std::max<std::int64_t>(1, page_size);
  model.sort = sort;
  model.filter = filter;
  model.redis_type_filter = normalized_type;

  const auto started = std::chrono::steady_clock::now();
  const std::int64_t req_page = std::max<std::int64_t>(0, page);
  const std::int64_t req_offset =
      (req_page > 0 && model.page_size > std::numeric_limits<std::int64_t>::max() / req_page)
          ? std::numeric_limits<std::int64_t>::max()
          : req_page * model.page_size;
  RedisPageResult page_res = LoadRedisKeysPageBlocking(client, object, req_offset, model.page_size,
                                                       sort, filter, cancel_flag, normalized_type);
  std::int64_t effective_offset = req_offset;
  if (!page_res.error.empty()) {
    model.error = page_res.error;
    model.connection_lost = !client.IsConnected();
    return model;
  }

  model.total = page_res.total;
  const std::int64_t page_count =
      model.total <= 0 ? 1 : ((model.total - 1) / model.page_size + 1);
  const std::int64_t clamped_page = std::clamp<std::int64_t>(req_page, 0, page_count - 1);
  if (clamped_page != req_page) {
    const std::int64_t clamped_offset =
        (clamped_page > 0 &&
         model.page_size > std::numeric_limits<std::int64_t>::max() / clamped_page)
            ? std::numeric_limits<std::int64_t>::max()
            : clamped_page * model.page_size;
    effective_offset = clamped_offset;
    page_res = LoadRedisKeysPageBlocking(client, object, clamped_offset, model.page_size, sort,
                                         filter, cancel_flag, normalized_type);
    if (!page_res.error.empty()) {
      model.error = page_res.error;
      model.connection_lost = !client.IsConnected();
      return model;
    }
    model.total = page_res.total;
  }
  model.elapsed_us = std::max<std::int64_t>(
      0, std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - started)
             .count());
  model.executed_sql = "SCAN " + object.name +
                       (filter.empty() ? std::string{} : " MATCH " + filter) +
                       (normalized_type.empty() ? std::string{} : " TYPE " + normalized_type) +
                       " LIMIT " + std::to_string(model.page_size) +
                       " OFFSET " + std::to_string(effective_offset);
  model.page = clamped_page;
  model.snapshot = std::make_shared<const GridSnapshot>(GridSnapshot{.rows = std::move(page_res.rows)});
  model.message = TrFmt("{0} keys · {1} pages", "{0} 个键 · 共 {1} 页",
                        {std::to_string(model.total), std::to_string(page_count)});
  if (page_res.scan_iterations_capped) {
    model.message += TrFmt(" (scan paused after {0} batches — narrow MATCH pattern)",
                           "（已达 {0} 批扫描上限，请缩小 MATCH 过滤范围）",
                           {std::to_string(detail::kRedisMaxScanIterations)});
  } else if (page_res.scan_capped) {
    model.message += TrFmt(" (scan limit {0} keys)", "（最多扫描 {0} 键）",
                           {std::to_string(detail::kRedisMaxScanKeys)});
  }
  return model;
}

namespace detail {

bool IsRedisValuePreviewTruncated(std::string_view type, std::int64_t size) noexcept {
  if (size <= 0) {
    return false;
  }
  if (type == "string") {
    return size > static_cast<std::int64_t>(kRedisMaxStringPreviewBytes);
  }
  if (type == "hash" || type == "set") {
    return size > static_cast<std::int64_t>(kRedisMaxCollectionPreview);
  }
  if (type == "list" || type == "zset") {
    return size > static_cast<std::int64_t>(kRedisMaxListZsetPreview);
  }
  if (type == "stream") {
    return size > static_cast<std::int64_t>(kRedisMaxStreamPreview);
  }
  return false;
}

void RedisTrimIncompleteUtf8Tail(std::string& s) { TrimIncompleteUtf8Tail(s); }

std::string RedisNormalizeTypeFilter(std::string_view raw) {
  return NormalizeRedisTypeFilter(raw);
}

CellValue RedisTextCell(std::string text) { return TextCell(std::move(text)); }
CellValue RedisNullCell() { return NullCell(); }
CellValue RedisReplyToCell(const RedisValue& val) { return ReplyToCell(val); }

} // namespace detail

} // namespace dbpp
