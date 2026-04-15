#include "locking_glass/integration/ffmpeg_probe.h"

#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace locking_glass::integration {

namespace {

#if defined(_WIN32)
using LibraryHandle = HMODULE;
#else
using LibraryHandle = void*;
#endif

std::vector<std::string> BuildLibraryCandidates() {
  std::vector<std::string> candidates;

  if (const char* direct_library = std::getenv("LOCKING_GLASS_FFMPEG_LIBRARY");
      direct_library != nullptr && direct_library[0] != '\0') {
    candidates.emplace_back(direct_library);
  }

  if (const char* home = std::getenv("LOCKING_GLASS_FFMPEG_HOME");
      home != nullptr && home[0] != '\0') {
#if defined(_WIN32)
    candidates.emplace_back(std::string(home) + "\\avutil-59.dll");
    candidates.emplace_back(std::string(home) + "\\avutil-58.dll");
#else
    candidates.emplace_back(std::string(home) + "/libavutil.so.59");
    candidates.emplace_back(std::string(home) + "/libavutil.so.58");
#endif
  }

#if defined(_WIN32)
  candidates.emplace_back("avutil-59.dll");
  candidates.emplace_back("avutil-58.dll");
  candidates.emplace_back("avutil-57.dll");
#else
  candidates.emplace_back("libavutil.so.59");
  candidates.emplace_back("libavutil.so.58");
  candidates.emplace_back("libavutil.so");
#endif

  return candidates;
}

LibraryHandle OpenLibrary(const std::string& candidate) {
#if defined(_WIN32)
  return LoadLibraryA(candidate.c_str());
#else
  return dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* LoadSymbol(const LibraryHandle handle, const char* symbol_name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(handle, symbol_name));
#else
  return dlsym(handle, symbol_name);
#endif
}

void CloseLibrary(const LibraryHandle handle) {
#if defined(_WIN32)
  if (handle != nullptr) {
    FreeLibrary(handle);
  }
#else
  if (handle != nullptr) {
    dlclose(handle);
  }
#endif
}

std::string JoinCandidates(const std::vector<std::string>& candidates) {
  std::ostringstream builder;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (index != 0U) {
      builder << ", ";
    }
    builder << candidates[index];
  }
  return builder.str();
}

class FfmpegProbeImpl final : public FfmpegProbe {
 public:
  CapabilityReport Probe() const override {
    const std::vector<std::string> candidates = BuildLibraryCandidates();
    std::vector<std::string> probe_notes;

    using AvVersionInfo = const char* (*)();
    for (const auto& candidate : candidates) {
      const LibraryHandle handle = OpenLibrary(candidate);
      if (handle == nullptr) {
        probe_notes.push_back(candidate + ": not loadable");
        continue;
      }

      const auto version_symbol =
          reinterpret_cast<AvVersionInfo>(LoadSymbol(handle, "av_version_info"));
      if (version_symbol == nullptr) {
        probe_notes.push_back(candidate + ": missing av_version_info");
        CloseLibrary(handle);
        continue;
      }

      const std::string version = version_symbol();
      CloseLibrary(handle);
      return CapabilityReport{
          .component = "ffmpeg",
          .status = CapabilityStatus::kReady,
          .detail = "Loaded " + candidate + " (av_version_info=" + version + ").",
      };
    }

    return CapabilityReport{
        .component = "ffmpeg",
        .status = CapabilityStatus::kUnavailable,
        .detail = "No FFmpeg avutil runtime found. Checked: " + JoinCandidates(candidates) +
                  ". Probe notes: " + JoinCandidates(probe_notes) + ".",
    };
  }
};

}  // namespace

std::unique_ptr<FfmpegProbe> CreateFfmpegProbe() {
  return std::make_unique<FfmpegProbeImpl>();
}

}  // namespace locking_glass::integration
