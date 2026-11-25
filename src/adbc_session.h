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

/// Result of extracting embedded credentials from a URI into dedicated fields
/// so URI display/copy stays clean while preserving credentials separately.
struct SanitizedUriResult {
  std::string uri;
  std::string username;
  std::string password;
  bool stripped_password = false;
};

/// Strips any embedded credentials from a `scheme://userinfo@host...` URI,
/// returning the sanitized URI plus extracted username/password.
[[nodiscard]] SanitizedUriResult SanitizeUriCredentials(std::string_view uri);

/// Driver path of the SQLite ADBC driver built next to this application.
[[nodiscard]] std::string DefaultSqliteDriverPath();

} // namespace dbpp
