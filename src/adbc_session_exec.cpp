module;

#include <arrow-adbc/adbc.h>
#include <arrow-adbc/adbc_driver_manager.h>
#include <huxerui/datatable.h>
#include <huxerui/task.h>

#include <cstdio>

module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.redis;

namespace dbpp {

namespace {

std::string ExtractSqlState(const AdbcError& error) {
  std::string out;
  for (int i = 0; i < 5; ++i) {
    const char ch = error.sqlstate[i];
    if (ch == '\0') {
      break;
    }
    if (std::isalnum(static_cast<unsigned char>(ch))) {
      out.push_back(ch);
    }
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> ExtractErrorDetails(const AdbcError& error) {
  std::vector<std::pair<std::string, std::string>> details;
  if (error.vendor_code != ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA) {
    return details;
  }
  const int count = AdbcErrorGetDetailCount(&error);
  for (int i = 0; i < count; ++i) {
    const AdbcErrorDetail d = AdbcErrorGetDetail(&error, i);
    if (d.key == nullptr || d.value == nullptr || d.value_length == 0) {
      continue;
    }
    const std::string_view raw(reinterpret_cast<const char*>(d.value), d.value_length);
    details.emplace_back(d.key, IsValidUtf8(raw) ? std::string(raw) : SanitizeUtf8(raw));
  }
  return details;
}

detail::AdbcCapturedError CaptureFromAdbcError(const AdbcError& error) {
  detail::AdbcCapturedError out;
  out.message = error.message != nullptr ? detail::AdbcDecodeSystemTextToUtf8(error.message)
                                       : "unknown ADBC error";
  out.sqlstate = ExtractSqlState(error);
  if (error.vendor_code != ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA) {
    out.vendor_code = error.vendor_code;
  }
  out.details = ExtractErrorDetails(error);
  return out;
}

AdbcQueryResult ExecuteBlockingOnce(AdbcSession& session, const std::string& sql, bool want_rows,
                                    std::size_t max_rows, AdbcStatusCode& status_out,
                                    bool& before_execute_out) {
  AdbcQueryResult result;
  before_execute_out = true;
  if (!session.connection_created) {
    status_out = ADBC_STATUS_INVALID_STATE;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }

  session.cancel_requested.store(false, std::memory_order_release);

  detail::AdbcErrorGuard error;
  detail::AdbcStatementGuard statement_guard;
  status_out = AdbcStatementNew(&session.connection, &statement_guard.statement, error.Ptr());
  if (status_out != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.sqlstate = cap.sqlstate;
    result.vendor_code = cap.vendor_code;
    result.error_details = cap.details;
    return result;
  }
  statement_guard.created = true;

  status_out = AdbcStatementSetSqlQuery(&statement_guard.statement, sql.c_str(), error.Ptr());
  if (status_out != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.sqlstate = cap.sqlstate;
    return result;
  }

  detail::AdbcStreamHolder stream;
  std::int64_t affected = -1;
  before_execute_out = false;
  detail::AdbcRegisterActiveStatement(session, &statement_guard.statement);
  status_out = AdbcStatementExecuteQuery(&statement_guard.statement,
                                         want_rows ? &stream.stream : nullptr, &affected,
                                         error.Ptr());
  detail::AdbcClearActiveStatement(session, &statement_guard.statement);
  if (status_out != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.sqlstate = cap.sqlstate;
    result.vendor_code = cap.vendor_code;
    result.error_details = cap.details;
    result.cancelled = (status_out == ADBC_STATUS_CANCELLED) ||
                       session.cancel_requested.load(std::memory_order_acquire);
    return result;
  }
  result.affected = affected;

  if (!want_rows) {
    return result;
  }

  (void)detail::AdbcConsumeStreamRows(stream.stream, max_rows, &session, result);
  return result;
}

} // namespace

namespace detail {

void AdbcErrorGuard::Reset() noexcept {
  if (error_.release != nullptr) {
    error_.release(&error_);
    error_.release = nullptr;
  }
}

std::string AdbcErrorGuard::Take() {
  std::string message =
      error_.message != nullptr ? AdbcDecodeSystemTextToUtf8(error_.message) : "unknown ADBC error";
  Reset();
  return message;
}

AdbcCapturedError AdbcErrorGuard::TakeDetailed() {
  AdbcCapturedError captured = CaptureFromAdbcError(error_);
  Reset();
  return captured;
}

AdbcStatementGuard::~AdbcStatementGuard() {
  if (created) {
    AdbcErrorGuard release_error;
    AdbcStatementRelease(&statement, release_error.Ptr());
  }
}

AdbcStreamHolder::~AdbcStreamHolder() {
  if (stream.release != nullptr) {
    stream.release(&stream);
    stream = ArrowArrayStream{};
  }
}

AdbcSchemaHolder::~AdbcSchemaHolder() {
  if (schema.release != nullptr) {
    schema.release(&schema);
    schema = ArrowSchema{};
  }
}

void AdbcArrayHolder::Reset() noexcept {
  if (array.release != nullptr) {
    array.release(&array);
    array = ArrowArray{};
  }
}

AdbcCapturedError AdbcCaptureStreamError(ArrowArrayStream& stream, AdbcStatusCode* status_out) {
  AdbcStatusCode code = ADBC_STATUS_OK;
  const AdbcError* adbc_err = AdbcErrorFromArrayStream(&stream, &code);
  if (status_out != nullptr) {
    *status_out = code;
  }
  if (adbc_err != nullptr && adbc_err->message != nullptr) {
    return CaptureFromAdbcError(*adbc_err);
  }
  const char* message =
      stream.get_last_error != nullptr ? stream.get_last_error(&stream) : nullptr;
  AdbcCapturedError out;
  out.message = message != nullptr ? AdbcDecodeSystemTextToUtf8(message) : "Arrow stream error";
  return out;
}

void AdbcRegisterActiveStatement(AdbcSession& session, AdbcStatement* stmt) noexcept {
  std::lock_guard lock(session.cancel_mutex);
  session.active_statement = stmt;
}

void AdbcClearActiveStatement(AdbcSession& session, AdbcStatement* stmt) noexcept {
  std::lock_guard lock(session.cancel_mutex);
  if (session.active_statement == stmt) {
    session.active_statement = nullptr;
  }
}

void AdbcReleaseNativeHandles(AdbcSession& session) noexcept {
  {
    std::lock_guard lock(session.cancel_mutex);
    session.active_statement = nullptr;
  }
  if (session.connection_created) {
    AdbcErrorGuard error;
    AdbcConnectionRelease(&session.connection, error.Ptr());
    session.connection_created = false;
    session.connection = {};
  }
  if (session.database_created) {
    AdbcErrorGuard error;
    AdbcDatabaseRelease(&session.database, error.Ptr());
    session.database_created = false;
    session.database = {};
  }
  session.initialized = false;
}

} // namespace detail

bool AdbcSession::CancelActiveOperation() noexcept {
  cancel_requested.store(true, std::memory_order_release);
  if (IsClosed()) {
    return false;
  }
  std::lock_guard lock(cancel_mutex);
  if (active_statement != nullptr) {
    detail::AdbcErrorGuard err;
    return AdbcStatementCancel(active_statement, err.Ptr()) == ADBC_STATUS_OK;
  }
  if (connection_created) {
    detail::AdbcErrorGuard err;
    return AdbcConnectionCancel(&connection, err.Ptr()) == ADBC_STATUS_OK;
  }
  return false;
}

void AdbcSession::ReleaseResourcesBlocking() noexcept {
  detail::AdbcReleaseNativeHandles(*this);
  redis.reset();
  request = ConnectionRequest{};
  alive.store(false, std::memory_order_release);
}

AdbcSession::~AdbcSession() {
  MarkClosed();
  ReleaseResourcesBlocking();
}

namespace {

AdbcOpenResult OpenAdbcBlockingSingle(const ConnectionRequest& request) {
  AdbcOpenResult result;
  auto session = std::make_shared<AdbcSession>();
  session->request = request;
  session->remark = request.remark;
  session->read_only = request.read_only;
  session->production_mode = request.production_mode;
  session->show_system_databases = request.show_system_databases;
  const Dialect dialect = detail::AdbcDetectDialect(request.driver_path, request.uri);
  if (dialect == Dialect::Redis) {
    const RedisParseUriResult parsed =
        ParseRedisUri(request.uri, request.username, request.password);
    if (!parsed.error.empty()) {
      result.error = parsed.error;
      return result;
    }
    auto client = std::make_unique<RedisClient>();
    std::string conn_err;
    if (!client->Connect(parsed.endpoint, conn_err)) {
      result.error = conn_err;
      return result;
    }
    session->dialect = Dialect::Redis;
    session->redis = std::move(client);
    session->initialized = true;
    session->connection_info = LoadConnectionInfoBlocking(*session);
    session->alive.store(true, std::memory_order_release);
    result.session = std::move(session);
    return result;
  }

  detail::AdbcErrorGuard error;
  const auto fail = [&](std::string_view operation) -> AdbcOpenResult {
    result.error = std::string(operation) + ": " + error.Take();
    result.session.reset();
    return result;
  };

  if (AdbcDatabaseNew(&session->database, error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("create ADBC database", "创建 ADBC database"));
  }
  session->database_created = true;

  if (AdbcDriverManagerDatabaseSetLoadFlags(&session->database, ADBC_LOAD_FLAG_DEFAULT,
                                            error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("set ADBC load flags", "设置 ADBC 加载标志"));
  }
  const std::string extra_paths = detail::AdbcBuildAdditionalSearchPathList();
  if (!extra_paths.empty() &&
      AdbcDriverManagerDatabaseSetAdditionalSearchPathList(
          &session->database, extra_paths.c_str(), error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("set ADBC search paths", "设置 ADBC 搜索路径"));
  }

  if (AdbcDatabaseSetOption(&session->database, "driver",
                            request.driver_path.c_str(), error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("set ADBC driver", "设置 ADBC driver"));
  }

  const std::string lower_uri = detail::AdbcLowerAscii(request.uri);
  const std::string lower_driver = detail::AdbcLowerAscii(request.driver_path);
  bool credentials_in_uri = false;
  std::string effective_uri = request.uri;
  if (dialect == Dialect::Postgresql) {
    effective_uri =
        BuildPostgresqlUriWithCredentials(request.uri, request.username, request.password);
    credentials_in_uri = true;
  } else if ((!request.username.empty() || !request.password.empty()) &&
             (lower_uri.starts_with("mysql://") || lower_uri.starts_with("mariadb://") ||
              lower_uri.starts_with("mssql://") || lower_uri.starts_with("sqlserver://") ||
              (lower_driver.find("clickhouse") != std::string::npos &&
               (lower_uri.starts_with("http://") || lower_uri.starts_with("https://"))))) {
    effective_uri =
        detail::AdbcInjectSchemeUriCredentials(request.uri, request.username, request.password);
    credentials_in_uri = true;
  }
  if (!effective_uri.empty() &&
      AdbcDatabaseSetOption(&session->database, ADBC_OPTION_URI, effective_uri.c_str(),
                            error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("set URI", "设置 URI"));
  }

  if (dialect == Dialect::Sqlite && !request.uri.empty() &&
      request.uri != ":memory:" &&
      request.uri.rfind("file:", 0) != 0 && request.uri.find("://") == std::string::npos) {
    std::error_code filesystem_error;
    const std::filesystem::path database_path(request.uri);
    if (database_path.has_parent_path()) {
      std::filesystem::create_directories(database_path.parent_path(), filesystem_error);
    }
  }
  if (!credentials_in_uri && dialect != Dialect::Sqlite) {
    if (!request.username.empty() &&
        AdbcDatabaseSetOption(&session->database, "username", request.username.c_str(),
                              error.Ptr()) != ADBC_STATUS_OK) {
      return fail(Tr("set username", "设置用户名"));
    }
    if (!request.password.empty() &&
        AdbcDatabaseSetOption(&session->database, "password", request.password.c_str(),
                              error.Ptr()) != ADBC_STATUS_OK) {
      return fail(Tr("set password", "设置密码"));
    }
  }
  if (AdbcDatabaseInit(&session->database, error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("initialize database", "初始化数据库"));
  }

  if (AdbcConnectionNew(&session->connection, error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("create connection", "创建连接"));
  }
  session->connection_created = true;
  bool tried_native_readonly = false;
  if (request.read_only && dialect != Dialect::Sqlite) {
    const AdbcStatusCode ro_status = AdbcConnectionSetOption(
        &session->connection, ADBC_CONNECTION_OPTION_READ_ONLY, ADBC_OPTION_VALUE_ENABLED,
        error.Ptr());
    tried_native_readonly = (ro_status == ADBC_STATUS_OK);
    error.Reset();
  }
  if (AdbcConnectionInit(&session->connection, &session->database, error.Ptr()) !=
      ADBC_STATUS_OK) {
    if (tried_native_readonly) {
      // Retry without adbc.connection.readonly if the driver validates options lazily at Init.
      error.Reset();
      AdbcConnectionRelease(&session->connection, error.Ptr());
      error.Reset();
      session->connection = {};
      session->connection_created = false;
      if (AdbcConnectionNew(&session->connection, error.Ptr()) != ADBC_STATUS_OK) {
        return fail(Tr("create connection", "创建连接"));
      }
      session->connection_created = true;
      if (AdbcConnectionInit(&session->connection, &session->database, error.Ptr()) !=
          ADBC_STATUS_OK) {
        return fail(Tr("initialize connection", "初始化连接"));
      }
    } else {
      return fail(Tr("initialize connection", "初始化连接"));
    }
  }

  session->dialect = dialect;
  session->initialized = true;
  if (request.read_only && dialect == Dialect::Sqlite) {
    (void)detail::AdbcExecuteBlockingRaw(*session, "PRAGMA query_only = ON", false, 0);
  }
  session->connection_info = LoadConnectionInfoBlocking(*session);
  session->alive.store(true, std::memory_order_release);
  result.session = std::move(session);
  return result;
}

} // namespace

AdbcOpenResult OpenAdbcBlocking(const ConnectionRequest& request) {
  AdbcOpenResult primary = OpenAdbcBlockingSingle(request);
  if (primary.session || request.fallback_uri.empty() ||
      request.fallback_uri == request.uri) {
    return primary;
  }
  ConnectionRequest fallback_req = request;
  fallback_req.uri = request.fallback_uri;
  fallback_req.fallback_uri.clear();
  AdbcOpenResult secondary = OpenAdbcBlockingSingle(fallback_req);
  if (secondary.session) {
    return secondary;
  }
  return primary;
}

bool ReconnectBlocking(AdbcSession& session, std::string& error_out) {
  error_out.clear();
  if (session.IsClosed()) {
    error_out = Tr("Connection is closed", "连接已关闭");
    return false;
  }
  if (session.dialect == Dialect::Redis && session.redis) {
    const bool ok = session.redis->Connect(session.redis->Endpoint(), error_out);
    session.alive.store(ok, std::memory_order_release);
    return ok;
  }
  if (session.dialect == Dialect::Sqlite || session.request.uri == ":memory:" ||
      (session.request.driver_path.empty() && session.request.uri.empty())) {
    return false;
  }

  AdbcOpenResult fresh = OpenAdbcBlocking(session.request);
  if (!fresh.session) {
    detail::AdbcReleaseNativeHandles(session);
    session.alive.store(false, std::memory_order_release);
    error_out = std::move(fresh.error);
    return false;
  }

  detail::AdbcReleaseNativeHandles(session);
  session.database = fresh.session->database;
  session.connection = fresh.session->connection;
  session.database_created = fresh.session->database_created;
  session.connection_created = fresh.session->connection_created;
  session.initialized = fresh.session->initialized;
  session.dialect = fresh.session->dialect;
  session.connection_info = std::move(fresh.session->connection_info);
  session.autocommit = true;
  session.in_transaction = false;
  fresh.session->database = {};
  fresh.session->connection = {};
  fresh.session->database_created = false;
  fresh.session->connection_created = false;
  fresh.session->initialized = false;
  session.alive.store(true, std::memory_order_release);
  return true;
}

AdbcObjectsResult LoadObjectsBlocking(AdbcSession& session) {
  if (session.IsClosed()) {
    AdbcObjectsResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (session.dialect == Dialect::Redis && session.redis) {
    AdbcObjectsResult result = LoadRedisObjectsBlocking(*session.redis);
    if (!result.error.empty() &&
        (!session.redis->IsConnected() || IsConnectionLostError(0, result.error))) {
      result.connection_lost = true;
      session.alive.store(false, std::memory_order_release);
    } else if (result.error.empty()) {
      session.alive.store(true, std::memory_order_release);
    }
    return result;
  }

  AdbcStatusCode status = ADBC_STATUS_OK;
  AdbcObjectsResult result = detail::AdbcLoadObjectsBlockingOnce(session, status);
  if (!result.error.empty() &&
      (!session.connection_created ||
       IsConnectionLostError(status, result.error, result.sqlstate))) {
    std::string reconnect_err;
    if (ReconnectBlocking(session, reconnect_err)) {
      status = ADBC_STATUS_OK;
      result = detail::AdbcLoadObjectsBlockingOnce(session, status);
    } else if (!reconnect_err.empty()) {
      result.error = std::move(reconnect_err);
    }
    if (!result.error.empty() &&
        (!session.connection_created ||
         IsConnectionLostError(status, result.error, result.sqlstate) ||
         !session.IsAlive())) {
      result.connection_lost = true;
      session.alive.store(false, std::memory_order_release);
      return result;
    }
  }
  if (result.error.empty()) {
    session.alive.store(true, std::memory_order_release);
    detail::AdbcEnrichCatalogObjects(session, result);
  }
  return result;
}

AdbcSchemaResult LoadSchemaBlocking(AdbcSession& session, const DbObject& object) {
  if (session.IsClosed()) {
    AdbcSchemaResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (session.dialect == Dialect::Redis) {
    AdbcSchemaResult result;
    result.columns = RedisKeySchema();
    return result;
  }

  AdbcStatusCode status = ADBC_STATUS_OK;
  AdbcSchemaResult result = detail::AdbcLoadSchemaBlockingOnce(session, object, status);
  if (!result.error.empty() &&
      (!session.connection_created ||
       IsConnectionLostError(status, result.error, result.sqlstate))) {
    std::string reconnect_err;
    if (ReconnectBlocking(session, reconnect_err)) {
      status = ADBC_STATUS_OK;
      result = detail::AdbcLoadSchemaBlockingOnce(session, object, status);
    } else if (!reconnect_err.empty()) {
      result.error = std::move(reconnect_err);
    }
    if (!result.error.empty() &&
        (!session.connection_created ||
         IsConnectionLostError(status, result.error, result.sqlstate) ||
         !session.IsAlive())) {
      result.connection_lost = true;
      session.alive.store(false, std::memory_order_release);
      return result;
    }
  }
  if (result.error.empty()) {
    session.alive.store(true, std::memory_order_release);
  }
  return result;
}

namespace detail {

AdbcQueryResult AdbcExecuteBlockingRaw(AdbcSession& session, std::string sql, bool want_rows,
                                       std::size_t max_rows) {
  if (session.IsClosed()) {
    AdbcQueryResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  const auto started = std::chrono::steady_clock::now();
  const auto elapsed_micros = [started]() -> std::int64_t {
    return std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - started)
               .count());
  };
  if (session.dialect == Dialect::Redis && session.redis) {
    AdbcQueryResult result = ExecuteRedisCommandBlocking(*session.redis, sql, max_rows);
    result.elapsed_us = elapsed_micros();
    if (!result.error.empty() &&
        (!session.redis->IsConnected() || IsConnectionLostError(0, result.error))) {
      result.connection_lost = true;
      session.alive.store(false, std::memory_order_release);
    } else if (result.error.empty()) {
      session.alive.store(true, std::memory_order_release);
    }
    return result;
  }

  AdbcStatusCode status = ADBC_STATUS_OK;
  bool before_execute = false;
  AdbcQueryResult result =
      ExecuteBlockingOnce(session, sql, want_rows, max_rows, status, before_execute);
  if (!result.error.empty() && !result.cancelled &&
      (!session.connection_created ||
       IsConnectionLostError(status, result.error, result.sqlstate))) {
    if (before_execute || IsSqlReadStatement(sql)) {
      std::string reconnect_err;
      if (ReconnectBlocking(session, reconnect_err)) {
        status = ADBC_STATUS_OK;
        result = ExecuteBlockingOnce(session, sql, want_rows, max_rows, status, before_execute);
      } else if (!reconnect_err.empty()) {
        result.error = std::move(reconnect_err);
      }
    }
    if (!result.error.empty() &&
        (!session.connection_created ||
         IsConnectionLostError(status, result.error, result.sqlstate) ||
         !session.IsAlive())) {
      result.connection_lost = true;
      result.elapsed_us = elapsed_micros();
      session.alive.store(false, std::memory_order_release);
      return result;
    }
  }
  if (result.error.empty()) {
    session.alive.store(true, std::memory_order_release);
  }
  result.elapsed_us = elapsed_micros();
  return result;
}

} // namespace detail

AdbcQueryResult ExecuteBlocking(AdbcSession& session, std::string sql, bool want_rows,
                                std::size_t max_rows) {
  if (session.IsClosed()) {
    AdbcQueryResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (session.read_only && !IsPureReadQuery(session.dialect, sql)) {
    AdbcQueryResult result;
    result.error = ReadOnlyWriteBlockedMessage();
    return result;
  }
  return detail::AdbcExecuteBlockingRaw(session, std::move(sql), want_rows, max_rows);
}

AdbcQueryResult ExecuteSwitchDatabaseSqlBlocking(AdbcSession& session, std::string sql) {
  return detail::AdbcExecuteBlockingRaw(session, std::move(sql), false, 0);
}

bool SetAutocommitBlocking(AdbcSession& session, bool enabled, std::string& error_out) {
  error_out.clear();
  if (session.IsClosed() || !session.connection_created) {
    error_out = Tr("Connection is closed", "连接已关闭");
    return false;
  }
  if (session.dialect == Dialect::Redis) {
    error_out = Tr("Transactions are not supported on Redis", "Redis 不支持 ADBC 事务");
    return false;
  }
  detail::AdbcErrorGuard err;
  const AdbcStatusCode st = AdbcConnectionSetOption(
      &session.connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
      enabled ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED, err.Ptr());
  if (st == ADBC_STATUS_OK) {
    session.autocommit = enabled;
    session.in_transaction = !enabled;
    return true;
  }
  if (st == ADBC_STATUS_NOT_IMPLEMENTED) {
    const char* sql = enabled ? "COMMIT" : "BEGIN";
    const AdbcQueryResult res = ExecuteBlocking(session, sql, false, 0);
    if (!res.error.empty()) {
      error_out = res.error;
      return false;
    }
    session.autocommit = enabled;
    session.in_transaction = !enabled;
    return true;
  }
  error_out = err.Take();
  return false;
}

bool CommitTransactionBlocking(AdbcSession& session, std::string& error_out) {
  error_out.clear();
  if (session.IsClosed() || !session.connection_created) {
    error_out = Tr("Connection is closed", "连接已关闭");
    return false;
  }
  detail::AdbcErrorGuard err;
  const AdbcStatusCode st = AdbcConnectionCommit(&session.connection, err.Ptr());
  if (st == ADBC_STATUS_OK) {
    return true;
  }
  if (st == ADBC_STATUS_NOT_IMPLEMENTED) {
    const AdbcQueryResult res = ExecuteBlocking(session, "COMMIT", false, 0);
    if (!res.error.empty()) {
      error_out = res.error;
      return false;
    }
    return true;
  }
  error_out = err.Take();
  return false;
}

bool RollbackTransactionBlocking(AdbcSession& session, std::string& error_out) {
  error_out.clear();
  if (session.IsClosed() || !session.connection_created) {
    error_out = Tr("Connection is closed", "连接已关闭");
    return false;
  }
  detail::AdbcErrorGuard err;
  const AdbcStatusCode st = AdbcConnectionRollback(&session.connection, err.Ptr());
  if (st == ADBC_STATUS_OK) {
    return true;
  }
  if (st == ADBC_STATUS_NOT_IMPLEMENTED) {
    const AdbcQueryResult res = ExecuteBlocking(session, "ROLLBACK", false, 0);
    if (!res.error.empty()) {
      error_out = res.error;
      return false;
    }
    return true;
  }
  error_out = err.Take();
  return false;
}

bool SetIsolationLevelBlocking(AdbcSession& session, std::string_view level,
                               std::string& error_out) {
  error_out.clear();
  if (session.IsClosed() || !session.connection_created) {
    error_out = Tr("Connection is closed", "连接已关闭");
    return false;
  }
  const std::string level_str(level);
  detail::AdbcErrorGuard err;
  const AdbcStatusCode st = AdbcConnectionSetOption(
      &session.connection, ADBC_CONNECTION_OPTION_ISOLATION_LEVEL, level_str.c_str(), err.Ptr());
  if (st != ADBC_STATUS_OK) {
    error_out = err.Take();
    return false;
  }
  return true;
}

} // namespace dbpp
