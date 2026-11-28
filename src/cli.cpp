module;

#include <huxerui/datatable.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef DBPP_VERSION
#define DBPP_VERSION "0.4.6"
#endif

#include <cstdlib>

module dbpp.cli;

import std;
import dbpp.types;
import dbpp.adbc;
import dbpp.redis;
import dbpp.driver_installer;
import dbpp.db;

namespace dbpp {


namespace {

#if defined(_WIN32)
void AttachWindowsParentConsoleIfInteractive() {
  HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
  const DWORD out_type =
      (h_out != nullptr && h_out != INVALID_HANDLE_VALUE) ? GetFileType(h_out)
                                                          : FILE_TYPE_UNKNOWN;
  if (out_type != FILE_TYPE_UNKNOWN) {
    return;
  }
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
    return;
  }
  std::freopen("CONOUT$", "w", stdout);
  std::freopen("CONOUT$", "w", stderr);
  HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
  const DWORD in_type =
      (h_in != nullptr && h_in != INVALID_HANDLE_VALUE) ? GetFileType(h_in)
                                                        : FILE_TYPE_UNKNOWN;
  if (in_type == FILE_TYPE_UNKNOWN) {
    std::freopen("CONIN$", "r", stdin);
  }
  SetConsoleOutputCP(CP_UTF8);
}
#endif

void ApplyGuiLaunchEnv(const std::vector<std::string>& args) {
  if (args.empty()) {
    return;
  }
  if (args[0] != "gui" && args[0] != "open") {
    if (!args[0].empty()) {
      detail::CliSetEnvVar("DBPP_OPEN", args[0].c_str());
    }
    return;
  }
  std::string uri;
  std::string driver;
  for (std::size_t i = 1; i < args.size(); ++i) {
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    detail::CliSplitFlag(args[i], flag, inline_val);
    if (flag == "--driver" || flag == "-d") {
      if (inline_val.has_value()) {
        driver = std::string(*inline_val);
      } else if (i + 1 < args.size()) {
        driver = args[++i];
      }
      continue;
    }
    if (flag == "--uri" || flag == "-u") {
      if (inline_val.has_value()) {
        uri = std::string(*inline_val);
      } else if (i + 1 < args.size()) {
        uri = args[++i];
      }
      continue;
    }
    if (uri.empty() && !args[i].empty() && args[i].front() != '-') {
      uri = args[i];
    }
  }
  if (!uri.empty()) {
    detail::CliSetEnvVar("DBPP_OPEN", uri.c_str());
  }
  if (!driver.empty()) {
    detail::CliSetEnvVar("DBPP_DRIVER", driver.c_str());
  }
}

} // namespace

bool ShouldDispatchCli(const std::vector<std::string>& args) {
  if (args.empty()) {
    return false;
  }
  const std::string& first = args[0];
  if (first == "gui" || first == "open") {
    return detail::CliContainsHelpFlag(args);
  }
  if (args.size() == 1) {
    return detail::CliIsKnownCommand(first) || (!first.empty() && first.front() == '-');
  }
  return true;
}

int RunCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
           std::istream& in) {
  if (args.empty()) {
    detail::CliPrintTopLevelHelp(out);
    return kCliExitOk;
  }

  const bool early_json = detail::CliDetectJsonRequestedEarly(args);
  const std::string& cmd = args[0];

  if (cmd == "-h" || cmd == "--help") {
    detail::CliPrintTopLevelHelp(out);
    return kCliExitOk;
  }
  if (cmd == "help") {
    if (args.size() > 1 && args[1] != "-h" && args[1] != "--help") {
      if (!detail::CliPrintCommandHelp(args[1], out)) {
        return detail::CliEmitError(err, early_json, "未知命令: " + args[1], kCliExitUsage);
      }
      return kCliExitOk;
    }
    detail::CliPrintTopLevelHelp(out);
    return kCliExitOk;
  }
  if (cmd == "-v" || cmd == "--version") {
    return detail::CliHandleVersion({"version"}, out, err, early_json);
  }
  if (cmd == "version") {
    return detail::CliHandleVersion(args, out, err, early_json);
  }
  if (cmd == "doctor") {
    return detail::CliHandleDoctor(args, out, err, early_json);
  }
  if (cmd == "driver" || cmd == "drivers") {
    return detail::CliHandleDriver(args, out, err, early_json);
  }
  if (cmd == "profile" || cmd == "profiles") {
    return detail::CliHandleProfile(args, out, err, early_json);
  }
  if (cmd == "tables" || cmd == "objects") {
    return detail::CliHandleTables(args, out, err, early_json);
  }
  if (cmd == "schema" || cmd == "describe" || cmd == "desc") {
    return detail::CliHandleSchema(args, out, err, early_json);
  }
  if (cmd == "rows") {
    return detail::CliHandleRows(args, out, err, early_json);
  }
  if (cmd == "query" || cmd == "exec" || cmd == "sql") {
    return detail::CliHandleQueryOrExecOrSql(cmd, args, out, err, in, early_json);
  }
  if (cmd == "info") {
    return detail::CliHandleInfo(args, out, err, early_json);
  }
  if (cmd == "stats") {
    return detail::CliHandleStats(args, out, err, early_json);
  }
  if (cmd == "import") {
    return detail::CliHandleImport(args, out, err, early_json);
  }
  if (cmd == "export") {
    return detail::CliHandleExport(args, out, err, in, early_json);
  }
  if (cmd == "copy") {
    return detail::CliHandleCopy(args, out, err, early_json);
  }
  if (cmd == "gui" || cmd == "open") {
    detail::CliPrintCommandHelp(cmd, out);
    return kCliExitOk;
  }

  return detail::CliEmitError(err, early_json, "未知命令: " + cmd + " (使用 --help 查看用法)",
                   kCliExitUsage);
}

std::optional<int> TryRunCli(int argc, char** argv) {
  std::vector<std::string> args;
  if (argc > 1 && argv != nullptr) {
    args.reserve(static_cast<std::size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
      if (argv[i] != nullptr) {
        args.emplace_back(argv[i]);
      }
    }
  }

  if (!ShouldDispatchCli(args)) {
    ApplyGuiLaunchEnv(args);
    return std::nullopt;
  }

#if defined(_WIN32)
  AttachWindowsParentConsoleIfInteractive();
#endif
  return RunCli(args, std::cout, std::cerr, std::cin);
}

} // namespace dbpp
