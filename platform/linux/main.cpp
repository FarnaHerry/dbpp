#include <huxerui/app.h>

#include <cstdlib>

int main(int argc, char** argv) {
  if (argc > 1 && argv[1] != nullptr && *argv[1] != '\0') {
    setenv("DBPP_OPEN", argv[1], 1);
  }
  return huxerui::RunApplication();
}
