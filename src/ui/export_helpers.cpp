#include "ui/ui_common.h"

#include <array>

namespace dbpp {

using namespace huxerui;

void ExportSqlWithPicker(std::shared_ptr<AdbcSession> adbc, std::string sql,
                         ExportFormat format, std::string suggested_name,
                         std::shared_ptr<FilePicker> picker, TaskScope tasks,
                         NotifyFn notify, LocalizedCatalog loc,
                         std::string sql_table_name) {
  if (!adbc || adbc->IsClosed()) {
    if (notify) {
      notify(loc.Get(app::strings::toast_connection_closed));
    }
    return;
  }
  if (!picker || !picker->CanSaveFiles()) {
    if (notify) {
      notify(loc.Fmt(app::strings::sql_export_failed, "FilePicker unavailable"));
    }
    return;
  }

  tasks.Launch([adbc = std::move(adbc), sql = std::move(sql), format,
                suggested_name = std::move(suggested_name), picker = std::move(picker),
                notify = std::move(notify), loc,
                sql_table_name = std::move(sql_table_name)]() mutable -> Task<void> {
    const std::string temp_path = MakeTempExportPath(format);

    const ExportStreamResult res = co_await adbc->sequence.Run(
        [adbc, sql = std::move(sql), format, temp_path,
         sql_table_name = std::move(sql_table_name)](std::stop_token) mutable {
          return ExportQueryToFilePathBlocking(*adbc, std::move(sql), format, temp_path, 0,
                                               sql_table_name);
        });

    auto cleanup = [&temp_path] { RemoveFileQuietly(temp_path); };

    if (!res.error.empty()) {
      cleanup();
      if (notify) {
        notify(loc.Fmt(app::strings::sql_export_failed, res.error));
      }
      co_return;
    }

    const ExportFormatInfo info = GetExportFormatInfo(format);
    SaveFileOptions options{
        .suggested_name = suggested_name,
        .filter = FilePickerFilter{
            .name = std::string(info.filter_name),
            .extensions = {std::string(info.extension)},
            .content_types = {std::string(info.content_type)},
        },
    };
    const bool saved = co_await picker->SaveFileAsync(File(temp_path), options);
    cleanup();
    if (saved && notify) {
      notify(loc.Fmt(app::strings::sql_export_done, std::to_string(res.rows_written)));
    }
  });
}

void AppendDataExportMenuEntries(std::vector<MenuEntry>& entries,
                                 std::shared_ptr<AdbcSession> adbc, std::string export_sql,
                                 std::string base_name, std::string sql_table_name,
                                 std::shared_ptr<FilePicker> picker, TaskScope tasks,
                                 NotifyFn notify, LocalizedCatalog loc) {
  struct FormatChoice {
    ExportFormat format;
    const StringResource* label;
  };
  const auto add_choice = [&](ExportFormat fmt, const StringResource& label) {
    const ExportFormatInfo info = GetExportFormatInfo(fmt);
    const std::string filename = base_name + "." + std::string(info.extension);
    entries.emplace_back(MenuItem(
        app::images::external_link, loc.Get(label),
        [adbc, export_sql, fmt, filename, picker, tasks, notify, loc, sql_table_name] {
          ExportSqlWithPicker(adbc, export_sql, fmt, filename, picker, tasks, notify, loc,
                              sql_table_name);
        }));
  };

  const std::array<FormatChoice, 3> tabular{{
      {ExportFormat::Csv, &app::strings::sql_export_csv},
      {ExportFormat::Tsv, &app::strings::sql_export_tsv},
      {ExportFormat::Xlsx, &app::strings::sql_export_xlsx},
  }};
  const std::array<FormatChoice, 3> structured{{
      {ExportFormat::Json, &app::strings::sql_export_json},
      {ExportFormat::Jsonl, &app::strings::sql_export_jsonl},
      {ExportFormat::SqlInsert, &app::strings::sql_export_sql_insert},
  }};
  const std::array<FormatChoice, 3> documents{{
      {ExportFormat::Markdown, &app::strings::sql_export_markdown},
      {ExportFormat::Html, &app::strings::sql_export_html},
      {ExportFormat::Xml, &app::strings::sql_export_xml},
  }};

  for (const auto& c : tabular) add_choice(c.format, *c.label);
  entries.emplace_back(MenuSection{});
  for (const auto& c : structured) add_choice(c.format, *c.label);
  entries.emplace_back(MenuSection{});
  for (const auto& c : documents) add_choice(c.format, *c.label);
}

void AppendDataImportMenuEntries(
    std::vector<MenuEntry>& entries, const std::shared_ptr<FilePicker>& picker,
    const std::shared_ptr<AdbcSession>& adbc_ptr, std::string target_table_name,
    std::string target_catalog, std::string target_schema, bool is_production,
    std::string prod_conn_name, const DialogHandle& dialogs, const TaskScope& tasks,
    NotifyFn notify, LocalizedCatalog loc, std::function<void()> on_done,
    AppThemeConfig theme_config, LanguageMode language_mode, bool system_dark) {
  struct FormatChoice {
    ExportFormat format;
    const StringResource* label;
  };
  const auto add_choice = [&](ExportFormat fmt, const StringResource& label) {
    entries.emplace_back(MenuItem(
        app::images::add, loc.Get(label),
        [fmt, picker, adbc_ptr, target_table_name, target_catalog, target_schema, is_production,
         prod_conn_name, dialogs, tasks, notify, loc, on_done, theme_config, language_mode,
         system_dark] {
          LaunchTableFileImport(fmt, picker, adbc_ptr, target_table_name, target_catalog,
                                target_schema, is_production, prod_conn_name, dialogs, tasks,
                                notify, loc, on_done, theme_config, language_mode, system_dark);
        }));
  };

  const std::array<FormatChoice, 3> tabular{{
      {ExportFormat::Csv, &app::strings::table_import_csv},
      {ExportFormat::Tsv, &app::strings::table_import_tsv},
      {ExportFormat::Xlsx, &app::strings::table_import_xlsx},
  }};
  const std::array<FormatChoice, 3> structured{{
      {ExportFormat::Json, &app::strings::table_import_json},
      {ExportFormat::Jsonl, &app::strings::table_import_jsonl},
      {ExportFormat::SqlInsert, &app::strings::table_import_sql},
  }};
  const std::array<FormatChoice, 3> documents{{
      {ExportFormat::Markdown, &app::strings::table_import_markdown},
      {ExportFormat::Html, &app::strings::table_import_html},
      {ExportFormat::Xml, &app::strings::table_import_xml},
  }};

  for (const auto& c : tabular) add_choice(c.format, *c.label);
  entries.emplace_back(MenuSection{});
  for (const auto& c : structured) add_choice(c.format, *c.label);
  entries.emplace_back(MenuSection{});
  for (const auto& c : documents) add_choice(c.format, *c.label);
}

} // namespace dbpp
