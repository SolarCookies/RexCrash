/**
 * @file        reporter/ui_imgui.cpp
 * @brief       The crash reporter window: ImGui over Win32 and Direct3D 11
 *
 * @remarks     This program runs because something already went badly wrong,
 *              quite possibly in the graphics driver, so it must degrade rather
 *              than fail. Direct3D 11 is tried on the GPU first and then on
 *              WARP, Windows' own software rasteriser, which works with no
 *              usable GPU at all. When neither starts, RunWin32Ui's plain
 *              controls take over. Vulkan has no software fallback of its own,
 *              and it is the driver stack the game was using when it died.
 */

#include "util.h"

#include <string>
#include <vector>

#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "reporter.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace reporter {

namespace {

bool g_quit = false;
bool g_restart = false;

void ApplyStyle() {
  ImGuiStyle& style = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  style.WindowRounding = 0.0f;
  style.FrameRounding = 3.0f;
  style.GrabRounding = 3.0f;
  style.ScrollbarRounding = 3.0f;
  style.WindowPadding = ImVec2(16, 14);
  style.FramePadding = ImVec2(8, 5);
  style.ItemSpacing = ImVec2(8, 8);
}

void DrawReadOnly(const char* id, const std::string& text, float height) {
  // InputTextMultiline rather than TextUnformatted so the player can select and
  // copy any part of it.
  ImGui::InputTextMultiline(id, const_cast<char*>(text.c_str()), text.size() + 1,
                            ImVec2(-FLT_MIN, height), ImGuiInputTextFlags_ReadOnly);
}

void DrawUi(const std::string& headline, std::vector<char>& message, bool& auto_send,
            std::string& status) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("##crash", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                   | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

  // A bug report is not a failure, so it does not get the alarming red.
  const ImVec4 headline_colour = g_session.bug_report ? ImVec4(0.80f, 0.85f, 0.95f, 1.0f)
                                                      : ImVec4(1.00f, 0.42f, 0.38f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, headline_colour);
  ImGui::TextUnformatted(headline.c_str());
  ImGui::PopStyleColor();

  // Says what is true right now rather than what was true on arrival: telling
  // someone nothing has been sent immediately after they sent it reads like the
  // send failed.
  if (g_session.sent) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.55f, 1.0f));
    ImGui::TextUnformatted(g_session.bug_report ? "Thank you, your report is on its way."
                                                : "Your report has been sent. Thank you.");
    ImGui::PopStyleColor();
  } else if (!g_session.can_send()) {
    ImGui::TextDisabled("No crash report was captured, so there is nothing to send.");
  } else if (g_session.bug_report) {
    ImGui::TextDisabled("Describe the problem below. The log is attached as it stands now.");
  } else {
    ImGui::TextDisabled("Nothing has been sent. Look over what would be reported below.");
  }
  ImGui::Spacing();

  const float footer = ImGui::GetFrameHeightWithSpacing() * 5.0f + 40.0f;
  if (ImGui::BeginTabBar("##views")) {
    // Nothing crashed in bug report mode, so there is no exception to explain.
    if (!g_session.bug_report && ImGui::BeginTabItem("Crash details")) {
      DrawReadOnly("##details", g_session.details, -footer);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Log")) {
      DrawReadOnly("##log", g_session.log, -footer);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::Spacing();
  ImGui::BeginDisabled(g_session.sent);
  ImGui::TextUnformatted(g_session.bug_report
                             ? "What went wrong?"
                             : "What were you doing when it crashed? (optional)");
  ImGui::InputTextMultiline("##message", message.data(), message.size(),
                            ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 2.2f));

  if (!g_session.reporting_enabled && !g_session.bug_report) {
    ImGui::Checkbox("Send future crash reports automatically", &auto_send);
  }
  ImGui::EndDisabled();

  ImGui::Spacing();

  const std::string typed(message.data());
  const bool empty_message = typed.empty();
  // A bug report with nothing written in it is just a log dump. The crash path
  // has no such requirement, because there the crash itself is the content.
  const bool sendable =
      g_session.can_send() && !g_session.sent && !(g_session.bug_report && empty_message);

  ImGui::BeginDisabled(!sendable);
  if (ImGui::Button("Send report", ImVec2(150, 34))) {
    if (g_session.bug_report) {
      if (WriteMessageForGame(typed)) {
        g_session.sent = true;
        g_quit = true;  // The game does the sending; nothing left to show here.
      } else {
        status = "Could not hand the report back to the game.";
      }
    } else if (SendReport(typed)) {
      g_session.sent = true;
      if (auto_send) {
        EnableReportingInConfig();
      }
      status.clear();  // The subtitle now carries the outcome.
    } else {
      status = "Could not send the report.";
    }
  }
  ImGui::EndDisabled();

  if (!g_session.bug_report) {
    ImGui::SameLine();
    ImGui::BeginDisabled(g_session.game_exe.empty());
    if (ImGui::Button("Restart", ImVec2(120, 34))) {
      g_restart = true;
      g_quit = true;
    }
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  if (ImGui::Button(g_session.bug_report ? "Cancel" : "Quit", ImVec2(120, 34))) {
    g_quit = true;
  }

  ImGui::SameLine();
  if (ImGui::Button("Copy details", ImVec2(130, 34))) {
    const std::string all =
        g_session.bug_report ? g_session.log : g_session.details + "\n\n" + g_session.log;
    ImGui::SetClipboardText(all.c_str());
    status = "Copied to the clipboard.";
  }

  if (g_session.bug_report && empty_message && !g_session.sent) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Type a description to enable sending.");
  }

  if (!status.empty()) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", status.c_str());
  }

  ImGui::End();
}

struct Direct3D {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGISwapChain* swap_chain = nullptr;
  ID3D11RenderTargetView* target = nullptr;

  bool Create(HWND window) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    // The GPU, then WARP when the GPU or its driver won't start.
    for (const D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
      D3D_FEATURE_LEVEL level;
      const HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context);
      if (SUCCEEDED(result)) {
        CreateTarget();
        return true;
      }
      g_session.window_error = "D3D11CreateDeviceAndSwapChain failed: " + std::to_string(static_cast<unsigned long>(result));
    }
    return false;
  }

  void CreateTarget() {
    ID3D11Texture2D* back_buffer = nullptr;
    swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    device->CreateRenderTargetView(back_buffer, nullptr, &target);
    back_buffer->Release();
  }

  void Resize(UINT width, UINT height) {
    if (!swap_chain || width == 0 || height == 0) return;
    if (target) target->Release();
    target = nullptr;
    swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateTarget();
  }

  void Destroy() {
    if (target) target->Release();
    if (swap_chain) swap_chain->Release();
    if (context) context->Release();
    if (device) device->Release();
  }
};

Direct3D g_d3d;

LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (ImGui_ImplWin32_WndProcHandler(window, msg, wparam, lparam)) return true;
  switch (msg) {
    case WM_SIZE:
      if (wparam != SIZE_MINIMIZED) g_d3d.Resize(LOWORD(lparam), HIWORD(lparam));
      return 0;
    case WM_CLOSE:
      g_quit = true;
      return 0;
    default:
      return DefWindowProcW(window, msg, wparam, lparam);
  }
}

}  // namespace

bool RunImGuiUi() {
  ImGui_ImplWin32_EnableDpiAwareness();
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class = {sizeof(window_class)};
  window_class.style = CS_CLASSDC;
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"rexcrashreporter";
  RegisterClassExW(&window_class);

  const std::string title = g_session.app_name + (g_session.bug_report ? " - bug report" : " - crash report");
  const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
  HWND window = CreateWindowExW(0, window_class.lpszClassName, Widen(title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(900 * scale), static_cast<int>(700 * scale), nullptr, nullptr, instance, nullptr);
  if (!window || !g_d3d.Create(window)) {
    if (!window) g_session.window_error = "CreateWindowExW failed";
    g_d3d.Destroy();
    if (window) DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, instance);
    return false;
  }
  ShowWindow(window, SW_SHOWDEFAULT);
  UpdateWindow(window);
  SetForegroundWindow(window);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;  // Never leave settings files behind.
  ApplyStyle();
  ImGui::GetStyle().ScaleAllSizes(scale);
  ImGui::GetStyle().FontScaleDpi = scale;
  ImGui_ImplWin32_Init(window);
  ImGui_ImplDX11_Init(g_d3d.device, g_d3d.context);

  const std::string headline = g_session.bug_report ? std::string("Send a bug report") : g_session.app_name + " has crashed.";
  std::vector<char> message(4096, '\0');
  bool auto_send = false;
  std::string status;

  while (!g_quit) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) g_quit = true;
    }
    if (g_quit) break;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUi(headline, message, auto_send, status);
    ImGui::Render();

    const float clear[4] = {0.09f, 0.09f, 0.11f, 1.0f};
    g_d3d.context->OMSetRenderTargets(1, &g_d3d.target, nullptr);
    g_d3d.context->ClearRenderTargetView(g_d3d.target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_d3d.swap_chain->Present(1, 0);
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  g_d3d.Destroy();
  DestroyWindow(window);
  UnregisterClassW(window_class.lpszClassName, instance);

  // Only a crash leaves a report on disk to clean up.
  if (!g_session.sent && !g_session.bug_report) {
    DiscardReport();
  }
  if (g_restart) {
    RestartGame();
  }
  return true;
}

}  // namespace reporter
