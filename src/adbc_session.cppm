module;

#include <arrow-adbc/adbc.h>
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
  void ReleaseResourcesBlocking() noexcept;

  AdbcDatabase database{};
  AdbcConnection connection{};
  std::unique_ptr<RedisClient> redis;
  huxerui::WorkerSequence sequence;
  ConnectionRequest request;
  Dialect dialect = Dialect::Generic;
  bool database_created = false;
  bool connection_created = false;
  bool initialized = false;
  std::atomic<bool> closed{false};
  std::atomic<bool> alive{true};
  std::atomic<bool> ping_in_flight{false};
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
[[nodiscard]] bool ShouldHealthCheckSession(Dialect dialect, std::string_view uri);
bool ReconnectBlocking(AdbcSession& session, std::string& error_out);
AdbcQueryResult PingSessionBlocking(AdbcSession& session);

AdbcOpenResult OpenAdbcBlocking(const ConnectionRequest& request);
AdbcObjectsResult LoadObjectsBlocking(AdbcSession& session);
AdbcSchemaResult LoadSchemaBlocking(AdbcSession& session, const DbObject& object);
AdbcQueryResult ExecuteBlocking(AdbcSession& session, std::string sql, bool want_rows,
                                std::size_t max_rows);

} // namespace dbpp
