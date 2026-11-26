#include <huxerui/app.h>

import std;
import dbpp.cli;
import dbpp.instance;

int main(int argc, char** argv) {
  if (const auto exit_code = dbpp::TryRunCli(argc, argv)) {
    return *exit_code;
  }
  if (!dbpp::AcquireSingleInstanceOrActivate(true, dbpp::PeekEnvOpenRequest())) {
    return 0;
  }
  const int exit_code = huxerui::RunApplication();
  dbpp::ReleaseSingleInstance();
  return exit_code;
}
