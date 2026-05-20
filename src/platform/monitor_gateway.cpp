#include "locking_glass/platform/monitor_gateway.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace locking_glass::platform {

namespace {

#if defined(_WIN32)
std::string Narrow(const std::wstring& value) {
  if (value.empty()) {
    return "";
  }

  const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr,
                                       0, nullptr, nullptr);
  if (size <= 1) {
    return "";
  }

  std::string buffer(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, buffer.data(), size,
                      nullptr, nullptr);
  buffer.pop_back();
  return buffer;
}

std::string Narrow(const wchar_t* value) {
  if (value == nullptr || value[0] == L'\0') {
    return "";
  }
  return Narrow(std::wstring(value));
}

struct DisplayIdentity {
  std::string stable_id;
  std::string device_path;
  std::string display_name;
};

std::string FormatFallbackStableId(const std::wstring& source_name,
                                   const LUID adapter_id,
                                   const UINT32 target_id) {
  std::ostringstream builder;
  builder << "display:"
          << static_cast<unsigned long>(adapter_id.HighPart) << ':'
          << adapter_id.LowPart << ':' << target_id;
  if (!source_name.empty()) {
    builder << ':' << Narrow(source_name);
  }
  return builder.str();
}

std::vector<DISPLAYCONFIG_PATH_INFO> QueryActiveDisplayPaths() {
  for (int attempt = 0; attempt < 4; ++attempt) {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count,
                                    &mode_count) != ERROR_SUCCESS) {
      return {};
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    const LONG query_result =
        QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                           &mode_count, modes.data(), nullptr);
    if (query_result == ERROR_SUCCESS) {
      paths.resize(path_count);
      return paths;
    }

    // Display topology can change between the size query and the real query;
    // retry the documented race instead of treating it as no monitors.
    if (query_result != ERROR_INSUFFICIENT_BUFFER) {
      return {};
    }
  }

  return {};
}

std::vector<std::pair<std::wstring, DisplayIdentity>> CollectDisplayIdentities() {
  std::vector<std::pair<std::wstring, DisplayIdentity>> identities;

  // QueryDisplayConfig gives stable target identity; EnumDisplayMonitors later
  // gives live bounds. The GDI source name is the join key between those APIs.
  for (const auto& path : QueryActiveDisplayPaths()) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name{};
    source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source_name.header.size = sizeof(source_name);
    source_name.header.adapterId = path.sourceInfo.adapterId;
    source_name.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS) {
      continue;
    }

    DISPLAYCONFIG_TARGET_DEVICE_NAME target_name{};
    target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target_name.header.size = sizeof(target_name);
    target_name.header.adapterId = path.targetInfo.adapterId;
    target_name.header.id = path.targetInfo.id;

    const LONG target_result = DisplayConfigGetDeviceInfo(&target_name.header);
    const std::wstring gdi_name(source_name.viewGdiDeviceName);

    DisplayIdentity identity;
    if (target_result == ERROR_SUCCESS) {
      identity.device_path = Narrow(target_name.monitorDevicePath);
      identity.display_name = Narrow(target_name.monitorFriendlyDeviceName);
    }

    if (identity.display_name.empty()) {
      identity.display_name = Narrow(gdi_name);
    }

    identity.stable_id =
        !identity.device_path.empty()
            ? identity.device_path
            : FormatFallbackStableId(gdi_name, path.targetInfo.adapterId,
                                     path.targetInfo.id);

    identities.emplace_back(gdi_name, std::move(identity));
  }

  return identities;
}

struct MonitorCollectionContext {
  std::vector<MonitorDescriptor>* monitors = nullptr;
  const std::vector<std::pair<std::wstring, DisplayIdentity>>* identities =
      nullptr;
};

const DisplayIdentity* FindIdentity(
    const MonitorCollectionContext& context,
    const std::wstring& source_name) {
  for (const auto& [candidate_name, candidate_identity] :
       *context.identities) {
    if (candidate_name == source_name) {
      return &candidate_identity;
    }
  }
  return nullptr;
}

BOOL CALLBACK CaptureMonitor(HMONITOR monitor, HDC, LPRECT,
                             LPARAM raw_context) {
  auto* context = reinterpret_cast<MonitorCollectionContext*>(raw_context);

  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) {
    return TRUE;
  }

  const std::wstring source_name(info.szDevice);
  const DisplayIdentity* identity = FindIdentity(*context, source_name);
  const std::string fallback_name = Narrow(source_name);
  const RECT& bounds = info.rcMonitor;

  context->monitors->push_back(MonitorDescriptor{
      .stable_id =
          identity != nullptr ? identity->stable_id : "gdi:" + fallback_name,
      .device_path =
          identity != nullptr && !identity->device_path.empty()
              ? identity->device_path
              : fallback_name,
      .edid_serial = "",
      .display_name =
          identity != nullptr && !identity->display_name.empty()
              ? identity->display_name
              : fallback_name,
      .label = "",
      .bounds =
          MonitorBounds{
              .left = bounds.left,
              .top = bounds.top,
              .right = bounds.right,
              .bottom = bounds.bottom,
          },
      .is_primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0U,
  });
  return TRUE;
}
#endif

#if defined(_WIN32)
void SortAndLabelMonitors(std::vector<MonitorDescriptor>* monitors) {
  std::sort(monitors->begin(), monitors->end(),
            [](const MonitorDescriptor& left, const MonitorDescriptor& right) {
              if (left.bounds.top != right.bounds.top) {
                return left.bounds.top < right.bounds.top;
              }
              if (left.bounds.left != right.bounds.left) {
                return left.bounds.left < right.bounds.left;
              }
              if (left.is_primary != right.is_primary) {
                return left.is_primary;
              }
              if (left.bounds.bottom != right.bounds.bottom) {
                return left.bounds.bottom < right.bounds.bottom;
              }
              if (left.bounds.right != right.bounds.right) {
                return left.bounds.right < right.bounds.right;
              }
              return left.stable_id < right.stable_id;
            });

  for (std::size_t index = 0; index < monitors->size(); ++index) {
    (*monitors)[index].label = "Display " + std::to_string(index + 1);
  }
}
#endif

class MonitorGatewayImpl final : public MonitorGateway {
 public:
  std::vector<MonitorDescriptor> Enumerate() const override {
#if defined(_WIN32)
    std::vector<MonitorDescriptor> monitors;
    const auto identities = CollectDisplayIdentities();
    MonitorCollectionContext context{
        .monitors = &monitors,
        .identities = &identities,
    };
    EnumDisplayMonitors(nullptr, nullptr, CaptureMonitor,
                        reinterpret_cast<LPARAM>(&context));
    SortAndLabelMonitors(&monitors);
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
