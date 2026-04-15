#if defined(_WIN32)
#define LOCKING_GLASS_EXPORT extern "C" __declspec(dllexport)
#else
#define LOCKING_GLASS_EXPORT extern "C"
#endif

LOCKING_GLASS_EXPORT const char* av_version_info() {
  return "fake-ffmpeg-1.0";
}
