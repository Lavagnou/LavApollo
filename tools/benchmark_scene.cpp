/**
 * @file tools/benchmark_scene.cpp
 * @brief The animated scene LavApollo's built-in "Benchmark" app shows.
 *
 * LavArtemis clients stream this while they compare codecs, bitrates and frame pacing modes. For
 * their runs to be comparable, every run must show the same images: everything drawn here is a
 * function of the frame number alone, never of the wall clock, so two runs at the same rate encode
 * the same frames whatever the host was doing in between.
 *
 * The content loads an encoder the way a game does: a continuous pan over fine detail, a rotating
 * zone plate, scrolling "text" lines and moving particles, plus film grain that changes every frame.
 * It alternates a calm and a busy phase every 5 seconds so that bitrate makes a visible difference.
 *
 * The scene paces itself at the stream rate (`SUNSHINE_CLIENT_FPS`) with a high-resolution timer
 * rather than following the display: a VRR client's virtual display refreshes at 1000 Hz.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
  const char *const kShader = R"hlsl(
cbuffer Frame : register(b0) {
  float time;        // frame number / rate: the only input that moves anything
  float busy;        // 0 in the calm phase, 1 in the busy one, eased in between
  float2 resolution;
  uint frame;
  uint3 pad;
};

struct VSOut {
  float4 pos : SV_Position;
};

VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

float hash12(float2 p) {
  float3 p3 = frac(float3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return frac((p3.x + p3.y) * p3.z);
}

float value_noise(float2 p) {
  float2 i = floor(p);
  float2 f = frac(p);
  float2 u = f * f * (3.0 - 2.0 * f);
  return lerp(lerp(hash12(i), hash12(i + float2(1, 0)), u.x),
              lerp(hash12(i + float2(0, 1)), hash12(i + float2(1, 1)), u.x), u.y);
}

float3 palette(float t) {
  return 0.5 + 0.5 * cos(6.28318 * (t + float3(0.0, 0.33, 0.67)));
}

float4 ps_main(VSOut input) : SV_Target {
  float2 px = input.pos.xy;
  float2 uv = px / resolution.y;          // height-normalised: same look at any aspect ratio
  float aspect = resolution.x / resolution.y;

  // Background: a terrain of fine detail, panning continuously, faster when busy
  float pan_speed = lerp(0.04, 0.22, busy);
  float2 p = uv * 6.0 + float2(time * pan_speed * 6.0, time * pan_speed * 2.0);
  float n = value_noise(p) * 0.55 + value_noise(p * 4.0) * 0.3 + value_noise(p * 16.0) * 0.15;
  float3 col = palette(n * 0.8 + 0.1) * (0.35 + 0.65 * n);

  // Fine checker that only shows up close to the edges of the noise: high frequencies to encode
  float2 checker = floor(px / 3.0);
  col *= 0.9 + 0.1 * fmod(checker.x + checker.y, 2.0);

  // Rotating zone plate in the right third
  float2 zc = float2(aspect * 0.78, 0.5);
  float2 zd = uv - zc;
  float zr = length(zd);
  if (zr < 0.3) {
    float angle = time * lerp(0.4, 1.6, busy);
    float2 rd = float2(zd.x * cos(angle) - zd.y * sin(angle), zd.x * sin(angle) + zd.y * cos(angle));
    float zone = 0.5 + 0.5 * cos(900.0 * dot(rd, rd) + 40.0 * rd.x);
    col = lerp(col, float3(zone, zone, zone), smoothstep(0.3, 0.28, zr));
  }

  // Scrolling "text": rows of word-like blocks in the left third, like a chat or a menu
  if (uv.x > 0.04 && uv.x < aspect * 0.32) {
    float line_h = 0.028;
    float scroll = time * lerp(0.05, 0.25, busy);
    float y = uv.y + scroll;
    float row = floor(y / line_h);
    float in_row = frac(y / line_h);
    float glyph = floor((uv.x - 0.04) / 0.011);
    float word = floor((uv.x - 0.04) / 0.07);
    float present = step(0.25, hash12(float2(word, row))) * step(hash12(float2(row, 7.0)) * 0.9, (uv.x - 0.04) / (aspect * 0.28));
    float shape = step(0.2, in_row) * step(in_row, 0.75) * step(0.35, hash12(float2(glyph, row * 3.1)));
    float2 cell = frac(float2((uv.x - 0.04) / 0.011, in_row * 3.0));
    float stroke = step(0.2, cell.x) * step(cell.x, 0.8) * step(0.15, cell.y);
    col = lerp(col * 0.35, float3(0.95, 0.95, 0.9), present * shape * stroke);
  }

  // Particles: 48 bright points on fixed-seed trajectories
  int count = (int)lerp(16.0, 48.0, busy);
  for (int i = 0; i < count; i++) {
    float fi = (float)i;
    float2 base = float2(hash12(float2(fi, 1.7)) * aspect, hash12(float2(fi, 9.2)));
    float2 vel = (float2(hash12(float2(fi, 3.3)), hash12(float2(fi, 5.1))) - 0.5) * 0.6;
    float2 pos = base + vel * time;
    pos = abs(frac(pos / float2(aspect, 1.0) * 0.5) * 2.0 - 1.0) * float2(aspect, 1.0);  // bounce
    float d = length(uv - pos);
    col += palette(fi * 0.13) * (0.0025 / (d * d + 0.0025)) * 0.35;
  }

  // Film grain, new every frame: the part no motion estimation can predict
  float grain = hash12(px + float2(frame % 997, (frame * 7) % 991)) - 0.5;
  col += grain * lerp(0.015, 0.09, busy);

  // Frame number as 16 bits in the bottom-left corner, for tools that want to spot repeats
  if (px.y > resolution.y - 12.0 && px.x < 16.0 * 12.0) {
    uint bit = (uint)(px.x / 12.0);
    col = ((frame >> bit) & 1u) ? float3(1, 1, 1) : float3(0, 0, 0);
  }

  return float4(saturate(col), 1.0);
}
)hlsl";

  struct frame_constants_t {
    float time;
    float busy;
    float width;
    float height;
    std::uint32_t frame;
    std::uint32_t pad[3];
  };

  template<class T>
  void release(T *&p) {
    if (p) {
      p->Release();
      p = nullptr;
    }
  }

  double env_number(const char *name, double fallback) {
    const char *value = std::getenv(name);
    if (!value || !*value) {
      return fallback;
    }
    char *end = nullptr;
    double parsed = std::strtod(value, &end);
    return end != value && parsed > 0 ? parsed : fallback;
  }

  struct monitor_search_t {
    std::wstring device;  // empty: match by size
    int width;
    int height;
    RECT found;
    bool matched;
  };

  BOOL CALLBACK find_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto *search = reinterpret_cast<monitor_search_t *>(param);
    MONITORINFOEXW info {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
      return TRUE;
    }
    const RECT &r = info.rcMonitor;
    const bool match = search->device.empty()
      ? r.right - r.left == search->width && r.bottom - r.top == search->height
      : _wcsicmp(info.szDevice, search->device.c_str()) == 0;
    if (match) {
      search->found = r;
      search->matched = true;
      return FALSE;
    }
    return TRUE;
  }

  /**
   * The monitor to cover: the one the host captures (LAVAPOLLO_CAPTURE_DISPLAY), else the one
   * the size of the stream, else the primary one. A physical monitor can have the stream's
   * size too, which is why the name comes first: on the wrong monitor, the stream would show
   * an idle desktop and every run would encode next to nothing.
   */
  RECT pick_monitor(int width, int height) {
    if (const char *capture = std::getenv("LAVAPOLLO_CAPTURE_DISPLAY"); capture && *capture) {
      std::string first(capture);
      first = first.substr(0, first.find('+'));
      monitor_search_t by_name {std::wstring(first.begin(), first.end()), 0, 0, {}, false};
      EnumDisplayMonitors(nullptr, nullptr, find_monitor, reinterpret_cast<LPARAM>(&by_name));
      if (by_name.matched) {
        return by_name.found;
      }
    }

    monitor_search_t search {{}, width, height, {}, false};
    EnumDisplayMonitors(nullptr, nullptr, find_monitor, reinterpret_cast<LPARAM>(&search));
    if (search.matched) {
      return search.found;
    }
    POINT origin {0, 0};
    MONITORINFO info {sizeof(info)};
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &info);
    return info.rcMonitor;
  }

  LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
      case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
          PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
      case WM_SETCURSOR:
        SetCursor(nullptr);
        return TRUE;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
  }

  ID3DBlob *compile(const char *entry, const char *target) {
    ID3DBlob *code = nullptr;
    ID3DBlob *errors = nullptr;
    HRESULT status = D3DCompile(kShader, std::strlen(kShader), "benchmark_scene", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(status)) {
      if (errors) {
        OutputDebugStringA(static_cast<const char *>(errors->GetBufferPointer()));
      }
      release(errors);
      return nullptr;
    }
    release(errors);
    return code;
  }
}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  double rate = env_number("APOLLO_CLIENT_FPS", env_number("SUNSHINE_CLIENT_FPS", 60.0));
  if (rate > 1000.0) {
    rate /= 1000.0;  // millihertz
  }
  const int width = (int) env_number("SUNSHINE_CLIENT_WIDTH", 1920.0);
  const int height = (int) env_number("SUNSHINE_CLIENT_HEIGHT", 1080.0);
  const RECT area = pick_monitor(width, height);

  WNDCLASSEXW wc {sizeof(wc)};
  wc.lpfnWndProc = window_proc;
  wc.hInstance = instance;
  wc.lpszClassName = L"LavApolloBenchmarkScene";
  RegisterClassExW(&wc);

  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"LavApollo Benchmark", WS_POPUP | WS_VISIBLE, area.left, area.top, area.right - area.left, area.bottom - area.top, nullptr, nullptr, instance, nullptr);
  if (!hwnd) {
    return 1;
  }
  SetForegroundWindow(hwnd);

  const UINT surface_width = (UINT) (area.right - area.left);
  const UINT surface_height = (UINT) (area.bottom - area.top);

  DXGI_SWAP_CHAIN_DESC desc {};
  desc.BufferDesc.Width = surface_width;
  desc.BufferDesc.Height = surface_height;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.OutputWindow = hwnd;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  IDXGISwapChain *swap_chain = nullptr;
  if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swap_chain, &device, nullptr, &context))) {
    return 1;
  }

  ID3D11Texture2D *back_buffer = nullptr;
  ID3D11RenderTargetView *target = nullptr;
  swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back_buffer));
  device->CreateRenderTargetView(back_buffer, nullptr, &target);
  release(back_buffer);

  ID3DBlob *vs_code = compile("vs_main", "vs_5_0");
  ID3DBlob *ps_code = compile("ps_main", "ps_5_0");
  if (!vs_code || !ps_code) {
    return 1;
  }
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs);
  device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps);
  release(vs_code);
  release(ps_code);

  D3D11_BUFFER_DESC cb_desc {};
  cb_desc.ByteWidth = sizeof(frame_constants_t);
  cb_desc.Usage = D3D11_USAGE_DEFAULT;
  cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ID3D11Buffer *constants = nullptr;
  device->CreateBuffer(&cb_desc, nullptr, &constants);

  D3D11_VIEWPORT viewport {0, 0, (float) surface_width, (float) surface_height, 0, 1};

  // Frame n is due at start + n * period. A frame that misses its slot is skipped, never shown
  // late: the animation stays a function of n, and n stays tied to the schedule.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  LARGE_INTEGER freq, start;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  const double period_ticks = (double) freq.QuadPart / rate;
  std::uint64_t n = 0;

  MSG msg {};
  for (;;) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        goto done;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double due = (double) start.QuadPart + (double) n * period_ticks;
    if ((double) now.QuadPart < due) {
      LARGE_INTEGER wait;
      wait.QuadPart = -(LONGLONG) (((due - (double) now.QuadPart) * 10'000'000.0) / (double) freq.QuadPart);
      if (timer && wait.QuadPart < 0 && SetWaitableTimer(timer, &wait, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, 100);
      }
    } else {
      const auto late_slots = (std::uint64_t) (((double) now.QuadPart - due) / period_ticks);
      n += late_slots;
    }

    const double t = (double) n / rate;
    // 5 s calm, 5 s busy, with a 0.5 s ease between them
    const double cycle = std::fmod(t, 10.0);
    double busy = cycle < 5.0 ? 0.0 : 1.0;
    if (cycle < 0.5) {
      busy = 1.0 - cycle / 0.5;
    } else if (cycle >= 5.0 && cycle < 5.5) {
      busy = (cycle - 5.0) / 0.5;
    }

    frame_constants_t c {(float) t, (float) busy, (float) surface_width, (float) surface_height, (std::uint32_t) n, {}};
    context->UpdateSubresource(constants, 0, nullptr, &c, 0, 0);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &constants);
    context->Draw(3, 0);
    swap_chain->Present(0, 0);
    n++;
  }

done:
  if (timer) {
    CloseHandle(timer);
  }
  release(constants);
  release(ps);
  release(vs);
  release(target);
  release(swap_chain);
  release(context);
  release(device);
  return 0;
}
