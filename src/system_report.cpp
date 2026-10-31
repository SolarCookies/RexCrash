/**
 * @file        core/system_report.cpp
 * @brief       Host hardware and OS inventory
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "system_report.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/string.h>
#include <rex/string/utf8.h>
#include <rex/thread.h>

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <dxgi.h>
#include <intrin.h>
#else
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <filesystem>
#endif

namespace rexcrash {

namespace {

#if REX_PLATFORM_WIN32
// wchar_t and char16_t are distinct types even where both are 16 bits, so the
// Win32 strings need a reinterpret before the shared UTF-8 conversion.
std::string WideToUtf8(const wchar_t* text) {
  if (!text)
    return {};
  return rex::string::to_utf8(std::u16string_view(reinterpret_cast<const char16_t*>(text), std::wcslen(text)));
}
#endif

std::string Trim(std::string_view text) {
  const size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos)
    return {};
  const size_t end = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(begin, end - begin + 1));
}

#if REX_PLATFORM_WIN32

// The x86 brand string, which is what users actually recognise. CPUID leaves
// 0x80000002..4 hold it as three 16-byte chunks.
std::string QueryCpuBrand() {
  std::array<int, 4> regs{};
  __cpuid(regs.data(), 0x80000000);
  if (static_cast<unsigned>(regs[0]) < 0x80000004u)
    return {};

  char brand[49] = {};
  for (unsigned leaf = 0; leaf < 3; ++leaf) {
    __cpuid(regs.data(), static_cast<int>(0x80000002u + leaf));
    std::memcpy(brand + leaf * 16, regs.data(), 16);
  }
  return Trim(brand);
}

uint32_t QueryPhysicalCoreCount() {
  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
  if (!length || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    return 0;

  std::vector<uint8_t> buffer(length);
  auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length))
    return 0;

  uint32_t cores = 0;
  for (DWORD offset = 0; offset < length;) {
    auto* entry = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
    if (entry->Relationship == RelationProcessorCore)
      ++cores;
    offset += entry->Size;
  }
  return cores;
}

std::string ReadRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
  wchar_t data[512] = {};
  DWORD size = sizeof(data);
  if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, data, &size) != ERROR_SUCCESS)
    return {};
  return WideToUtf8(data);
}

uint32_t ReadRegistryDword(HKEY root, const wchar_t* subkey, const wchar_t* value) {
  DWORD data = 0;
  DWORD size = sizeof(data);
  if (RegGetValueW(root, subkey, value, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS)
    return 0;
  return data;
}

// GetVersionEx reports whatever the app manifest claims to support, so go to
// the kernel's own RtlGetVersion for the real numbers and to the registry for
// the marketing name.
void QueryWindowsVersion(SystemReport& report) {
  constexpr const wchar_t* kCurrentVersion =
      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

  const std::string product = ReadRegistryString(HKEY_LOCAL_MACHINE, kCurrentVersion,
                                                 L"ProductName");
  const std::string display = ReadRegistryString(HKEY_LOCAL_MACHINE, kCurrentVersion,
                                                 L"DisplayVersion");
  const std::string build = ReadRegistryString(HKEY_LOCAL_MACHINE, kCurrentVersion,
                                               L"CurrentBuildNumber");
  const uint32_t revision = ReadRegistryDword(HKEY_LOCAL_MACHINE, kCurrentVersion, L"UBR");

  RTL_OSVERSIONINFOEXW version = {};
  version.dwOSVersionInfoSize = sizeof(version);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOEXW*);
    if (auto rtl_get_version =
            reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
      rtl_get_version(&version);
    }

    // Wine (and therefore Proton) exports this; native Windows never does.
    using WineGetVersionFn = const char*(__cdecl*)();
    if (auto wine_get_version =
            reinterpret_cast<WineGetVersionFn>(GetProcAddress(ntdll, "wine_get_version"))) {
      report.compatibility = fmt::format("Wine {}", wine_get_version());
    }
  }

  // ProductName still says "Windows 10" on 11; the build number is the truth.
  std::string name = product.empty() ? "Windows" : product;
  if (version.dwBuildNumber >= 22000 && name.find("Windows 10") != std::string::npos)
    name.replace(name.find("Windows 10"), 10, "Windows 11");

  report.os = name;
  if (!display.empty())
    report.os += " " + display;
  if (!build.empty()) {
    report.os += revision ? fmt::format(" (build {}.{})", build, revision)
                          : fmt::format(" (build {})", build);
  }
  if (version.dwMajorVersion) {
    report.kernel = fmt::format("NT {}.{}.{}", version.dwMajorVersion, version.dwMinorVersion,
                                version.dwBuildNumber);
  }
}

// DXGI is loaded by name rather than linked: this file is compiled into every
// consumer of rexcore, including tool-mode targets that never touch a GPU.
void QueryGpus(SystemReport& report) {
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  if (!dxgi)
    return;

  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  auto create_factory =
      reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
  if (!create_factory) {
    FreeLibrary(dxgi);
    return;
  }

  IDXGIFactory1* factory = nullptr;
  if (SUCCEEDED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    // One physical GPU is enumerated once per WDDM path, so a machine with a
    // virtual display driver installed lists the same card two or three times,
    // each with its own LUID. Key on the PCI identity instead, which is what
    // actually names the hardware.
    std::vector<std::array<uint32_t, 4>> seen;

    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
      DXGI_ADAPTER_DESC1 desc = {};
      const bool usable =
          SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);

      const std::array<uint32_t, 4> identity{desc.VendorId, desc.DeviceId, desc.SubSysId,
                                             desc.Revision};
      if (usable && std::find(seen.begin(), seen.end(), identity) == seen.end()) {
        seen.push_back(identity);

        GpuReport gpu;
        gpu.name = WideToUtf8(desc.Description);
        gpu.dedicated_vram = desc.DedicatedVideoMemory;
        gpu.shared_memory = desc.SharedSystemMemory;

        LARGE_INTEGER umd = {};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
          gpu.driver_version = fmt::format("{}.{}.{}.{}", HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                                           HIWORD(umd.LowPart), LOWORD(umd.LowPart));
        }
        report.gpus.push_back(std::move(gpu));
      }
      adapter->Release();
      adapter = nullptr;
    }
    factory->Release();
  }
  FreeLibrary(dxgi);
}

#else  // !REX_PLATFORM_WIN32

std::string ReadFirstLine(const std::filesystem::path& path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return Trim(line);
}

// Value of `key` in a `key=value` / `key : value` file such as os-release or
// /proc/cpuinfo, with surrounding quotes stripped.
std::string ReadKeyedValue(const std::filesystem::path& path, std::string_view key,
                           char separator) {
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    const size_t sep = line.find(separator);
    if (sep == std::string::npos)
      continue;
    if (Trim(std::string_view(line).substr(0, sep)) != key)
      continue;
    std::string value = Trim(std::string_view(line).substr(sep + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    return value;
  }
  return {};
}

void QueryLinuxVersion(SystemReport& report) {
  report.os = ReadKeyedValue("/etc/os-release", "PRETTY_NAME", '=');
  if (report.os.empty())
    report.os = "Linux";

  utsname uts = {};
  if (uname(&uts) == 0)
    report.kernel = fmt::format("{} {}", uts.sysname, uts.release);
}

// DRM exposes the bound driver but not a marketing name, so report the driver
// plus the PCI ID and let the reader look it up.
void QueryGpus(SystemReport& report) {
  std::error_code ec;
  for (const auto& card : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
    const std::string name = card.path().filename().string();
    if (!name.starts_with("card") || name.find('-') != std::string::npos)
      continue;

    const auto device = card.path() / "device";
    const std::string driver = ReadKeyedValue(device / "uevent", "DRIVER", '=');
    const std::string vendor = ReadFirstLine(device / "vendor");
    const std::string product = ReadFirstLine(device / "device");
    if (vendor.empty() && driver.empty())
      continue;

    GpuReport gpu;
    gpu.name = driver.empty() ? "unknown" : driver;
    if (!vendor.empty() && !product.empty())
      gpu.name += fmt::format(" [{}:{}]", vendor, product);
    report.gpus.push_back(std::move(gpu));
  }
}

#endif  // REX_PLATFORM_WIN32

}  // namespace

std::string FormatBytes(uint64_t bytes) {
  constexpr std::array<const char*, 5> kUnits = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < kUnits.size()) {
    value /= 1024.0;
    ++unit;
  }
  return unit == 0 ? fmt::format("{} {}", bytes, kUnits[unit])
                   : fmt::format("{:.1f} {}", value, kUnits[unit]);
}

SystemReport QuerySystemReport() {
  SystemReport report;
  report.cpu_threads = rex::thread::logical_processor_count();

#if REX_PLATFORM_WIN32
  QueryWindowsVersion(report);
  report.cpu = QueryCpuBrand();
  report.cpu_cores = QueryPhysicalCoreCount();

  MEMORYSTATUSEX memory = {};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) {
    report.ram_total = memory.ullTotalPhys;
    report.ram_available = memory.ullAvailPhys;
  }

  wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
  if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH))
    report.locale = WideToUtf8(locale);
#else
  QueryLinuxVersion(report);
  report.cpu = ReadKeyedValue("/proc/cpuinfo", "model name", ':');
  if (report.cpu.empty())
    report.cpu = ReadKeyedValue("/proc/cpuinfo", "Model", ':');

  const long cores = sysconf(_SC_NPROCESSORS_CONF);
  if (cores > 0)
    report.cpu_cores = static_cast<uint32_t>(cores);

  struct sysinfo info = {};
  if (sysinfo(&info) == 0) {
    report.ram_total = static_cast<uint64_t>(info.totalram) * info.mem_unit;
    report.ram_available = static_cast<uint64_t>(info.freeram) * info.mem_unit;
  }

  if (const char* lang = std::getenv("LANG"))
    report.locale = lang;
#endif

  QueryGpus(report);
  return report;
}

const SystemReport& GetSystemReport() {
  static const SystemReport report = QuerySystemReport();
  return report;
}

void LogSystemReport(const SystemReport& report) {
  REXLOG_INFO("System:");
  REXLOG_INFO("  OS:      {}", report.os.empty() ? "unknown" : report.os);
  if (!report.kernel.empty())
    REXLOG_INFO("  Kernel:  {}", report.kernel);
  if (!report.compatibility.empty())
    REXLOG_INFO("  Layer:   {}", report.compatibility);

  if (report.cpu_cores) {
    REXLOG_INFO("  CPU:     {} ({} cores / {} threads)",
                report.cpu.empty() ? "unknown" : report.cpu, report.cpu_cores,
                report.cpu_threads);
  } else {
    REXLOG_INFO("  CPU:     {} ({} threads)", report.cpu.empty() ? "unknown" : report.cpu,
                report.cpu_threads);
  }

  if (report.ram_total) {
    REXLOG_INFO("  RAM:     {} total, {} free at startup", FormatBytes(report.ram_total),
                FormatBytes(report.ram_available));
  }

  if (report.gpus.empty()) {
    REXLOG_INFO("  GPU:     none detected");
  }
  for (size_t i = 0; i < report.gpus.size(); ++i) {
    const auto& gpu = report.gpus[i];
    std::string line = fmt::format("  GPU {}:   {}", i, gpu.name);
    if (gpu.dedicated_vram)
      line += fmt::format(", {} VRAM", FormatBytes(gpu.dedicated_vram));
    if (!gpu.driver_version.empty())
      line += fmt::format(", driver {}", gpu.driver_version);
    REXLOG_INFO("{}", line);
  }

  if (!report.locale.empty())
    REXLOG_INFO("  Locale:  {}", report.locale);
}

}  // namespace rexcrash
