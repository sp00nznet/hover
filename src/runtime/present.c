/*
 * The presenter: Hover!'s picture in a window of the host's own, scaled on
 * the GPU. docs/presenter.md has the design and why it is its own window.
 *
 * The game keeps its frame window at the size it insists on (516x388 client),
 * cloaked as in headless runs, and draws into it as it always did. host.c
 * mirrors every blit onto a shadow DIB (the same one --record uses), and
 * this thread shows that shadow in a resizable, fullscreen-capable window
 * through Direct3D 11: sharp-bilinear, smooth, nearest or integer scaling,
 * an optional CRT (scanlines, aperture grille, curvature, vignette), retro
 * ordered dithering, and a vivid colour look.
 *
 * A bar of the game's own popup menus sits here, and messages go back:
 * keys are posted to the frame (the accelerator table and MFC see them as
 * their own), menu commands and WM_INITMENUPOPUP are forwarded, and closing
 * this window closes the game. Activation is forwarded only when this window
 * really gains or loses it, so the game pauses in the background as the
 * original did (host.c drops every other deactivation).
 *
 * Everything here runs on the presenter's own thread: it never holds the
 * machine lock and never runs guest code.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "present.h"

typedef HRESULT (WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);

/* Shader modes, in menu order. */
enum { F_SHARP, F_SMOOTH, F_NEAREST, F_INTEGER, F_COUNT };
static const char* const k_filters[F_COUNT] = { "sharp", "smooth", "nearest", "integer" };
enum { D_OFF, D_16BIT, D_8BIT, D_COUNT };
static const char* const k_dithers[D_COUNT] = { "off", "16bit", "8bit" };

static const char k_hlsl[] =
"Texture2D t0 : register(t0);\n"
"SamplerState s_lin : register(s0);\n"
"SamplerState s_pt : register(s1);\n"
"cbuffer C : register(b0) { float2 src; float2 dst; int mode; int crt; int curve; int dither;\n"
"                           int look; int3 pad; };\n"
"struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint id : SV_VertexID) {\n"
"  V o; float2 p = float2((id << 1) & 2, id & 2);\n"
"  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
"}\n"
"float3 sharp(float2 uv) {\n"                        /* sharp-bilinear (gunman) */
"  float2 scale = max(dst / src, 1.0);\n"
"  float2 texel = uv * src, fl = floor(texel), c = frac(texel) - 0.5;\n"
"  float2 range = 0.5 - 0.5 / scale;\n"
"  float2 f = (c - clamp(c, -range, range)) * scale + 0.5;\n"
"  return t0.Sample(s_lin, (fl + f) / src).rgb;\n"
"}\n"
"float3 base(float2 uv) {\n"
"  if (mode == 1) return t0.Sample(s_lin, uv).rgb;\n"
"  if (mode >= 2) return t0.Sample(s_pt, uv).rgb;\n"
"  return sharp(uv);\n"
"}\n"
"static const float bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n"
"float4 ps(V i) : SV_Target {\n"
"  float2 uv = i.uv;\n"
"  if (crt && curve) {\n"                             /* a gentle barrel, as on a 14-inch tube */
"    float2 c = uv * 2 - 1; c *= 1 + (c.yx * c.yx) * float2(0.035, 0.05); uv = c * 0.5 + 0.5;\n"
"    if (any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, 1);\n"
"  }\n"
"  float3 c = base(uv);\n"
"  if (dither) {\n"                                   /* ordered, per game pixel: chunky like 1995 */
"    int2 p = int2(uv * src) & 3;\n"
"    float3 lv = dither == 1 ? float3(31, 63, 31) : float3(5, 5, 5);\n"
"    c = floor(c * lv + bayer[p.y * 4 + p.x] / 16.0) / lv;\n"
"  }\n"
"  if (crt) {\n"
"    /* Scanlines need a few screen rows per game row; below ~3x they beat\n"
"     * against the pixel grid, so they fade in from 1.5x to 3x (gunman). */\n"
"    float k = saturate((dst.y / src.y - 1.5) / 1.5);\n"
"    float d = frac(uv.y * src.y) - 0.5;\n"
"    c *= lerp(1.0, exp(-d * d * 8.0) * 1.6, k);\n"
"    uint m = (uint)i.pos.x % 3u;\n"
"    float3 mask = m == 0u ? float3(1.25, 0.87, 0.87) : m == 1u ? float3(0.87, 1.25, 0.87)\n"
"                                                     : float3(0.87, 0.87, 1.25);\n"
"    c *= lerp(float3(1, 1, 1), mask, k);\n"
"    float2 v = uv * (1 - uv.yx); c *= pow(saturate(v.x * v.y * 18.0), 0.2);\n"   /* vignette */
"  }\n"
"  if (look) {\n"                                     /* vivid: saturation, a little contrast */
"    float l = dot(c, float3(0.299, 0.587, 0.114));\n"
"    c = lerp(float3(l, l, l), c, 1.3);\n"
"    c = saturate((c - 0.5) * 1.08 + 0.5);\n"
"  }\n"
"  return float4(saturate(c), 1);\n"
"}\n";

/* ------------------------------------------------------------ state */

static char   g_ini[MAX_PATH];
static int    g_filter = F_SHARP, g_crt, g_curve = 1, g_dither, g_vivid, g_scale = 2, g_full;
static int    g_pause_bg = 1;            /* pause when this window loses focus, as the original */
static HWND   g_wnd, g_frame;
static HMENU  g_menu;                    /* a bar of the frame's own popups */
static HICON  g_icon;
static HANDLE g_ready;                   /* auto-reset: a frame (or a resize) to show */
static CRITICAL_SECTION g_lock;          /* the shadow, against host.c's blits */
static const uint32_t* g_src;
static int    g_sw, g_sh;
static uint32_t* g_copy;                 /* this thread's copy, uploaded unlocked */
static char   g_title[256] = "Hover!";
static LONG   g_saved_style;
static RECT   g_saved_rect;

static struct {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    IDXGISwapChain* sc;
    ID3D11RenderTargetView* rtv;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11SamplerState* samp[2];
    ID3D11Buffer* cb;
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    int tw, th, cw, ch, failed;
} d;

/* ------------------------------------------------------------ settings */

static int pick(const char* key, const char* const* names, int n, int dflt) {
    char b[32];
    GetPrivateProfileStringA("video", key, names[dflt], b, sizeof b, g_ini);
    for (int i = 0; i < n; i++)
        if (!_stricmp(b, names[i])) return i;
    return dflt;
}

static void put(const char* key, const char* v) { WritePrivateProfileStringA("video", key, v, g_ini); }
static void put_int(const char* key, int v) { char b[16]; _snprintf(b, sizeof b, "%d", v); put(key, b); }

void present_load(void) {
    char b[8];
    if (!GetPrivateProfileStringA("video", "presenter", "", b, sizeof b, g_ini)) {
        WritePrivateProfileStringA("video", NULL, NULL, g_ini);
        put_int("presenter", 1);
        put("filter", "sharp");
        put_int("scale", 2);
        put_int("crt", 0);
        put_int("curvature", 1);
        put("dither", "off");
        put_int("vivid", 0);
        put_int("fullscreen", 0);
        put_int("pause_in_background", 1);
    }
    g_filter = pick("filter", k_filters, F_COUNT, F_SHARP);
    g_dither = pick("dither", k_dithers, D_COUNT, D_OFF);
    g_scale = GetPrivateProfileIntA("video", "scale", 2, g_ini);
    g_crt = GetPrivateProfileIntA("video", "crt", 0, g_ini);
    g_curve = GetPrivateProfileIntA("video", "curvature", 1, g_ini);
    g_vivid = GetPrivateProfileIntA("video", "vivid", 0, g_ini);
    g_pause_bg = GetPrivateProfileIntA("video", "pause_in_background", 1, g_ini);
    if (g_scale < 1 || g_scale > 8) g_scale = 2;
    if (g_ready) SetEvent(g_ready);
}

int present_wanted(const char* ini) {
    strncpy(g_ini, ini, sizeof g_ini - 1);
    present_load();
    return GetPrivateProfileIntA("video", "presenter", 1, g_ini);
}

/* ------------------------------------------------------------ Direct3D */

static int d3d_fail(const char* what, HRESULT hr) {
    fprintf(stderr, "[present] Direct3D 11 unavailable (%s, 0x%08lX): showing the frame with GDI\n",
            what, (unsigned long)hr);
    d.failed = 1;
    return 0;
}

static int d3d_init(void) {
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1,
                                               D3D11_SDK_VERSION, &sd, &d.sc, &d.dev, NULL, &d.ctx);
    if (FAILED(hr))     /* no GPU (an RDP session without one): WARP is still a GPU pipeline */
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &fl, 1,
                                           D3D11_SDK_VERSION, &sd, &d.sc, &d.dev, NULL, &d.ctx);
    if (FAILED(hr)) return d3d_fail("device", hr);
    IDXGIFactory* f = NULL;             /* Alt+Enter is ours (borderless), not DXGI's */
    if (SUCCEEDED(IDXGISwapChain_GetParent(d.sc, &IID_IDXGIFactory, (void**)&f))) {
        IDXGIFactory_MakeWindowAssociation(f, g_wnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        IDXGIFactory_Release(f);
    }
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    compile_fn compile = dc ? (compile_fn)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile) return d3d_fail("d3dcompiler_47.dll", 0);
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    hr = compile(k_hlsl, sizeof k_hlsl - 1, "present", NULL, NULL, "vs", "vs_4_0", 0, 0, &vb, &err);
    if (SUCCEEDED(hr))
        hr = compile(k_hlsl, sizeof k_hlsl - 1, "present", NULL, NULL, "ps", "ps_4_0", 0, 0, &pb, &err);
    if (FAILED(hr)) {
        if (err) fprintf(stderr, "[present] %s\n", (const char*)ID3D10Blob_GetBufferPointer(err));
        return d3d_fail("shader", hr);
    }
    ID3D11Device_CreateVertexShader(d.dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &d.vs);
    ID3D11Device_CreatePixelShader(d.dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &d.ps);
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);
    D3D11_SAMPLER_DESC s = {0};
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11Device_CreateSamplerState(d.dev, &s, &d.samp[0]);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(d.dev, &s, &d.samp[1]);
    D3D11_BUFFER_DESC b = {0};
    b.ByteWidth = 48;
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(d.dev, &b, NULL, &d.cb);
    fprintf(stderr, "[present] Direct3D 11 presenter\n");
    return 1;
}

/* Where the frame goes in a cw x ch client: aspect kept, letterboxed. */
static RECT fit(int cw, int ch, int w, int h) {
    RECT r;
    int dw, dh;
    if (g_filter == F_INTEGER) {
        int k = min(cw / w, ch / h);
        if (k < 1) k = 1;
        dw = w * k;
        dh = h * k;
    } else if ((long long)cw * h > (long long)ch * w) {
        dh = ch;
        dw = (int)((long long)ch * w / h);
    } else {
        dw = cw;
        dh = (int)((long long)cw * h / w);
    }
    r.left = (cw - dw) / 2;
    r.top = (ch - dh) / 2;
    r.right = r.left + dw;
    r.bottom = r.top + dh;
    return r;
}

static void draw(void) {
    RECT cr;
    GetClientRect(g_wnd, &cr);
    int cw = cr.right, ch = cr.bottom, w, h;
    if (cw <= 0 || ch <= 0) return;
    EnterCriticalSection(&g_lock);
    w = g_sw;
    h = g_sh;
    if (g_src && w > 0) {
        if (!g_copy) g_copy = (uint32_t*)malloc((size_t)w * h * 4);
        memcpy(g_copy, g_src, (size_t)w * h * 4);
    }
    LeaveCriticalSection(&g_lock);
    if (!g_copy) return;
    RECT r = fit(cw, ch, w, h);

    if (d.failed || (!d.dev && !d3d_init())) {          /* GDI: one StretchDIBits */
        HDC dc = GetDC(g_wnd);
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
        SetStretchBltMode(dc, g_filter == F_SMOOTH || g_filter == F_SHARP ? HALFTONE : COLORONCOLOR);
        StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, w, h, g_copy, &bi,
                      DIB_RGB_COLORS, SRCCOPY);
        ExcludeClipRect(dc, r.left, r.top, r.right, r.bottom);
        FillRect(dc, &cr, (HBRUSH)GetStockObject(BLACK_BRUSH));
        ReleaseDC(g_wnd, dc);
        return;
    }
    if (!d.tex || d.tw != w || d.th != h) {
        if (d.srv) ID3D11ShaderResourceView_Release(d.srv);
        if (d.tex) ID3D11Texture2D_Release(d.tex);
        D3D11_TEXTURE2D_DESC td = {0};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8X8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(d.dev, &td, NULL, &d.tex))) { d3d_fail("texture", 0); return; }
        ID3D11Device_CreateShaderResourceView(d.dev, (ID3D11Resource*)d.tex, NULL, &d.srv);
        d.tw = w;
        d.th = h;
    }
    ID3D11DeviceContext_UpdateSubresource(d.ctx, (ID3D11Resource*)d.tex, 0, NULL, g_copy, w * 4, 0);
    if (!d.rtv || cw != d.cw || ch != d.ch) {
        if (d.rtv) { ID3D11RenderTargetView_Release(d.rtv); d.rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets(d.ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain_ResizeBuffers(d.sc, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))) return;
        ID3D11Texture2D* back = NULL;
        IDXGISwapChain_GetBuffer(d.sc, 0, &IID_ID3D11Texture2D, (void**)&back);
        ID3D11Device_CreateRenderTargetView(d.dev, (ID3D11Resource*)back, NULL, &d.rtv);
        ID3D11Texture2D_Release(back);
        d.cw = cw;
        d.ch = ch;
    }
    struct { float src[2], dst[2]; int mode, crt, curve, dither, look, pad[3]; } c = {
        { (float)w, (float)h }, { (float)(r.right - r.left), (float)(r.bottom - r.top) },
        g_filter, g_crt, g_curve, g_dither, g_vivid, { 0 } };
    ID3D11DeviceContext_UpdateSubresource(d.ctx, (ID3D11Resource*)d.cb, 0, NULL, &c, 0, 0);
    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(d.ctx, d.rtv, black);
    D3D11_VIEWPORT vp = { (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), 0, 1 };
    ID3D11DeviceContext_RSSetViewports(d.ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(d.ctx, 1, &d.rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(d.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(d.ctx, d.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(d.ctx, d.ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(d.ctx, 0, 1, &d.srv);
    ID3D11DeviceContext_PSSetSamplers(d.ctx, 0, 2, d.samp);
    ID3D11DeviceContext_PSSetConstantBuffers(d.ctx, 0, 1, &d.cb);
    ID3D11DeviceContext_Draw(d.ctx, 3, 0);
    IDXGISwapChain_Present(d.sc, 1, 0);
}

/* ------------------------------------------------------------ window */

#define WM_PRESENT_TITLE (WM_APP + 1)

static void set_fullscreen(int on) {
    if (on == g_full) return;
    g_full = on;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        g_saved_style = GetWindowLongA(g_wnd, GWL_STYLE);
        GetWindowRect(g_wnd, &g_saved_rect);
        GetMonitorInfoA(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetMenu(g_wnd, NULL);
        SetWindowLongA(g_wnd, GWL_STYLE, (g_saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongA(g_wnd, GWL_STYLE, g_saved_style);
        SetMenu(g_wnd, g_menu);
        SetWindowPos(g_wnd, NULL, g_saved_rect.left, g_saved_rect.top, g_saved_rect.right - g_saved_rect.left,
                     g_saved_rect.bottom - g_saved_rect.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    put_int("fullscreen", on);
    SetEvent(g_ready);
}

/* Window size for the frame at `scale` times, with this window's chrome. */
static void size_to(int scale) {
    RECT r = { 0, 0, (g_sw > 0 ? g_sw : 516) * scale, (g_sh > 0 ? g_sh : 388) * scale };
    if (g_full) set_fullscreen(0);
    AdjustWindowRectEx(&r, GetWindowLongA(g_wnd, GWL_STYLE), GetMenu(g_wnd) != NULL, 0);
    SetWindowPos(g_wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

/* Keep the (cloaked) frame centred on this window: the game's dialogs and
 * message boxes centre on their owner, which is the frame. */
static void follow(void) {
    RECT r, f;
    if (!g_frame || !GetWindowRect(g_wnd, &r) || !GetWindowRect(g_frame, &f)) return;
    SetWindowPos(g_frame, NULL, (r.left + r.right) / 2 - (f.right - f.left) / 2,
                 (r.top + r.bottom) / 2 - (f.bottom - f.top) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (m == WM_KEYDOWN && w == VK_F11) { set_fullscreen(!g_full); return 0; }
        if (m == WM_KEYDOWN && w == VK_ESCAPE && g_full) { set_fullscreen(0); return 0; }
        PostMessageA(g_frame, m, w, l);
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN) { set_fullscreen(!g_full); return 0; }   /* Alt+Enter */
        break;
    case WM_CHAR:
        return 0;                                /* the game translates its own keys */
    case WM_INITMENU:
    case WM_INITMENUPOPUP:
        /* MFC enables, greys and checks the game's items; host.c's subclass
         * of the frame does the Recomp menu. The frame's thread is the
         * game's UI thread, which pumps messages even inside its dialogs. */
        SendMessageA(g_frame, m, w, l);
        return 0;
    case WM_COMMAND:
        if (!present_command(LOWORD(w))) PostMessageA(g_frame, m, w, l);
        return 0;
    case WM_ACTIVATEAPP:
        /* Only a real switch reaches the game (host.c drops the rest). */
        fprintf(stderr, "[present] %s the foreground%s\n", w ? "has" : "lost",
                g_pause_bg ? "" : " (the game keeps running)");
        if (g_pause_bg) SendMessageA(g_frame, WM_ACTIVATEAPP, w, PRESENT_REAL_ACTIVATION);
        break;
    case WM_CLOSE:
        PostMessageA(g_frame, WM_CLOSE, 0, 0);   /* the game asks, saves and exits */
        return 0;
    case WM_MOVE:
    case WM_SIZE:
        follow();
        SetEvent(g_ready);
        break;
    case WM_PAINT:
        ValidateRect(h, NULL);
        SetEvent(g_ready);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PRESENT_TITLE:
        SetWindowTextA(h, g_title);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI thread(LPVOID unused) {
    (void)unused;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hIcon = g_icon;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "HoverRecompPresenter";
    RegisterClassA(&wc);
    RECT r = { 0, 0, 516 * g_scale, 388 * g_scale };
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, g_menu != NULL, 0);
    g_wnd = CreateWindowExA(0, wc.lpszClassName, g_title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, NULL, g_menu, wc.hInstance, NULL);
    if (!g_wnd) { fprintf(stderr, "[present] no window (%lu)\n", GetLastError()); return 1; }
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    if (GetPrivateProfileIntA("video", "fullscreen", 0, g_ini)) set_fullscreen(1);
    follow();
    for (;;) {
        MSG msg;
        DWORD why = MsgWaitForMultipleObjects(1, &g_ready, FALSE, 250, QS_ALLINPUT);
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (why == WAIT_OBJECT_0 || why == WAIT_TIMEOUT) draw();
    }
}

/* ------------------------------------------------------------ host.c's side */

void present_start(HWND frame, HMENU menu, HICON icon) {
    g_frame = frame;
    g_menu = menu;
    g_icon = icon;
    InitializeCriticalSection(&g_lock);
    g_ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, thread, NULL, 0, NULL));
}

int present_running(void) { return g_ready != NULL; }
int present_focused(void) { return g_wnd && GetForegroundWindow() == g_wnd; }

void present_source(const uint32_t* bgra, int w, int h) {
    g_src = bgra;
    g_sw = w;
    g_sh = h;
}

void present_lock(void) { if (g_ready) EnterCriticalSection(&g_lock); }
void present_unlock(void) {
    if (!g_ready) return;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_ready);
}

void present_set_title(const char* t) {
    strncpy(g_title, t, sizeof g_title - 1);
    if (g_wnd) PostMessageA(g_wnd, WM_PRESENT_TITLE, 0, 0);
}

/* ------------------------------------------------------------ menu */

enum { ID_FILTER = 0x6E00,  /* + F_* */
       ID_SCALE = 0x6E10,   /* + 1..4 */
       ID_DITHER = 0x6E20,  /* + D_* */
       ID_CRT = 0x6E30, ID_CURVE, ID_VIVID, ID_FULL, ID_PAUSE_BG };

void present_menu(HMENU recomp) {
    HMENU v = CreatePopupMenu(), f = CreatePopupMenu(), s = CreatePopupMenu(), dm = CreatePopupMenu();
    AppendMenuA(f, MF_STRING, ID_FILTER + F_SHARP, "&Sharp (crisp pixels, smooth edges)");
    AppendMenuA(f, MF_STRING, ID_FILTER + F_SMOOTH, "S&mooth (bilinear)");
    AppendMenuA(f, MF_STRING, ID_FILTER + F_NEAREST, "&Nearest (raw pixels)");
    AppendMenuA(f, MF_STRING, ID_FILTER + F_INTEGER, "&Integer (whole multiples only)");
    for (int i = 1; i <= 4; i++) {
        char t[32];
        _snprintf(t, sizeof t, "&%dx (%dx%d)", i, 516 * i, 388 * i);
        AppendMenuA(s, MF_STRING, ID_SCALE + i, t);
    }
    AppendMenuA(dm, MF_STRING, ID_DITHER + D_OFF, "&Off");
    AppendMenuA(dm, MF_STRING, ID_DITHER + D_16BIT, "&16-bit (High Color desktop)");
    AppendMenuA(dm, MF_STRING, ID_DITHER + D_8BIT, "&8-bit (216-colour web palette)");
    AppendMenuA(v, MF_POPUP, (UINT_PTR)f, "&Filter");
    AppendMenuA(v, MF_POPUP, (UINT_PTR)s, "Window &size");
    AppendMenuA(v, MF_STRING, ID_FULL, "F&ullscreen\tF11");
    AppendMenuA(v, MF_SEPARATOR, 0, NULL);
    AppendMenuA(v, MF_STRING, ID_CRT, "&CRT (scanlines, mask)");
    AppendMenuA(v, MF_STRING, ID_CURVE, "CRT c&urvature");
    AppendMenuA(v, MF_POPUP, (UINT_PTR)dm, "&Dithering");
    AppendMenuA(v, MF_STRING, ID_VIVID, "&Vivid colour");
    AppendMenuA(v, MF_SEPARATOR, 0, NULL);
    AppendMenuA(v, MF_STRING, ID_PAUSE_BG, "&Pause when in the background");
    AppendMenuA(recomp, MF_POPUP, (UINT_PTR)v, "&Video");
}

static void check(HMENU m, UINT id, int on) {
    EnableMenuItem(m, id, MF_BYCOMMAND | MF_ENABLED);
    CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
}

void present_update_menu(HMENU popup) {
    for (int i = 0; i < F_COUNT; i++) check(popup, ID_FILTER + i, g_filter == i);
    for (int i = 1; i <= 4; i++) check(popup, ID_SCALE + i, 0);
    for (int i = 0; i < D_COUNT; i++) check(popup, ID_DITHER + i, g_dither == i);
    check(popup, ID_CRT, g_crt);
    check(popup, ID_CURVE, g_curve);
    check(popup, ID_VIVID, g_vivid);
    check(popup, ID_FULL, g_full);
    check(popup, ID_PAUSE_BG, g_pause_bg);
    if (!g_ready)      /* the classic window: the Video items need the presenter */
        for (UINT id = ID_FILTER; id <= ID_PAUSE_BG; id++) EnableMenuItem(popup, id, MF_BYCOMMAND | MF_GRAYED);
}

int present_command(UINT id) {
    if (id >= ID_FILTER && id < ID_FILTER + F_COUNT) put("filter", k_filters[id - ID_FILTER]);
    else if (id >= ID_DITHER && id < ID_DITHER + D_COUNT) put("dither", k_dithers[id - ID_DITHER]);
    else if (id >= ID_SCALE + 1 && id <= ID_SCALE + 4) {
        put_int("scale", (int)(id - ID_SCALE));
        if (g_wnd) size_to((int)(id - ID_SCALE));
    }
    else if (id == ID_CRT) put_int("crt", !g_crt);
    else if (id == ID_CURVE) put_int("curvature", !g_curve);
    else if (id == ID_VIVID) put_int("vivid", !g_vivid);
    else if (id == ID_PAUSE_BG) put_int("pause_in_background", !g_pause_bg);
    else if (id == ID_FULL) { if (g_wnd) set_fullscreen(!g_full); return 1; }
    else return 0;
    present_load();
    return 1;
}
