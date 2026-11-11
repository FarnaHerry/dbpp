#pragma once

#include "model.h"

#include <arrow-adbc/adbc.h>

#include <huxerui/task.h>

namespace dbpp {

/// Owns one ADBC database and connection. All ADBC calls for this session are
/// serialized through `sequence`, which is the only supported access path.
struct AdbcSession {
  AdbcSession() = default;
  AdbcSession(const AdbcSession&) = delete;
  AdbcSession& operator=(const AdbcSession&) = delete;
  ~AdbcSession();

  AdbcDatabase database{};
  AdbcConnection connection{};
  huxerui::WorkerSequence sequence;
  Dialect dialect = Dialect::Generic;
  bool database_created = false;
  bool connection_created = false;
  bool initialized = false;
};

/// Driver path of the SQLite ADBC driver built next to this application.
[[nodiscard]] std::string DefaultSqliteDriverPath();

} // namespace dbpp
