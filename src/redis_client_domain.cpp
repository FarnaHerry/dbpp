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

} // namespace

// ---- db++ Domain Adapters --------------------------------------------------

std::vector<ColumnMeta> RedisKeySchema() {
  return {
      ColumnMeta{.name = "key",
                 .type = "TEXT",
                 .nullable = false,
                 .primary_key = true,
                 .primary_key_order = 1},
      ColumnMeta{.name = "type", .type = "TEXT", .nullable = false},
      ColumnMeta{.name = "ttl", .type = "INTEGER", .nullable = false},
      ColumnMeta{.name = "size", .type = "INTEGER", .nullable = false},
      ColumnMeta{.name = "value", .type = "TEXT", .nullable = true},
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
                                          std::string filter) {
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

  const std::string_view trimmed_filter = detail::RedisTrimAscii(filter);
  const std::string pattern = trimmed_filter.empty() ? "*" : std::string(trimmed_filter);

  std::vector<std::string> keys;
  std::unordered_set<std::string> seen;
  std::string cursor = "0";
  for (;;) {
    RedisValue scan_reply;
    if (!client.Command({"SCAN", cursor, "MATCH", pattern, "COUNT", "500"}, scan_reply,
                        result.error)) {
      return result;
    }
    if (scan_reply.IsError()) {
      result.error = scan_reply.str;
      return result;
    }
    if (scan_reply.kind != RedisValueKind::Array || scan_reply.elements.size() < 2) {
      result.error = Tr("Invalid SCAN response format", "SCAN 返回格式无效");
      return result;
    }
    cursor = scan_reply.elements[0].ToDisplayString();
    const RedisValue& batch = scan_reply.elements[1];
    if (batch.kind == RedisValueKind::Array) {
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
    }
    if (cursor == "0" || keys.size() >= detail::kRedisMaxScanKeys) {
      break;
    }
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
      if (i * 2 + 1 < meta_replies.size() &&
          meta_replies[i * 2 + 1].kind == RedisValueKind::Integer) {
        ttls[i] = meta_replies[i * 2 + 1].integer;
      }
    }

    // Batch fetch size and value preview for each key according to its type.
    struct FetchMapping {
      std::size_t key_idx = 0;
      std::size_t size_cmd_idx = static_cast<std::size_t>(-1);
      std::size_t val_cmd_idx = static_cast<std::size_t>(-1);
    };
    std::vector<std::vector<std::string>> data_cmds;
    std::vector<FetchMapping> mappings;
    mappings.reserve(page_keys.size());

    for (std::size_t i = 0; i < page_keys.size(); ++i) {
      const std::string& k = page_keys[i];
      const std::string& t = types[i];
      FetchMapping map{.key_idx = i};
      if (t == "string") {
        map.size_cmd_idx = data_cmds.size();
        data_cmds.push_back({"STRLEN", k});
        map.val_cmd_idx = data_cmds.size();
        data_cmds.push_back({"GET", k});
      } else if (t == "hash") {
        map.size_cmd_idx = data_cmds.size();
        data_cmds.push_back({"HLEN", k});
        map.val_cmd_idx = data_cmds.size();
        data_cmds.push_back({"HGETALL", k});
      } else if (t == "list") {
        map.size_cmd_idx = data_cmds.size();
        data_cmds.push_back({"LLEN", k});
        map.val_cmd_idx = data_cmds.size();
        data_cmds.push_back(
            {"LRANGE", k, "0", std::to_string(detail::kRedisMaxListZsetPreview - 1)});
      } else if (t == "set") {
        map.size_cmd_idx = data_cmds.size();
        data_cmds.push_back({"SCARD", k});
        map.val_cmd_idx = data_cmds.size();
        data_cmds.push_back({"SMEMBERS", k});
      } else if (t == "zset") {
        map.size_cmd_idx = data_cmds.size();
        data_cmds.push_back({"ZCARD", k});
        map.val_cmd_idx = data_cmds.size();
        data_cmds.push_back({"ZRANGE", k, "0", std::to_string(detail::kRedisMaxListZsetPreview - 1),
                             "WITHSCORES"});
      } else if (t == "stream") {
        map.size_cmd_idx = data_cmds.size();
        data_cmds.push_back({"XLEN", k});
        map.val_cmd_idx = data_cmds.size();
        data_cmds.push_back({"XRANGE", k, "-", "+", "COUNT",
                             std::to_string(detail::kRedisMaxStreamPreview)});
      }
      mappings.push_back(map);
    }

    std::vector<RedisValue> data_replies;
    if (!data_cmds.empty() && !client.Pipeline(data_cmds, data_replies, err_out)) {
      return false;
    }

    rows_out.reserve(page_keys.size());
    for (const FetchMapping& m : mappings) {
      const std::string& k = page_keys[m.key_idx];
      const std::string& t = types[m.key_idx];
      if (t == "none") {
        continue;
      }
      std::string size_str = "0";
      if (m.size_cmd_idx < data_replies.size() &&
          data_replies[m.size_cmd_idx].kind == RedisValueKind::Integer) {
        size_str = std::to_string(data_replies[m.size_cmd_idx].integer);
      }

      CellValue val_cell = NullCell();
      if (m.val_cmd_idx < data_replies.size()) {
        const RedisValue& vr = data_replies[m.val_cmd_idx];
        if (!vr.IsNull() && !vr.IsError()) {
          if (t == "string") {
            val_cell = TextCell(vr.ToDisplayString());
          } else if (t == "hash") {
            val_cell = TextCell(detail::RedisFormatJsonHashPairs(vr.elements, detail::kRedisMaxCollectionPreview));
          } else if (t == "list" || t == "set") {
            val_cell = TextCell(detail::RedisFormatJsonArrayOfStrings(vr.elements, detail::kRedisMaxCollectionPreview));
          } else if (t == "zset") {
            val_cell = TextCell(detail::RedisFormatJsonZsetPairs(vr.elements, detail::kRedisMaxListZsetPreview));
          } else if (t == "stream") {
            val_cell = TextCell(detail::RedisFormatJsonStreamEntries(vr.elements));
          }
        }
      } else {
        val_cell = TextCell("<" + t + ">");
      }

      GridRow row;
      row.values = {
          TextCell(k),
          TextCell(t),
          TextCell(std::to_string(ttls[m.key_idx])),
          TextCell(size_str),
          std::move(val_cell),
      };
      row.key_values = {row.values[0]};
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
    const bool numeric_col = (sort_col == 2 || sort_col == 3);
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
                                  std::optional<huxerui::DataTableSort> sort, std::string filter) {
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

  const auto started = std::chrono::steady_clock::now();
  const std::int64_t req_page = std::max<std::int64_t>(0, page);
  const std::int64_t req_offset =
      (req_page > 0 && model.page_size > std::numeric_limits<std::int64_t>::max() / req_page)
          ? std::numeric_limits<std::int64_t>::max()
          : req_page * model.page_size;
  RedisPageResult page_res =
      LoadRedisKeysPageBlocking(client, object, req_offset, model.page_size, sort, filter);
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
    page_res =
        LoadRedisKeysPageBlocking(client, object, clamped_offset, model.page_size, sort, filter);
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
                       " LIMIT " + std::to_string(model.page_size) +
                       " OFFSET " + std::to_string(effective_offset);
  model.page = clamped_page;
  model.snapshot = std::make_shared<const GridSnapshot>(GridSnapshot{.rows = std::move(page_res.rows)});
  model.message = TrFmt("{0} keys · {1} pages", "{0} 个键 · 共 {1} 页",
                        {std::to_string(model.total), std::to_string(page_count)});
  if (page_res.scan_capped) {
    model.message += TrFmt(" (scan limit {0} keys)", "（最多扫描 {0} 键）",
                           {std::to_string(detail::kRedisMaxScanKeys)});
  }
  return model;
}

namespace detail {

CellValue RedisTextCell(std::string text) { return TextCell(std::move(text)); }
CellValue RedisNullCell() { return NullCell(); }
CellValue RedisReplyToCell(const RedisValue& val) { return ReplyToCell(val); }

} // namespace detail

} // namespace dbpp
