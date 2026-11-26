#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace dbpp_test {

void Expect(bool condition, std::string_view message);
void RunAdbcCoreExtraTests(const std::filesystem::path& dir, const std::string& driver_path);
void RunAdbcCapabilitiesTests(const std::filesystem::path& dir, const std::string& driver_path);

} // namespace dbpp_test
