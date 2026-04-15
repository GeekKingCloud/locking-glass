#include "locking_glass/core/runtime.h"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  const auto runtime = locking_glass::core::BuildRuntime();
  const auto diagnostics = locking_glass::core::CollectStartupDiagnostics(runtime);

  if (argc > 1 && std::string_view(argv[1]) == "--self-check") {
    std::cout << locking_glass::core::FormatDiagnostics(diagnostics);
    return 0;
  }

  std::cout << "LockingGlass scaffold bootstrap\n";
  std::cout << locking_glass::core::FormatDiagnostics(diagnostics);
  return 0;
}
