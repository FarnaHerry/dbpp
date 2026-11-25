#pragma once

#include "model.h"

#include <arrow-adbc/adbc.h>

#include <huxerui/task.h>

#include <memory>

namespace dbpp {

class RedisClient;

/// Owns one ADBC database/connection or a native Redis client. All calls for
/// this session are serialized through `sequence`, which is the only supported
/// access path.
struct AdbcSession {
  AdbcSession() = default;
  AdbcSession(const AdbcSession&) = delete;
  AdbcSession& operator=(const AdbcSession&) = delete;
  ~AdbcSession();

  AdbcDatabase database{};
  AdbcConnection connection{};
  std::unique_ptr<RedisClient> redis;
  huxerui::WorkerSequence sequence;
  Dialect dialect = Dialect::Generic;
  bool database_created = false;
  bool connection_created = false;
  bool initialized = false;
};

/// Driver path of the SQLite ADBC driver built next to this application.
[[nodiscard]] std::string DefaultSqliteDriverPath();

} // namespace dbpp
