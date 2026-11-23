#include "driver_installer.h"
#include "model.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace dbpp {

namespace {

std::atomic<std::uint64_t> g_temp_counter{1};

std::string NextUniqueSuffix() {
  const std::uint64_t id = g_temp_counter.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::to_string(now) + "-" + std::to_string(id);
}

std::string LowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

std::string_view TrimAscii(std::string_view text) {
  while (!text.empty() &&
         (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
          text.front() == '\n')) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
          text.back() == '\n')) {
    text.remove_suffix(1);
  }
  return text;
}

std::string UnquoteScalar(std::string_view text) {
  text = TrimAscii(text);
  if (text.size() >= 2) {
    const char first = text.front();
    const char last = text.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      text.remove_prefix(1);
      text.remove_suffix(1);
    }
  }
  return std::string(text);
}

std::string NormalizeSha256(std::string_view text) {
  std::string cleaned = UnquoteScalar(text);
  const std::string lower = LowerAscii(cleaned);
  if (lower.starts_with("sha256:")) {
    cleaned = UnquoteScalar(std::string_view(cleaned).substr(7));
  }
  if (cleaned.size() != 64) {
    return {};
  }
  for (char& c : cleaned) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (!std::isxdigit(uc)) {
      return {};
    }
    c = static_cast<char>(std::tolower(uc));
  }
  return cleaned;
}

bool SplitKeyValue(std::string_view text, std::string_view& key, std::string_view& value) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  if (colon + 1 < text.size() && text[colon + 1] != ' ' && text[colon + 1] != '\t') {
    return false;
  }
  key = TrimAscii(text.substr(0, colon));
  value = colon + 1 < text.size() ? TrimAscii(text.substr(colon + 1)) : std::string_view{};
  return true;
}

std::string ResolvePackageUrl(std::string_view base_url, std::string_view url) {
  if (url.empty()) {
    return {};
  }
  const std::string lower = LowerAscii(url);
  if (lower.starts_with("http://") || lower.starts_with("https://")) {
    return std::string(url);
  }
  std::string_view base = base_url;
  while (base.ends_with('/')) {
    base.remove_suffix(1);
  }
  std::string_view rel = url;
  while (rel.starts_with('/')) {
    rel.remove_prefix(1);
  }
  if (base.empty()) {
    return std::string(rel);
  }
  return std::string(base) + "/" + std::string(rel);
}

struct SemVer {
  int major = 0;
  int minor = 0;
  int patch = 0;
  bool has_prerelease = false;
  std::string prerelease;
  bool valid = false;
};

SemVer ParseSemVer(std::string_view text) {
  SemVer ver;
  text = TrimAscii(text);
  if (text.empty()) {
    return ver;
  }
  if (text.front() == 'v' || text.front() == 'V') {
    text.remove_prefix(1);
  }
  if (const std::size_t plus = text.find('+'); plus != std::string_view::npos) {
    text = text.substr(0, plus);
  }
  if (const std::size_t dash = text.find('-'); dash != std::string_view::npos) {
    ver.has_prerelease = true;
    ver.prerelease = std::string(text.substr(dash + 1));
    text = text.substr(0, dash);
  }

  auto parse_part = [](std::string_view part, int& out) -> bool {
    if (part.empty()) {
      return false;
    }
    const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), out);
    return ec == std::errc{} && end == part.data() + part.size() && out >= 0;
  };

  const std::size_t dot1 = text.find('.');
  if (dot1 == std::string_view::npos) {
    ver.valid = parse_part(text, ver.major);
    return ver;
  }
  if (!parse_part(text.substr(0, dot1), ver.major)) {
    return ver;
  }
  const std::string_view rest = text.substr(dot1 + 1);
  const std::size_t dot2 = rest.find('.');
  if (dot2 == std::string_view::npos) {
    ver.valid = parse_part(rest, ver.minor);
    return ver;
  }
  if (!parse_part(rest.substr(0, dot2), ver.minor)) {
    return ver;
  }
  ver.valid = parse_part(rest.substr(dot2 + 1), ver.patch);
  return ver;
}

std::vector<std::string_view> SplitDots(std::string_view text) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '.') {
      parts.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  return parts;
}

int ComparePrerelease(std::string_view left, std::string_view right) {
  const auto a = SplitDots(left);
  const auto b = SplitDots(right);
  const std::size_t count = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (a[i] == b[i]) {
      continue;
    }
    std::uint64_t na = 0;
    std::uint64_t nb = 0;
    const auto ra = std::from_chars(a[i].data(), a[i].data() + a[i].size(), na);
    const auto rb = std::from_chars(b[i].data(), b[i].data() + b[i].size(), nb);
    const bool a_num = ra.ec == std::errc{} && ra.ptr == a[i].data() + a[i].size();
    const bool b_num = rb.ec == std::errc{} && rb.ptr == b[i].data() + b[i].size();
    if (a_num && b_num) {
      return na < nb ? -1 : 1;
    }
    if (a_num != b_num) {
      return a_num ? -1 : 1;
    }
    return a[i] < b[i] ? -1 : 1;
  }
  if (a.size() != b.size()) {
    return a.size() < b.size() ? -1 : 1;
  }
  return 0;
}

int CompareSemVer(const SemVer& left, const SemVer& right) {
  if (left.valid != right.valid) {
    return left.valid ? 1 : -1;
  }
  if (!left.valid && !right.valid) {
    return 0;
  }
  // Prefer stable releases over prereleases when selecting default packages.
  if (left.has_prerelease != right.has_prerelease) {
    return left.has_prerelease ? -1 : 1;
  }
  if (left.major != right.major) {
    return left.major < right.major ? -1 : 1;
  }
  if (left.minor != right.minor) {
    return left.minor < right.minor ? -1 : 1;
  }
  if (left.patch != right.patch) {
    return left.patch < right.patch ? -1 : 1;
  }
  if (left.has_prerelease && right.has_prerelease) {
    return ComparePrerelease(left.prerelease, right.prerelease);
  }
  return 0;
}

int CoreDriverRank(std::string_view slug) {
  if (slug == "postgresql") return 0;
  if (slug == "mysql") return 1;
  if (slug == "mariadb") return 2;
  if (slug == "duckdb") return 3;
  if (slug == "mssql") return 4;
  if (slug == "clickhouse") return 5;
  if (slug == "flightsql") return 6;
  if (slug == "sqlite") return 7;
  return 8;
}

class Sha256Hasher {
public:
  void Update(const unsigned char* data, std::size_t len) {
    total_bits_ += static_cast<std::uint64_t>(len) * 8ULL;
    while (len > 0) {
      const std::size_t take = std::min(len, 64 - buf_len_);
      std::memcpy(buf_.data() + buf_len_, data, take);
      buf_len_ += take;
      data += take;
      len -= take;
      if (buf_len_ == 64) {
        Transform(buf_.data());
        buf_len_ = 0;
      }
    }
  }

  std::string FinalHex() {
    buf_[buf_len_++] = 0x80;
    if (buf_len_ > 56) {
      while (buf_len_ < 64) {
        buf_[buf_len_++] = 0x00;
      }
      Transform(buf_.data());
      buf_len_ = 0;
    }
    while (buf_len_ < 56) {
      buf_[buf_len_++] = 0x00;
    }
    for (int i = 7; i >= 0; --i) {
      buf_[56 + (7 - i)] = static_cast<unsigned char>((total_bits_ >> (i * 8)) & 0xFFU);
    }
    Transform(buf_.data());

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const std::uint32_t word : state_) {
      for (int shift = 28; shift >= 0; shift -= 4) {
        hex.push_back(kHex[(word >> shift) & 0xFU]);
      }
    }
    return hex;
  }

private:
  static constexpr std::uint32_t RotR(std::uint32_t v, int bits) {
    return (v >> bits) | (v << (32 - bits));
  }

  void Transform(const unsigned char block[64]) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
        0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
        0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
        0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };

    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
             (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
      const std::uint32_t s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
      const std::uint32_t S1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
      const std::uint32_t ch = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 = h + S1 + ch + k[i] + w[i];
      const std::uint32_t S0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = S0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::array<unsigned char, 64> buf_{};
  std::size_t buf_len_ = 0;
  std::uint64_t total_bits_ = 0;
};

struct ProcessResult {
  int exit_code = -1;
  std::string output;
  std::string spawn_error;
};

ProcessResult RunProcess(const std::vector<std::string>& args) {
  ProcessResult result;
  if (args.empty()) {
    result.spawn_error = "空命令";
    return result;
  }

#if defined(_WIN32)
  auto quote_arg = [](std::wstring_view arg) -> std::wstring {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
      return std::wstring(arg);
    }
    std::wstring quoted = L"\"";
    for (std::size_t i = 0; i < arg.size(); ++i) {
      std::size_t backslashes = 0;
      while (i < arg.size() && arg[i] == L'\\') {
        ++backslashes;
        ++i;
      }
      if (i == arg.size()) {
        quoted.append(backslashes * 2, L'\\');
        break;
      }
      if (arg[i] == L'"') {
        quoted.append(backslashes * 2 + 1, L'\\');
        quoted.push_back(L'"');
      } else {
        quoted.append(backslashes, L'\\');
        quoted.push_back(arg[i]);
      }
    }
    quoted.push_back(L'"');
    return quoted;
  };

  auto utf8_to_wide = [](std::string_view text) -> std::wstring {
    if (text.empty()) {
      return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
      return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        needed);
    return wide;
  };

  std::wstring cmdline;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i > 0) {
      cmdline.push_back(L' ');
    }
    cmdline += quote_arg(utf8_to_wide(args[i]));
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
    result.spawn_error = "无法创建管道";
    return result;
  }
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi{};
  const BOOL created = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(write_pipe);
  if (!created) {
    CloseHandle(read_pipe);
    result.spawn_error = "无法启动进程: " + args.front();
    return result;
  }

  char buf[4096];
  DWORD bytes_read = 0;
  while (ReadFile(read_pipe, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
    result.output.append(buf, bytes_read);
  }
  CloseHandle(read_pipe);

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  result.exit_code = static_cast<int>(code);
  return result;
#else
  int pipefd[2]{-1, -1};
  if (pipe(pipefd) != 0) {
    result.spawn_error = "创建管道失败";
    return result;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    result.spawn_error = "fork 进程失败";
    return result;
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    if (pipefd[1] != STDOUT_FILENO && pipefd[1] != STDERR_FILENO) {
      close(pipefd[1]);
    }
    std::vector<std::string> mutable_args = args;
    std::vector<char*> argv;
    argv.reserve(mutable_args.size() + 1);
    for (std::string& arg : mutable_args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }

  close(pipefd[1]);
  char buf[4096];
  for (;;) {
    const ssize_t n = read(pipefd[0], buf, sizeof(buf));
    if (n > 0) {
      result.output.append(buf, static_cast<std::size_t>(n));
    } else if (n == 0) {
      break;
    } else if (errno != EINTR) {
      break;
    }
  }
  close(pipefd[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else {
    result.exit_code = -1;
  }
  return result;
#endif
}

bool IsValidSlug(std::string_view slug) {
  if (slug.empty() || slug == "profile") {
    return false;
  }
  for (const char c : slug) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool IsSafeRelativeArchivePath(std::string_view entry) {
  if (entry.empty()) {
    return false;
  }
  if (entry.front() == '/' || entry.front() == '\\') {
    return false;
  }
  if (entry.find(':') != std::string_view::npos || entry.find('\0') != std::string_view::npos) {
    return false;
  }
  std::size_t start = 0;
  for (std::size_t i = 0; i <= entry.size(); ++i) {
    if (i == entry.size() || entry[i] == '/' || entry[i] == '\\') {
      const std::string_view part = entry.substr(start, i - start);
      if (part == "..") {
        return false;
      }
      start = i + 1;
    }
  }
  return true;
}

bool IsPathWithin(const std::filesystem::path& base_canonical,
                  const std::filesystem::path& target_canonical) {
  auto base_it = base_canonical.begin();
  auto target_it = target_canonical.begin();
  for (; base_it != base_canonical.end(); ++base_it, ++target_it) {
    if (target_it == target_canonical.end() || *base_it != *target_it) {
      return false;
    }
  }
  return true;
}

std::string SanitizeVersionDirName(std::string_view version) {
  std::string out;
  out.reserve(version.size());
  for (const char c : version) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-';
    out.push_back(ok ? c : '_');
  }
  if (out.empty() || out == "." || out == "..") {
    return "latest";
  }
  return out;
}

struct ScopedDirectoryRemoval {
  std::filesystem::path path;
  ~ScopedDirectoryRemoval() {
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  }
};

struct ScopedFileRemoval {
  std::filesystem::path path;
  ~ScopedFileRemoval() {
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }
};

std::filesystem::path ResolveUserDriversDir(const std::filesystem::path& override_dir) {
  if (!override_dir.empty()) {
    return override_dir;
  }
  const std::string dir = UserAdbcDriversDir();
  return dir.empty() ? std::filesystem::path{} : std::filesystem::path(dir);
}

std::string FormatProgressText(std::string_view slug, std::uint64_t received,
                               std::optional<std::uint64_t> total) {
  auto to_mb = [](std::uint64_t bytes) -> std::string {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2)
        << (static_cast<double>(bytes) / (1024.0 * 1024.0));
    return oss.str();
  };
  if (total.has_value() && *total > 0) {
    const double ratio =
        std::clamp(static_cast<double>(received) / static_cast<double>(*total), 0.0, 1.0);
    const int percent = static_cast<int>(ratio * 100.0);
    return "正在下载 " + std::string(slug) + ": " + to_mb(received) + " / " +
           to_mb(*total) + " MB (" + std::to_string(percent) + "%)";
  }
  return "正在下载 " + std::string(slug) + ": " + to_mb(received) + " MB";
}

} // namespace

std::string CurrentPlatformTuple() {
#if defined(_WIN32)
  const std::string os = "windows";
#elif defined(__APPLE__)
  const std::string os = "macos";
#elif defined(__linux__)
  const std::string os = "linux";
#else
  const std::string os = "unknown";
#endif

#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64) || defined(_M_AMD64)
  const std::string arch = "amd64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  const std::string arch = "arm64";
#elif defined(__i386__) || defined(_M_IX86)
  const std::string arch = "x86";
#else
  const std::string arch = "unknown";
#endif
  return os + "_" + arch;
}

std::string AdbcRegistryBaseUrl() {
  std::string url = "https://dbc-cdn.columnar.tech";
  if (const char* override_url = std::getenv("DBPP_ADBC_REGISTRY_URL");
      override_url != nullptr && *override_url != '\0') {
    url = override_url;
  }
  while (url.size() > 1 && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

std::vector<RegistryDriverInfo> ParseRegistryIndexYaml(std::string_view yaml_text,
                                                       std::string_view base_url,
                                                       std::string_view platform_tuple) {
  struct RawPackage {
    std::string platform;
    std::string url;
    std::string sha256;
  };
  struct RawPkgInfo {
    std::string version;
    std::vector<RawPackage> packages;
  };
  struct RawDriver {
    std::string name;
    std::string description;
    std::string license;
    std::string slug;
    std::vector<RawPkgInfo> pkginfo;
  };

  enum class PendingPkgField { None, Url, Hash };

  std::vector<RegistryDriverInfo> result;
  RawDriver cur_driver;
  RawPkgInfo cur_pkginfo;
  RawPackage cur_pkg;
  bool has_driver = false;
  bool has_pkginfo = false;
  bool has_pkg = false;
  bool in_description = false;
  PendingPkgField pending_pkg_field = PendingPkgField::None;

  const auto flush_pkg = [&]() {
    if (has_pkg) {
      if (!cur_pkg.platform.empty() && !cur_pkg.url.empty()) {
        cur_pkginfo.packages.push_back(std::move(cur_pkg));
      }
      cur_pkg = RawPackage{};
      has_pkg = false;
    }
    pending_pkg_field = PendingPkgField::None;
  };

  const auto flush_pkginfo = [&]() {
    flush_pkg();
    if (has_pkginfo) {
      if (!cur_pkginfo.packages.empty()) {
        cur_driver.pkginfo.push_back(std::move(cur_pkginfo));
      }
      cur_pkginfo = RawPkgInfo{};
      has_pkginfo = false;
    }
  };

  const auto flush_driver = [&]() {
    flush_pkginfo();
    if (!has_driver) {
      return;
    }
    if (!cur_driver.slug.empty()) {
      bool found = false;
      SemVer best_ver{};
      std::string best_version_str;
      std::string best_url;
      std::string best_sha256;

      for (const RawPkgInfo& info : cur_driver.pkginfo) {
        for (const RawPackage& pkg : info.packages) {
          if (pkg.platform != platform_tuple || pkg.url.empty()) {
            continue;
          }
          const SemVer candidate = ParseSemVer(info.version);
          if (!found || CompareSemVer(candidate, best_ver) > 0) {
            found = true;
            best_ver = candidate;
            best_version_str = info.version;
            best_url = pkg.url;
            best_sha256 = pkg.sha256;
          }
        }
      }

      if (found) {
        result.push_back(RegistryDriverInfo{
            .slug = cur_driver.slug,
            .name = cur_driver.name.empty() ? cur_driver.slug : cur_driver.name,
            .description = cur_driver.description,
            .license = cur_driver.license,
            .version = best_version_str,
            .package_url = ResolvePackageUrl(base_url, best_url),
            .sha256 = best_sha256,
        });
      }
    }
    cur_driver = RawDriver{};
    has_driver = false;
    in_description = false;
  };

  std::size_t line_start = 0;
  while (line_start <= yaml_text.size()) {
    const std::size_t line_end = yaml_text.find('\n', line_start);
    std::string_view line = line_end == std::string_view::npos
                                ? yaml_text.substr(line_start)
                                : yaml_text.substr(line_start, line_end - line_start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    std::size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') {
      ++indent;
    }
    const std::string_view trimmed = TrimAscii(line.substr(indent));
    if (!trimmed.empty() && trimmed.front() != '#') {
      if (indent == 0) {
        if (trimmed.starts_with("- ")) {
          const std::string_view item = TrimAscii(trimmed.substr(2));
          std::string_view key;
          std::string_view val;
          if (SplitKeyValue(item, key, val) && key == "name") {
            flush_driver();
            has_driver = true;
            cur_driver.name = UnquoteScalar(val);
          } else {
            flush_driver();
          }
        } else {
          flush_driver();
        }
      } else if (has_driver) {
        if (indent == 2) {
          in_description = false;
          pending_pkg_field = PendingPkgField::None;
          if (trimmed.starts_with("- ")) {
            const std::string_view item = TrimAscii(trimmed.substr(2));
            std::string_view key;
            std::string_view val;
            if (SplitKeyValue(item, key, val) && key == "packages") {
              flush_pkginfo();
              has_pkginfo = true;
            }
          } else {
            std::string_view key;
            std::string_view val;
            if (SplitKeyValue(trimmed, key, val)) {
              if (key == "description") {
                cur_driver.description = UnquoteScalar(val);
                in_description = true;
              } else if (key == "license") {
                cur_driver.license = UnquoteScalar(val);
              } else if (key == "path") {
                cur_driver.slug = UnquoteScalar(val);
              }
            }
          }
        } else if (indent == 4) {
          pending_pkg_field = PendingPkgField::None;
          if (in_description) {
            if (!cur_driver.description.empty()) {
              cur_driver.description.push_back(' ');
            }
            cur_driver.description += UnquoteScalar(trimmed);
          } else if (has_pkginfo) {
            if (trimmed.starts_with("- ")) {
              const std::string_view item = TrimAscii(trimmed.substr(2));
              std::string_view key;
              std::string_view val;
              if (SplitKeyValue(item, key, val) && key == "platform") {
                flush_pkg();
                has_pkg = true;
                cur_pkg.platform = UnquoteScalar(val);
              }
            } else {
              std::string_view key;
              std::string_view val;
              if (SplitKeyValue(trimmed, key, val) && key == "version") {
                cur_pkginfo.version = UnquoteScalar(val);
              }
            }
          }
        } else if (indent == 6) {
          if (in_description) {
            if (!cur_driver.description.empty()) {
              cur_driver.description.push_back(' ');
            }
            cur_driver.description += UnquoteScalar(trimmed);
          } else if (has_pkg) {
            pending_pkg_field = PendingPkgField::None;
            std::string_view key;
            std::string_view val;
            if (SplitKeyValue(trimmed, key, val)) {
              if (key == "url") {
                if (!val.empty()) {
                  cur_pkg.url = UnquoteScalar(val);
                } else {
                  pending_pkg_field = PendingPkgField::Url;
                }
              } else if (key == "hash") {
                if (!val.empty()) {
                  cur_pkg.sha256 = NormalizeSha256(val);
                } else {
                  pending_pkg_field = PendingPkgField::Hash;
                }
              }
            }
          }
        } else if (indent >= 8 && has_pkg) {
          if (pending_pkg_field == PendingPkgField::Url) {
            cur_pkg.url = UnquoteScalar(trimmed);
            pending_pkg_field = PendingPkgField::None;
          } else if (pending_pkg_field == PendingPkgField::Hash) {
            cur_pkg.sha256 = NormalizeSha256(trimmed);
            pending_pkg_field = PendingPkgField::None;
          }
        }
      }
    }

    if (line_end == std::string_view::npos) {
      break;
    }
    line_start = line_end + 1;
  }

  flush_driver();

  std::ranges::sort(result, [](const RegistryDriverInfo& left, const RegistryDriverInfo& right) {
    const int left_rank = CoreDriverRank(left.slug);
    const int right_rank = CoreDriverRank(right.slug);
    if (left_rank != right_rank) {
      return left_rank < right_rank;
    }
    return left.slug < right.slug;
  });
  return result;
}

std::string Sha256HexFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  Sha256Hasher hasher;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      hasher.Update(reinterpret_cast<const unsigned char*>(buffer.data()),
                    static_cast<std::size_t>(count));
    }
  }
  if (input.bad()) {
    return {};
  }
  return hasher.FinalHex();
}

bool IsUserInstalledAdbcDriver(std::string_view slug,
                               const std::filesystem::path& user_drivers_dir) {
  if (!IsValidSlug(slug)) {
    return false;
  }
  const std::filesystem::path dir = ResolveUserDriversDir(user_drivers_dir);
  if (dir.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::is_regular_file(dir / (std::string(slug) + ".toml"), ec);
}

DriverInstallResult InstallDriverArchiveBlocking(
    const RegistryDriverInfo& driver, const std::filesystem::path& archive_path,
    const std::filesystem::path& user_drivers_dir) {
  DriverInstallResult result;
  result.slug = driver.slug;

  if (!IsValidSlug(driver.slug)) {
    result.message = "非法的驱动标识: " + driver.slug;
    return result;
  }

  const std::filesystem::path drivers_dir = ResolveUserDriversDir(user_drivers_dir);
  if (drivers_dir.empty()) {
    result.message = "无法确定用户 ADBC 驱动目录";
    return result;
  }

  std::error_code ec;
  if (!std::filesystem::is_regular_file(archive_path, ec)) {
    result.message = "驱动安装包不存在: " + archive_path.string();
    return result;
  }

  if (!driver.sha256.empty()) {
    const std::string expected = NormalizeSha256(driver.sha256);
    const std::string actual = Sha256HexFile(archive_path);
    if (expected.empty() || actual.empty() || actual != expected) {
      result.message = "SHA-256 校验失败: 预期 " + expected + "，实际 " +
                       (actual.empty() ? "<读取失败>" : actual);
      return result;
    }
  }

#if defined(_WIN32)
  const std::string tar_exe = "tar.exe";
#else
  const std::string tar_exe = "tar";
#endif

  // Inspect archive member paths before extracting to prevent path traversal.
  const ProcessResult listing = RunProcess({tar_exe, "-tzf", archive_path.string()});
  if (!listing.spawn_error.empty()) {
    result.message = "无法执行 tar: " + listing.spawn_error;
    return result;
  }
  if (listing.exit_code != 0) {
    result.message = "无法读取压缩包目录: " + listing.output;
    return result;
  }

  std::size_t entry_count = 0;
  std::istringstream entry_stream(listing.output);
  std::string entry;
  while (std::getline(entry_stream, entry)) {
    if (!entry.empty() && entry.back() == '\r') {
      entry.pop_back();
    }
    if (entry.empty()) {
      continue;
    }
    if (!IsSafeRelativeArchivePath(entry)) {
      result.message = "压缩包包含不安全路径: " + entry;
      return result;
    }
    ++entry_count;
  }
  if (entry_count == 0) {
    result.message = "驱动压缩包为空";
    return result;
  }

  const std::filesystem::path staging_root = drivers_dir / ".staging";
  std::filesystem::create_directories(staging_root, ec);
  if (ec) {
    result.message = "创建暂存目录失败: " + ec.message();
    return result;
  }
  const std::filesystem::path staging_dir =
      staging_root / (driver.slug + "-" + NextUniqueSuffix());
  std::filesystem::create_directories(staging_dir, ec);
  if (ec) {
    result.message = "创建解压目录失败: " + ec.message();
    return result;
  }
  ScopedDirectoryRemoval staging_cleanup{staging_dir};

  const ProcessResult extracted =
      RunProcess({tar_exe, "-xzf", archive_path.string(), "-C", staging_dir.string()});
  if (!extracted.spawn_error.empty()) {
    result.message = "解压失败: " + extracted.spawn_error;
    return result;
  }
  if (extracted.exit_code != 0) {
    result.message = "解压驱动包失败: " + extracted.output;
    return result;
  }

  const std::filesystem::path canonical_staging = std::filesystem::canonical(staging_dir, ec);
  if (ec) {
    result.message = "校验解压目录失败: " + ec.message();
    return result;
  }

  const std::filesystem::path package_manifest = staging_dir / "MANIFEST";
  if (std::filesystem::is_symlink(package_manifest, ec) ||
      !std::filesystem::is_regular_file(package_manifest, ec)) {
    result.message = "驱动包缺少有效的 MANIFEST 文件";
    return result;
  }
  const std::filesystem::path canonical_manifest =
      std::filesystem::canonical(package_manifest, ec);
  if (ec || !IsPathWithin(canonical_staging, canonical_manifest)) {
    result.message = "MANIFEST 路径越界";
    return result;
  }

  toml::table pkg_toml;
  try {
    pkg_toml = toml::parse_file(canonical_manifest.native());
  } catch (const std::exception& err) {
    result.message = std::string("解析 MANIFEST 失败: ") + err.what();
    return result;
  }

  std::string driver_rel_str;
  if (const auto* drv_node = pkg_toml.at_path("Files.driver").as_string()) {
    driver_rel_str = drv_node->get();
  }
  if (!IsSafeRelativeArchivePath(driver_rel_str)) {
    result.message = "MANIFEST 中的 Files.driver 无效";
    return result;
  }
  const std::filesystem::path driver_rel(driver_rel_str);
  const std::filesystem::path staged_driver = staging_dir / driver_rel;
  if (std::filesystem::is_symlink(staged_driver, ec) ||
      !std::filesystem::is_regular_file(staged_driver, ec)) {
    result.message = "驱动包缺少驱动文件: " + driver_rel_str;
    return result;
  }
  const std::filesystem::path canonical_driver = std::filesystem::canonical(staged_driver, ec);
  if (ec || !IsPathWithin(canonical_staging, canonical_driver)) {
    result.message = "驱动文件路径越界: " + driver_rel_str;
    return result;
  }

  std::string resolved_name = driver.name.empty() ? driver.slug : driver.name;
  if (const auto* n = pkg_toml["name"].as_string(); n != nullptr && !n->get().empty()) {
    resolved_name = n->get();
  }
  std::string publisher;
  if (const auto* p = pkg_toml["publisher"].as_string()) {
    publisher = p->get();
  }
  std::string resolved_license = driver.license;
  if (const auto* lic = pkg_toml["license"].as_string(); lic != nullptr && !lic->get().empty()) {
    resolved_license = lic->get();
  }
  std::string resolved_version = driver.version;
  if (const auto* ver = pkg_toml["version"].as_string(); ver != nullptr && !ver->get().empty()) {
    resolved_version = ver->get();
  }
  std::string adbc_version;
  if (const auto* av = pkg_toml.at_path("ADBC.version").as_string()) {
    adbc_version = av->get();
  }
  std::string entrypoint;
  if (const auto* ep = pkg_toml.at_path("Driver.entrypoint").as_string()) {
    entrypoint = ep->get();
  }

  const std::string safe_version = SanitizeVersionDirName(resolved_version);
  const std::filesystem::path pkg_slug_dir = drivers_dir / ".packages" / driver.slug;
  std::filesystem::create_directories(pkg_slug_dir, ec);
  if (ec) {
    result.message = "创建驱动安装目录失败: " + ec.message();
    return result;
  }

  const std::filesystem::path tmp_pkg_dir =
      pkg_slug_dir / (".tmp-" + safe_version + "-" + NextUniqueSuffix());
  std::filesystem::create_directories(tmp_pkg_dir, ec);
  if (ec) {
    result.message = "创建临时包目录失败: " + ec.message();
    return result;
  }
  ScopedDirectoryRemoval tmp_pkg_cleanup{tmp_pkg_dir};

  for (std::filesystem::recursive_directory_iterator it(
           staging_dir, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) {
      result.message = "遍历解压内容失败: " + ec.message();
      return result;
    }
    const auto& entry_path = it->path();
    if (it->is_symlink(ec)) {
      continue;
    }
    if (!it->is_regular_file(ec)) {
      continue;
    }
    const std::filesystem::path rel = std::filesystem::relative(entry_path, staging_dir, ec);
    if (ec || !IsSafeRelativeArchivePath(rel.generic_string()) || rel == "MANIFEST") {
      continue;
    }
    const std::filesystem::path dest = tmp_pkg_dir / rel;
    if (dest.has_parent_path()) {
      std::filesystem::create_directories(dest.parent_path(), ec);
      if (ec) {
        result.message = "创建目标子目录失败: " + ec.message();
        return result;
      }
    }
    std::filesystem::copy_file(entry_path, dest,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      result.message = "复制驱动文件失败: " + ec.message();
      return result;
    }
  }

  const std::filesystem::path final_pkg_dir = pkg_slug_dir / safe_version;
  std::filesystem::remove_all(final_pkg_dir, ec);
  ec.clear();
  std::filesystem::rename(tmp_pkg_dir, final_pkg_dir, ec);
  if (ec) {
    result.message = "提交驱动安装目录失败: " + ec.message();
    return result;
  }
  tmp_pkg_cleanup.path.clear();

  const std::filesystem::path installed_driver =
      std::filesystem::absolute(final_pkg_dir / driver_rel, ec);
  if (ec || !std::filesystem::is_regular_file(installed_driver, ec)) {
    result.message = "安装后未找到驱动库文件: " + installed_driver.string();
    return result;
  }

  toml::table out_manifest;
  out_manifest.insert("manifest_version", std::int64_t{1});
  if (!resolved_name.empty()) {
    out_manifest.insert("name", resolved_name);
  }
  if (!publisher.empty()) {
    out_manifest.insert("publisher", publisher);
  }
  if (!resolved_license.empty()) {
    out_manifest.insert("license", resolved_license);
  }
  if (!resolved_version.empty()) {
    out_manifest.insert("version", resolved_version);
  }
  out_manifest.insert("source", "dbpp");
  if (!adbc_version.empty()) {
    toml::table adbc_table;
    adbc_table.insert("version", adbc_version);
    out_manifest.insert("ADBC", std::move(adbc_table));
  }

  toml::table shared_table;
  shared_table.insert(CurrentPlatformTuple(), installed_driver.string());

  toml::table driver_table;
  if (!entrypoint.empty()) {
    driver_table.insert("entrypoint", entrypoint);
  }
  driver_table.insert("shared", std::move(shared_table));
  out_manifest.insert("Driver", std::move(driver_table));

  const std::filesystem::path final_manifest = drivers_dir / (driver.slug + ".toml");
  const std::filesystem::path tmp_manifest =
      drivers_dir / (driver.slug + ".toml.tmp-" + NextUniqueSuffix());
  ScopedFileRemoval tmp_manifest_cleanup{tmp_manifest};

  {
    std::ofstream out(tmp_manifest, std::ios::binary | std::ios::trunc);
    if (!out) {
      result.message = "写入 manifest 临时文件失败: " + tmp_manifest.string();
      return result;
    }
    out << out_manifest << '\n';
    out.flush();
    if (!out) {
      result.message = "刷新 manifest 临时文件失败: " + tmp_manifest.string();
      return result;
    }
  }

  std::filesystem::rename(tmp_manifest, final_manifest, ec);
  if (ec) {
    result.message = "替换 manifest 文件失败: " + ec.message();
    return result;
  }
  tmp_manifest_cleanup.path.clear();

  result.ok = true;
  result.version = resolved_version;
  result.manifest_path = final_manifest.string();
  result.message = "已安装 " + resolved_name +
                   (resolved_version.empty() ? std::string{} : " (" + resolved_version + ")");
  return result;
}

DriverInstallResult UninstallUserAdbcDriverBlocking(
    std::string_view slug, const std::filesystem::path& user_drivers_dir) {
  DriverInstallResult result;
  result.slug = std::string(slug);

  if (!IsValidSlug(slug)) {
    result.message = "非法的驱动标识: " + std::string(slug);
    return result;
  }

  const std::filesystem::path drivers_dir = ResolveUserDriversDir(user_drivers_dir);
  if (drivers_dir.empty()) {
    result.message = "无法确定用户 ADBC 驱动目录";
    return result;
  }

  const std::filesystem::path manifest = drivers_dir / (std::string(slug) + ".toml");
  const std::filesystem::path pkg_dir = drivers_dir / ".packages" / std::string(slug);

  std::error_code ec;
  const bool had_manifest = std::filesystem::exists(manifest, ec);
  if (had_manifest) {
    std::filesystem::remove(manifest, ec);
    if (ec) {
      result.message = "删除 manifest 失败: " + ec.message();
      return result;
    }
  }

  std::error_code pkg_ec;
  if (std::filesystem::exists(pkg_dir, pkg_ec)) {
    std::filesystem::remove_all(pkg_dir, pkg_ec);
    if (pkg_ec) {
#if defined(_WIN32)
      if (had_manifest) {
        result.ok = true;
        result.manifest_path = manifest.string();
        result.message = "已卸载 " + std::string(slug) +
                         "（驱动库当前被进程占用，重启后可清理残留文件）";
        return result;
      }
#endif
      result.message = "删除驱动文件失败: " + pkg_ec.message();
      return result;
    }
  }

  result.ok = true;
  result.manifest_path = manifest.string();
  result.message = had_manifest ? ("已卸载 " + std::string(slug))
                                : (std::string(slug) + " 未在用户目录安装");
  return result;
}

huxerui::Task<RegistryCatalogResult> FetchRegistryCatalogAsync(
    std::shared_ptr<huxerui::HttpClient> http) {
  RegistryCatalogResult result;
  if (!http) {
    result.error = "HTTP 客户端不可用";
    co_return result;
  }

  const std::string base_url = AdbcRegistryBaseUrl();
  const huxerui::HttpResult<huxerui::HttpResponse> response = co_await http->SendAsync(
      huxerui::HttpRequest{
          .url = base_url + "/index.yaml",
          .headers = {{"Accept", "application/yaml"}},
          .timeout = std::chrono::milliseconds{30000},
      });
  if (!response.Succeeded()) {
    result.error = "获取驱动目录失败: " + response.Error().message;
    co_return result;
  }

  const huxerui::HttpResponse& http_resp = response.Value();
  if (http_resp.status_code < 200 || http_resp.status_code >= 300) {
    result.error = "获取驱动目录失败: HTTP " + std::to_string(http_resp.status_code);
    co_return result;
  }

  std::string yaml_text;
  yaml_text.resize(http_resp.body.size());
  if (!http_resp.body.empty()) {
    std::memcpy(yaml_text.data(), http_resp.body.data(), http_resp.body.size());
  }

  const std::string platform = CurrentPlatformTuple();
  auto parse_job = [yaml_text = std::move(yaml_text), base_url, platform]() {
    return ParseRegistryIndexYaml(yaml_text, base_url, platform);
  };
  auto parsed = co_await huxerui::RunWorker(std::move(parse_job));
  result.drivers = std::move(parsed);
  if (result.drivers.empty()) {
    result.error = "当前平台 (" + platform + ") 暂无可用的在线驱动";
  }
  co_return result;
}

huxerui::Task<DriverInstallResult> InstallRegistryDriverAsync(
    std::shared_ptr<huxerui::HttpClient> http, RegistryDriverInfo driver,
    std::function<void(std::string)> on_progress) {
  DriverInstallResult result;
  result.slug = driver.slug;

  if (!http) {
    result.message = "HTTP 客户端不可用";
    co_return result;
  }
  if (!IsValidSlug(driver.slug)) {
    result.message = "非法的驱动标识: " + driver.slug;
    co_return result;
  }
  if (driver.package_url.empty()) {
    result.message = "驱动缺少下载地址";
    co_return result;
  }

  const std::string user_dir = UserAdbcDriversDir();
  if (user_dir.empty()) {
    result.message = "无法确定用户 ADBC 驱动目录";
    co_return result;
  }

  const std::filesystem::path downloads_dir = std::filesystem::path(user_dir) / ".downloads";
  std::error_code ec;
  std::filesystem::create_directories(downloads_dir, ec);
  if (ec) {
    result.message = "创建下载目录失败: " + ec.message();
    co_return result;
  }

  const std::filesystem::path tmp_archive =
      downloads_dir / (driver.slug + "-" + NextUniqueSuffix() + ".tar.gz");
  ScopedFileRemoval archive_cleanup{tmp_archive};

  if (on_progress) {
    on_progress("正在连接下载 " + driver.slug + " ...");
  }

  auto stream_result = co_await http->SendStreamAsync(
      huxerui::HttpRequest{
          .url = driver.package_url,
          .headers = {{"Accept", "application/octet-stream"}},
          .timeout = std::chrono::milliseconds{300000},
      },
      [slug = driver.slug, on_progress](huxerui::HttpProgress progress) {
        if (on_progress && progress.kind == huxerui::HttpProgressKind::Download) {
          on_progress(FormatProgressText(slug, progress.transferred_bytes, progress.total_bytes));
        }
      });
  if (!stream_result.Succeeded()) {
    result.message = "下载失败: " + stream_result.Error().message;
    co_return result;
  }

  huxerui::HttpResponseStream stream = std::move(stream_result).Value();
  const int status_code = stream.StatusCode();
  if (status_code < 200 || status_code >= 300) {
    result.message = "下载被拒绝: HTTP " + std::to_string(status_code);
    co_return result;
  }

  const huxerui::File output_file(tmp_archive.string());
  auto opened_output = co_await output_file.OpenWriteAsync();
  if (!opened_output.Succeeded()) {
    result.message = "打开临时文件失败: " + opened_output.Error().message;
    co_return result;
  }

  huxerui::AsyncOutputStream output_stream = std::move(opened_output).Value();
  auto copied = co_await stream.Body().CopyToAsync(output_stream, 64U * 1024U);
  if (!copied.Succeeded()) {
    result.message = "保存安装包失败: " + copied.Error().message;
    co_return result;
  }
  auto closed = co_await output_stream.CloseAsync();
  if (!closed.Succeeded()) {
    result.message = "写入安装包失败: " + closed.Error().message;
    co_return result;
  }

  if (on_progress) {
    on_progress("正在校验并解压安装 " + driver.slug + " ...");
  }

  auto install_job = [driver = std::move(driver), tmp_archive]() {
    return InstallDriverArchiveBlocking(driver, tmp_archive);
  };
  auto installed = co_await huxerui::RunWorker(std::move(install_job));
  co_return installed;
}

} // namespace dbpp
