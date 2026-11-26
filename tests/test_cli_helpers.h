#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dbpp_test {

struct CliRunOutput {
  int code = -1;
  std::string out;
  std::string err;
};

void Expect(bool condition, std::string_view message);
CliRunOutput InvokeCli(const std::vector<std::string>& args, std::string_view stdin_text = {});
std::string PathToFileUrl(const std::filesystem::path& path);
std::string ReadBinaryFile(const std::filesystem::path& path);
void WriteBinaryFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);
void WriteTextFile(const std::filesystem::path& path, std::string_view text);
std::vector<std::uint8_t> BuildTarGz(
    const std::vector<std::pair<std::string, std::string>>& entries);
void RunCliCommandTests(const std::filesystem::path& dir, const std::string& driver_path);

} // namespace dbpp_test
