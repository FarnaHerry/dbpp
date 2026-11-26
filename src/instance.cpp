module;

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cwchar>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#endif
#endif

#include <csignal>
#include <cstdio>
#include <cstdlib>

module dbpp.instance;

import std;
import dbpp.adbc;

namespace dbpp {

namespace {

constexpr std::string_view kLockFileName = "dbpp.instance.lock";
constexpr std::string_view kOpenRequestFileName = "dbpp.open_request.tsv";

struct InstanceInfo {
  long long pid = 0;
  std::string state;
  long long timestamp = 0;
};

std::filesystem::path g_active_lock_dir;

std::string LowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

std::string EncodeField(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '%': result += "%25"; break;
      case '\t': result += "%09"; break;
      case '\n': result += "%0A"; break;
      case '\r': result += "%0D"; break;
      default: result.push_back(c); break;
    }
  }
  return result;
}

std::string DecodeField(std::string_view text) {
  const auto hex_val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string result;
  result.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int hi = hex_val(text[i + 1]);
      const int lo = hex_val(text[i + 2]);
      if (hi >= 0 && lo >= 0) {
        result.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    result.push_back(text[i]);
  }
  return result;
}

void ClearEnvVar(const char* name) {
#if defined(_WIN32)
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

std::filesystem::path ResolveLockDir(const std::filesystem::path& override_dir) {
  if (!override_dir.empty()) {
    return override_dir;
  }
  const std::string data_dir = DefaultAppDataDir();
  if (!data_dir.empty()) {
    return std::filesystem::path(data_dir);
  }
  std::error_code ec;
  const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
  return tmp.empty() ? std::filesystem::path(".") : (tmp / "dbpp");
}

long long CurrentUnixSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

InstanceInfo ParseInstanceInfo(std::string_view text) {
  InstanceInfo info;
  std::istringstream iss{std::string(text)};
  if (iss >> info.pid) {
    if (!(iss >> info.state)) {
      info.state = "running";
    }
    iss >> info.timestamp;
  }
  return info;
}

std::string CurrentExecutableStem() {
#if defined(_WIN32)
  std::wstring buf(512, L'\0');
  for (;;) {
    const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0) {
      return "dbpp";
    }
    if (len < buf.size()) {
      buf.resize(len);
      const std::u8string u8 = std::filesystem::path(buf).stem().u8string();
      return LowerAscii(std::string(reinterpret_cast<const char*>(u8.data()), u8.size()));
    }
    if (buf.size() >= 32768) {
      return "dbpp";
    }
    buf.resize(buf.size() * 2, L'\0');
  }
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  if (size == 0) {
    return "dbpp";
  }
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0) {
    return "dbpp";
  }
  return LowerAscii(std::filesystem::path(buf.c_str()).stem().string());
#else
  std::string buf(512, '\0');
  const ssize_t len = ::readlink("/proc/self/exe", buf.data(), buf.size());
  if (len <= 0) {
    return "dbpp";
  }
  buf.resize(static_cast<std::size_t>(len));
  return LowerAscii(std::filesystem::path(buf).stem().string());
#endif
}

bool MatchesExpectedBinaryName(std::string_view image_name) {
  if (image_name.empty()) {
    return false;
  }
  const std::string lower = LowerAscii(image_name);
  if (lower == "dbpp" || lower.find("dbpp") != std::string::npos) {
    return true;
  }
  const std::string self_stem = CurrentExecutableStem();
  return !self_stem.empty() && lower.find(self_stem) != std::string::npos;
}

void WriteOpenRequestFile(const std::filesystem::path& lock_dir,
                          const InstanceOpenRequest& request) {
  if (request.uri.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(lock_dir, ec);
  const std::filesystem::path target = lock_dir / kOpenRequestFileName;
  const std::filesystem::path temp = lock_dir / (std::string(kOpenRequestFileName) + ".tmp");
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return;
    }
    out << EncodeField(request.driver_path) << '\t' << EncodeField(request.uri) << '\n';
    out.flush();
    if (!out) {
      std::filesystem::remove(temp, ec);
      return;
    }
  }
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    std::filesystem::remove(target, ec);
    ec.clear();
    std::filesystem::rename(temp, target, ec);
    if (ec) {
      std::filesystem::remove(temp, ec);
    }
  }
}

#if defined(_WIN32)

HANDLE& InstanceMutexHandle() {
  static HANDLE handle = nullptr;
  return handle;
}

HANDLE& ActivationEventHandle() {
  static HANDLE handle = nullptr;
  return handle;
}

std::wstring MakeScopedWin32ObjectName(std::wstring_view base,
                                       const std::filesystem::path& lock_dir, bool use_global_default) {
  if (use_global_default) {
    return std::wstring(base);
  }
  const std::u8string u8 = lock_dir.lexically_normal().generic_u8string();
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char8_t ch : u8) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ULL;
  }
  wchar_t suffix[20];
  std::swprintf(suffix, 20, L".%016llx", static_cast<unsigned long long>(hash));
  return std::wstring(base) + suffix;
}

InstanceInfo ReadWin32InstanceInfo(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return ParseInstanceInfo(oss.str());
}

void WriteWin32InstanceInfo(const std::filesystem::path& path, long long pid,
                            std::string_view state) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return;
  }
  out << pid << ' ' << state << ' ' << CurrentUnixSeconds() << '\n';
}

bool IsExpectedInstanceProcess(long long pid) {
  if (pid <= 0) {
    return false;
  }
  HANDLE proc =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (proc == nullptr) {
    return false;
  }
  std::wstring path(512, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  const BOOL ok = QueryFullProcessImageNameW(proc, 0, path.data(), &size);
  CloseHandle(proc);
  if (!ok || size == 0) {
    return false;
  }
  path.resize(size);
  const std::u8string u8 = std::filesystem::path(path).stem().u8string();
  return MatchesExpectedBinaryName(
      std::string_view(reinterpret_cast<const char*>(u8.data()), u8.size()));
}

void TerminateOldProcess(long long pid) {
  if (!IsExpectedInstanceProcess(pid)) {
    return;
  }
  HANDLE proc =
      OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (proc == nullptr) {
    return;
  }
  TerminateProcess(proc, 0);
  WaitForSingleObject(proc, 1000);
  CloseHandle(proc);
}

void TryFocusExistingWin32Window() {
  if (HWND hwnd = FindWindowW(nullptr, L"db++"); hwnd != nullptr) {
    ShowWindow(hwnd, SW_RESTORE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
  }
}

#else

int& LockFileDescriptor() {
  static int fd = -1;
  return fd;
}

long long& OwnerPid() {
  static long long pid = 0;
  return pid;
}

volatile std::sig_atomic_t g_activation_requested = 0;

void OnActivationSignal(int) {
  g_activation_requested = 1;
}

bool InstallActivationSignalHandler() {
  struct sigaction action {};
  action.sa_handler = &OnActivationSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  return ::sigaction(SIGUSR1, &action, nullptr) == 0;
}

InstanceInfo ReadPosixInstanceInfo(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return {};
  }
  char buffer[128]{};
  const ssize_t count = ::read(fd, buffer, sizeof(buffer) - 1);
  ::close(fd);
  if (count <= 0) {
    return {};
  }
  return ParseInstanceInfo(std::string_view(buffer, static_cast<std::size_t>(count)));
}

void WritePosixLockState(int fd, long long pid, std::string_view state) {
  if (fd < 0) {
    return;
  }
  const std::string content =
      std::to_string(pid) + " " + std::string(state) + " " +
      std::to_string(CurrentUnixSeconds()) + "\n";
  if (::lseek(fd, 0, SEEK_SET) != 0) {
    return;
  }
  if (::ftruncate(fd, 0) != 0) {
    return;
  }
  std::size_t written_total = 0;
  while (written_total < content.size()) {
    const ssize_t n =
        ::write(fd, content.data() + written_total, content.size() - written_total);
    if (n > 0) {
      written_total += static_cast<std::size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  (void)::fsync(fd);
}

bool IsExpectedInstanceProcess(long long pid) {
  if (pid <= 0) {
    return false;
  }
#if defined(__APPLE__)
  char pathbuf[PROC_PIDPATHINFO_MAXSIZE]{};
  const int len = ::proc_pidpath(static_cast<int>(pid), pathbuf, sizeof(pathbuf));
  if (len > 0) {
    return MatchesExpectedBinaryName(std::filesystem::path(pathbuf).stem().string());
  }
  char namebuf[2 * MAXCOMLEN + 1]{};
  if (::proc_name(static_cast<int>(pid), namebuf, sizeof(namebuf)) > 0) {
    return MatchesExpectedBinaryName(namebuf);
  }
  return false;
#else
  std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
  std::string line;
  if (std::getline(comm, line) && MatchesExpectedBinaryName(line)) {
    return true;
  }
  std::string link_buf(512, '\0');
  const std::string exe_link = "/proc/" + std::to_string(pid) + "/exe";
  const ssize_t n = ::readlink(exe_link.c_str(), link_buf.data(), link_buf.size());
  if (n > 0) {
    link_buf.resize(static_cast<std::size_t>(n));
    return MatchesExpectedBinaryName(std::filesystem::path(link_buf).stem().string());
  }
  return false;
#endif
}

void TerminateOldProcess(long long pid) {
  if (!IsExpectedInstanceProcess(pid)) {
    return;
  }
  const auto target = static_cast<pid_t>(pid);
  if (::kill(target, 0) == 0) {
    (void)::kill(target, SIGTERM);
  }
  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (::kill(target, 0) != 0) {
      return;
    }
  }
  (void)::kill(target, SIGKILL);
  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (::kill(target, 0) != 0) {
      return;
    }
  }
}

void NotifyExistingProcess(long long pid) {
  if (pid > 0) {
    (void)::kill(static_cast<pid_t>(pid), SIGUSR1);
  }
}

#endif

} // namespace

std::optional<InstanceOpenRequest> PeekEnvOpenRequest() {
  const char* open_path = std::getenv("DBPP_OPEN");
  if (open_path == nullptr || *open_path == '\0') {
    return std::nullopt;
  }
  const char* driver = std::getenv("DBPP_DRIVER");
  InstanceOpenRequest req;
  req.uri = open_path;
  if (driver != nullptr && *driver != '\0') {
    req.driver_path = driver;
  }
  return req;
}

std::optional<InstanceOpenRequest> TakeEnvOpenRequest() {
  auto req = PeekEnvOpenRequest();
  if (req.has_value()) {
    ClearEnvVar("DBPP_OPEN");
    ClearEnvVar("DBPP_DRIVER");
  }
  return req;
}

bool AcquireSingleInstanceOrActivate(
    bool activate, const std::optional<InstanceOpenRequest>& open_request,
    const std::filesystem::path& lock_dir_override) {
  const std::filesystem::path lock_dir = ResolveLockDir(lock_dir_override);
  const std::filesystem::path lock_path = lock_dir / kLockFileName;

#if defined(_WIN32)
  const char* env_data_dir = std::getenv("DBPP_DATA_DIR");
  const bool use_global_default =
      lock_dir_override.empty() && (env_data_dir == nullptr || *env_data_dir == '\0');
  const std::wstring mutex_name =
      MakeScopedWin32ObjectName(L"Local\\dbpp.Singleton", lock_dir, use_global_default);
  const std::wstring event_name =
      MakeScopedWin32ObjectName(L"Local\\dbpp.Activate", lock_dir, use_global_default);

  HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
  if (mutex == nullptr) {
    g_active_lock_dir = lock_dir;
    return true;
  }

  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    const InstanceInfo info = ReadWin32InstanceInfo(lock_path);
    const long long self_pid = static_cast<long long>(GetCurrentProcessId());
    if (info.pid > 0 && info.pid != self_pid && info.state == "closing") {
      CloseHandle(mutex);
      mutex = nullptr;

      for (int i = 0; i < 10; ++i) {
        HANDLE candidate = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
        if (candidate != nullptr && GetLastError() != ERROR_ALREADY_EXISTS) {
          mutex = candidate;
          break;
        }
        if (candidate != nullptr) {
          CloseHandle(candidate);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }

      if (mutex == nullptr) {
        std::fprintf(stderr,
                     "db++: 发现旧实例 (PID %lld) 处于未完成的关闭状态，正在清理并接管...\n",
                     info.pid);
        TerminateOldProcess(info.pid);
        for (int i = 0; i < 20; ++i) {
          HANDLE candidate = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
          if (candidate != nullptr && GetLastError() != ERROR_ALREADY_EXISTS) {
            mutex = candidate;
            break;
          }
          if (candidate != nullptr) {
            CloseHandle(candidate);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      }

      if (mutex != nullptr) {
        InstanceMutexHandle() = mutex;
        ActivationEventHandle() = CreateEventW(nullptr, FALSE, FALSE, event_name.c_str());
        g_active_lock_dir = lock_dir;
        WriteWin32InstanceInfo(lock_path, self_pid, "running");
        return true;
      }
    }

    if (activate) {
      if (open_request.has_value() && !open_request->uri.empty()) {
        WriteOpenRequestFile(lock_dir, *open_request);
      }
      if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str());
          event != nullptr) {
        SetEvent(event);
        CloseHandle(event);
      }
      TryFocusExistingWin32Window();
    }
    if (mutex != nullptr) {
      CloseHandle(mutex);
    }
    return false;
  }

  InstanceMutexHandle() = mutex;
  ActivationEventHandle() = CreateEventW(nullptr, FALSE, FALSE, event_name.c_str());
  g_active_lock_dir = lock_dir;
  WriteWin32InstanceInfo(lock_path, static_cast<long long>(GetCurrentProcessId()), "running");
  return true;
#else
  std::error_code ec;
  std::filesystem::create_directories(lock_dir, ec);
  const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    g_active_lock_dir = lock_dir;
    return true;
  }

  const auto become_owner = [&](int owned_fd) -> bool {
    g_activation_requested = 0;
    OwnerPid() = static_cast<long long>(::getpid());
    WritePosixLockState(owned_fd, OwnerPid(), "running");
    LockFileDescriptor() = owned_fd;
    g_active_lock_dir = lock_dir;
    (void)InstallActivationSignalHandler();
    return true;
  };

  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const bool busy = errno == EWOULDBLOCK || errno == EAGAIN;
    if (!busy) {
      ::close(fd);
      g_active_lock_dir = lock_dir;
      return true;
    }

    const InstanceInfo info = ReadPosixInstanceInfo(lock_path);
    const long long self_pid = static_cast<long long>(::getpid());
    if (info.pid > 0 && info.pid != self_pid && info.state == "closing") {
      // Allow a clean shutdown to finish before forcefully terminating a hung process.
      for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
          return become_owner(fd);
        }
      }
      std::fprintf(stderr,
                   "db++: 发现旧实例 (PID %lld) 处于未完成的关闭状态，正在清理并接管...\n",
                   info.pid);
      TerminateOldProcess(info.pid);
      for (int i = 0; i < 20; ++i) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
          return become_owner(fd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }

    if (activate) {
      if (open_request.has_value() && !open_request->uri.empty()) {
        WriteOpenRequestFile(lock_dir, *open_request);
      }
      NotifyExistingProcess(info.pid);
    }
    ::close(fd);
    return false;
  }

  return become_owner(fd);
#endif
}

void MarkInstanceClosing() {
  if (g_active_lock_dir.empty()) {
    return;
  }
#if defined(_WIN32)
  if (InstanceMutexHandle() != nullptr) {
    WriteWin32InstanceInfo(g_active_lock_dir / kLockFileName,
                           static_cast<long long>(GetCurrentProcessId()), "closing");
  }
#else
  const int fd = LockFileDescriptor();
  if (fd >= 0) {
    WritePosixLockState(fd, OwnerPid(), "closing");
  }
#endif
}

void MarkInstanceRunning() {
  if (g_active_lock_dir.empty()) {
    return;
  }
#if defined(_WIN32)
  if (InstanceMutexHandle() != nullptr) {
    WriteWin32InstanceInfo(g_active_lock_dir / kLockFileName,
                           static_cast<long long>(GetCurrentProcessId()), "running");
  }
#else
  const int fd = LockFileDescriptor();
  if (fd >= 0) {
    WritePosixLockState(fd, OwnerPid(), "running");
  }
#endif
}

void ReleaseSingleInstance() {
#if defined(_WIN32)
  if (ActivationEventHandle() != nullptr) {
    CloseHandle(ActivationEventHandle());
    ActivationEventHandle() = nullptr;
  }
  if (InstanceMutexHandle() != nullptr) {
    ReleaseMutex(InstanceMutexHandle());
    CloseHandle(InstanceMutexHandle());
    InstanceMutexHandle() = nullptr;
  }
#else
  const int fd = LockFileDescriptor();
  if (fd >= 0) {
    (void)::flock(fd, LOCK_UN);
    ::close(fd);
    LockFileDescriptor() = -1;
  }
  OwnerPid() = 0;
  g_activation_requested = 0;
#endif
  g_active_lock_dir.clear();
}

bool ConsumeInstanceActivation() {
#if defined(_WIN32)
  return ActivationEventHandle() != nullptr &&
         WaitForSingleObject(ActivationEventHandle(), 0) == WAIT_OBJECT_0;
#else
  if (g_activation_requested == 0) {
    return false;
  }
  g_activation_requested = 0;
  return true;
#endif
}

std::optional<InstanceOpenRequest> TakePendingInstanceOpenRequest(
    const std::filesystem::path& lock_dir_override) {
  const std::filesystem::path lock_dir =
      !lock_dir_override.empty()
          ? lock_dir_override
          : (!g_active_lock_dir.empty() ? g_active_lock_dir : ResolveLockDir({}));
  const std::filesystem::path target = lock_dir / kOpenRequestFileName;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(target, ec)) {
    return std::nullopt;
  }

  const std::filesystem::path claimed =
      lock_dir / (std::string(kOpenRequestFileName) + ".claimed");
  std::filesystem::remove(claimed, ec);
  ec.clear();
  std::filesystem::rename(target, claimed, ec);
  const std::filesystem::path read_path = ec ? target : claimed;

  std::string line;
  {
    std::ifstream in(read_path, std::ios::binary);
    if (in) {
      std::getline(in, line);
    }
  }
  std::error_code rm_ec;
  std::filesystem::remove(read_path, rm_ec);

  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  const std::size_t tab = line.find('\t');
  if (tab == std::string::npos) {
    return std::nullopt;
  }
  InstanceOpenRequest req{
      .uri = DecodeField(std::string_view(line).substr(tab + 1)),
      .driver_path = DecodeField(std::string_view(line).substr(0, tab)),
  };
  if (req.uri.empty()) {
    return std::nullopt;
  }
  return req;
}

} // namespace dbpp
