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
      if (IsSchemaStaleError(load_error) || filter.empty()) {
        ScheduleMaintenanceRefresh(app, workspace, tasks, session_id, notify);
      }
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

void StageTableCellEdit(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                        std::uint64_t tab_id, const DataTableEditRequest& edit) {
  if (edit.cell_id.row_id.starts_with("draft:")) {
    return;
  }
  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table || !tab->table.editable ||
      tab->table.read_only || tab->table.committing || !tab->table.snapshot) {
    return;
  }
  const std::size_t row_index = detail::DbParseRowIndex(edit.cell_id.row_id);
  if (row_index >= tab->table.snapshot->rows.size() ||
      tab->table.IsRowPendingDelete(row_index)) {
    return;
  }
  std::size_t column_index = tab->table.columns.size();
  for (std::size_t i = 0; i < tab->table.columns.size(); ++i) {
    if (tab->table.columns[i].name == edit.cell_id.column_id) {
      column_index = i;
      break;
    }
  }
  if (column_index >= tab->table.columns.size()) {
    return;
  }

  const DbSession* session = detail::DbFindSession(app.Get(), tab->session_id);
  const Dialect dialect =
      (session != nullptr && session->session) ? session->session->dialect : Dialect::Generic;
  const ColumnMeta& col = tab->table.columns[column_index];
  const CellValue& orig = tab->table.snapshot->rows[row_index].values[column_index];
  const std::string lower_type = detail::DbLowerAscii(col.type);
  const bool is_textual = lower_type.empty() ||
                          lower_type.find("text") != std::string::npos ||
                          lower_type.find("char") != std::string::npos ||
                          lower_type.find("clob") != std::string::npos ||
                          lower_type.find("string") != std::string::npos ||
                          lower_type.find("json") != std::string::npos ||
                          lower_type.find("uuid") != std::string::npos;
  const std::string trimmed = detail::DbTrimAscii(edit.text);
  const bool is_null = detail::DbEqualsIgnoreCaseAscii(trimmed, "NULL") ||
                       (trimmed.empty() && (col.nullable || !is_textual));
  const std::string normalized_text =
      is_null ? std::string{"NULL"}
              : NormalizeTimestampForWrite(
                    edit.text, orig.is_null ? std::string_view{} : std::string_view(orig.text),
                    col.name, col.type, dialect);
  const std::string display_text =
      is_null ? std::string{}
              : LocalizeTimestampValue(normalized_text, col.name, col.type)
                    .value_or(normalized_text);

  const std::uint64_t expected_session = tab->session_id;
  const std::string expected_object = tab->table.object;
  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab* target = ws.FindTab(tab_id);
    if (target == nullptr || target->kind != WorkspaceTabKind::Table ||
        target->session_id != expected_session || target->table.object != expected_object ||
        !target->table.snapshot || row_index >= target->table.snapshot->rows.size() ||
        column_index >= target->table.columns.size()) {
      return;
    }
    const CellValue& cur = target->table.snapshot->rows[row_index].values[column_index];
    const bool matches_original =
        (cur.is_null && is_null) || (!cur.is_null && !is_null && cur.text == normalized_text);
    if (matches_original) {
      std::erase_if(target->table.pending_edits, [&](const PendingCellEdit& pe) {
        return pe.row_index == row_index && pe.column_index == column_index;
      });
      return;
    }
    if (PendingCellEdit* existing = target->table.FindPendingEditMutable(row_index, column_index)) {
      existing->raw_text = normalized_text;
      existing->display_text = display_text;
      existing->is_null = is_null;
    } else {
      target->table.pending_edits.push_back(PendingCellEdit{
          .row_index = row_index,
          .column_index = column_index,
          .raw_text = normalized_text,
          .display_text = display_text,
          .is_null = is_null,
      });
    }
  });
}

bool ToggleTableRowPendingDelete(const State<WorkspaceModel>& workspace,
                                 std::uint64_t tab_id, std::size_t row_index) {
  bool marked = false;
  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab* target = ws.FindTab(tab_id);
    if (target == nullptr || target->kind != WorkspaceTabKind::Table ||
        !target->table.editable || target->table.read_only || target->table.committing ||
        !target->table.snapshot || row_index >= target->table.snapshot->rows.size()) {
      return;
    }
    auto& deletes = target->table.pending_deletes;
    if (const auto it = std::ranges::find(deletes, row_index); it != deletes.end()) {
      deletes.erase(it);
      marked = false;
    } else {
      deletes.push_back(row_index);
      marked = true;
    }
  });
  return marked;
}

void RequestCommitTableChanges(const State<AppModel>& app,
                               const State<WorkspaceModel>& workspace,
                               const TaskScope& tasks, std::uint64_t tab_id, NotifyFn notify) {
  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table ||
      !tab->table.HasPendingChanges() || tab->table.committing) {
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
  std::vector<PendingCellEdit> edits = tab->table.pending_edits;
  std::vector<std::size_t> deletes = tab->table.pending_deletes;
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
                table_copy = std::move(table_copy), edits = std::move(edits),
                deletes = std::move(deletes), rows = std::move(rows), tab_id, session_id,
                object_name]() mutable -> Task<void> {
    BatchCommitResult res = co_await adbc->sequence.Run(
        [adbc, table_copy = std::move(table_copy), edits = std::move(edits),
         deletes = std::move(deletes), rows = std::move(rows)](std::stop_token) mutable {
          return CommitTableChangesBlocking(*adbc, table_copy, edits, deletes, rows);
        });

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
        if (!res.rolled_back) {
          if (res.updated_rows > 0) {
            target->table.pending_edits.clear();
          }
          if (res.deleted_rows > 0) {
            target->table.pending_deletes.clear();
          }
          if (res.inserted_rows > 0) {
            const std::size_t remove_count =
                std::min(res.inserted_rows, target->table.pending_inserts.size());
            target->table.pending_inserts.erase(target->table.pending_inserts.begin(),
                                                target->table.pending_inserts.begin() +
                                                    static_cast<std::ptrdiff_t>(remove_count));
          }
          if (res.updated_rows > 0 || res.deleted_rows > 0 || res.inserted_rows > 0) {
            partial_reload = true;
            reload_page = target->table.page;
            reload_sort = target->table.sort;
            reload_filter = target->table.filter;
          }
        }
      });
      if (notify) {
        notify(res.rolled_back
                   ? TrFmt("Commit failed, transaction rolled back (staged changes kept): {0}",
                           "提交失败，事务已回滚（暂存修改已保留）: {0}", {res.error})
                   : TrFmt("Commit failed: {0}", "提交失败: {0}", {res.error}));
      }
      if (IsSchemaStaleError(res.error)) {
        ScheduleMaintenanceRefresh(app, workspace, tasks, session_id, notify);
      }
      if (partial_reload) {
        RequestLoadTable(app, workspace, tasks, tab_id, reload_page, reload_sort,
                         std::move(reload_filter), true, notify);
      }
      co_return;
    }

    bool should_reload = false;
    bool keep_sel = res.deleted_rows == 0;
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
      const std::size_t page_rows =
          target->table.snapshot ? target->table.snapshot->rows.size() : 0;
      target->table.pending_edits.clear();
      target->table.pending_deletes.clear();
      target->table.pending_inserts.clear();
      target->table.committing = false;
      if (res.deleted_rows > 0 || (target->selected_cell.has_value() &&
                                   target->selected_cell->row_id.starts_with("draft:"))) {
        target->selected_cell = std::nullopt;
        keep_sel = false;
      }
      reload_sort = target->table.sort;
      reload_filter = target->table.filter;
      if (res.inserted_rows > 0 && !target->table.sort.has_value() &&
          target->table.filter.empty()) {
        const std::int64_t next_total =
            std::max<std::int64_t>(0, target->table.total +
                                          static_cast<std::int64_t>(res.inserted_rows) -
                                          static_cast<std::int64_t>(res.deleted_rows));
        const std::int64_t page_size = std::max<std::int64_t>(1, target->table.page_size);
        target_page = std::max<std::int64_t>(0, (next_total - 1) / page_size);
        keep_sel = false;
      } else if (res.deleted_rows >= page_rows && target->table.page > 0 &&
                 res.inserted_rows == 0) {
        target_page = target->table.page - 1;
        keep_sel = false;
      } else {
        target_page = target->table.page;
      }
    });

    if (should_reload) {
      RequestLoadTable(app, workspace, tasks, tab_id, target_page, reload_sort,
                       std::move(reload_filter), keep_sel, notify);
    }
    if (notify) {
      if (res.updated_rows > 0 && res.deleted_rows == 0 && res.inserted_rows == 0) {
        notify(TrFmt("Committed {0} updated row(s)", "已提交 {0} 行修改",
                     {std::to_string(res.updated_rows)}));
      } else if (res.deleted_rows > 0 && res.updated_rows == 0 && res.inserted_rows == 0) {
        notify(TrFmt("Committed {0} deleted row(s)", "已提交 {0} 行删除",
                     {std::to_string(res.deleted_rows)}));
      } else if (res.inserted_rows > 0 && res.updated_rows == 0 && res.deleted_rows == 0) {
        notify(TrFmt("Committed {0} row(s)", "已提交 {0} 行",
                     {std::to_string(res.inserted_rows)}));
      } else {
        notify(TrFmt("Committed {0} update(s), {1} delete(s), {2} insert(s)",
                     "已提交 {0} 行修改、{1} 行删除、{2} 行新增",
                     {std::to_string(res.updated_rows), std::to_string(res.deleted_rows),
                      std::to_string(res.inserted_rows)}));
      }
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
        !tab.table.HasPendingChanges()) {
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

namespace {

void RefreshSessionObjectsImpl(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                               const TaskScope& tasks, std::uint64_t session_id, NotifyFn notify,
                               bool notify_success, bool maintenance_mode) {
  const AppModel& snapshot = app.Get();
  const DbSession* session = detail::DbFindSession(snapshot, session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    if (session != nullptr) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
    }
    return;
  }

  auto adbc = session->session;
  if (maintenance_mode) {
    const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
    const std::int64_t prev_ms = adbc->last_maintenance_refresh_ms.load(std::memory_order_acquire);
    if (prev_ms > 0 && now_ms >= prev_ms && (now_ms - prev_ms) < 3000) {
      return;
    }
    if (adbc->refresh_in_flight.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    adbc->last_maintenance_refresh_ms.store(now_ms, std::memory_order_release);
  } else if (adbc->refresh_in_flight.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  std::string session_name = session->name;
  tasks.Launch([app, workspace, tasks, notify = std::move(notify), adbc, session_id,
                session_name = std::move(session_name), notify_success,
                maintenance_mode]() mutable -> Task<void> {
    struct RefreshGuard {
      AdbcSession* ptr = nullptr;
      ~RefreshGuard() {
        if (ptr != nullptr) {
          ptr->refresh_in_flight.store(false, std::memory_order_release);
        }
      }
    } guard{adbc.get()};

    AdbcObjectsResult objects = co_await adbc->sequence.Run(
        [adbc](std::stop_token) { return LoadObjectsBlocking(*adbc); });
    if (objects.connection_lost) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, objects.error, notify);
      co_return;
    }
    if (!objects.error.empty()) {
      if (!maintenance_mode && notify) {
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
        if (tab.session_id == session_id &&
            (tab.kind == WorkspaceTabKind::Table || tab.kind == WorkspaceTabKind::TableDesign) &&
            detail::DbFindObject(*refreshed, tab.object_name, tab.object_schema) == nullptr) {
          stale_tabs.push_back(tab.id);
        }
      }
    }
    if (!stale_tabs.empty()) {
      CloseWorkspaceTabs(app, workspace, stale_tabs);
    }

    if (maintenance_mode) {
      if (!stale_tabs.empty() && notify) {
        notify(TrFmt("Synced {0} metadata and closed {1} removed tab(s)",
                     "已自动刷新 {0} 目录并关闭 {1} 个已失效标签页",
                     {session_name, std::to_string(stale_tabs.size())}));
      }
      co_return;
    }

    const WorkspaceTab* active_tab = workspace.Get().ActiveTab();
    if (active_tab != nullptr && active_tab->kind == WorkspaceTabKind::Table &&
        active_tab->session_id == session_id && !active_tab->table.HasPendingChanges()) {
      RequestLoadTable(app, workspace, tasks, active_tab->id, active_tab->table.page,
                       active_tab->table.sort, active_tab->table.filter, true, notify);
    }
    if (notify && notify_success) {
      notify(TrFmt("Refreshed {0} ({1} objects)", "已刷新 {0}（{1} 个对象）",
                   {session_name, std::to_string(object_count)}));
    }
  });
}

} // namespace

void RefreshSessionObjects(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                           const TaskScope& tasks, std::uint64_t session_id, NotifyFn notify,
                           bool notify_success) {
  RefreshSessionObjectsImpl(app, workspace, tasks, session_id, std::move(notify), notify_success,
                            false);
}

void ScheduleMaintenanceRefresh(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                                const TaskScope& tasks, std::uint64_t session_id, NotifyFn notify) {
  RefreshSessionObjectsImpl(app, workspace, tasks, session_id, std::move(notify), false, true);
}

} // namespace dbpp
