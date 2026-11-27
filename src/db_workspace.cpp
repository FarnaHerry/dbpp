module;

#include <huxerui/datatable.h>
#include <huxerui/http.h>
#include <huxerui/huxerui.h>
#include <huxerui/task.h>

module dbpp.db;

import std;
import dbpp.settings;
import dbpp.i18n;
import dbpp.types;
import dbpp.adbc;
import dbpp.redis;

namespace dbpp {

using namespace huxerui;

void ActivateWorkspaceTab(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                          std::uint64_t tab_id) {
  if (workspace.Get().FindTab(tab_id) == nullptr) {
    return;
  }
  workspace.Update([tab_id](WorkspaceModel& ws) { ws.active_tab_id = tab_id; });
  const WorkspaceModel& ws_now = workspace.Get();
  app.Update([&ws_now](AppModel& model) { detail::DbSyncAppFromActiveTab(model, ws_now); });
}

void CloseWorkspaceTabs(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                        const std::vector<std::uint64_t>& tab_ids) {
  if (tab_ids.empty()) {
    return;
  }
  workspace.Update([&tab_ids](WorkspaceModel& ws) {
    const std::uint64_t old_active = ws.active_tab_id;
    std::size_t old_active_index = 0;
    for (std::size_t i = 0; i < ws.tabs.size(); ++i) {
      if (ws.tabs[i].id == old_active) {
        old_active_index = i;
        break;
      }
    }
    const auto should_remove = [&tab_ids](const WorkspaceTab& tab) {
      return std::ranges::find(tab_ids, tab.id) != tab_ids.end();
    };
    const bool active_removed = ws.FindTab(old_active) != nullptr &&
                                std::ranges::find(tab_ids, old_active) != tab_ids.end();
    std::erase_if(ws.tabs, should_remove);
    if (ws.tabs.empty()) {
      ws.active_tab_id = 0;
    } else if (active_removed || ws.FindTab(ws.active_tab_id) == nullptr) {
      const std::size_t next_index = old_active_index > 0
                                         ? std::min(old_active_index - 1, ws.tabs.size() - 1)
                                         : 0;
      ws.active_tab_id = ws.tabs[next_index].id;
    }
  });
  const WorkspaceModel& ws_now = workspace.Get();
  app.Update([&ws_now](AppModel& model) { detail::DbSyncAppFromActiveTab(model, ws_now); });
}

std::vector<std::string> ListSessionDatabases(const DbSession& session) {
  std::vector<std::string> databases;
  if (session.dialect == Dialect::Redis) {
    for (const DbObject& object : session.objects) {
      if (!object.name.empty() &&
          std::ranges::find(databases, object.name) == databases.end()) {
        databases.push_back(object.name);
      }
    }
    if (session.session && session.session->redis) {
      const std::string current = "db" + std::to_string(session.session->redis->CurrentDb());
      if (std::ranges::find(databases, current) == databases.end()) {
        databases.insert(databases.begin(), current);
      }
    }
    if (databases.empty()) {
      databases.emplace_back("db0");
    }
    return databases;
  }

  const bool multi_catalog = detail::DbSessionHasMultipleCatalogs(session);
  const DatabasePreset& preset = ResolvePresetForProfile(session.driver_path, session.uri);
  const std::string uri_db = detail::DbTrimAscii(DecomposePresetUri(preset, session.uri).database);

  const auto add_unique = [&databases](std::string name) {
    name = detail::DbTrimAscii(name);
    if (name.empty()) {
      return;
    }
    const std::string lower = detail::DbLowerAscii(name);
    for (const std::string& existing : databases) {
      if (detail::DbLowerAscii(existing) == lower) {
        return;
      }
    }
    databases.push_back(std::move(name));
  };

  for (const DbObject& object : session.objects) {
    add_unique(detail::DbSessionObjectBucket(session, object, multi_catalog));
  }

  if ((session.dialect == Dialect::Mysql || session.dialect == Dialect::Generic) &&
      !uri_db.empty()) {
    add_unique(uri_db);
  }
  if ((session.dialect == Dialect::Sqlite || session.dialect == Dialect::Duckdb) &&
      databases.empty()) {
    databases.emplace_back("main");
  }
  if (session.dialect == Dialect::Postgresql && databases.empty()) {
    databases.emplace_back("public");
  }

  std::string target_lower = detail::DbLowerAscii(uri_db);
  if (target_lower.empty() &&
      (session.dialect == Dialect::Sqlite || session.dialect == Dialect::Duckdb)) {
    target_lower = "main";
  } else if (session.dialect == Dialect::Postgresql) {
    const bool matches_uri_db =
        !target_lower.empty() &&
        std::ranges::any_of(databases, [&target_lower](const std::string& db) {
          return detail::DbLowerAscii(db) == target_lower;
        });
    if (!matches_uri_db) {
      target_lower = "public";
    }
  }

  std::ranges::sort(databases, [&target_lower](const std::string& a, const std::string& b) {
    const std::string la = detail::DbLowerAscii(a);
    const std::string lb = detail::DbLowerAscii(b);
    const bool a_target = !target_lower.empty() && la == target_lower;
    const bool b_target = !target_lower.empty() && lb == target_lower;
    if (a_target != b_target) {
      return a_target;
    }
    const bool a_sys = IsSystemDatabaseOrSchema(a);
    const bool b_sys = IsSystemDatabaseOrSchema(b);
    if (a_sys != b_sys) {
      return !a_sys;
    }
    return a < b;
  });
  return databases;
}

std::string DefaultSessionDatabase(const DbSession& session) {
  if (session.dialect == Dialect::Redis && session.session && session.session->redis) {
    return "db" + std::to_string(session.session->redis->CurrentDb());
  }
  const std::vector<std::string> databases = ListSessionDatabases(session);
  return databases.empty() ? std::string{} : databases.front();
}

std::string BuildSwitchDatabaseSql(Dialect dialect, std::string_view target_database) {
  const std::string trimmed = detail::DbTrimAscii(target_database);
  if (trimmed.empty() || dialect == Dialect::Sqlite || dialect == Dialect::Redis) {
    return {};
  }
  if (dialect == Dialect::Mysql) {
    return "USE " + QuoteIdentifier(Dialect::Mysql, trimmed);
  }
  if (dialect == Dialect::Postgresql) {
    const std::size_t dot = trimmed.rfind('.');
    const std::string_view schema =
        (dot != std::string_view::npos && dot + 1 < trimmed.size())
            ? std::string_view(trimmed).substr(dot + 1)
            : std::string_view(trimmed);
    return "SET search_path TO " + QuoteIdentifier(Dialect::Postgresql, schema);
  }
  std::string joined;
  std::size_t pos = 0;
  while (pos < trimmed.size()) {
    const std::size_t next = trimmed.find('.', pos);
    const std::string_view part =
        next == std::string_view::npos
            ? std::string_view(trimmed).substr(pos)
            : std::string_view(trimmed).substr(pos, next - pos);
    if (!part.empty()) {
      if (!joined.empty()) {
        joined.push_back('.');
      }
      joined += QuoteIdentifier(dialect, part);
    }
    if (next == std::string_view::npos) {
      break;
    }
    pos = next + 1;
  }
  if (joined.empty()) {
    return {};
  }
  return "USE " + joined;
}

std::uint64_t OpenSqlTab(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                         std::uint64_t session_id, std::string initial_sql,
                         std::string custom_title, std::string initial_database) {
  const DbSession* session = detail::DbFindSession(app.Get(), session_id);
  if (session == nullptr) {
    return 0;
  }
  std::string resolved_db = detail::DbTrimAscii(initial_database);
  if (resolved_db.empty()) {
    resolved_db = DefaultSessionDatabase(*session);
  }
  const std::int64_t initial_page_size =
      app.Get().default_page_size > 0 ? app.Get().default_page_size : kPageSize;
  std::uint64_t created_id = 0;
  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab tab;
    tab.id = ws.next_tab_id++;
    created_id = tab.id;
    tab.kind = WorkspaceTabKind::Sql;
    tab.session_id = session_id;
    tab.session_name = session->name;
    tab.session_uri = session->uri;
    const bool is_redis = session->dialect == Dialect::Redis;
    const std::string tab_prefix = is_redis ? "Redis " : std::string(Tr("Query ", "查询 "));
    tab.title = custom_title.empty()
                    ? (tab_prefix + std::to_string(ws.next_sql_index++))
                    : std::move(custom_title);
    tab.sql.editor = TextEditingValue::FromText(
        initial_sql.empty() ? std::string{is_redis ? "PING" : "SELECT 1;"}
                            : std::move(initial_sql));
    tab.sql.target_database = std::move(resolved_db);
    tab.sql.page_size = initial_page_size;
    ws.tabs.push_back(std::move(tab));
    ws.active_tab_id = created_id;
  });
  app.Update([session_id](AppModel& model) {
    model.active_session = session_id;
    model.active_object.clear();
    model.active_is_view = false;
  });
  return created_id;
}

bool SwitchSqlTabSession(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                         std::uint64_t tab_id, std::uint64_t session_id) {
  const DbSession* session = detail::DbFindSession(app.Get(), session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    return false;
  }
  const std::string target_name = session->name;
  const std::string target_uri = session->uri;
  const bool target_is_redis = session->dialect == Dialect::Redis;
  const std::string target_db = DefaultSessionDatabase(*session);

  bool switched = false;
  bool sync_app = false;
  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab* target = ws.FindTab(tab_id);
    if (target == nullptr || target->kind != WorkspaceTabKind::Sql || target->sql.running) {
      return;
    }
    if (target->session_id == session_id && target->session_name == target_name &&
        target->session_uri == target_uri) {
      return;
    }
    target->session_id = session_id;
    target->session_name = target_name;
    target->session_uri = target_uri;
    target->sql.target_database = target_db;
    target->sql.page = 0;
    target->sql.paginated = false;
    target->sql.has_next_page = false;
    target->sql.last_sql.clear();
    target->sql.last_database.clear();
    target->sql.last_executed_sql.clear();
    target->sql.last_elapsed_us = -1;
    target->sql.error.clear();
    target->sql.status.clear();
    const std::string trimmed = detail::DbTrimAscii(target->sql.editor.text);
    if (target_is_redis && trimmed == "SELECT 1;") {
      target->sql.editor = TextEditingValue::FromText("PING");
    } else if (!target_is_redis && trimmed == "PING") {
      target->sql.editor = TextEditingValue::FromText("SELECT 1;");
    }
    switched = true;
    sync_app = (ws.active_tab_id == tab_id);
  });
  if (switched && sync_app) {
    app.Update([session_id](AppModel& model) {
      model.active_session = session_id;
      model.active_object.clear();
      model.active_is_view = false;
    });
  }
  return switched;
}

bool SwitchSqlTabDatabase(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                          std::uint64_t tab_id, std::string database) {
  (void)app;
  std::string next_db = detail::DbTrimAscii(database);
  bool switched = false;
  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab* target = ws.FindTab(tab_id);
    if (target == nullptr || target->kind != WorkspaceTabKind::Sql || target->sql.running) {
      return;
    }
    if (target->sql.target_database == next_db) {
      return;
    }
    target->sql.target_database = std::move(next_db);
    target->sql.page = 0;
    target->sql.paginated = false;
    target->sql.has_next_page = false;
    target->sql.last_sql.clear();
    target->sql.last_database.clear();
    target->sql.last_executed_sql.clear();
    target->sql.last_elapsed_us = -1;
    target->sql.error.clear();
    switched = true;
  });
  return switched;
}

std::uint64_t OpenSettingsTab(const State<AppModel>& app, const State<WorkspaceModel>& workspace) {
  if (const WorkspaceTab* existing = workspace.Get().FindSettingsTab()) {
    ActivateWorkspaceTab(app, workspace, existing->id);
    return existing->id;
  }
  std::uint64_t created_id = 0;
  workspace.Update([&](WorkspaceModel& ws) {
    if (const WorkspaceTab* existing = ws.FindSettingsTab()) {
      created_id = existing->id;
      ws.active_tab_id = created_id;
      return;
    }
    WorkspaceTab tab;
    tab.id = ws.next_tab_id++;
    created_id = tab.id;
    tab.kind = WorkspaceTabKind::Settings;
    tab.session_id = 0;
    tab.title = std::string(Tr("Settings", "设置"));
    ws.tabs.push_back(std::move(tab));
    ws.active_tab_id = created_id;
  });
  const WorkspaceModel& ws_now = workspace.Get();
  app.Update([&ws_now](AppModel& model) { detail::DbSyncAppFromActiveTab(model, ws_now); });
  return created_id;
}

std::uint64_t OpenTableDesignTab(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                                 std::uint64_t session_id, DbObject object,
                                 std::string display_label) {
  if (const WorkspaceTab* existing =
          workspace.Get().FindTableDesignTab(session_id, object.name, object.db_schema)) {
    workspace.Update([&](WorkspaceModel& ws) {
      if (WorkspaceTab* mutable_tab = ws.FindTab(existing->id)) {
        mutable_tab->design_object = object;
        if (!display_label.empty()) {
          mutable_tab->design_display_label = display_label;
        }
      }
    });
    ActivateWorkspaceTab(app, workspace, existing->id);
    return existing->id;
  }
  const DbSession* session = detail::DbFindSession(app.Get(), session_id);
  if (session == nullptr) {
    return 0;
  }

  const std::string label =
      display_label.empty() ? detail::DbFormatTabTitle(*session, object.name, object.db_schema)
                            : display_label;
  std::uint64_t created_id = 0;
  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab tab;
    tab.id = ws.next_tab_id++;
    created_id = tab.id;
    tab.kind = WorkspaceTabKind::TableDesign;
    tab.session_id = session_id;
    tab.session_name = session->name;
    tab.session_uri = session->uri;
    tab.title = TrFmt("Design · {0}", "表结构 · {0}", {label});
    tab.object_name = object.name;
    tab.object_schema = object.db_schema;
    tab.is_view = object.is_view;
    tab.design_object = std::move(object);
    tab.design_display_label = label;
    ws.tabs.push_back(std::move(tab));
    ws.active_tab_id = created_id;
  });
  const WorkspaceModel& ws_now = workspace.Get();
  app.Update([&ws_now](AppModel& model) {
    detail::DbSyncAppFromActiveTab(model, ws_now);
    model.error.clear();
  });
  return created_id;
}

void SelectObject(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                  const TaskScope& tasks, std::uint64_t session_id, std::string object,
                  std::string db_schema, bool is_view, NotifyFn notify) {
  if (const WorkspaceTab* existing =
          workspace.Get().FindTableTab(session_id, object, db_schema, is_view)) {
    ActivateWorkspaceTab(app, workspace, existing->id);
    return;
  }
  const DbSession* session = detail::DbFindSession(app.Get(), session_id);
  if (session == nullptr) {
    return;
  }

  const std::int64_t initial_page_size =
      app.Get().default_page_size > 0 ? app.Get().default_page_size : kPageSize;
  std::uint64_t created_id = 0;
  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab tab;
    tab.id = ws.next_tab_id++;
    created_id = tab.id;
    tab.kind = WorkspaceTabKind::Table;
    tab.session_id = session_id;
    tab.session_name = session->name;
    tab.session_uri = session->uri;
    tab.title = detail::DbFormatTabTitle(*session, object, db_schema);
    tab.object_name = std::move(object);
    tab.object_schema = std::move(db_schema);
    tab.is_view = is_view;
    tab.table.page_size = initial_page_size;
    ws.tabs.push_back(std::move(tab));
    ws.active_tab_id = created_id;
  });
  const WorkspaceModel& ws_now = workspace.Get();
  app.Update([&ws_now](AppModel& model) {
    detail::DbSyncAppFromActiveTab(model, ws_now);
    model.error.clear();
  });
  RequestLoadTable(app, workspace, tasks, created_id, 0, std::nullopt, {}, false,
                   std::move(notify), initial_page_size);
}

namespace {

bool IsWideDisplayCodepoint(std::uint32_t cp) {
  return (cp >= 0x1100U && cp <= 0x115FU) || (cp >= 0x2E80U && cp <= 0x303EU) ||
         (cp >= 0x3040U && cp <= 0x33FFU) || (cp >= 0x3400U && cp <= 0x4DBFU) ||
         (cp >= 0x4E00U && cp <= 0x9FFFU) || (cp >= 0xA960U && cp <= 0xA97FU) ||
         (cp >= 0xAC00U && cp <= 0xD7A3U) || (cp >= 0xF900U && cp <= 0xFAFFU) ||
         (cp >= 0xFE10U && cp <= 0xFE6FU) || (cp >= 0xFF00U && cp <= 0xFF60U) ||
         (cp >= 0xFFE0U && cp <= 0xFFE6U) || cp >= 0x1F000U;
}

std::size_t Utf8CodepointLength(std::string_view text, std::size_t index, std::uint32_t* codepoint) {
  const auto b0 = static_cast<unsigned char>(text[index]);
  if (b0 < 0x80U) {
    if (codepoint != nullptr) {
      *codepoint = b0;
    }
    return 1;
  }
  const auto is_cont = [&](std::size_t pos) {
    return pos < text.size() && (static_cast<unsigned char>(text[pos]) & 0xC0U) == 0x80U;
  };
  if ((b0 & 0xE0U) == 0xC0U && is_cont(index + 1)) {
    if (codepoint != nullptr) {
      *codepoint = ((static_cast<std::uint32_t>(b0 & 0x1FU)) << 6U) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3FU);
    }
    return 2;
  }
  if ((b0 & 0xF0U) == 0xE0U && is_cont(index + 1) && is_cont(index + 2)) {
    if (codepoint != nullptr) {
      *codepoint =
          ((static_cast<std::uint32_t>(b0 & 0x0FU)) << 12U) |
          ((static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3FU)) << 6U) |
          static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 2]) & 0x3FU);
    }
    return 3;
  }
  if ((b0 & 0xF8U) == 0xF0U && is_cont(index + 1) && is_cont(index + 2) && is_cont(index + 3)) {
    if (codepoint != nullptr) {
      *codepoint =
          ((static_cast<std::uint32_t>(b0 & 0x07U)) << 18U) |
          ((static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3FU)) << 12U) |
          ((static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 2]) & 0x3FU)) << 6U) |
          static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 3]) & 0x3FU);
    }
    return 4;
  }
  if (codepoint != nullptr) {
    *codepoint = b0;
  }
  return 1;
}

float EstimateDisplayUnits(std::string_view text, float stop_units) {
  float units = 0.0F;
  std::size_t i = 0;
  while (i < text.size()) {
    std::uint32_t cp = 0;
    const std::size_t len = Utf8CodepointLength(text, i, &cp);
    if (cp < 0x80U) {
      units += 1.0F;
    } else if (IsWideDisplayCodepoint(cp)) {
      units += 1.8F;
    } else {
      units += 1.1F;
    }
    if (units >= stop_units) {
      return stop_units;
    }
    i += len;
  }
  return units;
}

} // namespace

std::string SingleLineCellPreview(std::string_view text) {
  std::string sanitized;
  if (!IsValidUtf8(text)) {
    sanitized = SanitizeUtf8(text);
    text = sanitized;
  }
  constexpr std::size_t kMaxPreviewBytes = 1024;
  std::string out;
  out.reserve(std::min(text.size(), kMaxPreviewBytes));
  bool truncated = false;
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '\r' || c == '\n' || c == '\t') {
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
      if (out.empty() || out.back() != ' ') {
        if (out.size() + 1 > kMaxPreviewBytes) {
          truncated = true;
          break;
        }
        out.push_back(' ');
      }
      ++i;
      continue;
    }
    const std::size_t len = Utf8CodepointLength(text, i, nullptr);
    if (out.size() + len > kMaxPreviewBytes) {
      truncated = true;
      break;
    }
    out.append(text.substr(i, len));
    i += len;
  }
  if (truncated) {
    out += "…";
  }
  return out;
}

float ComputeAutoColumnWidth(std::string_view header, std::size_t col_index,
                             const GridSnapshot* snapshot) {
  constexpr std::size_t kMaxSampleRows = 50;
  constexpr float kMaxMeasuredUnits = 64.0F;
  const float header_units =
      EstimateDisplayUnits(SingleLineCellPreview(header), kMaxMeasuredUnits);
  const float header_px = header_units * 8.5F + 44.0F;

  float content_px = 0.0F;
  if (snapshot != nullptr && !snapshot->rows.empty()) {
    float max_cell_units = 0.0F;
    const std::size_t row_limit = std::min(snapshot->rows.size(), kMaxSampleRows);
    for (std::size_t row_idx = 0; row_idx < row_limit; ++row_idx) {
      const GridRow& row = snapshot->rows[row_idx];
      if (col_index >= row.values.size()) {
        continue;
      }
      const CellValue& cell = row.values[col_index];
      const float units =
          cell.is_null ? 4.0F
                       : EstimateDisplayUnits(SingleLineCellPreview(cell.DisplayText()),
                                              kMaxMeasuredUnits);
      max_cell_units = std::max(max_cell_units, units);
      if (max_cell_units >= kMaxMeasuredUnits) {
        break;
      }
    }
    content_px = max_cell_units * 8.2F + 26.0F;
  }

  const float raw_width = std::max(header_px, content_px);
  return std::round(std::clamp(raw_width, kAutoMinColumnWidth, kAutoMaxColumnWidth));
}

std::vector<std::string> OrderConnectionKeys(const std::vector<std::string>& natural_keys,
                                             const std::vector<std::string>& custom_order) {
  std::vector<std::string> ordered;
  ordered.reserve(natural_keys.size());
  for (const std::string& key : custom_order) {
    if (std::ranges::find(natural_keys, key) != natural_keys.end() &&
        std::ranges::find(ordered, key) == ordered.end()) {
      ordered.push_back(key);
    }
  }
  for (const std::string& key : natural_keys) {
    if (std::ranges::find(ordered, key) == ordered.end()) {
      ordered.push_back(key);
    }
  }
  return ordered;
}

std::vector<std::string> ReorderConnectionKeys(const std::vector<std::string>& natural_keys,
                                               const std::vector<std::string>& custom_order,
                                               std::string_view from_key,
                                               std::string_view to_key) {
  std::vector<std::string> ordered = OrderConnectionKeys(natural_keys, custom_order);
  const auto from_it = std::ranges::find(ordered, from_key);
  const auto to_it = std::ranges::find(ordered, to_key);
  if (from_it == ordered.end() || to_it == ordered.end() || from_it == to_it) {
    return ordered;
  }
  const std::size_t from_idx = static_cast<std::size_t>(from_it - ordered.begin());
  const std::size_t to_idx = static_cast<std::size_t>(to_it - ordered.begin());
  std::string moved = std::move(ordered[from_idx]);
  ordered.erase(ordered.begin() + static_cast<std::ptrdiff_t>(from_idx));
  ordered.insert(ordered.begin() + static_cast<std::ptrdiff_t>(to_idx), std::move(moved));
  return ordered;
}

} // namespace dbpp
