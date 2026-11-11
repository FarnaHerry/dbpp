#include <huxerui/app.h>

#include <cstdlib>

int main(int argc, char** argv) {
  if (argc > 1 && argv[1] != nullptr && *argv[1] != '\0') {
    _putenv_s("DBPP_OPEN", argv[1]);
  }
  return huxerui::RunApplication();
}
