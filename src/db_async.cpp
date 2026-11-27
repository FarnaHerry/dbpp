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

Task<AdbcQueryResult> ExecuteQueryAsync(std::shared_ptr<AdbcSession> session,
                                        std::string sql, bool want_rows,
                                        std::size_t max_rows,
                                        std::string target_database) {
  if (!session || session->IsClosed()) {
    AdbcQueryResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    co_return result;
  }
  co_return co_await session->sequence.Run(
      [session, sql = std::move(sql), want_rows, max_rows,
       target_database = std::move(target_database)](std::stop_token) mutable {
        if (session->read_only && !IsPureReadQuery(session->dialect, sql)) {
          AdbcQueryResult result;
          result.error = ReadOnlyWriteBlockedMessage();
          return result;
        }
        const std::string trimmed_db = detail::DbTrimAscii(target_database);
        if (!trimmed_db.empty()) {
          if (session->dialect == Dialect::Redis && session->redis) {
            if (const auto db_idx = detail::DbParseRedisTargetDbIndex(trimmed_db)) {
              std::string err;
              if (!session->redis->EnsureDb(*db_idx, err)) {
                AdbcQueryResult result;
                result.connection_lost = !session->redis->IsConnected();
                result.error = std::move(err);
                return result;
              }
            }
          } else {
            std::string ns_err;
            const bool switched_native =
                SwitchSessionNamespaceBlocking(*session, {}, trimmed_db, ns_err);
            if (!switched_native) {
              const std::string switch_sql =
                  BuildSwitchDatabaseSql(session->dialect, trimmed_db);
              if (!switch_sql.empty()) {
                AdbcQueryResult sw =
                    ExecuteSwitchDatabaseSqlBlocking(*session, switch_sql);
                if (sw.connection_lost) {
                  return sw;
                }
                if (!sw.error.empty() &&
                    !(session->dialect == Dialect::Generic && trimmed_db == "default")) {
                  return sw;
                }
              }
            }
          }
        }
        return ExecuteBlocking(*session, std::move(sql), want_rows, max_rows);
      });
}

Task<AdbcQueryResult> ExecutePreparedQueryAsync(std::shared_ptr<AdbcSession> session,
                                                std::string sql,
                                                std::vector<PreparedParamValue> params,
                                                bool want_rows, std::size_t max_rows,
                                                std::string target_database) {
  if (!session || session->IsClosed()) {
    AdbcQueryResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    co_return result;
  }
  co_return co_await session->sequence.Run(
      [session, sql = std::move(sql), params = std::move(params), want_rows, max_rows,
       target_database = std::move(target_database)](std::stop_token) mutable {
        if (session->read_only && !IsPureReadQuery(session->dialect, sql)) {
          AdbcQueryResult result;
          result.error = ReadOnlyWriteBlockedMessage();
          return result;
        }
        const std::string trimmed_db = detail::DbTrimAscii(target_database);
        if (!trimmed_db.empty()) {
          std::string ns_err;
          if (!SwitchSessionNamespaceBlocking(*session, {}, trimmed_db, ns_err)) {
            const std::string switch_sql =
                BuildSwitchDatabaseSql(session->dialect, trimmed_db);
            if (!switch_sql.empty()) {
              AdbcQueryResult sw =
                  ExecuteSwitchDatabaseSqlBlocking(*session, switch_sql);
              if (sw.connection_lost) {
                return sw;
              }
            }
          }
        }
        return ExecutePreparedBlocking(*session, std::move(sql), params, want_rows, max_rows);
      });
}

Task<AdbcQueryResult> UpdateCellAsync(std::shared_ptr<AdbcSession> session, TableModel model,
                                      std::size_t row_index, std::size_t column_index,
                                      std::string text) {
  if (!session || session->IsClosed()) {
    AdbcQueryResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    co_return result;
  }
  co_return co_await session->sequence.Run(
      [session, model = std::move(model), row_index, column_index, text = std::move(text)](
          std::stop_token) mutable {
        if (session->IsClosed()) {
          AdbcQueryResult res;
          res.connection_lost = true;
          res.error = Tr("Connection is closed", "连接已关闭");
          return res;
        }
        return detail::DbUpdateCellBlocking(*session, model, row_index, column_index, text);
      });
}

Task<AdbcQueryResult> DeleteRowAsync(std::shared_ptr<AdbcSession> session, TableModel model,
                                     std::size_t row_index) {
  if (!session || session->IsClosed()) {
    AdbcQueryResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    co_return result;
  }
  co_return co_await session->sequence.Run(
      [session, model = std::move(model), row_index](std::stop_token) mutable {
        if (session->IsClosed()) {
          AdbcQueryResult res;
          res.connection_lost = true;
          res.error = Tr("Connection is closed", "连接已关闭");
          return res;
        }
        return detail::DbDeleteRowBlocking(*session, model, row_index);
      });
}

void RequestLoadTable(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                      const TaskScope& tasks, std::uint64_t tab_id, std::int64_t page,
                      std::optional<DataTableSort> sort, std::string filter,
                      bool keep_selection, NotifyFn notify, std::int64_t page_size) {
  const WorkspaceModel& ws_snapshot = workspace.Get();
  const WorkspaceTab* tab = ws_snapshot.FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table) {
    return;
  }

  const std::uint64_t session_id = tab->session_id;
  const std::string object_name = tab->object_name;
  const std::string object_schema = tab->object_schema;
  const std::int64_t effective_page_size =
      page_size > 0 ? page_size
                    : (tab->table.page_size > 0 ? tab->table.page_size : kPageSize);

  const AppModel& model = app.Get();
  const DbSession* session = detail::DbFindSession(model, session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    if (session != nullptr) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
      return;
    }
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target != nullptr && target->kind == WorkspaceTabKind::Table) {
        target->table = TableModel{};
        target->selected_cell = std::nullopt;
      }
    });
    return;
  }
  const DbObject* object = detail::DbFindObject(*session, object_name, object_schema);
  if (object == nullptr) {
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target != nullptr && target->kind == WorkspaceTabKind::Table) {
        target->table = TableModel{};
        target->selected_cell = std::nullopt;
      }
    });
    return;
  }

  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab* target = ws.FindTab(tab_id);
    if (target == nullptr || target->kind != WorkspaceTabKind::Table) {
      return;
    }
    target->table.loading = true;
    target->table.error.clear();
    target->table.page_size = effective_page_size;
    if (!keep_selection) {
      target->selected_cell = std::nullopt;
    }
  });

  auto adbc_session = session->session;
  DbObject object_copy = *object;

  tasks.Launch([app, workspace, tasks, notify = std::move(notify), adbc_session, tab_id, session_id,
                object_copy = std::move(object_copy), object_name, object_schema, page,
                effective_page_size, sort, filter, keep_selection]() mutable -> Task<void> {
    TableModel result = co_await adbc_session->sequence.Run(
        [adbc_session, session_id, object_copy = std::move(object_copy), page,
         effective_page_size, sort, filter](std::stop_token) mutable {
          return LoadTableBlocking(*adbc_session, session_id, std::move(object_copy), page,
                                   effective_page_size, sort, filter);
        });

    const bool load_lost = result.connection_lost;
    const std::string load_error = result.error;
    const bool has_snapshot = result.snapshot != nullptr;
    if (load_lost) {
      workspace.Update([&](WorkspaceModel& ws) {
        if (WorkspaceTab* target = ws.FindTab(tab_id);
            target != nullptr && target->kind == WorkspaceTabKind::Table) {
          target->table.loading = false;
        }
      });
      HandleSessionDisconnected(app, workspace, tasks, session_id, load_error, notify);
      co_return;
    }
    bool tab_matches = false;
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target == nullptr || target->kind != WorkspaceTabKind::Table ||
          target->session_id != session_id || target->object_name != object_name ||
          target->object_schema != object_schema) {
        return;
      }
      tab_matches = true;
      if (!load_error.empty() && !has_snapshot) {
        if (target->table.session_id != session_id || target->table.object != object_name) {
          target->table = TableModel{};
          target->table.session_id = session_id;
          target->table.object = object_name;
          target->table.error = load_error;
          return;
        }
        target->table.loading = false;
        target->table.error = load_error;
        return;
      }
      result.pending_inserts = std::move(target->table.pending_inserts);
      result.next_draft_id = target->table.next_draft_id;
      result.committing = target->table.committing;
      result.column_widths = std::move(target->table.column_widths);
      for (std::size_t i = 0; i < result.columns.size(); ++i) {
        const std::string& col_name = result.columns[i].name;
        if (!result.column_widths.contains(col_name)) {
          result.column_widths.emplace(
              col_name, ComputeAutoColumnWidth(col_name, i, result.snapshot.get()));
        }
      }
      if (!result.error.empty()) {
        result.error.clear();
      }
      result.snapshot_version = target->table.snapshot_version + 1;
      const bool had_valid_sel =
          keep_selection && target->selected_cell.has_value() && result.snapshot != nullptr &&
          detail::DbParseRowIndex(target->selected_cell->row_id) < result.snapshot->rows.size();
      target->table = std::move(result);
      if (!had_valid_sel) {
        target->selected_cell = std::nullopt;
      }
    });

    if (!tab_matches) {
      co_return;
    }
    if (!load_error.empty() && !has_snapshot) {
      if (notify) {
        notify(TrFmt("Failed to load: {0}", "加载失败: {0}", {load_error}));
      }
      co_return;
    }
    if (!load_error.empty() && notify) {
      notify(TrFmt("Load warning: {0}", "加载警告: {0}", {load_error}));
    }
  });
}

void RequestSaveCell(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                     const TaskScope& tasks, std::uint64_t tab_id,
                     const DataTableEditRequest& edit, NotifyFn notify) {
  if (edit.cell_id.row_id.starts_with("draft:")) {
    return;
  }
  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table) {
    return;
  }
  const DbSession* session = detail::DbFindSession(app.Get(), tab->session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    if (session != nullptr) {
      HandleSessionDisconnected(app, workspace, tasks, tab->session_id, {}, notify);
    }
    return;
  }
  TableModel table_value = tab->table;
  if (!table_value.snapshot) {
    return;
  }
  const std::size_t row_index = detail::DbParseRowIndex(edit.cell_id.row_id);
  if (row_index >= table_value.snapshot->rows.size()) {
    return;
  }
  std::size_t column_index = table_value.columns.size();
  for (std::size_t i = 0; i < table_value.columns.size(); ++i) {
    if (table_value.columns[i].name == edit.cell_id.column_id) {
      column_index = i;
      break;
    }
  }
  if (column_index >= table_value.columns.size()) {
    return;
  }
  const CellValue& current = table_value.snapshot->rows[row_index].values[column_index];
  const ColumnMeta& target_col = table_value.columns[column_index];
  const std::string normalized_text = NormalizeTimestampForWrite(
      edit.text, current.is_null ? std::string_view{} : std::string_view(current.text),
      target_col.name, target_col.type, session->session->dialect);
  if (!current.is_null && current.text == normalized_text) {
    return;
  }
  if (current.is_null && detail::DbEqualsIgnoreCaseAscii(normalized_text, "NULL")) {
    return;
  }

  auto adbc_session = session->session;
  const std::uint64_t session_id = session->id;
  const std::string object_name = table_value.object;

  tasks.Launch([app, workspace, tasks, notify = std::move(notify), adbc_session,
                table_value = std::move(table_value), tab_id, session_id, object_name,
                row_index, column_index, text = normalized_text]() mutable -> Task<void> {
    AdbcQueryResult result =
        co_await UpdateCellAsync(adbc_session, table_value, row_index, column_index, text);
    if (result.connection_lost) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, result.error, notify);
      co_return;
    }
    if (!result.error.empty()) {
      const std::string message = result.error;
      if (notify) {
        notify(TrFmt("Failed to save: {0}", "保存失败: {0}", {message}));
      }
      co_return;
    }
    const WorkspaceTab* current_tab = workspace.Get().FindTab(tab_id);
    if (current_tab == nullptr || current_tab->kind != WorkspaceTabKind::Table ||
        current_tab->session_id != session_id || current_tab->object_name != object_name) {
      co_return;
    }
    RequestLoadTable(app, workspace, tasks, tab_id, table_value.page, table_value.sort,
                     table_value.filter, true, notify);
    if (notify) {
      notify(std::string(Tr("Record updated", "记录已更新")));
    }
  });
}

void RequestDeleteSelected(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                           const TaskScope& tasks, std::uint64_t tab_id, NotifyFn notify) {
  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table || !tab->selected_cell.has_value()) {
    return;
  }
  if (tab->selected_cell->row_id.starts_with("draft:")) {
    return;
  }
  const DbSession* session = detail::DbFindSession(app.Get(), tab->session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    if (session != nullptr) {
      HandleSessionDisconnected(app, workspace, tasks, tab->session_id, {}, notify);
    }
    return;
  }
  TableModel table_value = tab->table;
  if (!table_value.snapshot) {
    return;
  }
  const std::size_t row_index = detail::DbParseRowIndex(tab->selected_cell->row_id);
  if (row_index >= table_value.snapshot->rows.size()) {
    return;
  }
  auto adbc_session = session->session;
  const std::uint64_t session_id = session->id;
  const std::string object_name = table_value.object;
  const std::int64_t page = table_value.page;
  const std::int64_t remaining = static_cast<std::int64_t>(table_value.snapshot->rows.size());

  tasks.Launch([app, workspace, tasks, notify = std::move(notify), adbc_session,
                table_value = std::move(table_value), tab_id, session_id, object_name,
                row_index, page, remaining]() mutable -> Task<void> {
    AdbcQueryResult result = co_await DeleteRowAsync(adbc_session, table_value, row_index);
    if (result.connection_lost) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, result.error, notify);
      co_return;
    }
    if (!result.error.empty()) {
      const std::string message = result.error;
      if (notify) {
        notify(TrFmt("Failed to delete: {0}", "删除失败: {0}", {message}));
      }
      co_return;
    }
    const WorkspaceTab* current_tab = workspace.Get().FindTab(tab_id);
    if (current_tab == nullptr || current_tab->kind != WorkspaceTabKind::Table ||
        current_tab->session_id != session_id || current_tab->object_name != object_name) {
      co_return;
    }
    const std::int64_t next_page = remaining <= 1 && page > 0 ? page - 1 : page;
    RequestLoadTable(app, workspace, tasks, tab_id, next_page, table_value.sort,
                     table_value.filter, false, notify);
    if (notify) {
      notify(std::string(Tr("Deleted 1 row", "已删除 1 行")));
    }
  });
}

void RequestCommitPendingInserts(const State<AppModel>& app,
                                 const State<WorkspaceModel>& workspace,
                                 const TaskScope& tasks, std::uint64_t tab_id,
                                 NotifyFn notify) {
  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table ||
      tab->table.pending_inserts.empty() || tab->table.committing) {
    return;
  }
  const DbSession* session = detail::DbFindSession(app.Get(), tab->session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    if (session != nullptr) {
      HandleSessionDisconnected(app, workspace, tasks, tab->session_id, {}, notify);
    } else if (notify) {
      notify(std::string(Tr("Connection is closed", "连接已关闭")));
    }
    return;
  }

  auto adbc = session->session;
  const std::uint64_t session_id = tab->session_id;
  const std::string object_name = tab->object_name;
  TableModel table_copy = tab->table;
  std::vector<PendingInsertRow> rows = tab->table.pending_inserts;
  for (PendingInsertRow& row : rows) {
    for (std::size_t col_idx = 0;
         col_idx < row.values.size() && col_idx < table_copy.columns.size(); ++col_idx) {
      if (!row.values[col_idx].empty()) {
        const ColumnMeta& col = table_copy.columns[col_idx];
        row.values[col_idx] = NormalizeTimestampForWrite(
            row.values[col_idx], {}, col.name, col.type, adbc->dialect);
      }
    }
  }

  workspace.Update([&](WorkspaceModel& ws) {
    if (WorkspaceTab* target = ws.FindTab(tab_id);
        target != nullptr && target->kind == WorkspaceTabKind::Table) {
      target->table.committing = true;
    }
  });

  tasks.Launch([app, workspace, tasks, notify = std::move(notify), adbc,
                table_copy = std::move(table_copy), rows = std::move(rows), tab_id,
                session_id, object_name]() mutable -> Task<void> {
    BatchInsertResult res = co_await adbc->sequence.Run(
        [adbc, table_copy = std::move(table_copy), rows = std::move(rows)](
            std::stop_token) mutable { return InsertRowsBlocking(*adbc, table_copy, rows); });

    if (res.connection_lost) {
      workspace.Update([&](WorkspaceModel& ws) {
        if (WorkspaceTab* target = ws.FindTab(tab_id);
            target != nullptr && target->kind == WorkspaceTabKind::Table) {
          target->table.committing = false;
        }
      });
      HandleSessionDisconnected(app, workspace, tasks, session_id, res.error, notify);
      co_return;
    }

    if (!res.error.empty()) {
      bool partial_reload = false;
      std::int64_t reload_page = 0;
      std::optional<DataTableSort> reload_sort;
      std::string reload_filter;
      workspace.Update([&](WorkspaceModel& ws) {
        WorkspaceTab* target = ws.FindTab(tab_id);
        if (target == nullptr || target->kind != WorkspaceTabKind::Table ||
            target->session_id != session_id || target->object_name != object_name) {
          return;
        }
        target->table.committing = false;
        if (!res.rolled_back && res.inserted > 0) {
          const std::size_t remove_count =
              std::min(res.inserted, target->table.pending_inserts.size());
          target->table.pending_inserts.erase(target->table.pending_inserts.begin(),
                                              target->table.pending_inserts.begin() +
                                                  static_cast<std::ptrdiff_t>(remove_count));
          partial_reload = true;
          reload_page = target->table.page;
          reload_sort = target->table.sort;
          reload_filter = target->table.filter;
        }
      });
      if (notify) {
        notify(TrFmt("Commit failed: {0}", "提交失败: {0}", {res.error}));
      }
      if (partial_reload) {
        RequestLoadTable(app, workspace, tasks, tab_id, reload_page, reload_sort,
                         std::move(reload_filter), false, notify);
      }
      co_return;
    }

    bool should_reload = false;
    std::int64_t target_page = 0;
    std::optional<DataTableSort> reload_sort;
    std::string reload_filter;
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target == nullptr || target->kind != WorkspaceTabKind::Table ||
          target->session_id != session_id || target->object_name != object_name) {
        return;
      }
      should_reload = true;
      target->table.pending_inserts.clear();
      target->table.committing = false;
      if (target->selected_cell.has_value() &&
          target->selected_cell->row_id.starts_with("draft:")) {
        target->selected_cell = std::nullopt;
      }
      reload_sort = target->table.sort;
      reload_filter = target->table.filter;
      if (!target->table.sort.has_value() && target->table.filter.empty()) {
        const std::int64_t next_total =
            target->table.total + static_cast<std::int64_t>(res.inserted);
        const std::int64_t page_size = std::max<std::int64_t>(1, target->table.page_size);
        target_page = std::max<std::int64_t>(0, (next_total - 1) / page_size);
      } else {
        target_page = target->table.page;
      }
    });

    if (should_reload) {
      RequestLoadTable(app, workspace, tasks, tab_id, target_page, reload_sort,
                       std::move(reload_filter), false, notify);
    }
    if (notify) {
      notify(TrFmt("Committed {0} row(s)", "已提交 {0} 行", {std::to_string(res.inserted)}));
    }
  });
}

std::string ConnectionFailureKey(std::string_view driver_path, std::string_view uri) {
  return std::string(driver_path) + "\t" + SanitizeUriCredentials(uri).uri;
}

void OpenConnection(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                    const TaskScope& tasks, ConnectionRequest request, NotifyFn notify,
                    std::function<void(std::uint64_t)> on_opened) {
  const std::string fail_key = ConnectionFailureKey(request.driver_path, request.uri);
  app.Update([&](AppModel& model) {
    model.error.clear();
    model.status =
        TrFmt("Connecting to {0}...", "正在连接 {0}…",
              {DisplayNameFromUri(request.uri, Tr("Database", "数据库"))});
  });
  tasks.Launch([app, workspace, tasks, request = std::move(request),
                fail_key, notify = std::move(notify),
                on_opened = std::move(on_opened)]() mutable -> Task<void> {
    AdbcOpenResult opened =
        co_await RunWorker([request] { return OpenAdbcBlocking(request); });
    if (!opened.session) {
      const std::string fail_detail = opened.error;
      const std::string message =
          TrFmt("Connection failed: {0}", "连接失败: {0}", {opened.error});
      app.Update([&fail_key, &fail_detail](AppModel& model) {
        model.status.clear();
        model.error.clear();
        model.failed_connections[fail_key] = fail_detail;
      });
      if (notify) {
        notify(message);
      }
      co_return;
    }

    AdbcObjectsResult objects = co_await opened.session->sequence.Run(
        [session = opened.session](std::stop_token) {
          return LoadObjectsBlocking(*session);
        });
    detail::DbApplyOpenedSession(app, workspace, tasks, std::move(opened), std::move(request),
                       std::move(objects), std::move(notify), std::move(on_opened));
  });
}

void CloseSession(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                  const TaskScope& tasks, std::uint64_t session_id, NotifyFn notify) {
  (void)notify;
  std::shared_ptr<AdbcSession> adbc_to_close;
  std::vector<std::uint64_t> tabs_to_close;
  for (const WorkspaceTab& tab : workspace.Get().tabs) {
    if (tab.session_id == session_id) {
      tabs_to_close.push_back(tab.id);
    }
  }
  app.Update([&](AppModel& model) {
    for (DbSession& session : model.sessions) {
      if (session.id == session_id) {
        model.failed_connections.erase(ConnectionFailureKey(session.driver_path, session.uri));
        adbc_to_close = std::move(session.session);
        break;
      }
    }
    std::erase_if(model.sessions, [session_id](const DbSession& session) {
      return session.id == session_id;
    });
    if (model.sessions.empty()) {
      model.active_session = 0;
      model.active_object.clear();
      model.active_is_view = false;
      model.status.clear();
    } else if (model.active_session == session_id) {
      model.active_session = model.sessions.front().id;
      model.active_object.clear();
      model.active_is_view = false;
    }
    ++model.revision;
  });
  detail::DbCloseSessionAsync(std::move(adbc_to_close), tasks);
  if (!tabs_to_close.empty()) {
    CloseWorkspaceTabs(app, workspace, tabs_to_close);
  }
}

void HandleSessionDisconnected(const State<AppModel>& app,
                                const State<WorkspaceModel>& workspace,
                                const TaskScope& tasks, std::uint64_t session_id,
                                std::string reason, NotifyFn notify) {
  std::shared_ptr<AdbcSession> adbc_to_close;
  std::string session_name;
  bool removed = false;
  app.Update([&](AppModel& model) {
    for (auto it = model.sessions.begin(); it != model.sessions.end(); ++it) {
      if (it->id == session_id) {
        session_name = it->name;
        model.failed_connections[ConnectionFailureKey(it->driver_path, it->uri)] = reason;
        adbc_to_close = std::move(it->session);
        model.sessions.erase(it);
        removed = true;
        break;
      }
    }
    if (!removed) {
      return;
    }
    if (model.sessions.empty()) {
      model.active_session = 0;
      model.active_object.clear();
      model.active_is_view = false;
      model.status.clear();
    } else if (model.active_session == session_id) {
      model.active_session = model.sessions.front().id;
      model.active_object.clear();
      model.active_is_view = false;
    }
    ++model.revision;
  });
  if (!removed) {
    return;
  }
  detail::DbCloseSessionAsync(std::move(adbc_to_close), tasks);

  // Close clean table tabs for the lost session, while keeping SQL tabs and
  // dirty table tabs so unsaved drafts and editor queries remain intact.
  std::vector<std::uint64_t> tabs_to_close;
  for (const WorkspaceTab& tab : workspace.Get().tabs) {
    if (tab.session_id == session_id && tab.kind == WorkspaceTabKind::Table &&
        tab.table.pending_inserts.empty()) {
      tabs_to_close.push_back(tab.id);
    }
  }
  if (!tabs_to_close.empty()) {
    CloseWorkspaceTabs(app, workspace, tabs_to_close);
  }

  if (notify) {
    notify(reason.empty()
               ? TrFmt("Connection {0} disconnected", "连接 {0} 已断开", {session_name})
               : TrFmt("Connection {0} disconnected: {1}", "连接 {0} 已断开: {1}",
                       {session_name, reason}));
  }
}

void CloseAllSessions(const State<AppModel>& app, const TaskScope& tasks) {
  std::vector<std::shared_ptr<AdbcSession>> closing;
  app.Update([&](AppModel& model) {
    closing.reserve(model.sessions.size());
    for (DbSession& session : model.sessions) {
      if (session.session) {
        closing.push_back(std::move(session.session));
      }
    }
    model.sessions.clear();
    model.active_session = 0;
    model.active_object.clear();
    model.active_is_view = false;
    model.status.clear();
    ++model.revision;
  });
  for (std::shared_ptr<AdbcSession>& adbc : closing) {
    detail::DbCloseSessionAsync(std::move(adbc), tasks);
  }
}

void ScheduleSessionHealthCheck(const State<AppModel>& app,
                                const State<WorkspaceModel>& workspace,
                                const TaskScope& tasks, NotifyFn notify) {
  struct ProbeTarget {
    std::uint64_t session_id = 0;
    std::shared_ptr<AdbcSession> adbc;
  };
  std::vector<ProbeTarget> targets;
  {
    const AppModel& model = app.Get();
    targets.reserve(model.sessions.size());
    for (const DbSession& session : model.sessions) {
      if (session.session && !session.session->IsClosed() &&
          ShouldHealthCheckSession(session.dialect, session.uri)) {
        targets.push_back(ProbeTarget{.session_id = session.id, .adbc = session.session});
      }
    }
  }
  for (ProbeTarget& target : targets) {
    if (target.adbc->ping_in_flight.exchange(true, std::memory_order_acq_rel)) {
      continue;
    }
    tasks.Launch([app, workspace, tasks, notify, session_id = target.session_id,
                  adbc = std::move(target.adbc)]() mutable -> Task<void> {
      struct InFlightGuard {
        AdbcSession* ptr = nullptr;
        ~InFlightGuard() {
          if (ptr != nullptr) {
            ptr->ping_in_flight.store(false, std::memory_order_release);
          }
        }
      } guard{adbc.get()};

      const AdbcQueryResult ping = co_await adbc->sequence.Run(
          [adbc](std::stop_token) { return PingSessionBlocking(*adbc); });
      if (ping.connection_lost && !adbc->IsClosed()) {
        HandleSessionDisconnected(app, workspace, tasks, session_id, ping.error, notify);
      }
    });
  }
}

void RefreshSessionObjects(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                           const TaskScope& tasks, std::uint64_t session_id, NotifyFn notify,
                           bool notify_success) {
  const AppModel& snapshot = app.Get();
  const DbSession* session = detail::DbFindSession(snapshot, session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    if (session != nullptr) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
    }
    return;
  }

  auto adbc = session->session;
  std::string session_name = session->name;
  tasks.Launch([app, workspace, tasks, notify = std::move(notify), adbc, session_id,
                session_name = std::move(session_name), notify_success]() mutable -> Task<void> {
    AdbcObjectsResult objects = co_await adbc->sequence.Run(
        [adbc](std::stop_token) { return LoadObjectsBlocking(*adbc); });
    if (objects.connection_lost) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, objects.error, notify);
      co_return;
    }
    if (!objects.error.empty()) {
      if (notify) {
        notify(TrFmt("Refresh failed: {0}", "刷新失败: {0}", {objects.error}));
      }
      co_return;
    }

    bool session_present = false;
    const std::size_t object_count = objects.objects.size();
    app.Update([&](AppModel& value) {
      for (DbSession& open : value.sessions) {
        if (open.id != session_id) {
          continue;
        }
        session_present = true;
        open.objects = std::move(objects.objects);
        open.databases = std::move(objects.databases);
        open.triggers = std::move(objects.triggers);
        open.routines = std::move(objects.routines);
        open.events = std::move(objects.events);
        ++value.revision;
        break;
      }
    });

    if (!session_present) {
      co_return;
    }

    const DbSession* refreshed = detail::DbFindSession(app.Get(), session_id);
    std::vector<std::uint64_t> stale_tabs;
    if (refreshed != nullptr) {
      for (const WorkspaceTab& tab : workspace.Get().tabs) {
        if (tab.session_id == session_id && tab.kind == WorkspaceTabKind::Table &&
            detail::DbFindObject(*refreshed, tab.object_name, tab.object_schema) == nullptr) {
          stale_tabs.push_back(tab.id);
        }
      }
    }
    if (!stale_tabs.empty()) {
      CloseWorkspaceTabs(app, workspace, stale_tabs);
    }

    const WorkspaceTab* active_tab = workspace.Get().ActiveTab();
    if (active_tab != nullptr && active_tab->kind == WorkspaceTabKind::Table &&
        active_tab->session_id == session_id && active_tab->table.pending_inserts.empty()) {
      RequestLoadTable(app, workspace, tasks, active_tab->id, active_tab->table.page,
                       active_tab->table.sort, active_tab->table.filter, true, notify);
    }
    if (notify && notify_success) {
      notify(TrFmt("Refreshed {0} ({1} objects)", "已刷新 {0}（{1} 个对象）",
                   {session_name, std::to_string(object_count)}));
    }
  });
}

} // namespace dbpp
