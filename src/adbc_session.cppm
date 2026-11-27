module;

#include <arrow-adbc/adbc.h>
#include <nanoarrow/nanoarrow.h>
#include <huxerui/datatable.h>
#include <huxerui/task.h>

export module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.redis;

export namespace dbpp {

/// Owns one ADBC database/connection or a native Redis client. All calls for
/// this session are serialized through `sequence`, which is the only supported
/// access path.
struct AdbcSession {
  AdbcSession() = default;
  AdbcSession(const AdbcSession&) = delete;
  AdbcSession& operator=(const AdbcSession&) = delete;
  ~AdbcSession();

  [[nodiscard]] bool IsClosed() const noexcept { return closed.load(std::memory_order_acquire); }
  [[nodiscard]] bool IsAlive() const noexcept {
    return !IsClosed() && alive.load(std::memory_order_acquire);
  }
  void MarkClosed() noexcept {
    closed.store(true, std::memory_order_release);
    alive.store(false, std::memory_order_release);
  }
  bool CancelActiveOperation() noexcept;
  void ReleaseResourcesBlocking() noexcept;

  AdbcDatabase database{};
  AdbcConnection connection{};
  std::unique_ptr<RedisClient> redis;
  huxerui::WorkerSequence sequence;
  ConnectionRequest request;
  Dialect dialect = Dialect::Generic;
  AdbcConnectionInfo connection_info;
  std::string remark;
  bool read_only = false;
  bool production_mode = false;
  bool show_system_databases = false;
  bool autocommit = true;
  bool in_transaction = false;
  bool database_created = false;
  bool connection_created = false;
  bool initialized = false;
  std::atomic<bool> closed{false};
  std::atomic<bool> alive{true};
  std::atomic<bool> ping_in_flight{false};
  std::atomic<bool> cancel_requested{false};
  std::mutex cancel_mutex;
  AdbcStatement* active_statement = nullptr;
};

struct AdbcParamSchemaResult {
  std::vector<QueryParameterMeta> parameters;
  bool supported = false;
  bool connection_lost = false;
  std::string error;
};

struct ExportStreamResult {
  std::size_t rows_written = 0;
  bool truncated = false;
  bool cancelled = false;
  bool connection_lost = false;
  std::string error;
};

struct AdbcOpenResult {
  std::shared_ptr<AdbcSession> session;
  std::string error;
};

[[nodiscard]] SanitizedUriResult SanitizeUriCredentials(std::string_view uri);
[[nodiscard]] std::string DefaultSqliteDriverPath();

[[nodiscard]] bool IsValidUtf8(std::string_view text) noexcept;
[[nodiscard]] std::string SanitizeUtf8(std::string_view text);
[[nodiscard]] std::string QuoteIdentifier(Dialect dialect, std::string_view identifier);
[[nodiscard]] std::string SqlLiteral(Dialect dialect, std::string_view text,
                                     std::string_view declared_type, bool is_null);
[[nodiscard]] std::string ObjectDatabaseName(Dialect dialect, const DbObject& object);
[[nodiscard]] bool IsSystemDatabaseOrSchema(std::string_view name);
[[nodiscard]] std::string QualifiedName(Dialect dialect, const DbObject& object);
[[nodiscard]] std::string NormalizeSqlFilter(std::string_view raw);
[[nodiscard]] std::string BuildDropObjectSql(Dialect dialect, const DbObject& object);
[[nodiscard]] std::string BuildRenameTableSql(Dialect dialect, const DbObject& object,
                                              std::string_view new_name);
[[nodiscard]] std::string BuildAddColumnSql(Dialect dialect, const DbObject& object,
                                            std::string_view column_name,
                                            std::string_view column_type, bool nullable,
                                            std::string_view default_expr = {});
[[nodiscard]] std::string BuildRenameColumnSql(Dialect dialect, const DbObject& object,
                                               std::string_view old_name,
                                               std::string_view new_name);
[[nodiscard]] std::string BuildDropColumnSql(Dialect dialect, const DbObject& object,
                                             std::string_view column_name);
[[nodiscard]] std::string BuildAlterColumnTypeSql(Dialect dialect, const DbObject& object,
                                                  std::string_view column_name,
                                                  std::string_view new_type, bool nullable);
[[nodiscard]] bool IsSqlReadStatement(std::string_view sql);
[[nodiscard]] bool IsReadStatement(Dialect dialect, std::string_view text);
[[nodiscard]] bool IsSqlPureReadQuery(std::string_view sql);
[[nodiscard]] bool IsPureReadQuery(Dialect dialect, std::string_view text);
[[nodiscard]] std::string ReadOnlyWriteBlockedMessage();
[[nodiscard]] std::string InferDriverForUri(std::string_view uri);
[[nodiscard]] DatabaseBrand DetectDatabaseBrand(std::string_view driver_or_id,
                                                std::string_view uri = {},
                                                Dialect dialect = Dialect::Generic);
[[nodiscard]] std::string DisplayNameFromUri(std::string_view uri, std::string_view fallback);
[[nodiscard]] std::string DefaultAppDataDir();
[[nodiscard]] std::string BuildPostgresqlUriWithCredentials(std::string_view uri,
                                                            std::string_view username,
                                                            std::string_view password);
[[nodiscard]] const std::vector<DatabasePreset>& SupportedDatabasePresets();
[[nodiscard]] std::string ComposePresetUri(const DatabasePreset& preset, std::string_view host,
                                           std::string_view port, std::string_view database,
                                           std::string_view extra_query = {});
[[nodiscard]] DecomposedPresetUri DecomposePresetUri(const DatabasePreset& preset,
                                                     std::string_view uri);
[[nodiscard]] const DatabasePreset& ResolvePresetForProfile(std::string_view driver_path,
                                                            std::string_view uri);
[[nodiscard]] std::optional<AdbcDriverInfo> FindInstalledDriverForPreset(
    const DatabasePreset& preset, const std::vector<AdbcDriverInfo>& discovered);
[[nodiscard]] std::string UserAdbcDriversDir();
[[nodiscard]] std::vector<AdbcDriverInfo> DiscoverAdbcDrivers();

[[nodiscard]] bool IsConnectionLostError(int adbc_status, std::string_view message);
[[nodiscard]] bool IsConnectionLostError(int adbc_status, std::string_view message,
                                         std::string_view sqlstate);
[[nodiscard]] bool ShouldHealthCheckSession(Dialect dialect, std::string_view uri);
bool ReconnectBlocking(AdbcSession& session, std::string& error_out);
AdbcQueryResult PingSessionBlocking(AdbcSession& session);

AdbcOpenResult OpenAdbcBlocking(const ConnectionRequest& request);
AdbcObjectsResult LoadObjectsBlocking(AdbcSession& session);
AdbcSchemaResult LoadSchemaBlocking(AdbcSession& session, const DbObject& object);
AdbcQueryResult ExecuteBlocking(AdbcSession& session, std::string sql, bool want_rows,
                                std::size_t max_rows);
AdbcQueryResult ExecuteSwitchDatabaseSqlBlocking(AdbcSession& session, std::string sql);

AdbcConnectionInfo LoadConnectionInfoBlocking(AdbcSession& session);
std::vector<std::string> LoadTableTypesBlocking(AdbcSession& session, std::string& error_out);
TableStatisticsResult LoadTableStatisticsBlocking(AdbcSession& session, const DbObject& object,
                                                  bool approximate = true);
bool SwitchSessionNamespaceBlocking(AdbcSession& session, std::string_view catalog,
                                    std::string_view db_schema, std::string& error_out);
AdbcSchemaResult InspectQuerySchemaBlocking(AdbcSession& session, std::string sql);

bool SetAutocommitBlocking(AdbcSession& session, bool enabled, std::string& error_out);
bool CommitTransactionBlocking(AdbcSession& session, std::string& error_out);
bool RollbackTransactionBlocking(AdbcSession& session, std::string& error_out);
bool SetIsolationLevelBlocking(AdbcSession& session, std::string_view level,
                               std::string& error_out);

AdbcParamSchemaResult InspectStatementParametersBlocking(AdbcSession& session, std::string sql);
AdbcQueryResult ExecutePreparedBlocking(AdbcSession& session, std::string sql,
                                        const std::vector<PreparedParamValue>& params,
                                        bool want_rows, std::size_t max_rows);
bool ExecutePreparedNonQuery(AdbcSession& session, std::string_view sql,
                             const std::vector<PreparedParamValue>& params,
                             const std::vector<std::string>& declared_types,
                             AdbcQueryResult& out, int& status_out);

ExportStreamResult ExportQueryStreamBlocking(AdbcSession& session, std::string sql,
                                             ExportFormat format, std::ostream& out,
                                             std::size_t max_rows = 0);
ExportStreamResult ExportQueryToFileBlocking(AdbcSession& session, std::string sql,
                                             ExportFormat format,
                                             const std::filesystem::path& path,
                                             std::size_t max_rows = 0);
std::string MakeTempExportPath(ExportFormat format);
void RemoveFileQuietly(std::string_view path);
ExportStreamResult ExportQueryToFilePathBlocking(AdbcSession& session, std::string sql,
                                                 ExportFormat format, std::string_view path,
                                                 std::size_t max_rows = 0);

BulkIngestResult IngestCsvFileBlocking(AdbcSession& session, const std::filesystem::path& path,
                                       std::string_view target_table, IngestMode mode,
                                       std::string_view target_catalog = {},
                                       std::string_view target_schema = {},
                                       char delimiter = ',', bool has_header = true);
BulkIngestResult IngestCsvPathBlocking(AdbcSession& session, std::string_view path,
                                       std::string_view target_table, IngestMode mode,
                                       std::string_view target_catalog = {},
                                       std::string_view target_schema = {},
                                       char delimiter = ',', bool has_header = true);
BulkIngestResult IngestJsonlFileBlocking(AdbcSession& session, const std::filesystem::path& path,
                                         std::string_view target_table, IngestMode mode,
                                         std::string_view target_catalog = {},
                                         std::string_view target_schema = {});
BulkIngestResult IngestJsonlPathBlocking(AdbcSession& session, std::string_view path,
                                         std::string_view target_table, IngestMode mode,
                                         std::string_view target_catalog = {},
                                         std::string_view target_schema = {});
BulkIngestResult CopyQueryToTableBlocking(AdbcSession& source, std::string source_sql,
                                          AdbcSession& target, std::string_view target_table,
                                          IngestMode mode,
                                          std::string_view target_catalog = {},
                                          std::string_view target_schema = {});

struct SqlDumpOptions {
  std::string target_database;
  std::optional<DbObject> single_object;
  bool include_schema = true;
  bool include_data = true;
};

struct SqlDumpResult {
  std::size_t tables_exported = 0;
  std::size_t rows_written = 0;
  bool connection_lost = false;
  std::string error;
};

struct SqlScriptImportResult {
  std::size_t statements_executed = 0;
  std::int64_t rows_affected = -1;
  bool connection_lost = false;
  std::string error;
};

struct TableDesignSnapshot {
  DbObject object;
  std::vector<TriggerMeta> triggers;
  std::string ddl;
  bool connection_lost = false;
  std::string error;
};

[[nodiscard]] std::vector<std::string> SplitSqlStatements(std::string_view script,
                                                          Dialect dialect);
[[nodiscard]] std::string FormatSqlStatement(std::string_view sql,
                                             Dialect dialect = Dialect::Generic);
[[nodiscard]] std::string FormatSqlScript(std::string_view script,
                                          Dialect dialect = Dialect::Generic);
[[nodiscard]] std::string MakeTempSqlExportPath();
TableDesignSnapshot LoadTableDesignBlocking(AdbcSession& session, const DbObject& object);
SqlDumpResult ExportDatabaseSqlToFileBlocking(AdbcSession& session,
                                              const SqlDumpOptions& options,
                                              const std::filesystem::path& path);
SqlDumpResult ExportDatabaseSqlToFilePathBlocking(AdbcSession& session,
                                                  const SqlDumpOptions& options,
                                                  std::string_view path);
SqlScriptImportResult ImportSqlScriptFileBlocking(AdbcSession& session,
                                                  const std::filesystem::path& path,
                                                  std::string_view target_database = {});
SqlScriptImportResult ImportSqlScriptPathBlocking(AdbcSession& session,
                                                  std::string_view path,
                                                  std::string_view target_database = {});

[[nodiscard]] bool IsTimestampColumnHint(std::string_view column_name,
                                         std::string_view column_type);
[[nodiscard]] std::optional<std::string> LocalizeTimestampValue(
    std::string_view raw_value, std::string_view column_name = {},
    std::string_view column_type = {});
[[nodiscard]] std::string NormalizeTimestampForWrite(
    std::string_view input, std::string_view raw_original = {},
    std::string_view column_name = {}, std::string_view column_type = {});
void ApplyResultTimestampLocalization(const std::vector<std::string>& columns,
                                      const std::vector<ColumnMeta>* meta,
                                      std::vector<GridRow>& rows);

} // namespace dbpp

namespace dbpp::detail {

struct AdbcCapturedError {
  std::string message;
  std::string sqlstate;
  std::int32_t vendor_code = 0;
  std::vector<std::pair<std::string, std::string>> details;
};

struct AdbcErrorGuard {
  AdbcErrorGuard() = default;
  AdbcErrorGuard(const AdbcErrorGuard&) = delete;
  AdbcErrorGuard& operator=(const AdbcErrorGuard&) = delete;
  ~AdbcErrorGuard() { Reset(); }

  AdbcError* Ptr() noexcept { return &error_; }
  void Reset() noexcept;
  [[nodiscard]] std::string Take();
  [[nodiscard]] AdbcCapturedError TakeDetailed();

  AdbcError error_ = ADBC_ERROR_INIT;
};

struct AdbcStatementGuard {
  AdbcStatement statement{};
  bool created = false;
  ~AdbcStatementGuard();
};

struct AdbcStreamHolder {
  ArrowArrayStream stream{};
  AdbcStreamHolder() = default;
  AdbcStreamHolder(const AdbcStreamHolder&) = delete;
  AdbcStreamHolder& operator=(const AdbcStreamHolder&) = delete;
  ~AdbcStreamHolder();
};

struct AdbcSchemaHolder {
  ArrowSchema schema{};
  AdbcSchemaHolder() = default;
  AdbcSchemaHolder(const AdbcSchemaHolder&) = delete;
  AdbcSchemaHolder& operator=(const AdbcSchemaHolder&) = delete;
  ~AdbcSchemaHolder();
};

struct AdbcArrayHolder {
  ArrowArray array{};
  AdbcArrayHolder() = default;
  AdbcArrayHolder(const AdbcArrayHolder&) = delete;
  AdbcArrayHolder& operator=(const AdbcArrayHolder&) = delete;
  ~AdbcArrayHolder() { Reset(); }
  void Reset() noexcept;
};

AdbcCapturedError AdbcCaptureStreamError(ArrowArrayStream& stream,
                                         AdbcStatusCode* status_out = nullptr);
void AdbcRegisterActiveStatement(AdbcSession& session, AdbcStatement* stmt) noexcept;
void AdbcClearActiveStatement(AdbcSession& session, AdbcStatement* stmt) noexcept;
void AdbcReleaseNativeHandles(AdbcSession& session) noexcept;
std::string AdbcFormatTimestampTz(std::int64_t value, int time_unit, std::string_view tz);
std::string AdbcFormatLocalTimestamp(std::int64_t value, int time_unit);
std::string AdbcFormatTimeUnit(std::int64_t value, int time_unit);
bool AdbcConsumeStreamRows(ArrowArrayStream& stream, std::size_t max_rows,
                           AdbcSession* cancel_session, AdbcQueryResult& out);
void AdbcPopulateColumnsFromArrowSchema(const ArrowSchema& schema, const DbObject* hint_object,
                                        std::vector<ColumnMeta>& out);
std::string AdbcFormatArrowSqlLiteral(const ArrowArrayView& view, std::int64_t row,
                                      const ArrowSchema* schema, Dialect dialect,
                                      std::string_view declared_type);
AdbcObjectsResult AdbcLoadObjectsBlockingOnce(AdbcSession& session, AdbcStatusCode& status_out);
void AdbcEnrichCatalogObjects(AdbcSession& session, AdbcObjectsResult& result);
AdbcQueryResult AdbcExecuteBlockingRaw(AdbcSession& session, std::string sql, bool want_rows,
                                       std::size_t max_rows);
AdbcSchemaResult AdbcLoadSchemaBlockingOnce(AdbcSession& session, const DbObject& object,
                                            AdbcStatusCode& status_out);
std::optional<std::int64_t> AdbcParseInt64(std::string_view text);
std::optional<double> AdbcParseDouble(std::string_view text);
std::optional<bool> AdbcParseBool(std::string_view text);

struct ParsedDriverManifest {
  std::string name;
  std::string version;
  std::string shared_hint;
};

bool AdbcDecodeUtf8Sequence(std::string_view text, std::size_t index,
                            std::size_t& out_len) noexcept;
std::string AdbcDecodeSystemTextToUtf8(std::string_view raw);
std::string AdbcLowerAscii(std::string_view text);
bool AdbcContainsLower(std::string_view haystack, std::string_view needle);
std::string_view AdbcTrimAscii(std::string_view text);
std::vector<std::string> AdbcSplitSearchPath(std::string_view value);
std::string AdbcSystemAdbcDriversDir();
std::filesystem::path AdbcEnvAdbcDriversDir(const char* env_var);
bool AdbcHasTomlExtension(const std::filesystem::path& path);
bool AdbcIsProfileManifest(const std::filesystem::path& path);
std::string AdbcBuildAdditionalSearchPathList();
std::optional<ParsedDriverManifest> AdbcParseDriverManifest(const std::filesystem::path& path);
Dialect AdbcDetectDialect(std::string_view driver_path, std::string_view uri);
std::string AdbcFormatDouble(double value);
std::string AdbcFormatDate(std::chrono::sys_days days);
std::string AdbcFormatTime(std::chrono::seconds seconds);
std::filesystem::path AdbcExecutableDirectory();
std::filesystem::path AdbcBundledDriverDirectory();

std::string AdbcPercentEncodeUriComponent(std::string_view text);
std::string AdbcPercentDecodeUriComponent(std::string_view text);
bool AdbcEqualsIgnoreCase(std::string_view left, std::string_view right);
std::string AdbcInjectSchemeUriCredentials(std::string_view uri, std::string_view username,
                                           std::string_view password);

} // namespace dbpp::detail
