#include "ui/ui_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

namespace {

constexpr std::size_t kMaxVisibleTreeRows = 800;

std::vector<std::string> EntryColumnsForType(std::string_view type) {
  if (type == "hash") return {"field", "value"};
  if (type == "list") return {"index", "value"};
  if (type == "set") return {"#", "member"};
  if (type == "zset") return {"member", "score"};
  if (type == "stream") return {"id", "fields"};
  return {"item", "value"};
}

std::shared_ptr<const GridSnapshot> SnapshotFromEntries(
    const std::vector<std::pair<std::string, std::string>>& entries) {
  GridSnapshot snap;
  snap.rows.reserve(entries.size());
  for (const auto& [k, v] : entries) {
    GridRow r;
    r.values = {
        CellValue{.text = k, .is_null = false, .display_text = {}},
        CellValue{.text = v, .is_null = false, .display_text = {}},
    };
    snap.rows.push_back(std::move(r));
  }
  return std::make_shared<const GridSnapshot>(std::move(snap));
}

} // namespace

[[huxerui::composable]] View RedisTypeFilterButton(
    const MenuHandle& menu, const ViewHandle& anchor, const LocalizedCatalog& loc,
    const std::string& current_type, bool enabled, float height,
    std::function<void(std::string)> on_select) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const bool active = !current_type.empty();
  const std::string label = active ? UseString(app::strings::table_redis_type_active, current_type)
                                   : UseString(app::strings::table_redis_type_all);
  const Color fg = active ? spec.colors.on_primary_container : spec.colors.on_surface;
  const Color bg = active ? spec.colors.primary_container : spec.colors.surface;
  const Color border = active ? WithAlpha(spec.colors.primary, 0.45F) : semantic.border_subtle;
  const Color icon_color = active ? spec.colors.primary : spec.colors.on_surface_variant;

  return Row{
      TintedIcon(app::images::index, icon_color, 13.0F),
      Text(label)
          .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::Medium), fg})
          .LineLimit(1),
      TintedIcon(app::images::chevron_down, icon_color, 12.0F),
  }
      .With(Frame{.height = height, .min_height = height, .max_height = height},
            Padding{EdgeInsets::Symmetric(8.0F, 0.0F)}, Spacing{5.0F},
            CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
            Background{VisualFill{bg}}, Border{border, 1.0F}, anchor, Enabled{enabled})
      .OnClick([menu, anchor, loc, current_type, on_select = std::move(on_select)] {
        std::vector<MenuEntry> entries;
        entries.emplace_back(
            MenuItem(loc.Get(app::strings::table_redis_type_menu_all),
                     [on_select] {
                       if (on_select) on_select({});
                     })
                .Checked(current_type.empty()));
        entries.emplace_back(MenuSection{});
        static constexpr std::array<std::string_view, 6> kTypes = {
            "string", "hash", "list", "set", "zset", "stream"};
        for (const std::string_view t : kTypes) {
          const std::string ts(t);
          entries.emplace_back(
              MenuItem(ts,
                       [on_select, ts] {
                         if (on_select) on_select(ts);
                       })
                  .Checked(current_type == t));
        }
        menu.Show(anchor, std::move(entries),
                  MenuOptions{
                      .placement = {.side = AnchorSide::Below,
                                    .alignment = AnchorAlignment::Start},
                      .gap = 4.0F,
                      .viewport_margin = 8.0F,
                      .offset = {},
                      .width = std::nullopt,
                      .dismiss_on_outside_press = true,
                      .dismiss_on_cancel = true,
                      .on_dismiss_request = {},
                  });
      });
}

[[huxerui::composable]] View RedisViewModeSwitch(
    RedisBrowserViewMode mode, bool enabled, float height,
    std::function<void(RedisBrowserViewMode)> on_change) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();

  const auto seg = [&](StringResource text_res, RedisBrowserViewMode target) {
    const bool selected = (mode == target);
    const Color fg = selected ? spec.colors.on_primary_container : spec.colors.on_surface_variant;
    const Color bg = selected ? spec.colors.primary_container : Color::Transparent();
    return Row{
        Text(text_res)
            .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::Medium), fg})
            .LineLimit(1),
    }
        .With(Frame{.height = std::max(20.0F, height - 4.0F)},
              Padding{EdgeInsets::Symmetric(9.0F, 0.0F)},
              CrossAlign{CrossAxisAlignment::Center}, CornerRadius{4.0F},
              Background{VisualFill{bg}}, Enabled{enabled})
        .OnClick([mode, target, on_change] {
          if (mode != target && on_change) {
            on_change(target);
          }
        });
  };

  return Row{
      seg(app::strings::table_view_table, RedisBrowserViewMode::Table),
      seg(app::strings::table_view_tree, RedisBrowserViewMode::Tree),
  }
      .With(Frame{.height = height, .min_height = height, .max_height = height},
            Padding{EdgeInsets::All(2.0F)}, Spacing{2.0F},
            CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
            Background{VisualFill{spec.colors.surface}},
            Border{semantic.border_subtle, 1.0F});
}

[[huxerui::composable]] View RedisTreeWorkspace(
    State<AppModel> app, State<WorkspaceModel> workspace, std::uint64_t tab_id,
    std::uint64_t reload_token, TaskScope tasks, NotifyFn notify, AppThemeConfig theme_config,
    LanguageMode language_mode, bool system_dark) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto dialogs = UseDialog();
  const auto menu = UseMenu();
  const auto decoder_anchor = UseViewHandle();
  const auto clipboard = UseApplication().Clipboard();

  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table) {
    return TableEmptyState(app::strings::table_tab_closed);
  }
  const TableModel& model = tab->table;
  const DbSession* session = nullptr;
  for (const DbSession& open : app.Get().sessions) {
    if (open.id == tab->session_id) {
      session = &open;
      break;
    }
  }
  if (session == nullptr || !session->session || !session->session->redis) {
    return TableEmptyState(app::strings::table_conn_disconnected);
  }

  const auto scan_state = UseState(RedisKeyListResult{});
  const auto scan_loading = UseState(true);
  const auto expanded_folders = UseState(std::unordered_set<std::string>{});
  const auto selected_key = UseState(std::string{});
  const auto detail_state = UseState(RedisKeyDetail{});
  const auto detail_loading = UseState(false);
  const auto string_editor = UseState(TextEditingValue{});
  const auto ttl_editor = UseState(TextEditingValue{});
  const auto active_decoder = UseState(ValueDecoderKind::Raw);
  const auto busy_write = UseState(false);
  const auto split_frames = UseState(std::array<Frame, 2>{
      Frame{.width = 280.0F, .min_width = 180.0F, .max_width = 520.0F},
      Frame{.min_width = 340.0F},
  });
  const auto split_grows = UseState(std::array<Grow, 2>{Grow(0.0F), Grow(1.0F)});

  const std::shared_ptr<AdbcSession> adbc = session->session;
  const std::uint64_t session_id = session->id;
  const bool read_only = session->read_only;
  const bool is_production = session->production_mode;
  const std::string prod_conn_name = session->name;
  const DbObject redis_db_obj{.name = tab->object_name, .table_type = "REDIS DB"};
  const std::string active_filter = model.filter;
  const std::string active_type = model.redis_type_filter;
  const std::string delimiter = tab->redis_tree_delimiter;

  const auto fetch_detail = [app, workspace, tasks, notify, adbc, session_id, redis_db_obj,
                             selected_key, detail_state, detail_loading, string_editor,
                             ttl_editor, active_decoder](std::string key, bool full_string) {
    if (selected_key.Get() != key) {
      active_decoder = ValueDecoderKind::Raw;
    }
    selected_key = key;
    detail_loading = true;
    tasks.Launch([app, workspace, tasks, notify, adbc, session_id, redis_db_obj, key, full_string,
                  selected_key, detail_state, detail_loading, string_editor,
                  ttl_editor]() mutable -> Task<void> {
      RedisKeyDetail res = co_await adbc->sequence.Run([adbc, redis_db_obj, key,
                                                        full_string](std::stop_token) {
        return LoadRedisKeyDetailBlocking(*adbc->redis, redis_db_obj, key, full_string);
      });
      if (!res.error.empty() && !adbc->IsAlive()) {
        detail_loading = false;
        HandleSessionDisconnected(app, workspace, tasks, session_id, res.error, notify);
        co_return;
      }
      if (selected_key.Get() != key) {
        co_return;
      }
      detail_loading = false;
      detail_state = res;
      if (res.exists) {
        ttl_editor = TextEditingValue::FromText(std::to_string(res.ttl));
        if (res.type == "string") {
          string_editor = TextEditingValue::FromText(res.string_value);
        } else {
          string_editor = TextEditingValue{};
        }
      }
      if (!res.error.empty() && notify) {
        notify(res.error);
      }
    });
  };

  const std::string scan_dep_key = std::to_string(tab_id) + "|" + std::to_string(session_id) + "|" +
                                   tab->object_name + "|" + active_filter + "|" + active_type +
                                   "|" + std::to_string(reload_token);
  Lifecycle(
      [app, workspace, tasks, notify, adbc, session_id, redis_db_obj, active_filter, active_type,
       scan_state, scan_loading, selected_key, detail_state]() mutable {
        auto alive = std::make_shared<std::atomic<bool>>(true);
        scan_loading = true;
        selected_key = std::string{};
        detail_state = RedisKeyDetail{};
        tasks.Launch([alive, app, workspace, tasks, notify, adbc, session_id, redis_db_obj,
                      active_filter, active_type, scan_state,
                      scan_loading]() mutable -> Task<void> {
          adbc->cancel_requested.store(false, std::memory_order_release);
          RedisKeyListResult res = co_await adbc->sequence.Run(
              [adbc, redis_db_obj, active_filter, active_type](std::stop_token) {
                return ScanRedisKeyNamesBlocking(*adbc->redis, redis_db_obj, active_filter,
                                                 active_type, &adbc->cancel_requested);
              });
          if (!alive->load(std::memory_order_acquire)) {
            co_return;
          }
          scan_loading = false;
          if (!res.error.empty() && !adbc->IsAlive()) {
            HandleSessionDisconnected(app, workspace, tasks, session_id, res.error, notify);
            co_return;
          }
          scan_state = res;
          if (!res.error.empty() && notify) {
            notify(res.error);
          }
        });
        return [alive] { alive->store(false, std::memory_order_release); };
      },
      scan_dep_key);

  const RedisKeyListResult scanned = scan_state.Get();
  std::unordered_set<std::string> all_folder_ids;
  const std::vector<RedisTreeFlatRow> flat_rows = BuildRedisTreeFlatRows(
      scanned.keys, delimiter, expanded_folders.Get(), &all_folder_ids);
  const std::string cur_selected = selected_key.Get();

  Views tree_rows;
  const std::size_t render_limit = std::min(flat_rows.size(), kMaxVisibleTreeRows);
  for (std::size_t i = 0; i < render_limit; ++i) {
    const RedisTreeFlatRow& r = flat_rows[i];
    if (r.is_folder) {
      const std::string folder_id = r.id;
      const auto toggle = [expanded_folders, folder_id] {
        expanded_folders.Update([&folder_id](std::unordered_set<std::string>& set) {
          if (!set.erase(folder_id)) {
            set.insert(folder_id);
          }
        });
      };
      tree_rows.Add(
          TreeRow(TreeRowOptions{
                      .label = r.label,
                      .trailing_text = std::to_string(r.key_count),
                      .indent_level = r.depth,
                      .emphasize = r.expanded,
                      .leading_icon = TintedIcon(app::images::folder, semantic.accent_database, 14.0F),
                      .expanded = r.expanded,
                      .on_toggle_expand = toggle,
                  },
                  toggle)
              .Key("redis-tree-dir-" + folder_id));
    } else {
      const std::string full_key = r.full_key;
      const bool is_sel = (full_key == cur_selected);
      tree_rows.Add(
          TreeRow(TreeRowOptions{
                      .label = r.label,
                      .indent_level = r.depth,
                      .selected = is_sel,
                      .leading_icon = TintedIcon(app::images::index, semantic.accent_table, 13.0F),
                  },
                  [fetch_detail, full_key] { fetch_detail(full_key, false); })
              .With(Tooltip(StringVariant(full_key)))
              .Key("redis-tree-key-" + full_key));
    }
  }
  if (flat_rows.size() > render_limit) {
    tree_rows.Add(MutedText(UseString(app::strings::redis_tree_visible_cap, render_limit,
                                      flat_rows.size()))
                      .With(Padding{EdgeInsets::Symmetric(8.0F, 6.0F)}));
  }

  const auto run_guarded_write = [read_only, is_production, prod_conn_name, dialogs, notify, loc,
                                  theme_config, language_mode, system_dark](
                                     std::string summary, std::function<void()> fn) {
    RunGuardedWrite(
        WriteGuardContext{
            .read_only = read_only,
            .production_mode = is_production,
            .connection_name = prod_conn_name,
            .dialogs = dialogs,
            .notify = notify,
            .loc = loc,
            .theme_config = theme_config,
            .language_mode = language_mode,
            .system_dark = system_dark,
        },
        std::move(summary), std::move(fn));
  };

  const auto expand_ancestors_for_key = [expanded_folders, delimiter](std::string_view full_key) {
    if (delimiter.empty()) {
      return;
    }
    expanded_folders.Update([&full_key, &delimiter](std::unordered_set<std::string>& set) {
      std::size_t cursor = 0;
      for (int depth = 0; depth < 16; ++depth) {
        const std::size_t pos = full_key.find(delimiter, cursor);
        if (pos == std::string_view::npos) {
          break;
        }
        const std::size_t next_cursor = pos + delimiter.size();
        if (pos == cursor || next_cursor >= full_key.size()) {
          break;
        }
        set.insert(std::string(full_key.substr(0, next_cursor)));
        cursor = next_cursor;
      }
    });
  };

  const bool can_add_key = !read_only && !busy_write.Get();
  const auto open_add_key = [can_add_key, dialogs, theme_config, language_mode, system_dark,
                             run_guarded_write, busy_write, tasks, adbc, redis_db_obj, app,
                             workspace, session_id, notify, scan_state, loc, fetch_detail,
                             expand_ancestors_for_key] {
    if (!can_add_key) {
      return;
    }
    dialogs.Show([theme_config, language_mode, system_dark, run_guarded_write, busy_write, tasks,
                  adbc, redis_db_obj, app, workspace, session_id, notify, scan_state, loc,
                  fetch_detail, expand_ancestors_for_key](DialogContext dlg) {
      return WithDialogTheme(
          theme_config, language_mode, system_dark,
          AddRedisKeyDialog(
              dlg,
              [run_guarded_write, busy_write, tasks, adbc, redis_db_obj, app, workspace, session_id,
               notify, scan_state, loc, fetch_detail,
               expand_ancestors_for_key](NewRedisKeyInput input) mutable {
                const std::string created_key = input.key;
                run_guarded_write(
                    "ADD " + created_key,
                    [input = std::move(input), created_key, busy_write, tasks, adbc, redis_db_obj,
                     app, workspace, session_id, notify, scan_state, loc, fetch_detail,
                     expand_ancestors_for_key]() mutable {
                      busy_write = true;
                      tasks.Launch([input = std::move(input), created_key, busy_write, adbc,
                                    redis_db_obj, app, workspace, tasks, session_id, notify,
                                    scan_state, loc, fetch_detail,
                                    expand_ancestors_for_key]() mutable -> Task<void> {
                        PendingInsertRow row;
                        row.draft_id = 1;
                        row.values = {input.key, input.value, input.type, input.ttl, ""};
                        TableModel insert_model;
                        insert_model.object = redis_db_obj.name;
                        BatchInsertResult r = co_await adbc->sequence.Run(
                            [adbc, insert_model = std::move(insert_model),
                             row = std::move(row)](std::stop_token) {
                              return InsertRedisRowsBlocking(*adbc->redis, insert_model, {row});
                            });
                        busy_write = false;
                        if (r.connection_lost) {
                          HandleSessionDisconnected(app, workspace, tasks, session_id, r.error,
                                                    notify);
                          co_return;
                        }
                        if (!r.error.empty()) {
                          if (notify) {
                            notify(r.error);
                          }
                          co_return;
                        }
                        scan_state.Update([&created_key](RedisKeyListResult& s) {
                          if (std::ranges::find(s.keys, created_key) == s.keys.end()) {
                            s.keys.push_back(created_key);
                            std::ranges::sort(s.keys);
                          }
                        });
                        expand_ancestors_for_key(created_key);
                        if (notify) {
                          notify(loc.Get(app::strings::redis_detail_created));
                        }
                        fetch_detail(created_key, false);
                      });
                    });
              }));
    });
  };

  View add_key_btn =
      Row{TintedIcon(app::images::add,
                     can_add_key ? spec.colors.primary : spec.colors.on_surface_variant, 15.0F)}
          .With(Frame{.width = 24.0F, .height = 24.0F, .min_width = 24.0F, .max_width = 24.0F,
                      .min_height = 24.0F, .max_height = 24.0F},
                MainAlign{MainAxisAlignment::Center}, CrossAlign{CrossAxisAlignment::Center},
                CornerRadius{4.0F},
                Tooltip(StringVariant(app::strings::redis_add_key_tooltip)),
                Enabled{can_add_key})
          .OnClick(open_add_key);

  View left_body =
      flat_rows.empty() && !scan_loading.Get()
          ? TableEmptyState(app::strings::redis_tree_empty)
          : ScrollView(Column{std::move(tree_rows)}.With(Spacing{1.0F},
                                                         CrossAlign{CrossAxisAlignment::Stretch}))
                .With(Grow(1.0F));
  View left_pane = Column{
      Row{
          Text(UseString(app::strings::redis_tree_keys_count, scanned.keys.size()))
              .Style(TextStyle{Font::System(font_size::kCaption).WithWeight(FontWeight::Medium),
                               spec.colors.on_surface_variant})
              .With(Grow(1.0F)),
          std::move(add_key_btn),
          CompactActionButton(
              Button(app::strings::redis_tree_expand_all)
                  .OnClick([expanded_folders, all_folder_ids] { expanded_folders = all_folder_ids; }),
              ButtonTone::Subtle, 24.0F, 7.0F)
              .With(Enabled{!all_folder_ids.empty()}),
          CompactActionButton(
              Button(app::strings::redis_tree_collapse_all)
                  .OnClick([expanded_folders] { expanded_folders = std::unordered_set<std::string>{}; }),
              ButtonTone::Subtle, 24.0F, 7.0F)
              .With(Enabled{!expanded_folders.Get().empty()}),
      }.With(Padding{EdgeInsets::Symmetric(8.0F, 4.0F)}, Spacing{4.0F},
             CrossAlign{CrossAxisAlignment::Center},
             Background{VisualFill{spec.colors.surface_container_low}},
             Border{semantic.border_subtle, 1.0F}),
      LoadingOverlay(std::move(left_body), scan_loading.Get(), app::strings::table_loading)
          .With(Grow(1.0F)),
  }.With(Border{semantic.border_subtle, 1.0F}, CrossAlign{CrossAxisAlignment::Stretch});

  View right_content;
  const RedisKeyDetail detail = detail_state.Get();
  if (cur_selected.empty()) {
    right_content = TableEmptyState(app::strings::redis_tree_select_hint);
  } else if (!detail.exists && !detail_loading.Get()) {
    right_content = TableEmptyState(app::strings::redis_detail_missing);
  } else {
    const bool write_enabled = !read_only && !busy_write.Get() && !detail_loading.Get() && detail.exists;
    View key_header = Row{
        Text(detail.key.empty() ? cur_selected : detail.key)
            .Style(TextStyle{Font::Monospace(font_size::kBody).WithWeight(FontWeight::SemiBold),
                             spec.colors.on_surface})
            .LineLimit(1)
            .Overflow(TextOverflow::Ellipsis)
            .With(Grow(1.0F)),
        StatusBadge(detail.type, spec.colors.primary, spec.colors.primary_container,
                    Color::Transparent()),
        StatusBadge("size " + std::to_string(detail.size), spec.colors.on_surface_variant,
                    spec.colors.surface_container, Color::Transparent()),
        CompactActionButton(
            Button(app::strings::action_refresh)
                .OnClick([fetch_detail, cur_selected] { fetch_detail(cur_selected, false); }),
            ButtonTone::Default, 26.0F, 8.0F),
        CompactActionButton(
            Button(app::strings::redis_detail_rename)
                .OnClick([run_guarded_write, cur_selected, dialogs, theme_config, language_mode,
                          system_dark, busy_write, tasks, adbc, redis_db_obj, app, workspace,
                          session_id, notify, scan_state, loc, fetch_detail] {
                  run_guarded_write(
                      "RENAMENX " + cur_selected,
                      [cur_selected, dialogs, theme_config, language_mode, system_dark, busy_write,
                       tasks, adbc, redis_db_obj, app, workspace, session_id, notify, scan_state,
                       loc, fetch_detail] {
                        dialogs.Show([cur_selected, theme_config, language_mode, system_dark,
                                      busy_write, tasks, adbc, redis_db_obj, app, workspace,
                                      session_id, notify, scan_state, loc,
                                      fetch_detail](DialogContext dlg) {
                          return WithDialogTheme(
                              theme_config, language_mode, system_dark,
                              RenameKeyDialog(
                                  dlg, cur_selected,
                                  [cur_selected, busy_write, tasks, adbc, redis_db_obj, app,
                                   workspace, session_id, notify, scan_state, loc,
                                   fetch_detail](std::string new_key) mutable {
                                    if (new_key == cur_selected) return;
                                    busy_write = true;
                                    tasks.Launch([cur_selected, new_key, busy_write, adbc,
                                                  redis_db_obj, app, workspace, tasks, session_id,
                                                  notify, scan_state, loc,
                                                  fetch_detail]() mutable -> Task<void> {
                                      AdbcQueryResult r =
                                          co_await adbc->sequence.Run([adbc, redis_db_obj,
                                                                       cur_selected,
                                                                       new_key](std::stop_token) {
                                            return RenameRedisKeyBlocking(*adbc->redis, redis_db_obj,
                                                                          cur_selected, new_key);
                                          });
                                      busy_write = false;
                                      if (r.connection_lost) {
                                        HandleSessionDisconnected(app, workspace, tasks, session_id,
                                                                  r.error, notify);
                                        co_return;
                                      }
                                      if (!r.error.empty()) {
                                        if (notify) notify(r.error);
                                        co_return;
                                      }
                                      scan_state.Update([&cur_selected, &new_key](RedisKeyListResult& s) {
                                        for (std::string& item : s.keys) {
                                          if (item == cur_selected) {
                                            item = new_key;
                                            break;
                                          }
                                        }
                                        std::ranges::sort(s.keys);
                                      });
                                      if (notify) notify(loc.Get(app::strings::redis_detail_renamed));
                                      fetch_detail(new_key, false);
                                    });
                                  }));
                        });
                      });
                }),
            ButtonTone::Default, 26.0F, 8.0F)
            .With(Enabled{write_enabled}),
        CompactActionButton(
            Button(app::strings::table_delete_key)
                .OnClick([run_guarded_write, cur_selected, busy_write, tasks, adbc, redis_db_obj,
                          app, workspace, session_id, notify, scan_state, selected_key,
                          detail_state, loc] {
                  run_guarded_write(
                      "DEL " + cur_selected,
                      [cur_selected, busy_write, tasks, adbc, redis_db_obj, app, workspace,
                       session_id, notify, scan_state, selected_key, detail_state, loc] {
                        busy_write = true;
                        tasks.Launch([cur_selected, busy_write, adbc, redis_db_obj, app, workspace,
                                      tasks, session_id, notify, scan_state, selected_key,
                                      detail_state, loc]() mutable -> Task<void> {
                          AdbcQueryResult r = co_await adbc->sequence.Run([adbc, redis_db_obj,
                                                                           cur_selected](std::stop_token) {
                            return DeleteRedisKeyByNameBlocking(*adbc->redis, redis_db_obj,
                                                                cur_selected);
                          });
                          busy_write = false;
                          if (r.connection_lost) {
                            HandleSessionDisconnected(app, workspace, tasks, session_id, r.error,
                                                      notify);
                            co_return;
                          }
                          if (!r.error.empty()) {
                            if (notify) notify(r.error);
                            co_return;
                          }
                          scan_state.Update([&cur_selected](RedisKeyListResult& s) {
                            std::erase(s.keys, cur_selected);
                          });
                          selected_key = std::string{};
                          detail_state = RedisKeyDetail{};
                          if (notify) notify(loc.Get(app::strings::redis_detail_deleted));
                        });
                      });
                }),
            ButtonTone::Danger, 26.0F, 8.0F)
            .With(Enabled{write_enabled}),
    }.With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, Spacing{6.0F},
           CrossAlign{CrossAxisAlignment::Center},
           Background{VisualFill{spec.colors.surface_container_low}},
           Border{semantic.border_subtle, 1.0F});

    const auto apply_ttl = [run_guarded_write, adbc, redis_db_obj, cur_selected, busy_write,
                            tasks, app, workspace, session_id, notify, loc,
                            fetch_detail](std::string ttl_val) {
      run_guarded_write("EXPIRE/PERSIST " + cur_selected,
                        [adbc, redis_db_obj, cur_selected, busy_write, tasks, app, workspace,
                         session_id, notify, loc, fetch_detail, ttl_val]() mutable {
                          busy_write = true;
                          tasks.Launch([adbc, redis_db_obj, cur_selected, busy_write, app,
                                        workspace, tasks, session_id, notify, loc, fetch_detail,
                                        ttl_val]() mutable -> Task<void> {
                            AdbcQueryResult r = co_await adbc->sequence.Run(
                                [adbc, redis_db_obj, cur_selected, ttl_val](std::stop_token) {
                                  return UpdateRedisKeyTtlBlocking(*adbc->redis, redis_db_obj,
                                                                   cur_selected, ttl_val);
                                });
                            busy_write = false;
                            if (r.connection_lost) {
                              HandleSessionDisconnected(app, workspace, tasks, session_id, r.error,
                                                        notify);
                              co_return;
                            }
                            if (!r.error.empty()) {
                              if (notify) notify(r.error);
                              co_return;
                            }
                            if (notify) notify(loc.Get(app::strings::redis_detail_ttl_updated));
                            fetch_detail(cur_selected, false);
                          });
                        });
    };

    View ttl_bar = Row{
        MutedText("TTL"),
        CompactTextField(
            TextField(ttl_editor)
                .Placeholder(app::strings::redis_detail_ttl_ph)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([ttl_editor](const TextEditingValue& next) { ttl_editor = next; })
                .OnSubmitted([apply_ttl, ttl_editor] { apply_ttl(ttl_editor.Get().text); }),
            26.0F, 8.0F, font_size::kChip)
            .With(Frame{.width = 140.0F, .height = 26.0F, .min_height = 26.0F, .max_height = 26.0F},
                  Enabled{write_enabled}),
        CompactActionButton(
            Button(app::strings::redis_detail_set_ttl)
                .OnClick([apply_ttl, ttl_editor] { apply_ttl(ttl_editor.Get().text); }),
            ButtonTone::Default, 26.0F, 8.0F)
            .With(Enabled{write_enabled}),
        CompactActionButton(
            Button(app::strings::redis_detail_persist).OnClick([apply_ttl] { apply_ttl("-1"); }),
            ButtonTone::Default, 26.0F, 8.0F)
            .With(Enabled{write_enabled}),
        Spacer(),
        CompactActionButton(
            Button(app::strings::table_ctx_copy_value).OnClick([clipboard, detail, notify, loc] {
              const std::string copy_text =
                  detail.type == "string" ? detail.string_value : detail.key;
              if (clipboard && clipboard->SupportsSynchronousText() &&
                  clipboard->WriteText(copy_text)) {
                if (notify) {
                  notify(loc.Fmt(app::strings::toast_copied,
                                 loc.Get(app::strings::table_copy_value_label)));
                }
              }
            }),
            ButtonTone::Subtle, 26.0F, 8.0F),
    }.With(Padding{EdgeInsets::Symmetric(10.0F, 4.0F)}, Spacing{6.0F},
           CrossAlign{CrossAxisAlignment::Center});

    View body_area;
    if (detail.type == "string") {
      Views str_views;
      if (detail.truncated) {
        str_views.Add(
            Row{
                MutedText(UseString(app::strings::redis_detail_truncated_str,
                                    detail.string_value.size(), detail.size))
                    .With(Grow(1.0F)),
                CompactActionButton(
                    Button(app::strings::redis_detail_load_full)
                        .OnClick([fetch_detail, cur_selected] { fetch_detail(cur_selected, true); }),
                    ButtonTone::Default, 26.0F, 8.0F),
            }.With(Padding{EdgeInsets::Symmetric(10.0F, 4.0F)}, Spacing{8.0F},
                   CrossAlign{CrossAxisAlignment::Center}));
      }
      const ValueDecoderKind cur_dec = active_decoder.Get();
      const std::string cur_raw_str = string_editor.Get().text;
      const ValueDecoderKind suggested_dec = DetectSuggestedDecoder(cur_raw_str);
      Views dec_bar;
      dec_bar.Add(DecoderSelectorButton(
          menu, decoder_anchor, loc, cur_dec, suggested_dec, true, 26.0F,
          [active_decoder](ValueDecoderKind next) { active_decoder = next; }));
      if (cur_dec == ValueDecoderKind::Raw) {
        if (suggested_dec == ValueDecoderKind::Json) {
          dec_bar.Add(
              CompactActionButton(
                  Button(app::strings::decoder_apply_json)
                      .OnClick([string_editor, cur_raw_str] {
                        const DecodedValueResult r = DecodeValue(cur_raw_str, ValueDecoderKind::Json);
                        if (r.ok) string_editor = TextEditingValue::FromText(r.text);
                      }),
                  ButtonTone::Default, 26.0F, 8.0F)
                  .With(Enabled{write_enabled && !detail.truncated}));
          dec_bar.Add(
              CompactActionButton(
                  Button(app::strings::decoder_minify_json)
                      .OnClick([string_editor, cur_raw_str] {
                        const DecodedValueResult r = MinifyJsonValue(cur_raw_str);
                        if (r.ok) string_editor = TextEditingValue::FromText(r.text);
                      }),
                  ButtonTone::Default, 26.0F, 8.0F)
                  .With(Enabled{write_enabled && !detail.truncated}));
        }
        dec_bar.Add(Spacer());
      } else {
        const DecodedValueResult decoded = DecodeValue(cur_raw_str, cur_dec);
        dec_bar.Add(MutedText(loc.Get(app::strings::decoder_readonly_notice)).With(Grow(1.0F)));
        dec_bar.Add(
            CompactActionButton(
                Button(app::strings::decoder_copy_decoded).OnClick([clipboard, decoded, notify, loc] {
                  if (clipboard && clipboard->SupportsSynchronousText() &&
                      clipboard->WriteText(decoded.ok ? decoded.text : decoded.error)) {
                    if (notify) {
                      notify(loc.Fmt(app::strings::toast_copied,
                                     loc.Get(app::strings::table_copy_value_label)));
                    }
                  }
                }),
                ButtonTone::Default, 26.0F, 8.0F));
      }
      str_views.Add(Row{std::move(dec_bar)}.With(Padding{EdgeInsets::Symmetric(10.0F, 2.0F)},
                                                 Spacing{6.0F},
                                                 CrossAlign{CrossAxisAlignment::Center}));

      if (cur_dec == ValueDecoderKind::Raw) {
        str_views.Add(
            MonospaceField(
                TextField(string_editor)
                    .Variant(TextFieldVariant::Outlined)
                    .LineLimits(TextFieldLineLimits::MultiLine(8, 28))
                    .OnChanged([string_editor](const TextEditingValue& v) { string_editor = v; }))
                .With(Grow(1.0F), Padding{EdgeInsets::Symmetric(10.0F, 4.0F)},
                      Enabled{write_enabled && !detail.truncated}));
      } else {
        str_views.Add(DecodedValuePreviewBox(DecodeValue(cur_raw_str, cur_dec), 8, 28)
                          .With(Grow(1.0F), Padding{EdgeInsets::Symmetric(10.0F, 4.0F)}));
      }
      str_views.Add(
          Row{
              Spacer(),
              CompactActionButton(
                  Button(app::strings::redis_detail_save_val)
                      .OnClick([string_editor, run_guarded_write, cur_selected, busy_write, tasks,
                                adbc, redis_db_obj, app, workspace, session_id, notify, loc,
                                fetch_detail, was_truncated = detail.truncated] {
                        const std::string new_val = string_editor.Get().text;
                        run_guarded_write(
                            "SET " + cur_selected,
                            [new_val, cur_selected, busy_write, tasks, adbc, redis_db_obj, app,
                             workspace, session_id, notify, loc, fetch_detail,
                             was_truncated]() mutable {
                              busy_write = true;
                              tasks.Launch([new_val, cur_selected, busy_write, adbc, redis_db_obj,
                                            app, workspace, tasks, session_id, notify, loc,
                                            fetch_detail,
                                            was_truncated]() mutable -> Task<void> {
                                AdbcQueryResult r = co_await adbc->sequence.Run(
                                    [adbc, redis_db_obj, cur_selected, new_val](std::stop_token) {
                                      return UpdateRedisKeyStringBlocking(*adbc->redis, redis_db_obj,
                                                                          cur_selected, new_val);
                                    });
                                busy_write = false;
                                if (r.connection_lost) {
                                  HandleSessionDisconnected(app, workspace, tasks, session_id,
                                                            r.error, notify);
                                  co_return;
                                }
                                if (!r.error.empty()) {
                                  if (notify) notify(r.error);
                                  co_return;
                                }
                                if (notify) notify(loc.Get(app::strings::redis_detail_saved));
                                fetch_detail(cur_selected, !was_truncated);
                              });
                            });
                      }),
                  ButtonTone::Primary, 28.0F, 12.0F)
                  .With(Enabled{write_enabled && !detail.truncated &&
                                cur_dec == ValueDecoderKind::Raw}),
          }.With(Padding{EdgeInsets::Symmetric(10.0F, 6.0F)}));
      body_area = Column{std::move(str_views)}.With(Grow(1.0F), Spacing{2.0F},
                                                    CrossAlign{CrossAxisAlignment::Stretch});
    } else {
      const std::vector<std::string> col_names = EntryColumnsForType(detail.type);
      const std::shared_ptr<const GridSnapshot> snap = SnapshotFromEntries(detail.entries);
      std::unordered_map<std::string, float> widths;
      for (std::size_t c = 0; c < col_names.size(); ++c) {
        widths.emplace(col_names[c], ComputeAutoColumnWidth(col_names[c], c, snap.get()));
      }
      DataTableOptions opts{
          .rows = MakeRows(snap),
          .columns = BuildReadOnlyColumns(
              col_names, snap, widths, spec.colors.on_surface, semantic.null_text, 0,
              spec.colors.on_surface_variant,
              [snap, menu, dialogs, clipboard, notify, loc, theme_config, language_mode,
               system_dark, col_names](Point pos, std::size_t row_idx, std::size_t col_idx) {
                const GridRow* gr = RowAt(snap, row_idx);
                if (gr == nullptr || col_idx >= gr->values.size()) return;
                const std::string cell_disp = gr->values[col_idx].DisplayText();
                const std::string row_tsv = gr->values[0].DisplayText() + "\t" +
                                            (gr->values.size() > 1 ? gr->values[1].DisplayText()
                                                                   : std::string{});
                const std::string title =
                    col_idx < col_names.size() ? col_names[col_idx] : std::string("value");
                auto inspect_cb = [dialogs, title, raw = gr->values[col_idx].text, clipboard, notify,
                                   theme_config, language_mode, system_dark] {
                  dialogs.Show([title, raw, clipboard, notify, theme_config, language_mode,
                                system_dark](DialogContext dialog) {
                    return WithDialogTheme(
                        theme_config, language_mode, system_dark,
                        ValueDecodeDialog(dialog, title, raw, clipboard, notify));
                  });
                };
                ShowReadOnlyCellContextMenu(pos, menu, clipboard, cell_disp, row_tsv, notify, loc,
                                            std::move(inspect_cb));
              }),
          .row_height = 26.0F,
          .header_height = 28.0F,
          .frozen_column_count = 0,
          .sort = std::nullopt,
          .selected_cell = std::nullopt,
      };
      Views col_views;
      if (detail.truncated) {
        col_views.Add(
            MutedText(UseString(app::strings::redis_detail_truncated_col, detail.entries.size(),
                                detail.size))
                .With(Padding{EdgeInsets::Symmetric(10.0F, 4.0F)}));
      }
      col_views.Add(DataTable(std::move(opts))
                        .With(Grow(1.0F), Border{semantic.border_subtle, 1.0F})
                        .Key("redis-detail-grid-" + cur_selected + "-" +
                             std::to_string(detail.entries.size())));
      body_area = Column{std::move(col_views)}.With(Grow(1.0F), Spacing{2.0F},
                                                    CrossAlign{CrossAxisAlignment::Stretch});
    }

    right_content = Column{
        std::move(key_header),
        std::move(ttl_bar),
        std::move(body_area),
    }.With(Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch});
  }

  View right_pane = LoadingOverlay(std::move(right_content), detail_loading.Get(),
                                   app::strings::table_loading)
                        .With(Grow(1.0F), Border{semantic.border_subtle, 1.0F});

  const std::array<Frame, 2> cur_frames = split_frames.Get();
  const std::array<Grow, 2> cur_grows = split_grows.Get();
  View split_body =
      SplitLayout{
          std::move(left_pane).With(cur_frames[0], cur_grows[0]).Key("redis-tree-left"),
          std::move(right_pane).With(cur_frames[1], cur_grows[1]).Key("redis-tree-right"),
      }
          .Orientation(Axis::Horizontal)
          .OnChanged([split_frames, split_grows](const std::vector<SplitChange>& changes) {
            std::array<Frame, 2> nf = split_frames.Get();
            std::array<Grow, 2> ng = split_grows.Get();
            for (const SplitChange& c : changes) {
              if (c.index < nf.size()) {
                nf[c.index] = c.frame;
                ng[c.index] = c.grow;
              }
            }
            nf[0].min_width = 180.0F;
            nf[0].max_width = 520.0F;
            if (nf[0].width.has_value()) {
              nf[0].width = std::clamp(*nf[0].width, 180.0F, 520.0F);
            }
            nf[1].min_width = 340.0F;
            split_frames = std::move(nf);
            split_grows = std::move(ng);
          })
          .With(Grow(1.0F));

  std::string status_summary = UseString(app::strings::redis_tree_keys_count, scanned.keys.size());
  if (scanned.scan_iterations_capped) {
    status_summary += " · " + std::to_string(kRedisMaxScanIterations) + " batch cap";
  } else if (scanned.scan_capped) {
    status_summary += " · " + std::to_string(kRedisMaxScanKeys) + " key cap";
  }
  Views status_items;
  status_items.Add(AuditDurationBadge(scanned.elapsed_us));
  status_items.Add(AuditCommandBadge(scanned.executed_sql, notify));
  if (!active_filter.empty()) {
    status_items.Add(
        StatusBadge(UseString(app::strings::table_active_filter, active_filter),
                    spec.colors.primary, spec.colors.primary_container, Color::Transparent())
            .With(LayoutPriority(1.0F)));
  }
  if (!active_type.empty()) {
    status_items.Add(
        StatusBadge(UseString(app::strings::table_active_type_filter, active_type),
                    spec.colors.primary, spec.colors.primary_container, Color::Transparent())
            .With(LayoutPriority(1.0F)));
  }
  if (read_only) {
    status_items.Add(
        StatusBadge(UseString(app::strings::table_read_only), spec.colors.on_surface_variant,
                    spec.colors.surface_container, Color::Transparent())
            .With(LayoutPriority(1.0F)));
  }
  if (is_production) {
    status_items.Add(
        StatusBadge(loc.Get(app::strings::badge_prod), semantic.danger,
                    semantic.danger_container, WithAlpha(semantic.danger, 0.35F))
            .With(LayoutPriority(1.0F)));
  }
  status_items.Add(Spacer());
  status_items.Add(Text(status_summary)
                       .Style(TextStyle{Font::System(font_size::kCaption),
                                        spec.colors.on_surface_variant})
                       .LineLimit(1)
                       .Overflow(TextOverflow::Ellipsis)
                       .With(LayoutPriority(1.0F)));

  return Column{
      std::move(split_body),
      BottomStatusBar(std::move(status_items)),
  }.With(Grow(1.0F), Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Stretch});
}

} // namespace dbpp
