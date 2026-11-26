export module dbpp.cli;

import std;
import dbpp.types;
import dbpp.adbc;
import dbpp.redis;
import dbpp.driver_installer;
import dbpp.db;

export namespace dbpp {

inline constexpr int kCliExitOk = 0;
inline constexpr int kCliExitError = 1;
inline constexpr int kCliExitUsage = 2;

[[nodiscard]] bool ShouldDispatchCli(const std::vector<std::string>& args);
int RunCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
           std::istream& in);
std::optional<int> TryRunCli(int argc, char** argv);

} // namespace dbpp
