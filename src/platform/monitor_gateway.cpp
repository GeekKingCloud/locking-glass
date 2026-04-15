#include "locking_glass/platform/monitor_gateway.h"

#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace locking_glass::platform {

namespace {

#if defined(_WIN32)
std::string Narrow(const wchar_t* value) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return "unknown-monitor";
  }

  std::string buffer(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, buffer.data(), size, nullptr, nullptr);
  buffer.pop_back();
  return buffer;
}

struct MonitorCollectionContext {
  std::vector<MonitorDescriptor>* monitors;
  int next_index;
};

BOOL CALLBACK CaptureMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM raw_context) {
  auto* context = reinterpret_cast<MonitorCollectionContext*>(raw_context);

  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) {
    return TRUE;
  }

  const RECT& bounds = info.rcMonitor;
  context->monitors->push_back(MonitorDescriptor{
      .stable_id = Narrow(info.szDevice),
      .label = "Display " + std::to_string(context->next_index),
      .bounds =
          MonitorBounds{
              .left = bounds.left,
              .top = bounds.top,
              .right = bounds.right,
              .bottom = bounds.bottom,
          },
      .is_primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0U,
  });
  ++context->next_index;
  return TRUE;
}
#endif

class MonitorGatewayImpl final : public MonitorGateway {
 public:
  std::vector<MonitorDescriptor> Enumerate() const override {
#if defined(_WIN32)
    std::vector<MonitorDescriptor> monitors;
    MonitorCollectionContext context{
        .monitors = &monitors,
        .next_index = 1,
    };
    EnumDisplayMonitors(nullptr, nullptr, CaptureMonitor,
                        reinterpret_cast<LPARAM>(&context));
    return monitors;
#else
    return {};
#endif
  }
};

}  // namespace

std::unique_ptr<MonitorGateway> CreateMonitorGateway() {
  return std::make_unique<MonitorGatewayImpl>();
}

}  // namespace locking_glass::platform
