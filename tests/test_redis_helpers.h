#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace dbpp_test {

class MockRedisServer {
public:
  explicit MockRedisServer(std::string require_password = {}, std::string require_user = {});
  ~MockRedisServer();

  MockRedisServer(const MockRedisServer&) = delete;
  MockRedisServer& operator=(const MockRedisServer&) = delete;

  [[nodiscard]] std::uint16_t Port() const;
  void SetLegacyAuthOnly(bool value);
  void SetDisableInfo(bool value);
  void SetDropOnConfig(bool value);
  void SetScanStepSize(std::size_t step);
  void SetDisableScanType(bool value);
  [[nodiscard]] std::size_t CommandCount(std::string_view cmd) const;
  void ResetCommandCounts();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace dbpp_test
