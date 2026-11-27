#include "ui/ui_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <ranges>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

[[huxerui::composable]] View ObjectSchemaDialog(DialogContext dialog,
                                                 std::shared_ptr<AdbcSession> adbc,
                                                 std::uint64_t session_id, DbObject object,
                                                 std::string display_label,
                                                 std::string qualified_name, State<AppModel> app,
                                                 State<WorkspaceModel> workspace, TaskScope tasks,
                                                 NotifyFn notify) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  auto active_tab = UseState<int>(0);
  auto loading = UseState(true);
  auto design = UseState(TableDesignSnapshot{.object = object});
  auto stats = UseState(TableStatisticsResult{});
  auto error = UseState(std::string{});
  auto column_widths = UseState(std::unordered_map<std::string, float>{});
  auto dialog_tasks = UseTaskScope();

  Lifecycle([dialog, dialog_tasks, adbc, session_id, object, app, workspace, tasks, loading,
             design, stats, error, notify, loc] {
    if (!adbc || adbc->IsClosed()) {
      loading = false;
      error = loc.Get(app::strings::toast_connection_closed);
    } else {
      dialog_tasks.Launch([dialog, adbc, session_id, object, app, workspace, tasks, loading,
                           design, stats, error, notify, loc]() -> Task<void> {
        struct SchemaAndStats {
          TableDesignSnapshot design;
          TableStatisticsResult stats;
        };
        DbObject object_copy = object;
        SchemaAndStats loaded = co_await adbc->sequence.Run(
            [adbc, object_copy = std::move(object_copy)](std::stop_token) {
              SchemaAndStats out;
              out.design = LoadTableDesignBlocking(*adbc, object_copy);
              if (!out.design.connection_lost) {
                out.stats = LoadTableStatisticsBlocking(*adbc, object_copy, true);
              }
              return out;
            });
        loading = false;
        if (loaded.design.connection_lost) {
          error = loaded.design.error;
          HandleSessionDisconnected(app, workspace, tasks, session_id, loaded.design.error, notify);
          dialog.Dismiss();
          co_return;
        }
        if (!loaded.design.error.empty()) {
          error = loaded.design.error;
          if (notify) {
            notify(loc.Fmt(app::strings::toast_load_schema_failed, loaded.design.error));
          }
          if (!loaded.design.object.columns.empty()) {
            design = std::move(loaded.design);
          }
        } else {
          design = std::move(loaded.design);
        }
        stats = std::move(loaded.stats);
      });
    }
    return [] {};
  });

  const TableDesignSnapshot& snap = design.Get();
  const std::vector<ColumnMeta>& cols = snap.object.columns;
  const std::vector<ForeignKeyMeta>& fks = snap.object.foreign_keys;
  const TableStatisticsResult& stats_val = stats.Get();
  const std::unordered_map<std::string, float>& widths = column_widths.Get();
  const auto on_col_resized = [column_widths](const DataTableColumnResize& r) {
    column_widths.Update([&r](std::unordered_map<std::string, float>& m) {
      m[r.column_id] = r.width;
    });
  };

  const int cur_tab = active_tab.Get();
  View tab_strip = Row{
      SchemaSectionPill(loc.Fmt(app::strings::design_tab_columns, std::to_string(cols.size())),
                        cur_tab == 0, [active_tab] { active_tab = 0; }, spec, semantic),
      SchemaSectionPill(loc.Fmt(app::strings::design_tab_fks, std::to_string(fks.size())),
                        cur_tab == 1, [active_tab] { active_tab = 1; }, spec, semantic),
      SchemaSectionPill(loc.Get(app::strings::design_tab_stats), cur_tab == 2,
                        [active_tab] { active_tab = 2; }, spec, semantic),
      Spacer(),
  }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center});

  View body_content;
  if (loading.Get()) {
    body_content = LoadingState(app::strings::schema_dialog_loading).With(Frame{.height = 320.0F});
  } else if (cur_tab == 0) {
    if (cols.empty()) {
      body_content = TableEmptyState(error.Get().empty() ? app::strings::schema_dialog_empty
                                                         : app::strings::schema_dialog_load_failed)
                         .With(Frame{.height = 240.0F});
    } else {
      DataTableOptions opts{
          .rows = MakeSequentialRows(cols.size()),
          .columns = BuildSchemaColumnsTable(cols, widths, spec, semantic, loc),
          .row_height = 28.0F,
          .header_height = 30.0F,
          .frozen_column_count = 0,
          .sort = std::nullopt,
          .selected_cell = std::nullopt,
      };
      body_content = DataTable(std::move(opts))
                         .On<DataTableEvents::ColumnResized>(on_col_resized)
                         .With(Frame{.height = 340.0F}, CornerRadius{6.0F},
                               Border{semantic.border_subtle, 1.0F})
                         .Key("schema-dlg-cols");
    }
  } else if (cur_tab == 1) {
    if (fks.empty()) {
      body_content = TableEmptyState(app::strings::design_fks_empty).With(Frame{.height = 240.0F});
    } else {
      DataTableOptions opts{
          .rows = MakeSequentialRows(fks.size()),
          .columns = BuildSchemaForeignKeysTable(fks, widths, spec, semantic, loc),
          .row_height = 28.0F,
          .header_height = 30.0F,
          .frozen_column_count = 0,
          .sort = std::nullopt,
          .selected_cell = std::nullopt,
      };
      body_content = DataTable(std::move(opts))
                         .On<DataTableEvents::ColumnResized>(on_col_resized)
                         .With(Frame{.height = 340.0F}, CornerRadius{6.0F},
                               Border{semantic.border_subtle, 1.0F})
                         .Key("schema-dlg-fks");
    }
  } else {
    if (!stats_val.supported) {
      body_content =
          TableEmptyState(app::strings::schema_dialog_stats_unsupported).With(Frame{.height = 240.0F});
    } else {
      Views stat_views;
      if (stats_val.row_count.has_value()) {
        std::string rc_text = UseString(app::strings::schema_dialog_stats_row_count,
                                        std::to_string(*stats_val.row_count));
        if (stats_val.row_count_approximate) {
          rc_text += " ";
          rc_text += UseString(app::strings::schema_dialog_stats_approx);
        }
        stat_views.Add(MutedText(std::move(rc_text)).Key("schema-stat-rc"));
      }
      std::vector<ColumnStatistic> visible_stats;
      for (const ColumnStatistic& st : stats_val.statistics) {
        if (!st.column_name.empty()) {
          visible_stats.push_back(st);
        }
      }
      if (!visible_stats.empty()) {
        std::vector<DataTableColumn> stat_cols;
        stat_cols.reserve(4);
        stat_cols.push_back(MakeRowNumberColumn(0, visible_stats.size(), widths,
                                                spec.colors.on_surface_variant,
                                                spec.colors.on_surface_variant));
        auto make_stat_col = [&](std::string id, std::string header, float w,
                                 auto getter) -> DataTableColumn {
          DataTableColumn col;
          col.id = id;
          col.header = std::move(header);
          col.sortable = false;
          col.width = DataTableColumnWidth{.initial = w, .min = 80.0F, .max = 480.0F};
          col.render_cell = [visible_stats, getter, &spec](
                                const DataTableCellContext& cell) -> DataTableCellContent {
            const std::size_t r = RowIndexFromId(cell.id.row_id);
            if (r == static_cast<std::size_t>(-1) || r >= visible_stats.size()) {
              return DataTableCellContent{.view = View{}, .label = {}};
            }
            std::string val = getter(visible_stats[r]);
            return DataTableCellContent{
                .view = Text(val.empty() ? std::string{"—"} : val)
                            .Style(TextStyle{Font::System(font_size::kChip), spec.colors.on_surface})
                            .LineLimit(1)
                            .Overflow(TextOverflow::Ellipsis),
                .label = std::move(val),
            };
          };
          return col;
        };
        stat_cols.push_back(make_stat_col("stat_col", loc.Get(app::strings::schema_col_name),
                                          220.0F, [](const ColumnStatistic& s) {
                                            return s.column_name;
                                          }));
        stat_cols.push_back(make_stat_col("stat_metric", loc.Get(app::strings::schema_stat_metric),
                                          220.0F, [](const ColumnStatistic& s) {
                                            return s.statistic_name;
                                          }));
        stat_cols.push_back(make_stat_col("stat_val", loc.Get(app::strings::schema_stat_value),
                                          220.0F, [](const ColumnStatistic& s) {
                                            return s.value_text;
                                          }));
        stat_views.Add(DataTable(DataTableOptions{
                                     .rows = MakeSequentialRows(visible_stats.size()),
                                     .columns = std::move(stat_cols),
                                     .row_height = 28.0F,
                                     .header_height = 30.0F,
                                     .frozen_column_count = 0,
                                     .sort = std::nullopt,
                                     .selected_cell = std::nullopt,
                                 })
                           .On<DataTableEvents::ColumnResized>(on_col_resized)
                           .With(Frame{.height = 300.0F}, CornerRadius{6.0F},
                                 Border{semantic.border_subtle, 1.0F})
                           .Key("schema-dlg-stats"));
      }
      body_content = Column{std::move(stat_views)}
                         .With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Stretch});
    }
  }

  Views dlg_items;
  dlg_items.Add(Row{
                    Text::Format(TextRole::Title, app::strings::schema_dialog_title, display_label),
                    Spacer(),
                    ActionButton(Button(app::strings::action_close).OnClick([dialog] {
                      dialog.Dismiss();
                    })),
                }
                    .With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F})
                    .Key("schema-dlg-title"));
  dlg_items.Add(MutedText(UseString(app::strings::schema_dialog_summary,
                                    UseString(ObjectKindResource(object)), qualified_name,
                                    cols.size()))
                    .Key("schema-dlg-summary"));
  if (adbc && adbc->connection_info.supported) {
    dlg_items.Add(MutedText(UseString(app::strings::schema_dialog_conn_info,
                                      adbc->connection_info.vendor_name,
                                      adbc->connection_info.vendor_version,
                                      adbc->connection_info.driver_name,
                                      adbc->connection_info.driver_version))
                      .Key("schema-dlg-conn-info"));
  }
  dlg_items.Add(std::move(tab_strip).Key("schema-dlg-tabs"));
  dlg_items.Add(std::move(body_content).Key("schema-dlg-body"));

  return DialogCard(Column{std::move(dlg_items)}
                        .With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                        .With(Frame{.width = 820.0F}));
}

[[huxerui::composable]] View DropObjectDialog(
    DialogContext dialog, std::shared_ptr<AdbcSession> adbc, std::uint64_t session_id,
    DbObject object, std::string display_label, std::string drop_sql, State<AppModel> app,
    State<WorkspaceModel> workspace, TaskScope tasks, NotifyFn notify) {
  auto busy = UseState(false);
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const StringResource kind_res = ObjectKindResource(object);
  const std::string kind = UseString(kind_res);

  const auto confirm = [dialog, adbc, session_id, display_label, kind_res, drop_sql, app,
                        workspace, tasks, busy, notify, loc] {
    if (busy.Get()) {
      return;
    }
    if (adbc && adbc->read_only) {
      if (notify) {
        notify(loc.Get(app::strings::toast_readonly_blocked));
      }
      return;
    }
    if (!adbc || adbc->IsClosed()) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
      dialog.Dismiss();
      return;
    }
    busy = true;
    tasks.Launch([dialog, adbc, session_id, display_label, kind_res, drop_sql, app, workspace,
                  tasks, busy, notify, loc]() -> Task<void> {
      AdbcQueryResult res = co_await ExecuteQueryAsync(adbc, drop_sql, false, 0);
      if (res.connection_lost) {
        busy = false;
        dialog.Dismiss();
        HandleSessionDisconnected(app, workspace, tasks, session_id, res.error, notify);
        co_return;
      }
      if (!res.error.empty()) {
        busy = false;
        if (notify) {
          notify(loc.Fmt(app::strings::toast_drop_failed, res.error));
        }
        co_return;
      }
      dialog.Dismiss();
      RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
      if (notify) {
        notify(loc.Fmt(app::strings::toast_drop_done, loc.Get(kind_res), display_label));
      }
    });
  };

  return DialogCard(LoadingOverlay(
      Column{
          Row{
              Text::Format(TextRole::Title, app::strings::drop_dialog_title, kind),
              Spacer(),
              ActionButton(Button(app::strings::action_cancel)
                               .OnClick([dialog] { dialog.Dismiss(); })
                               .With(Enabled{!busy.Get()})),
              ActionButton(Button(busy.Get() ? app::strings::drop_dialog_confirming
                                             : app::strings::drop_dialog_confirm)
                               .OnClick(confirm)
                               .With(Enabled{!busy.Get()}),
                           ButtonTone::DangerSolid),
          }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
          MutedText(UseString(app::strings::drop_dialog_warning, display_label)),
          MutedText(UseString(app::strings::drop_dialog_sql_preview, drop_sql)),
      }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
      busy.Get(), app::strings::drop_dialog_confirming)
                        .With(Frame{.width = 520.0F}));
}

} // namespace dbpp
