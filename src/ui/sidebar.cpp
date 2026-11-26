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

namespace {

struct ConnectionDragPayload {
  std::string key;
  std::string label;
};

struct SidebarConnectionEntry {
  std::string key;
  std::optional<ConnectionProfile> profile;
  const DbSession* session = nullptr;
};

} // namespace

[[huxerui::composable]] View Sidebar(
    State<AppModel> app, State<WorkspaceModel> workspace,
    State<std::vector<ConnectionProfile>> profiles, std::string profiles_path, TaskScope tasks,
    NotifyFn notify, AppThemeConfig theme_config, LanguageMode language_mode, bool system_dark,
    std::function<void(ConnectionProfile)> connect_profile) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const auto config_db = UseService<ConfigDbService>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto menu = UseMenu();
  const auto dialogs = UseDialog();
  const auto clipboard = UseApplication().Clipboard();
  const AppModel& model = app.Get();
  const WorkspaceTab* active_tab = workspace.Get().ActiveTab();
  auto collapsed_sessions = UseState(std::vector<std::uint64_t>{});
  auto collapsed_groups = UseState(std::vector<std::string>{});
  auto db_open_overrides = UseState(std::unordered_map<std::string, bool>{});
  auto custom_order = UseState(std::vector<std::string>{});
  auto drop_hover_key = UseState(std::string{});
  auto conn_search = UseState(TextEditingValue{});

  const auto copy_text = [clipboard, notify, loc](std::string text, StringResource label_res) {
    if (clipboard && clipboard->SupportsSynchronousText() && clipboard->WriteText(text)) {
      if (notify) {
        notify(loc.Fmt(app::strings::toast_copied, loc.Get(label_res)));
      }
    } else if (notify) {
      notify(loc.Get(app::strings::toast_clipboard_unsupported));
    }
  };

  const auto& collapsed_session_ids = collapsed_sessions.Get();
  const auto& collapsed_group_keys = collapsed_groups.Get();
  const auto& db_open_map = db_open_overrides.Get();
  const std::string& hovered_drop_key = drop_hover_key.Get();

  // Assemble saved profiles and live sessions in one unified order so
  // clicking a connection connects/expands in place without shifting rows.
  std::vector<SidebarConnectionEntry> candidates;
  const std::vector<ConnectionProfile>& saved_profiles = profiles.Get();
  std::vector<bool> claimed_sessions(model.sessions.size(), false);
  std::unordered_map<std::string, std::size_t> profile_key_counts;
  candidates.reserve(saved_profiles.size() + model.sessions.size());

  for (const ConnectionProfile& profile : saved_profiles) {
    const std::string base_key =
        "p:" + profile.name + "\t" + profile.driver_path + "\t" + profile.uri;
    const std::size_t occ = profile_key_counts[base_key]++;
    std::string entry_key = base_key;
    if (occ > 0) {
      entry_key += "#" + std::to_string(occ);
    }

    const DbSession* matched_session = nullptr;
    if (const auto match_idx = FindMatchingSessionIndex(profile, model.sessions, claimed_sessions)) {
      claimed_sessions[*match_idx] = true;
      matched_session = &model.sessions[*match_idx];
    }

    SidebarConnectionEntry entry;
    entry.key = std::move(entry_key);
    entry.profile = profile;
    entry.session = matched_session;
    candidates.push_back(std::move(entry));
  }

  for (std::size_t i = 0; i < model.sessions.size(); ++i) {
    if (claimed_sessions[i]) {
      continue;
    }
    const DbSession& session = model.sessions[i];
    SidebarConnectionEntry entry;
    entry.key = "s:" + std::to_string(session.id);
    entry.session = &session;
    candidates.push_back(std::move(entry));
  }

  std::vector<std::string> natural_keys;
  natural_keys.reserve(candidates.size());
  for (const SidebarConnectionEntry& entry : candidates) {
    natural_keys.push_back(entry.key);
  }
  const std::vector<std::string> ordered_keys =
      OrderConnectionKeys(natural_keys, custom_order.Get());

  std::vector<SidebarConnectionEntry> ordered_entries;
  ordered_entries.reserve(ordered_keys.size());
  for (const std::string& key : ordered_keys) {
    for (const SidebarConnectionEntry& entry : candidates) {
      if (entry.key == key) {
        ordered_entries.push_back(entry);
        break;
      }
    }
  }

  const auto reorder_connections = [profiles, profiles_path, config_db, custom_order, notify, loc,
                                    natural_keys, candidates](std::string from_key,
                                                              std::string to_key) {
    const std::vector<std::string> next_order =
        ReorderConnectionKeys(natural_keys, custom_order.Get(), from_key, to_key);
    custom_order = next_order;

    std::vector<ConnectionProfile> reordered_profiles;
    reordered_profiles.reserve(profiles.Get().size());
    for (const std::string& key : next_order) {
      for (const SidebarConnectionEntry& entry : candidates) {
        if (entry.key == key && entry.profile.has_value()) {
          reordered_profiles.push_back(*entry.profile);
          break;
        }
      }
    }
    if (reordered_profiles != profiles.Get()) {
      profiles = reordered_profiles;
      if (!PersistConnectionProfileList(config_db, profiles_path, reordered_profiles) && notify) {
        notify(loc.Get(app::strings::toast_save_order_failed));
      }
    }
  };

  const auto make_drag_preview = [spec, semantic](const std::string& label) {
    return [label, spec, semantic] {
      return Text(label)
          .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold),
                           spec.colors.on_surface})
          .LineLimit(1)
          .Overflow(TextOverflow::Ellipsis)
          .With(Padding{EdgeInsets::Symmetric(10.0F, 5.0F)},
                Background{VisualFill{spec.colors.surface_container_highest}},
                Border{semantic.border_strong, 1.0F}, CornerRadius{6.0F});
    };
  };

  const std::string conn_query = TrimmedSql(conn_search.Get().text);

  Views tree_rows;
  for (const SidebarConnectionEntry& entry : ordered_entries) {
    if (!conn_query.empty()) {
      const std::string entry_name =
          entry.session != nullptr
              ? entry.session->name
              : (entry.profile->name.empty() ? UriDisplayName(entry.profile->uri)
                                             : entry.profile->name);
      const std::string entry_uri =
          entry.session != nullptr ? entry.session->uri : entry.profile->uri;
      const std::string entry_driver =
          entry.session != nullptr ? entry.session->driver_path : entry.profile->driver_path;
      if (!ContainsIgnoreCaseAscii(entry_name, conn_query) &&
          !ContainsIgnoreCaseAscii(entry_uri, conn_query) &&
          !ContainsIgnoreCaseAscii(entry_driver, conn_query)) {
        continue;
      }
    }
    const std::string& entry_key = entry.key;
    const bool is_drop_hover = hovered_drop_key == entry_key;
    const Color drop_border = is_drop_hover ? spec.colors.primary : Color::Transparent();

    if (entry.session == nullptr) {
      const ConnectionProfile& profile = *entry.profile;
      const std::string display_name =
          profile.name.empty() ? UriDisplayName(profile.uri) : profile.name;
      const auto open_edit_profile_dialog = [dialogs, profile, app, workspace, profiles,
                                             profiles_path, tasks, notify, theme_config,
                                             language_mode, system_dark] {
        dialogs.Show([profile, app, workspace, profiles, profiles_path, tasks, notify,
                      theme_config, language_mode, system_dark](DialogContext dialog) {
          return WithDialogTheme(
              theme_config, language_mode, system_dark,
              EditConnectionProfileDialog(dialog, profile, profile, std::nullopt, app, workspace,
                                          profiles, profiles_path, tasks, notify));
        });
      };
      const auto open_delete_profile_dialog = [dialogs, profile, display_name, profiles,
                                               profiles_path, notify, theme_config, language_mode,
                                               system_dark] {
        dialogs.Show([profile, display_name, profiles, profiles_path, notify, theme_config,
                      language_mode, system_dark](DialogContext dialog) {
          return WithDialogTheme(theme_config, language_mode, system_dark,
                                 DeleteSavedProfileDialog(dialog, profile, display_name, profiles,
                                                          profiles_path, notify));
        });
      };

      const auto fail_it =
          model.failed_connections.find(ConnectionFailureKey(profile.driver_path, profile.uri));
      const bool has_failed = fail_it != model.failed_connections.end();
      View unopened_row =
          TreeRow(TreeRowOptions{
                      .prefix = {},
                      .label = display_name,
                      .trailing_text = {},
                      .indent_level = 0,
                      .selected = false,
                      .emphasize = false,
                      .active_hint = false,
                      .muted = true,
                      .prefix_color = std::nullopt,
                      .status_dot = has_failed
                                        ? semantic.status_failed
                                        : WithAlpha(spec.colors.on_surface_variant, 0.45F),
                      .leading_icon =
                          DatabaseIcon(DetectDatabaseBrand(profile.driver_path, profile.uri), 15.0F),
                      .expanded = false,
                  },
                  [connect_profile, profile] { connect_profile(profile); })
              .With(Grow(1.0F),
                    DragSource(ConnectionDragPayload{entry_key, display_name},
                               make_drag_preview(display_name),
                               DragGesture{.minimum_distance = 6.0F}))
              .On<DragSourceEvents::Ended>(
                  [drop_hover_key](const DragDropResult&) { drop_hover_key = std::string{}; })
              .On<DragSourceEvents::Canceled>(
                  [drop_hover_key](const DragEvent&) { drop_hover_key = std::string{}; })
              .On<ViewEvents::ContextMenuRequested>(
                  [menu, loc, connect_profile, profile, copy_text, open_edit_profile_dialog,
                   open_delete_profile_dialog](Point position) {
                    menu.ShowAt(position, {
                        MenuItem(loc.Get(app::strings::sidebar_ctx_connect),
                                 [connect_profile, profile] { connect_profile(profile); }),
                        MenuItem(loc.Get(app::strings::sidebar_ctx_edit_conn),
                                 open_edit_profile_dialog),
                        MenuSection{},
                        MenuItem(loc.Get(app::strings::sidebar_ctx_copy_uri),
                                 [copy_text, uri = profile.uri] {
                                   copy_text(uri, app::strings::sidebar_copy_uri_label);
                                 }),
                        MenuSection{},
                        MenuItem(loc.Get(app::strings::sidebar_ctx_delete_profile),
                                 open_delete_profile_dialog),
                    });
                  });
      if (has_failed && !fail_it->second.empty()) {
        unopened_row = std::move(unopened_row).With(Tooltip(StringVariant(fail_it->second)));
      }

      tree_rows.Add(
          Row{std::move(unopened_row)}
              .With(CornerRadius{5.0F}, Border{drop_border, 2.0F},
                    DropTarget::Accepts<ConnectionDragPayload>(
                        [entry_key](const ConnectionDragPayload& payload) {
                          return payload.key != entry_key;
                        }))
              .On<DropEvents<ConnectionDragPayload>::Entered>(
                  [drop_hover_key, entry_key](const ConnectionDragPayload&, const DropEvent&) {
                    drop_hover_key = entry_key;
                  })
              .On<DropEvents<ConnectionDragPayload>::Exited>(
                  [drop_hover_key, entry_key](const ConnectionDragPayload&, const DropEvent&) {
                    if (drop_hover_key.Get() == entry_key) {
                      drop_hover_key = std::string{};
                    }
                  })
              .On<DropEvents<ConnectionDragPayload>::Dropped>(
                  [drop_hover_key, reorder_connections, entry_key](
                      const ConnectionDragPayload& payload, const DropEvent&) {
                    drop_hover_key = std::string{};
                    reorder_connections(payload.key, entry_key);
                  })
              .Key("conn-" + entry_key));
      continue;
    }

    const DbSession& session = *entry.session;
    const std::uint64_t session_id = session.id;
    const bool is_active = session_id == model.active_session;
    const bool is_expanded =
        std::ranges::find(collapsed_session_ids, session_id) == collapsed_session_ids.end();
    const bool session_has_selected_child =
        active_tab != nullptr && active_tab->kind == WorkspaceTabKind::Table &&
        active_tab->session_id == session_id;
    const bool highlight_session =
        is_active && (!is_expanded || !session_has_selected_child);
    const bool active_parent_hint = is_expanded && session_has_selected_child;

    const auto toggle_session = [app, session_id, is_expanded, is_active, collapsed_sessions] {
      if (is_expanded) {
        collapsed_sessions.Update([session_id](std::vector<std::uint64_t>& ids) {
          if (std::ranges::find(ids, session_id) == ids.end()) {
            ids.push_back(session_id);
          }
        });
        return;
      }
      collapsed_sessions.Update(
          [session_id](std::vector<std::uint64_t>& ids) { std::erase(ids, session_id); });
      if (!is_active) {
        app.Update([session_id](AppModel& value) { value.active_session = session_id; });
      }
    };

    const auto refresh_session = [app, workspace, tasks, notify, session_id] {
      RefreshSessionObjects(app, workspace, tasks, session_id, notify, true);
    };

    const auto disconnect_session = [app, workspace, tasks, notify, session_id,
                                     collapsed_sessions, collapsed_groups, db_open_overrides] {
      collapsed_sessions.Update([session_id](std::vector<std::uint64_t>& ids) {
        std::erase(ids, session_id);
      });
      const std::string group_prefix = std::to_string(session_id) + ":";
      collapsed_groups.Update([&group_prefix](std::vector<std::string>& keys) {
        std::erase_if(keys, [&group_prefix](const std::string& key) {
          return key.starts_with(group_prefix);
        });
      });
      db_open_overrides.Update([&group_prefix](std::unordered_map<std::string, bool>& overrides) {
        std::erase_if(overrides, [&group_prefix](const auto& kv) {
          return kv.first.starts_with(group_prefix);
        });
      });
      CloseSession(app, workspace, tasks, session_id, notify);
    };

    View close = TintedIcon(app::images::close, spec.colors.on_surface_variant, 11.0F)
                     .With(Padding{EdgeInsets::Symmetric(4.0F, 4.0F)}, CornerRadius{4.0F})
                     .OnClick(disconnect_session);

    View connection_row =
        TreeRow(
            TreeRowOptions{
                .prefix = {},
                .label = session.name,
                .trailing_text = std::to_string(session.objects.size()),
                .indent_level = 0,
                .selected = highlight_session,
                .emphasize = true,
                .active_hint = active_parent_hint,
                .muted = false,
                .prefix_color = std::nullopt,
                .status_dot = semantic.status_connected,
                .leading_icon = DatabaseIcon(
                    DetectDatabaseBrand(session.driver_path, session.uri, session.dialect), 15.0F),
                .expanded = is_expanded,
                .trailing_action = std::move(close),
            },
            toggle_session)
            .With(Grow(1.0F),
                  DragSource(ConnectionDragPayload{entry_key, session.name},
                             make_drag_preview(session.name),
                             DragGesture{.minimum_distance = 6.0F}))
            .On<DragSourceEvents::Ended>(
                [drop_hover_key](const DragDropResult&) { drop_hover_key = std::string{}; })
            .On<DragSourceEvents::Canceled>(
                [drop_hover_key](const DragEvent&) { drop_hover_key = std::string{}; });

    const ConnectionProfile edit_initial = entry.profile.value_or(ConnectionProfile{
        .name = session.name,
        .driver_path = session.driver_path,
        .uri = session.uri,
        .username = {},
        .password = {},
    });
    const std::optional<ConnectionProfile> edit_original = entry.profile;
    const auto open_edit_session_dialog = [dialogs, edit_original, edit_initial, session_id, app,
                                           workspace, profiles, profiles_path, tasks, notify,
                                           theme_config, language_mode, system_dark] {
      dialogs.Show([edit_original, edit_initial, session_id, app, workspace, profiles,
                    profiles_path, tasks, notify, theme_config, language_mode,
                    system_dark](DialogContext dialog) {
        return WithDialogTheme(
            theme_config, language_mode, system_dark,
            EditConnectionProfileDialog(dialog, edit_original, edit_initial, session_id, app,
                                        workspace, profiles, profiles_path, tasks, notify));
      });
    };

    tree_rows.Add(
        Row{std::move(connection_row)}
            .With(CornerRadius{5.0F}, Border{drop_border, 2.0F},
                  DropTarget::Accepts<ConnectionDragPayload>(
                      [entry_key](const ConnectionDragPayload& payload) {
                        return payload.key != entry_key;
                      }))
            .On<DropEvents<ConnectionDragPayload>::Entered>(
                [drop_hover_key, entry_key](const ConnectionDragPayload&, const DropEvent&) {
                  drop_hover_key = entry_key;
                })
            .On<DropEvents<ConnectionDragPayload>::Exited>(
                [drop_hover_key, entry_key](const ConnectionDragPayload&, const DropEvent&) {
                  if (drop_hover_key.Get() == entry_key) {
                    drop_hover_key = std::string{};
                  }
                })
            .On<DropEvents<ConnectionDragPayload>::Dropped>(
                [drop_hover_key, reorder_connections, entry_key](
                    const ConnectionDragPayload& payload, const DropEvent&) {
                  drop_hover_key = std::string{};
                  reorder_connections(payload.key, entry_key);
                })
            .On<ViewEvents::ContextMenuRequested>(
                [menu, loc, is_expanded, toggle_session, refresh_session, app, workspace,
                 copy_text, open_edit_session_dialog, disconnect_session, session_id,
                 uri = session.uri, session_name = session.name,
                 is_redis = (session.dialect == Dialect::Redis), notify](Point position) {
                  menu.ShowAt(position, {
                      MenuItem(loc.Get(is_expanded
                                           ? (is_redis ? app::strings::sidebar_ctx_collapse_dbs
                                                       : app::strings::sidebar_ctx_collapse_objects)
                                           : (is_redis ? app::strings::sidebar_ctx_expand_dbs
                                                       : app::strings::sidebar_ctx_expand_objects)),
                               toggle_session),
                      MenuItem(loc.Get(is_redis ? app::strings::sidebar_ctx_refresh_dbs
                                                : app::strings::sidebar_ctx_refresh_objects),
                               refresh_session),
                      MenuItem(loc.Get(is_redis ? app::strings::sidebar_ctx_new_console
                                                : app::strings::nav_new_query),
                               [app, workspace, session_id] {
                                 (void)OpenSqlTab(app, workspace, session_id);
                               }),
                      MenuSection{},
                      MenuItem(loc.Get(app::strings::sidebar_ctx_edit_conn),
                               open_edit_session_dialog),
                      MenuItem(loc.Get(app::strings::sidebar_ctx_copy_uri),
                               [copy_text, uri] {
                                 copy_text(uri, app::strings::sidebar_copy_uri_label);
                               }),
                      MenuSection{},
                      MenuItem(loc.Get(app::strings::sidebar_ctx_disconnect),
                               [disconnect_session, session_name, notify, loc] {
                                 disconnect_session();
                                 if (notify) {
                                   notify(loc.Fmt(app::strings::toast_disconnected, session_name));
                                 }
                               }),
                  });
                })
            .Key("conn-" + entry_key));

    if (!is_expanded) {
      continue;
    }

    if (session.objects.empty()) {
      tree_rows.Add(TreeRow(TreeRowOptions{
                                .prefix = {},
                                .label = UseString(session.dialect == Dialect::Redis
                                                       ? app::strings::sidebar_empty_redis
                                                       : app::strings::sidebar_empty_tables),
                                .trailing_text = {},
                                .indent_level = 1,
                                .selected = false,
                                .emphasize = false,
                                .active_hint = false,
                                .muted = true,
                            })
                        .Key("empty-" + std::to_string(session_id)));
      continue;
    }

    const bool multi_schema = SessionHasMultipleSchemas(session);
    const bool use_db_hierarchy = SessionUsesDatabaseHierarchy(session);

    const auto render_object_row = [&](const DbObject& object, const std::string& key_prefix,
                                       std::size_t idx, int indent_level,
                                       bool hide_schema_prefix, Color icon_accent) {
      const bool is_redis = session.dialect == Dialect::Redis;
      const std::string object_schema =
          object.db_schema.empty() ? ObjectDatabaseName(session.dialect, object)
                                   : object.db_schema;
      const bool is_selected =
          active_tab != nullptr && active_tab->kind == WorkspaceTabKind::Table &&
          active_tab->session_id == session_id && active_tab->object_name == object.name &&
          active_tab->object_schema == object_schema &&
          active_tab->is_view == object.is_view;
      const std::string display_label =
          (!hide_schema_prefix && multi_schema && !object_schema.empty())
              ? (object_schema + "." + object.name)
              : object.name;
      const std::string name = object.name;
      const bool is_view = object.is_view;
      const std::string qualified =
          is_redis ? object.name : QualifiedName(session.dialect, object);
      const std::string open_in_sql = "SELECT * FROM " + qualified + ";";
      const std::string select_sql = "SELECT * FROM " + qualified + " LIMIT 100;";
      const std::string drop_sql = BuildDropObjectSql(session.dialect, object);
      const StringResource kind_res = ObjectKindResource(object);
      const std::string row_trailing =
          (is_redis && object.item_count.has_value())
              ? std::to_string(*object.item_count)
              : std::string{};
      std::string redis_db_index = "0";
      if (is_redis && object.name.size() > 2 &&
          (object.name.starts_with("db") || object.name.starts_with("DB"))) {
        redis_db_index = object.name.substr(2);
      }

      const auto open_object = [app, workspace, tasks, notify, session_id, name,
                                object_schema, is_view] {
        SelectObject(app, workspace, tasks, session_id, name, object_schema, is_view,
                     notify);
      };

      const auto open_schema_dialog = [dialogs, adbc = session.session, session_id, object,
                                       display_label, qualified, app, workspace, tasks,
                                       notify, theme_config, language_mode, system_dark] {
        if (!adbc || adbc->IsClosed()) {
          HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
          return;
        }
        dialogs.Show([adbc, session_id, object, display_label, qualified, app, workspace,
                      tasks, notify, theme_config, language_mode,
                      system_dark](DialogContext dialog) {
          return WithDialogTheme(
              theme_config, language_mode, system_dark,
              ObjectSchemaDialog(dialog, adbc, session_id, object, display_label, qualified,
                                 app, workspace, tasks, notify));
        });
      };

      const auto open_drop_dialog = [dialogs, adbc = session.session, session_id, object,
                                     display_label, drop_sql, app, workspace, tasks, notify,
                                     theme_config, language_mode, system_dark] {
        if (!adbc || adbc->IsClosed()) {
          HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
          return;
        }
        dialogs.Show([adbc, session_id, object, display_label, drop_sql, app, workspace,
                      tasks, notify, theme_config, language_mode,
                      system_dark](DialogContext dialog) {
          return WithDialogTheme(theme_config, language_mode, system_dark,
                                 DropObjectDialog(dialog, adbc, session_id, object,
                                                  display_label, drop_sql, app, workspace,
                                                  tasks, notify));
        });
      };

      const auto open_edit_table_dialog = [dialogs, adbc = session.session, session_id,
                                           dialect = session.dialect, object, display_label,
                                           app, workspace, tasks, notify, theme_config,
                                           language_mode, system_dark] {
        if (!adbc || adbc->IsClosed()) {
          HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
          return;
        }
        dialogs.Show([adbc, session_id, dialect, object, display_label, app, workspace, tasks,
                      notify, theme_config, language_mode,
                      system_dark](DialogContext dialog) {
          return WithDialogTheme(
              theme_config, language_mode, system_dark,
              EditTableDialog(dialog, adbc, session_id, dialect, object, display_label, app,
                              workspace, tasks, notify));
        });
      };

      const bool is_editable_table =
          !is_redis && !object.is_view &&
          !ContainsIgnoreCaseAscii(object.table_type, "foreign");
      const ImageResource& obj_icon =
          is_redis
              ? app::images::database
              : (ContainsIgnoreCaseAscii(object.table_type, "materialized")
                     ? app::images::mview
                     : (object.is_view
                            ? app::images::view
                            : (ContainsIgnoreCaseAscii(object.table_type, "foreign")
                                   ? app::images::foreign_table
                                   : app::images::table)));
      const Color resolved_icon_color = icon_accent;

      tree_rows.Add(
          TreeRow(TreeRowOptions{
                      .prefix = {},
                      .label = display_label,
                      .trailing_text = row_trailing,
                      .indent_level = indent_level,
                      .selected = is_selected,
                      .emphasize = is_redis && is_selected,
                      .active_hint = false,
                      .muted = false,
                      .prefix_color = icon_accent,
                      .status_dot = std::nullopt,
                      .leading_icon = TintedIcon(obj_icon, resolved_icon_color, 14.0F),
                  },
                  open_object)
              .On<ViewEvents::ContextMenuRequested>(
                  [menu, loc, open_object, app, workspace, open_schema_dialog,
                   open_edit_table_dialog, copy_text, refresh_session, open_drop_dialog,
                   session_id, open_in_sql, select_sql, display_label, kind_res, is_redis,
                   is_editable_table, redis_db_index](Point position) {
                    if (is_redis) {
                      menu.ShowAt(position, {
                          MenuItem(loc.Get(app::strings::sidebar_ctx_open_browse),
                                   open_object),
                          MenuItem(loc.Get(app::strings::sidebar_ctx_open_in_console),
                                   [app, workspace, session_id, redis_db_index,
                                    display_label] {
                                     (void)OpenSqlTab(app, workspace, session_id,
                                                      "SELECT " + redis_db_index,
                                                      "Redis · " + display_label,
                                                      "db" + redis_db_index);
                                   }),
                          MenuItem(loc.Get(app::strings::sidebar_ctx_view_fields),
                                   open_schema_dialog),
                          MenuSection{},
                          MenuItem(loc.Get(app::strings::sidebar_ctx_copy_name),
                                   [copy_text, display_label] {
                                     copy_text(display_label,
                                               app::strings::sidebar_copy_name_label);
                                   }),
                          MenuSection{},
                          MenuItem(loc.Get(app::strings::sidebar_ctx_refresh_dbs),
                                   refresh_session),
                      });
                      return;
                    }
                    std::vector<MenuEntry> entries;
                    entries.emplace_back(
                        MenuItem(loc.Get(app::strings::sidebar_ctx_open_browse), open_object));
                    entries.emplace_back(MenuItem(
                        loc.Get(app::strings::sidebar_ctx_open_in_sql),
                        [app, workspace, session_id, open_in_sql, display_label] {
                          (void)OpenSqlTab(app, workspace, session_id, open_in_sql,
                                           "SQL · " + display_label);
                        }));
                    entries.emplace_back(MenuItem(
                        loc.Get(app::strings::sidebar_ctx_view_schema), open_schema_dialog));
                    if (is_editable_table) {
                      entries.emplace_back(MenuItem(
                          loc.Get(app::strings::sidebar_ctx_edit_table),
                          open_edit_table_dialog));
                    }
                    entries.emplace_back(MenuSection{});
                    entries.emplace_back(MenuItem(
                        loc.Get(app::strings::sidebar_ctx_copy_name),
                        [copy_text, display_label] {
                          copy_text(display_label, app::strings::sidebar_copy_name_label);
                        }));
                    entries.emplace_back(MenuItem(
                        loc.Get(app::strings::sidebar_ctx_copy_select),
                        [copy_text, select_sql] {
                          copy_text(select_sql, app::strings::sidebar_copy_select_label);
                        }));
                    entries.emplace_back(MenuSection{});
                    entries.emplace_back(MenuItem(
                        loc.Get(app::strings::sidebar_ctx_refresh_objects), refresh_session));
                    entries.emplace_back(MenuItem(
                        loc.Fmt(app::strings::sidebar_ctx_drop_object, loc.Get(kind_res)),
                        open_drop_dialog));
                    menu.ShowAt(position, std::move(entries));
                  })
              .Key("obj-" + key_prefix + "-" + object_schema + "-" + name + "-" +
                   std::to_string(idx)));
    };

    const auto render_object_group =
        [&](const SidebarObjectGroup& group, const std::string& group_id, int group_indent,
            bool hide_schema_prefix) {
          const bool group_open =
              std::ranges::find(collapsed_group_keys, group_id) == collapsed_group_keys.end();

          const auto toggle_group = [collapsed_groups, group_id] {
            collapsed_groups.Update([&group_id](std::vector<std::string>& keys) {
              const auto it = std::ranges::find(keys, group_id);
              if (it == keys.end()) {
                keys.push_back(group_id);
              } else {
                keys.erase(it);
              }
            });
          };

          const bool is_view_group = group.key == "views" || group.key == "mviews";
          const bool is_foreign_group = group.key == "foreign";
          const Color group_accent =
              is_view_group ? semantic.accent_view
                            : (is_foreign_group ? semantic.accent_external : semantic.accent_table);
          const ImageResource& group_icon = ObjectGroupIconResource(group.key);

          tree_rows.Add(
              TreeRow(TreeRowOptions{
                          .prefix = {},
                          .label = UseString(group.label),
                          .trailing_text = std::to_string(group.items.size()),
                          .indent_level = group_indent,
                          .selected = false,
                          .emphasize = true,
                          .active_hint = false,
                          .muted = false,
                          .prefix_color = group_accent,
                          .status_dot = std::nullopt,
                          .leading_icon = TintedIcon(group_icon, group_accent, 14.0F),
                          .expanded = group_open,
                      },
                      toggle_group)
                  .On<ViewEvents::ContextMenuRequested>(
                      [menu, loc, group_open, toggle_group, refresh_session, app, workspace,
                       session_id](Point position) {
                        menu.ShowAt(position, {
                            MenuItem(loc.Get(group_open ? app::strings::sidebar_ctx_collapse_group
                                                        : app::strings::sidebar_ctx_expand_group),
                                     toggle_group),
                            MenuItem(loc.Get(app::strings::sidebar_ctx_refresh_objects),
                                     refresh_session),
                            MenuItem(loc.Get(app::strings::nav_new_query),
                                     [app, workspace, session_id] {
                                       (void)OpenSqlTab(app, workspace, session_id);
                                     }),
                        });
                      })
                  .Key("group-" + group_id));

          if (!group_open) {
            return;
          }

          for (std::size_t idx = 0; idx < group.items.size(); ++idx) {
            render_object_row(*group.items[idx], group_id, idx, group_indent + 1,
                              hide_schema_prefix, group_accent);
          }
        };

    if (session.dialect == Dialect::Redis) {
      const std::string key_prefix = std::to_string(session_id) + ":redis";
      for (std::size_t idx = 0; idx < session.objects.size(); ++idx) {
        render_object_row(session.objects[idx], key_prefix, idx, 1, true,
                          semantic.accent_database);
      }
    } else if (use_db_hierarchy) {
      const std::vector<SidebarDatabaseGroup> db_buckets = GroupSessionDatabases(session);
      for (const SidebarDatabaseGroup& db_bucket : db_buckets) {
        const std::string db_key = std::to_string(session_id) + ":db:" + db_bucket.name;
        const auto override_it = db_open_map.find(db_key);
        const bool db_open =
            override_it != db_open_map.end() ? override_it->second : db_bucket.default_open;
        const auto toggle_db = [db_open_overrides, db_key, db_open] {
          db_open_overrides.Update([&db_key, db_open](std::unordered_map<std::string, bool>& map) {
            map[db_key] = !db_open;
          });
        };

        const Color db_icon_color = (!db_open && db_bucket.is_system)
                                        ? WithAlpha(semantic.accent_database, 0.55F)
                                        : semantic.accent_database;
        tree_rows.Add(
            TreeRow(TreeRowOptions{
                        .prefix = {},
                        .label = db_bucket.name,
                        .trailing_text = std::to_string(db_bucket.object_count),
                        .indent_level = 1,
                        .selected = false,
                        .emphasize = db_open,
                        .active_hint = false,
                        .muted = !db_open && db_bucket.is_system,
                        .prefix_color = std::optional<Color>(db_icon_color),
                        .status_dot = std::nullopt,
                        .leading_icon = TintedIcon(app::images::database, db_icon_color, 14.0F),
                        .expanded = db_open,
                    },
                    toggle_db)
                .On<ViewEvents::ContextMenuRequested>(
                    [menu, loc, db_open, toggle_db, refresh_session, app, workspace,
                     session_id](Point position) {
                      menu.ShowAt(position, {
                          MenuItem(loc.Get(db_open ? app::strings::sidebar_ctx_collapse_db
                                                   : app::strings::sidebar_ctx_expand_db),
                                   toggle_db),
                          MenuItem(loc.Get(app::strings::sidebar_ctx_refresh_objects),
                                   refresh_session),
                          MenuItem(loc.Get(app::strings::nav_new_query),
                                   [app, workspace, session_id] {
                                     (void)OpenSqlTab(app, workspace, session_id);
                                   }),
                      });
                    })
                .Key("db-" + db_key));

        if (!db_open) {
          continue;
        }
        for (const SidebarObjectGroup& group : db_bucket.groups) {
          render_object_group(group, db_key + ":" + group.key, 2, true);
        }
      }
    } else {
      const std::vector<SidebarObjectGroup> groups = GroupSessionObjects(session);
      for (const SidebarObjectGroup& group : groups) {
        render_object_group(group, std::to_string(session_id) + ":" + group.key, 1, false);
      }
    }
  }

  TextField search_field =
      TextField(conn_search)
          .Placeholder(app::strings::sidebar_search_placeholder)
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .VerticalAlign(TextVerticalAlign::Center)
          .LeadingIcon(app::images::search)
          .OnChanged([conn_search](const TextEditingValue& value) { conn_search = value; });
  if (!conn_query.empty()) {
    search_field =
        std::move(search_field)
            .TrailingIcon(app::images::close, app::strings::action_clear)
            .OnTrailingIconClick([conn_search] { conn_search = TextEditingValue{}; });
  }

  View tree_view;
  if (tree_rows.Size() == 0) {
    const std::string empty_msg =
        conn_query.empty() ? UseString(app::strings::sidebar_empty)
                           : UseString(app::strings::sidebar_search_no_match, conn_query);
    tree_view = MutedText(empty_msg).With(Grow(1.0F));
  } else {
    tree_view = ScrollView(Column{std::move(tree_rows)}
                               .With(Spacing{1.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
                    .With(Grow(1.0F));
  }

  return Column{
      CompactTextField(std::move(search_field), 28.0F, 8.0F, font_size::kChip),
      std::move(tree_view),
  }.With(Padding{EdgeInsets::Symmetric(6.0F, 6.0F)}, Spacing{5.0F},
         CrossAlign{CrossAxisAlignment::Stretch},
         Background{VisualFill{spec.colors.surface_container_low}});
}

} // namespace dbpp
