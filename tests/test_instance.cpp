#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

import std;
import dbpp.instance;

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

struct ScopedTempDir {
  fs::path path;

  ScopedTempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() / ("dbpp_test_instance_" + std::to_string(stamp));
    fs::create_directories(path);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::string SelfExecutablePath(const char* argv0) {
#if defined(_WIN32)
  std::wstring buf(512, L'\0');
  for (;;) {
    const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0) {
      break;
    }
    if (len < buf.size()) {
      buf.resize(len);
      const int needed =
          WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0,
                              nullptr, nullptr);
      if (needed > 0) {
        std::string utf8(static_cast<std::size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), utf8.data(),
                            needed, nullptr, nullptr);
        return utf8;
      }
      break;
    }
    if (buf.size() >= 32768) {
      break;
    }
    buf.resize(buf.size() * 2, L'\0');
  }
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  if (size > 0) {
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
      return std::string(buf.c_str());
    }
  }
#else
  std::string buf(512, '\0');
  const ssize_t len = ::readlink("/proc/self/exe", buf.data(), buf.size());
  if (len > 0) {
    buf.resize(static_cast<std::size_t>(len));
    return buf;
  }
#endif
  return argv0 != nullptr ? std::string(argv0) : std::string{};
}

int RunSelfSubprocess(const std::string& exe_path, const std::vector<std::string>& args) {
#if defined(_WIN32)
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
  auto quote_arg = [](std::wstring_view arg) -> std::wstring {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
      return std::wstring(arg);
    }
    std::wstring quoted = L"\"";
    for (std::size_t i = 0; i < arg.size(); ++i) {
      std::size_t bs = 0;
      while (i < arg.size() && arg[i] == L'\\') {
        ++bs;
        ++i;
      }
      if (i == arg.size()) {
        quoted.append(bs * 2, L'\\');
        break;
      }
      if (arg[i] == L'"') {
        quoted.append(bs * 2 + 1, L'\\');
        quoted.push_back(L'"');
      } else {
        quoted.append(bs, L'\\');
        quoted.push_back(arg[i]);
      }
    }
    quoted.push_back(L'"');
    return quoted;
  };

  std::wstring cmdline = quote_arg(utf8_to_wide(exe_path));
  for (const std::string& arg : args) {
    cmdline.push_back(L' ');
    cmdline += quote_arg(utf8_to_wide(arg));
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &si, &pi)) {
    return -1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    std::vector<std::string> mutable_args;
    mutable_args.reserve(args.size() + 1);
    mutable_args.push_back(exe_path);
    for (const std::string& a : args) {
      mutable_args.push_back(a);
    }
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(mutable_args.size() + 1);
    for (std::string& a : mutable_args) {
      argv_ptrs.push_back(a.data());
    }
    argv_ptrs.push_back(nullptr);
    ::execv(exe_path.c_str(), argv_ptrs.data());
    ::_exit(127);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace

int main(int argc, char** argv) {
  if (argc >= 4 && std::string_view(argv[1]) == "--child-probe") {
    const fs::path lock_dir(argv[2]);
    const dbpp::InstanceOpenRequest req{
        .uri = argv[3],
        .driver_path = "postgresql",
    };
    const bool acquired = dbpp::AcquireSingleInstanceOrActivate(true, req, lock_dir);
    if (acquired) {
      dbpp::ReleaseSingleInstance();
      return 10;
    }
    return 0;
  }

  if (argc >= 3 && std::string_view(argv[1]) == "--child-acquire") {
    const fs::path lock_dir(argv[2]);
    const bool acquired =
        dbpp::AcquireSingleInstanceOrActivate(false, std::nullopt, lock_dir);
    if (!acquired) {
      return 11;
    }
    dbpp::ReleaseSingleInstance();
    return 0;
  }

  const std::string self_exe = SelfExecutablePath(argc > 0 ? argv[0] : nullptr);
  Expect(!self_exe.empty(), "SelfExecutablePath must not be empty");

  ScopedTempDir temp;

  const bool first_acquired =
      dbpp::AcquireSingleInstanceOrActivate(true, std::nullopt, temp.path);
  Expect(first_acquired, "Primary process must acquire single-instance lock");
  Expect(!dbpp::ConsumeInstanceActivation(),
         "Activation flag must initially be false after acquire");

  const int probe_code = RunSelfSubprocess(
      self_exe, {"--child-probe", temp.path.string(), "/tmp/second_launch.sqlite"});
  Expect(probe_code == 0,
         "Secondary process must detect existing owner and exit 0, got: " +
             std::to_string(probe_code));

  Expect(dbpp::ConsumeInstanceActivation(),
         "Primary process must receive activation signal from secondary launch");
  Expect(!dbpp::ConsumeInstanceActivation(),
         "Activation flag must reset after being consumed");

  const auto forwarded = dbpp::TakePendingInstanceOpenRequest(temp.path);
  Expect(forwarded.has_value(), "Forwarded open request must be present");
  if (forwarded.has_value()) {
    Expect(forwarded->uri == "/tmp/second_launch.sqlite",
           "Forwarded URI mismatch: " + forwarded->uri);
    Expect(forwarded->driver_path == "postgresql",
           "Forwarded driver mismatch: " + forwarded->driver_path);
  }
  Expect(!dbpp::TakePendingInstanceOpenRequest(temp.path).has_value(),
         "Forwarded open request must be cleared after consumption");

  dbpp::MarkInstanceClosing();
  dbpp::MarkInstanceRunning();

  dbpp::ReleaseSingleInstance();

  const int after_release_code =
      RunSelfSubprocess(self_exe, {"--child-acquire", temp.path.string()});
  Expect(after_release_code == 0,
         "Child must acquire lock after parent ReleaseSingleInstance, got: " +
             std::to_string(after_release_code));

  Expect(dbpp::AcquireSingleInstanceOrActivate(true, std::nullopt, temp.path),
         "Parent should re-acquire lock cleanly after child exits");
  dbpp::ReleaseSingleInstance();

  if (g_failures != 0) {
    std::cerr << g_failures << " instance test assertion(s) failed\n";
    return 1;
  }
  std::cout << "test_instance passed\n";
  return 0;
}
