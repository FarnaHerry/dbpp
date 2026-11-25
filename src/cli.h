#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace dbpp {

/// Headless, agent-friendly CLI for db++.
///
/// Shares ADBC execution, saved connection profiles (`connections.tsv`), and
/// user ADBC driver discovery/installation with the desktop GUI while running
/// completely headlessly (zero GUI/display dependency). Structured JSON output
/// (`--json`) and predictable exit codes are supported across every command:
///   0 = success
///   1 = runtime / database / IO / network error
///   2 = CLI usage / invalid argument error
///
/// Desktop integration compatibility:
///   - No args (`dbpp`) or a single file path (`dbpp /path/to/db.sqlite`,
///     e.g. `.desktop` `Exec=dbpp %f`) launches the GUI instead of the CLI.
///   - `dbpp gui [file]` / `dbpp open <uri> [--driver <drv>]` also launches GUI
///     unless `--help`/`-h` is requested.

constexpr int kCliExitOk = 0;
constexpr int kCliExitError = 1;
constexpr int kCliExitUsage = 2;

/// Returns true if `args` (typically `argv[1..argc-1]`) should execute in
/// headless CLI mode rather than launching the desktop GUI.
[[nodiscard]] bool ShouldDispatchCli(const std::vector<std::string>& args);

/// Executes CLI subcommands synchronously using injected streams (used both by
/// `TryRunCli` in the main binary and by headless CTest suites).
int RunCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
           std::istream& in);

/// Platform `main()` entry hook:
/// - When `ShouldDispatchCli(args)` is true: attaches parent console on Windows
///   only if stdout is not redirected, executes `RunCli`, and returns the exit
///   code so `main()` exits immediately without initializing GUI/tray runtime.
/// - When `ShouldDispatchCli(args)` is false: sets `DBPP_OPEN` (and optional
///   `DBPP_DRIVER`) if provided and returns `std::nullopt` so `main()` proceeds
///   to `huxerui::RunApplication()`.
std::optional<int> TryRunCli(int argc, char** argv);

} // namespace dbpp
