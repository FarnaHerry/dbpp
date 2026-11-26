module;

#include <huxerui/file.h>
#include <huxerui/http.h>
#include <huxerui/task.h>
#include <toml++/toml.hpp>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstring>

module dbpp.driver_installer;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.adbc;

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

struct ProcessResult {
  int exit_code = -1;
  std::string output;
  std::string spawn_error;
};

ProcessResult RunProcess(const std::vector<std::string>& args) {
  ProcessResult result;
  if (args.empty()) {
    result.spawn_error = Tr("Empty command", "空命令");
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
    result.spawn_error = Tr("Failed to create pipe", "无法创建管道");
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
    result.spawn_error = TrFmt("Failed to launch process: {0}", "无法启动进程: {0}",
                               {args.front()});
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
    result.spawn_error = Tr("Failed to create pipe", "创建管道失败");
    return result;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    result.spawn_error = Tr("Failed to fork process", "fork 进程失败");
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
    return TrFmt("Downloading {0}: {1} / {2} MB ({3}%)",
                 "正在下载 {0}: {1} / {2} MB ({3}%)",
                 {slug, to_mb(received), to_mb(*total), std::to_string(percent)});
  }
  return TrFmt("Downloading {0}: {1} MB", "正在下载 {0}: {1} MB",
               {slug, to_mb(received)});
}


bool DownloadUrlToFileBlocking(std::string_view url, const std::filesystem::path& dest_path,
                               std::string_view accept_header, int max_time_seconds,
                               std::string& error_out) {
  if (const auto local_path = detail::InstallerParseFileUrlPath(url)) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*local_path, ec)) {
      error_out = TrFmt("Local file not found: {0}", "本地文件不存在: {0}",
                        {local_path->string()});
      return false;
    }
    std::filesystem::copy_file(*local_path, dest_path,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      error_out = TrFmt("Failed to copy file: {0}", "复制文件失败: {0}", {ec.message()});
      return false;
    }
    return true;
  }

  const std::string lower = LowerAscii(url);
  if (!lower.starts_with("http://") && !lower.starts_with("https://")) {
    error_out = TrFmt("Unsupported URL scheme: {0}", "不支持的 URL 协议: {0}", {url});
    return false;
  }

#if defined(_WIN32)
  const std::string curl_bin = "curl.exe";
#else
  const std::string curl_bin = "curl";
#endif
  const ProcessResult proc = RunProcess({
      curl_bin,
      "-fsSL",
      "--connect-timeout",
      "15",
      "--max-time",
      std::to_string(max_time_seconds),
      "-H",
      "Accept: " + std::string(accept_header),
      "-o",
      dest_path.string(),
      std::string(url),
  });
  if (!proc.spawn_error.empty()) {
    error_out = proc.spawn_error;
    return false;
  }
  if (proc.exit_code != 0) {
    const std::string_view detail = TrimAscii(proc.output);
    error_out = detail.empty()
                    ? TrFmt("curl exit code {0}", "curl 退出码 {0}",
                            {std::to_string(proc.exit_code)})
                    : std::string(detail);
    return false;
  }
  return true;
}

} // namespace

namespace detail {

std::string InstallerNextUniqueSuffix() { return NextUniqueSuffix(); }
std::string InstallerLowerAscii(std::string_view text) { return LowerAscii(text); }
std::string_view InstallerTrimAscii(std::string_view text) { return TrimAscii(text); }
void InstallerRemoveFileQuiet(const std::filesystem::path& path) {
  if (!path.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}
bool InstallerDownloadUrlToFileBlocking(std::string_view url,
                                        const std::filesystem::path& dest_path,
                                        std::string_view accept_header, int max_time_seconds,
                                        std::string& error_out) {
  return DownloadUrlToFileBlocking(url, dest_path, accept_header, max_time_seconds, error_out);
}

} // namespace detail

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
    result.message = TrFmt("Invalid driver slug: {0}", "非法的驱动标识: {0}", {driver.slug});
    return result;
  }

  const std::filesystem::path drivers_dir = ResolveUserDriversDir(user_drivers_dir);
  if (drivers_dir.empty()) {
    result.message = Tr("Cannot determine user ADBC driver directory", "无法确定用户 ADBC 驱动目录");
    return result;
  }

  std::error_code ec;
  if (!std::filesystem::is_regular_file(archive_path, ec)) {
    result.message = TrFmt("Driver archive not found: {0}", "驱动安装包不存在: {0}",
                           {archive_path.string()});
    return result;
  }

  if (!driver.sha256.empty()) {
    const std::string expected = detail::InstallerNormalizeSha256(driver.sha256);
    const std::string actual = Sha256HexFile(archive_path);
    if (expected.empty() || actual.empty() || actual != expected) {
      result.message = TrFmt(
          "SHA-256 mismatch: expected {0}, got {1}",
          "SHA-256 校验失败: 预期 {0}，实际 {1}",
          {expected, actual.empty() ? std::string(Tr("<read failed>", "<读取失败>")) : actual});
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
    result.message = TrFmt("Failed to run tar: {0}", "无法执行 tar: {0}", {listing.spawn_error});
    return result;
  }
  if (listing.exit_code != 0) {
    result.message = TrFmt("Failed to list archive: {0}", "无法读取压缩包目录: {0}",
                           {listing.output});
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
      result.message = TrFmt("Unsafe path in archive: {0}", "压缩包包含不安全路径: {0}", {entry});
      return result;
    }
    ++entry_count;
  }
  if (entry_count == 0) {
    result.message = Tr("Driver archive is empty", "驱动压缩包为空");
    return result;
  }

  const std::filesystem::path staging_root = drivers_dir / ".staging";
  std::filesystem::create_directories(staging_root, ec);
  if (ec) {
    result.message = TrFmt("Failed to create staging dir: {0}", "创建暂存目录失败: {0}",
                           {ec.message()});
    return result;
  }
  const std::filesystem::path staging_dir =
      staging_root / (driver.slug + "-" + NextUniqueSuffix());
  std::filesystem::create_directories(staging_dir, ec);
  if (ec) {
    result.message = TrFmt("Failed to create extract dir: {0}", "创建解压目录失败: {0}",
                           {ec.message()});
    return result;
  }
  ScopedDirectoryRemoval staging_cleanup{staging_dir};

  const ProcessResult extracted =
      RunProcess({tar_exe, "-xzf", archive_path.string(), "-C", staging_dir.string()});
  if (!extracted.spawn_error.empty()) {
    result.message = TrFmt("Extraction failed: {0}", "解压失败: {0}", {extracted.spawn_error});
    return result;
  }
  if (extracted.exit_code != 0) {
    result.message = TrFmt("Failed to extract driver archive: {0}", "解压驱动包失败: {0}",
                           {extracted.output});
    return result;
  }

  const std::filesystem::path canonical_staging = std::filesystem::canonical(staging_dir, ec);
  if (ec) {
    result.message = TrFmt("Failed to validate extract dir: {0}", "校验解压目录失败: {0}",
                           {ec.message()});
    return result;
  }

  const std::filesystem::path package_manifest = staging_dir / "MANIFEST";
  if (std::filesystem::is_symlink(package_manifest, ec) ||
      !std::filesystem::is_regular_file(package_manifest, ec)) {
    result.message = Tr("Driver package lacks a valid MANIFEST file", "驱动包缺少有效的 MANIFEST 文件");
    return result;
  }
  const std::filesystem::path canonical_manifest =
      std::filesystem::canonical(package_manifest, ec);
  if (ec || !IsPathWithin(canonical_staging, canonical_manifest)) {
    result.message = Tr("MANIFEST path escapes staging dir", "MANIFEST 路径越界");
    return result;
  }

  toml::table pkg_toml;
  try {
    pkg_toml = toml::parse_file(canonical_manifest.native());
  } catch (const std::exception& err) {
    result.message = TrFmt("Failed to parse MANIFEST: {0}", "解析 MANIFEST 失败: {0}",
                           {err.what()});
    return result;
  }

  std::string driver_rel_str;
  if (const auto* drv_node = pkg_toml.at_path("Files.driver").as_string()) {
    driver_rel_str = drv_node->get();
  }
  if (!IsSafeRelativeArchivePath(driver_rel_str)) {
    result.message = Tr("Invalid Files.driver in MANIFEST", "MANIFEST 中的 Files.driver 无效");
    return result;
  }
  const std::filesystem::path driver_rel(driver_rel_str);
  const std::filesystem::path staged_driver = staging_dir / driver_rel;
  if (std::filesystem::is_symlink(staged_driver, ec) ||
      !std::filesystem::is_regular_file(staged_driver, ec)) {
    result.message = TrFmt("Driver library missing from archive: {0}", "驱动包缺少驱动文件: {0}",
                           {driver_rel_str});
    return result;
  }
  const std::filesystem::path canonical_driver = std::filesystem::canonical(staged_driver, ec);
  if (ec || !IsPathWithin(canonical_staging, canonical_driver)) {
    result.message = TrFmt("Driver library path escapes staging dir: {0}", "驱动文件路径越界: {0}",
                           {driver_rel_str});
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
    result.message = TrFmt("Failed to create driver install dir: {0}", "创建驱动安装目录失败: {0}",
                           {ec.message()});
    return result;
  }

  const std::filesystem::path tmp_pkg_dir =
      pkg_slug_dir / (".tmp-" + safe_version + "-" + NextUniqueSuffix());
  std::filesystem::create_directories(tmp_pkg_dir, ec);
  if (ec) {
    result.message = TrFmt("Failed to create temp package dir: {0}", "创建临时包目录失败: {0}",
                           {ec.message()});
    return result;
  }
  ScopedDirectoryRemoval tmp_pkg_cleanup{tmp_pkg_dir};

  for (std::filesystem::recursive_directory_iterator it(
           staging_dir, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) {
      result.message = TrFmt("Failed to walk extracted files: {0}", "遍历解压内容失败: {0}",
                             {ec.message()});
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
        result.message = TrFmt("Failed to create target subdir: {0}", "创建目标子目录失败: {0}",
                               {ec.message()});
        return result;
      }
    }
    std::filesystem::copy_file(entry_path, dest,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      result.message = TrFmt("Failed to copy driver file: {0}", "复制驱动文件失败: {0}",
                             {ec.message()});
      return result;
    }
  }

  const std::filesystem::path final_pkg_dir = pkg_slug_dir / safe_version;
  std::filesystem::remove_all(final_pkg_dir, ec);
  ec.clear();
  std::filesystem::rename(tmp_pkg_dir, final_pkg_dir, ec);
  if (ec) {
    result.message = TrFmt("Failed to finalize driver install dir: {0}",
                           "提交驱动安装目录失败: {0}", {ec.message()});
    return result;
  }
  tmp_pkg_cleanup.path.clear();

  const std::filesystem::path installed_driver =
      std::filesystem::absolute(final_pkg_dir / driver_rel, ec);
  if (ec || !std::filesystem::is_regular_file(installed_driver, ec)) {
    result.message = TrFmt("Installed driver library not found: {0}",
                           "安装后未找到驱动库文件: {0}", {installed_driver.string()});
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
      result.message = TrFmt("Failed to write temp manifest: {0}", "写入 manifest 临时文件失败: {0}",
                             {tmp_manifest.string()});
      return result;
    }
    out << out_manifest << '\n';
    out.flush();
    if (!out) {
      result.message = TrFmt("Failed to flush temp manifest: {0}", "刷新 manifest 临时文件失败: {0}",
                             {tmp_manifest.string()});
      return result;
    }
  }

  std::filesystem::rename(tmp_manifest, final_manifest, ec);
  if (ec) {
    result.message = TrFmt("Failed to replace manifest file: {0}", "替换 manifest 文件失败: {0}",
                           {ec.message()});
    return result;
  }
  tmp_manifest_cleanup.path.clear();

  result.ok = true;
  result.version = resolved_version;
  result.manifest_path = final_manifest.string();
  result.message =
      resolved_version.empty()
          ? TrFmt("Installed {0}", "已安装 {0}", {resolved_name})
          : TrFmt("Installed {0} ({1})", "已安装 {0} ({1})", {resolved_name, resolved_version});
  return result;
}

DriverInstallResult UninstallUserAdbcDriverBlocking(
    std::string_view slug, const std::filesystem::path& user_drivers_dir) {
  DriverInstallResult result;
  result.slug = std::string(slug);

  if (!IsValidSlug(slug)) {
    result.message = TrFmt("Invalid driver slug: {0}", "非法的驱动标识: {0}", {slug});
    return result;
  }

  const std::filesystem::path drivers_dir = ResolveUserDriversDir(user_drivers_dir);
  if (drivers_dir.empty()) {
    result.message = Tr("Cannot determine user ADBC driver directory", "无法确定用户 ADBC 驱动目录");
    return result;
  }

  const std::filesystem::path manifest = drivers_dir / (std::string(slug) + ".toml");
  const std::filesystem::path pkg_dir = drivers_dir / ".packages" / std::string(slug);

  std::error_code ec;
  const bool had_manifest = std::filesystem::exists(manifest, ec);
  if (had_manifest) {
    std::filesystem::remove(manifest, ec);
    if (ec) {
      result.message = TrFmt("Failed to delete manifest: {0}", "删除 manifest 失败: {0}",
                             {ec.message()});
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
        result.message = TrFmt(
            "Uninstalled {0} (library currently in use; leftover files can be cleaned up after restart)",
            "已卸载 {0}（驱动库当前被进程占用，重启后可清理残留文件）",
            {slug});
        return result;
      }
#endif
      result.message = TrFmt("Failed to delete driver files: {0}", "删除驱动文件失败: {0}",
                             {pkg_ec.message()});
      return result;
    }
  }

  result.ok = true;
  result.manifest_path = manifest.string();
  result.message = had_manifest
                       ? TrFmt("Uninstalled {0}", "已卸载 {0}", {slug})
                       : TrFmt("{0} is not installed in user directory",
                               "{0} 未在用户目录安装", {slug});
  return result;
}

DriverInstallResult InstallRegistryDriverBlocking(const RegistryDriverInfo& driver,
                                                  const std::filesystem::path& user_drivers_dir) {
  DriverInstallResult result;
  result.slug = driver.slug;

  if (!IsValidSlug(driver.slug)) {
    result.message = TrFmt("Invalid driver slug: {0}", "非法的驱动标识: {0}", {driver.slug});
    return result;
  }
  if (driver.package_url.empty()) {
    result.message = Tr("Driver lacks a download URL", "驱动缺少下载地址");
    return result;
  }

  const std::filesystem::path drivers_dir = ResolveUserDriversDir(user_drivers_dir);
  if (drivers_dir.empty()) {
    result.message = Tr("Cannot determine user ADBC driver directory", "无法确定用户 ADBC 驱动目录");
    return result;
  }

  if (const auto local_archive = detail::InstallerParseFileUrlPath(driver.package_url)) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*local_archive, ec)) {
      result.message = TrFmt("Local archive not found: {0}", "本地安装包不存在: {0}",
                             {local_archive->string()});
      return result;
    }
    return InstallDriverArchiveBlocking(driver, *local_archive, drivers_dir);
  }

  const std::filesystem::path downloads_dir = drivers_dir / ".downloads";
  std::error_code ec;
  std::filesystem::create_directories(downloads_dir, ec);
  if (ec) {
    result.message = TrFmt("Failed to create download dir: {0}", "创建下载目录失败: {0}",
                           {ec.message()});
    return result;
  }

  const std::filesystem::path tmp_archive =
      downloads_dir / (driver.slug + "-" + NextUniqueSuffix() + ".tar.gz");
  ScopedFileRemoval archive_cleanup{tmp_archive};

  std::string download_error;
  if (!DownloadUrlToFileBlocking(driver.package_url, tmp_archive, "application/octet-stream", 300,
                                 download_error)) {
    result.message = TrFmt("Download failed: {0}", "下载失败: {0}", {download_error});
    return result;
  }

  return InstallDriverArchiveBlocking(driver, tmp_archive, drivers_dir);
}

huxerui::Task<DriverInstallResult> InstallRegistryDriverAsync(
    std::shared_ptr<huxerui::HttpClient> http, RegistryDriverInfo driver,
    std::function<void(std::string)> on_progress) {
  DriverInstallResult result;
  result.slug = driver.slug;

  if (!http) {
    result.message = Tr("HTTP client is unavailable", "HTTP 客户端不可用");
    co_return result;
  }
  if (!IsValidSlug(driver.slug)) {
    result.message = TrFmt("Invalid driver slug: {0}", "非法的驱动标识: {0}", {driver.slug});
    co_return result;
  }
  if (driver.package_url.empty()) {
    result.message = Tr("Driver lacks a download URL", "驱动缺少下载地址");
    co_return result;
  }

  const std::string user_dir = UserAdbcDriversDir();
  if (user_dir.empty()) {
    result.message = Tr("Cannot determine user ADBC driver directory", "无法确定用户 ADBC 驱动目录");
    co_return result;
  }

  const std::filesystem::path downloads_dir = std::filesystem::path(user_dir) / ".downloads";
  std::error_code ec;
  std::filesystem::create_directories(downloads_dir, ec);
  if (ec) {
    result.message = TrFmt("Failed to create download dir: {0}", "创建下载目录失败: {0}",
                           {ec.message()});
    co_return result;
  }

  const std::filesystem::path tmp_archive =
      downloads_dir / (driver.slug + "-" + NextUniqueSuffix() + ".tar.gz");
  ScopedFileRemoval archive_cleanup{tmp_archive};

  if (on_progress) {
    on_progress(TrFmt("Connecting to download {0} ...", "正在连接下载 {0} ...", {driver.slug}));
  }

  huxerui::HttpRequest request;
  request.url = driver.package_url;
  request.headers.push_back(huxerui::HttpHeader{"Accept", "application/octet-stream"});
  request.timeout = std::chrono::milliseconds{300000};
  auto progress_cb = [slug = driver.slug, on_progress](huxerui::HttpProgress progress) {
    if (on_progress && progress.kind == huxerui::HttpProgressKind::Download) {
      on_progress(FormatProgressText(slug, progress.transferred_bytes, progress.total_bytes));
    }
  };
  auto stream_result =
      co_await http->SendStreamAsync(std::move(request), std::move(progress_cb));
  if (!stream_result.Succeeded()) {
    result.message = TrFmt("Download failed: {0}", "下载失败: {0}",
                           {stream_result.Error().message});
    co_return result;
  }

  huxerui::HttpResponseStream stream = std::move(stream_result).Value();
  const int status_code = stream.StatusCode();
  if (status_code < 200 || status_code >= 300) {
    result.message = TrFmt("Download rejected: HTTP {0}", "下载被拒绝: HTTP {0}",
                           {std::to_string(status_code)});
    co_return result;
  }

  const huxerui::File output_file(tmp_archive.string());
  auto opened_output = co_await output_file.OpenWriteAsync();
  if (!opened_output.Succeeded()) {
    result.message = TrFmt("Failed to open temp file: {0}", "打开临时文件失败: {0}",
                           {opened_output.Error().message});
    co_return result;
  }

  huxerui::AsyncOutputStream output_stream = std::move(opened_output).Value();
  auto copied = co_await stream.Body().CopyToAsync(output_stream, 64U * 1024U);
  if (!copied.Succeeded()) {
    result.message = TrFmt("Failed to save archive: {0}", "保存安装包失败: {0}",
                           {copied.Error().message});
    co_return result;
  }
  auto closed = co_await output_stream.CloseAsync();
  if (!closed.Succeeded()) {
    result.message = TrFmt("Failed to write archive: {0}", "写入安装包失败: {0}",
                           {closed.Error().message});
    co_return result;
  }

  if (on_progress) {
    on_progress(TrFmt("Verifying and extracting {0} ...", "正在校验并解压安装 {0} ...",
                      {driver.slug}));
  }

  auto install_job = [driver = std::move(driver), tmp_archive]() {
    return InstallDriverArchiveBlocking(driver, tmp_archive);
  };
  auto installed = co_await huxerui::RunWorker(std::move(install_job));
  co_return installed;
}

} // namespace dbpp
