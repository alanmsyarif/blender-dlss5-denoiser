// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ComfyUI-DLSS5-NR contributors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using NGXResult = int;
static constexpr NGXResult NGX_SUCCESS = 1;
static constexpr int NR_FEATURE_ID = 18;
static constexpr unsigned long long APP_ID = 141959980ULL;
static constexpr const char* PROJECT_ID = "53f803cc-a12f-4d69-90d5-19b7599cad19";

struct NGXHandle { unsigned int Id; };

// Minimal ABI-compatible interface used by the NVIDIA NGX parameter object.
//
// IMPORTANT: only the Get() half of this vtable matches the layout below. The
// driver core's capability block shuffles its Set() slots: uint is slot 3,
// resources go through the unsigned-long-long setter at slot 0, and the float
// setter is NOT at slot 1 - MEASURED at slot 6 on driver 591.86, matching what
// video2dlssnr found on 616.56. Hardcoding the header layout does not work. Never call Set() directly on the capability block -
// use VSetUInt/VSetFloat/VSetRes, which drive the probed slots. The typed Get()
// side working is exactly what makes probing possible.
struct NGXParameter {
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual NGXResult Get(const char*, unsigned long long*) const = 0;
    virtual NGXResult Get(const char*, float*) const = 0;
    virtual NGXResult Get(const char*, double*) const = 0;
    virtual NGXResult Get(const char*, unsigned int*) const = 0;
    virtual NGXResult Get(const char*, int*) const = 0;
    virtual NGXResult Get(const char*, ID3D11Resource**) const = 0;
    virtual NGXResult Get(const char*, ID3D12Resource**) const = 0;
    virtual NGXResult Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};

struct NGXPathListInfo {
    wchar_t const* const* Path;
    unsigned int Length;
};
enum NGXLoggingLevel { NGX_LOG_OFF = 0, NGX_LOG_ON = 1, NGX_LOG_VERBOSE = 2 };
using NGXLogCallback = void(__cdecl*)(const char*, NGXLoggingLevel, int);
struct NGXLoggingInfo {
    NGXLoggingLevel LoggingLevel;
    NGXLogCallback Callback;
    void* UserData;
    bool DisableOtherLoggingSinks;
};
struct NGXFeatureCommonInfoInternal;
struct NGXFeatureCommonInfo {
    NGXPathListInfo PathListInfo;
    NGXFeatureCommonInfoInternal* InternalData;
    NGXLoggingInfo LoggingInfo;
};

using InitExtFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using SnippetInitFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);
using InitProjectIdFn = NGXResult(__cdecl*)(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const void*);
using AllocParamsFn = NGXResult(__cdecl*)(NGXParameter**);
using GetCapsParamsFn = NGXResult(__cdecl*)(NGXParameter**);
using CreateFeatureFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using EvaluateFeatureFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ReleaseFeatureFn = NGXResult(__cdecl*)(NGXHandle*);
using ShutdownFn = NGXResult(__cdecl*)();

using ShimInitFn = NGXResult(__cdecl*)(void*, unsigned long long, const wchar_t*, ID3D12Device*, int, const void*, int*, NGXResult*);
using ShimCreateFn = NGXResult(__cdecl*)(void*, ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using ShimEvaluateFn = NGXResult(__cdecl*)(void*, ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ShimReleaseFn = NGXResult(__cdecl*)(void*, NGXHandle*);
using ShimLoadSnippetFn = void*(__cdecl*)(const wchar_t*, unsigned long*);

static std::mutex g_mutex;
static std::string g_last_error;
static std::wstring g_runtime_dir;
static int g_gpu_index = 0;
static std::string g_gpu_name = "unknown";
static bool g_initialized = false;

static HMODULE g_core_mod = nullptr;
static HMODULE g_nr_mod = nullptr;
static HMODULE g_shim_mod = nullptr;
static InitExtFn g_core_init_ext = nullptr;
static InitProjectIdFn g_core_init_project = nullptr;
static AllocParamsFn g_alloc_params = nullptr;
static GetCapsParamsFn g_get_caps_params = nullptr;
static CreateFeatureFn g_core_create = nullptr;
static EvaluateFeatureFn g_core_eval = nullptr;
static ReleaseFeatureFn g_core_release = nullptr;
static ShutdownFn g_core_shutdown = nullptr;
static SnippetInitFn g_nr_init = nullptr;
static CreateFeatureFn g_nr_create = nullptr;
static EvaluateFeatureFn g_nr_eval = nullptr;
static ReleaseFeatureFn g_nr_release = nullptr;
static ShimInitFn g_shim_init = nullptr;
static ShimCreateFn g_shim_create = nullptr;
static ShimEvaluateFn g_shim_eval = nullptr;
static ShimReleaseFn g_shim_release = nullptr;
static ShimLoadSnippetFn g_shim_load_snippet = nullptr;

static ComPtr<ID3D12Device> g_device;
static ComPtr<ID3D12CommandQueue> g_queue;
static ComPtr<ID3D12CommandAllocator> g_cmd_alloc;
static ComPtr<ID3D12GraphicsCommandList> g_cmd;
static ComPtr<ID3D12Fence> g_fence;
static UINT64 g_fence_value = 0;

static NGXParameter* g_params = nullptr;
static NGXHandle* g_feature = nullptr;
static ComPtr<ID3D12Resource> g_color;
static ComPtr<ID3D12Resource> g_output;
static ComPtr<ID3D12Resource> g_depth;
static ComPtr<ID3D12Resource> g_motion;
static ComPtr<ID3D12Resource> g_upload;
static ComPtr<ID3D12Resource> g_readback;
static constexpr int kMinLongSide = 96;
static UINT g_width = 0, g_height = 0, g_row_pitch = 0;
static UINT64 g_total_bytes = 0;
// Every parameter the model latches at CreateFeature. Changing any of them has
// no effect until the feature is rebuilt, so all of them are tracked.
struct LatchedParams {
    int style = -999;
    int preset = -999;
    float intensity = -999.0f;
    float tone = -999.0f;
    float structure = -999.0f;
    float skin = -999.0f;
    int automask = -999;
    bool operator==(const LatchedParams& o) const {
        return style == o.style && preset == o.preset && intensity == o.intensity &&
               tone == o.tone && structure == o.structure && skin == o.skin &&
               automask == o.automask;
    }
};
static LatchedParams g_latched;

// Raw-vtable setters for the capability block. See the note on NGXParameter.
using PfnSetULLVt = void(__cdecl*)(void*, const char*, unsigned long long);
using PfnSetFloatVt = void(__cdecl*)(void*, const char*, float);
using PfnSetUIntVt = void(__cdecl*)(void*, const char*, unsigned int);
static constexpr int VT_SET_ULL = 0;
static int g_uint_slot = 3;
static int g_float_slot = -1;
// 0 = not initialized, 1 = version before info pointer, 2 = info pointer
// before version. Reported in later errors because it identifies which
// snippet ABI this runtime actually has.
static int g_init_order = 0;

static void VSetUInt(const char* name, unsigned value) {
    void** vt = *reinterpret_cast<void***>(g_params);
    reinterpret_cast<PfnSetUIntVt>(vt[g_uint_slot])(g_params, name, value);
}

static void VSetFloat(const char* name, float value) {
    void** vt = *reinterpret_cast<void***>(g_params);
    reinterpret_cast<PfnSetFloatVt>(vt[g_float_slot < 0 ? 1 : g_float_slot])(g_params, name, value);
}

static void VSetRes(const char* name, ID3D12Resource* res) {
    void** vt = *reinterpret_cast<void***>(g_params);
    reinterpret_cast<PfnSetULLVt>(vt[VT_SET_ULL])(g_params, name, reinterpret_cast<unsigned long long>(res));
}

// Find which Set() slot actually stores a uint / a float by round-tripping a
// value through the typed Get(). Every slot in 0..7 is a Set overload, so an
// x64 call with (this, name, value) is ABI-safe whichever one is hit; only the
// interpretation of the value register differs.
static void DiscoverSetterSlots() {
    void** vt = *reinterpret_cast<void***>(g_params);
    for (int slot = 0; slot < 8; ++slot) {
        const unsigned probe = 0x1234u;
        unsigned readback = 0;
        reinterpret_cast<PfnSetUIntVt>(vt[slot])(g_params, "DLSSNR.UProbe", probe);
        if (g_params->Get("DLSSNR.UProbe", &readback) == NGX_SUCCESS && readback == probe) {
            g_uint_slot = slot;
            break;
        }
    }
    for (int slot = 0; slot < 8; ++slot) {
        const float probe = 0.3125f;  // exact in binary
        float readback = 0.0f;
        reinterpret_cast<PfnSetFloatVt>(vt[slot])(g_params, "DLSSNR.Probe", probe);
        if (g_params->Get("DLSSNR.Probe", &readback) == NGX_SUCCESS && readback == probe) {
            g_float_slot = slot;
            break;
        }
    }
    if (g_float_slot < 0) g_float_slot = 1;
}

// The model is trained on display-referred images. Cycles and the Blender
// Render Result hand us linear scene-referred colour, so encode on the way in
// and decode on the way out.
static float LinearToSrgb(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c <= 0.0031308f) return c * 12.92f;
    return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static float SrgbToLinear(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

// Reinhard, applied before the sRGB encode so the model still receives
// display-referred colour inside [0, 1). This replaces a hard clamp to 0..1,
// which discarded every highlight instead of compressing it: measured on a
// Cycles scene with ordinary emissives, the clamp flattened 16% of the image
// onto exactly 1.0 and cost 49% of the total image energy.
//
// Stateless on purpose. An exposure derived from the frame would fit the data
// better, but it would also change from frame to frame, and this denoiser runs
// per viewport update, where that reads as flicker.
static float TonemapForward(float c) {
    // The negated comparison also rejects NaN, which would otherwise reach the
    // model and come back as a dead pixel.
    if (!(c > 0.0f)) {
        return 0.0f;
    }
    return c / (1.0f + c);
}

// The largest value FP16 represents below 1.0 is 1 - 2^-11, so the inverse can
// carry roughly 2047 at most. Clamping to it keeps the division finite rather
// than returning infinity for a channel the model handed back as exactly 1.0.
static constexpr float kTonemapCeiling = 1.0f - 1.0f / 2048.0f;

static float TonemapInverse(float c) {
    if (!(c > 0.0f)) {
        return 0.0f;
    }
    c = std::min(c, kTonemapCeiling);
    return c / (1.0f - c);
}

static void SetError(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_last_error = buf;
}

static void CopyError(char* dst, int cap) {
    if (!dst || cap <= 0) return;
    const size_t n = std::min<size_t>(g_last_error.size(), static_cast<size_t>(cap - 1));
    memcpy(dst, g_last_error.data(), n);
    dst[n] = '\0';
}

static std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    wchar_t c = a.back();
    if (c == L'\\' || c == L'/') return a + b;
    return a + L"\\" + b;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned long long FileTimeKey(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static HMODULE LoadCoreNGX(const std::wstring& runtime) {
    // 1) Explicit local override. This is also the quickest workaround if
    // DriverStore auto-discovery ever misses a vendor-specific INF name.
    const std::wstring local = Join(runtime, L"_nvngx.dll");
    if (FileExists(local)) {
        if (HMODULE m = LoadLibraryW(local.c_str())) return m;
    }

    // 2) Normal loader search (works on systems where NVIDIA exposes it).
    if (HMODULE m = LoadLibraryW(L"_nvngx.dll")) return m;

    // 3) NVIDIA ships NGX core inside the active display-driver package in
    // DriverStore. The INF prefix is NOT always nv_dispi: depending on OEM,
    // notebook/desktop package and driver generation it can be nvddi, nvaci,
    // nvhmui, etc. Scan every NVIDIA-looking *.inf_* package instead.
    wchar_t windows_dir[MAX_PATH] = {};
    UINT windows_len = GetWindowsDirectoryW(windows_dir, MAX_PATH);
    if (windows_len == 0 || windows_len >= MAX_PATH) return nullptr;
    const std::wstring repo = std::wstring(windows_dir) + L"\\System32\\DriverStore\\FileRepository";
    const std::wstring pat = repo + L"\\nv*.inf_*";

    struct Candidate {
        std::wstring path;
        unsigned long long stamp;
    };
    std::vector<Candidate> candidates;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

            std::wstring candidate = repo + L"\\" + fd.cFileName + L"\\_nvngx.dll";
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &fad) &&
                !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                candidates.push_back({candidate, FileTimeKey(fad.ftLastWriteTime)});
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Prefer the newest package. DriverStore often retains older drivers after
    // updates, and loading a stale NGX core is worse than not finding one.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.stamp > b.stamp; });

    for (const Candidate& c : candidates) {
        if (HMODULE m = LoadLibraryW(c.path.c_str())) return m;
    }

    return nullptr;
}

static ComPtr<ID3D12Device> CreateDevice(int nvidia_index) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;

    int seen = 0;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10DE) continue;
        if (seen++ != nvidia_index) continue;
        char gpu_utf8[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu_utf8, static_cast<int>(sizeof(gpu_utf8)), nullptr, nullptr);
        if (gpu_utf8[0]) g_gpu_name = gpu_utf8;
        ComPtr<ID3D12Device> d;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d))))
            return d;
        return nullptr;
    }
    return nullptr;
}

static bool SetupD3D12() {
    g_device = CreateDevice(g_gpu_index);
    if (!g_device) { SetError("Could not create a D3D12 device for NVIDIA GPU index %d", g_gpu_index); return false; }

    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(&q, IID_PPV_ARGS(&g_queue)))) { SetError("CreateCommandQueue failed"); return false; }
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmd_alloc)))) { SetError("CreateCommandAllocator failed"); return false; }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&g_cmd)))) { SetError("CreateCommandList failed"); return false; }
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) { SetError("CreateFence failed"); return false; }
    return true;
}

static bool ExecuteAndWait() {
    HRESULT hr = g_cmd->Close();
    if (FAILED(hr)) { SetError("CommandList::Close failed (0x%08X)", static_cast<unsigned>(hr)); return false; }
    ID3D12CommandList* lists[] = { g_cmd.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    ++g_fence_value;
    hr = g_queue->Signal(g_fence.Get(), g_fence_value);
    if (FAILED(hr)) { SetError("Queue::Signal failed (0x%08X)", static_cast<unsigned>(hr)); return false; }
    if (g_fence->GetCompletedValue() < g_fence_value) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ev) { SetError("CreateEvent failed"); return false; }
        g_fence->SetEventOnCompletion(g_fence_value, ev);
        DWORD w = WaitForSingleObject(ev, 30000);
        CloseHandle(ev);
        if (w != WAIT_OBJECT_0) { SetError("Timed out waiting for DLSS5 NR GPU work"); return false; }
    }

    // A GPU fault fails none of the calls above: the fence still signals, the
    // readback still maps, and every pixel comes back zero. That surfaced as a
    // black frame reported as a successful denoise, and left the process unable
    // to create a D3D12 device again for as long as it ran, so every later
    // render failed with an error that pointed at initialization rather than at
    // the frame that actually broke it. Ask the device directly instead.
    if (g_device) {
        HRESULT removed = g_device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            SetError("D3D12 device was removed during DLSS5 NR work (0x%08X)",
                     static_cast<unsigned>(removed));
            return false;
        }
    }

    g_cmd_alloc->Reset();
    g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
    return true;
}

static void WaitQueueIdle() {
    if (!g_queue || !g_fence) return;
    ++g_fence_value;
    if (SUCCEEDED(g_queue->Signal(g_fence.Get(), g_fence_value)) && g_fence->GetCompletedValue() < g_fence_value) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev) {
            g_fence->SetEventOnCompletion(g_fence_value, ev);
            WaitForSingleObject(ev, 30000);
            CloseHandle(ev);
        }
    }
}

static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

static ComPtr<ID3D12Resource> CreateTexture(UINT w, UINT h, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
                                            DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> r;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

static ComPtr<ID3D12Resource> CreateLinearBuffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    ComPtr<ID3D12Resource> r;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

static uint16_t FloatToHalf(float f) {
    uint32_t x; memcpy(&x, &f, sizeof(x));
    uint32_t s = (x >> 16) & 0x8000u;
    int32_t e = static_cast<int32_t>((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) {
        if (e < -10) return static_cast<uint16_t>(s);
        m = (m | 0x800000u) >> (1 - e);
        return static_cast<uint16_t>(s | (m >> 13));
    }
    if (e >= 31) return static_cast<uint16_t>(s | 0x7c00u);
    return static_cast<uint16_t>(s | (static_cast<uint32_t>(e) << 10) | (m >> 13));
}

static float HalfToFloat(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
    if (e == 0) {
        if (m == 0) x = s << 31;
        else {
            e = 1;
            while (!(m & 0x400)) { m <<= 1; --e; }
            m &= 0x3ff;
            x = (s << 31) | ((e + 112) << 23) | (m << 13);
        }
    } else if (e == 0x1f) x = (s << 31) | 0x7f800000u | (m << 13);
    else x = (s << 31) | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &x, sizeof(f)); return f;
}

static void ReleaseFeatureAndResources() {
    WaitQueueIdle();
    if (g_feature) {
        if (g_nr_release && g_shim_release) g_shim_release(reinterpret_cast<void*>(g_nr_release), g_feature);
        else if (g_core_release) g_core_release(g_feature);
        g_feature = nullptr;
    }
    g_color.Reset(); g_output.Reset(); g_upload.Reset(); g_readback.Reset();
    g_depth.Reset(); g_motion.Reset();
    g_width = g_height = g_row_pitch = 0;
    g_total_bytes = 0;
    g_latched = LatchedParams{};
}

static bool AllocateFrameResources(UINT w, UINT h) {
    g_color = CreateTexture(w, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g_output = CreateTexture(w, h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!g_color || !g_output) { SetError("Failed to create RGBA16F D3D12 textures"); return false; }

    // Feature 18 wants Depth and MVec bound even when there is nothing to put
    // in them. Cycles guides are not wired up yet, so these stay cleared - the
    // model then behaves as a single-frame spatial denoiser.
    g_depth = CreateTexture(w, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, DXGI_FORMAT_R32_FLOAT);
    g_motion = CreateTexture(w, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, DXGI_FORMAT_R16G16_FLOAT);
    if (!g_depth || !g_motion) { SetError("Failed to create D3D12 depth/motion guide textures"); return false; }

    g_row_pitch = (w * 8u + 255u) & ~255u;
    g_total_bytes = static_cast<UINT64>(g_row_pitch) * h;
    g_upload = CreateLinearBuffer(g_total_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    g_readback = CreateLinearBuffer(g_total_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!g_upload || !g_readback) { SetError("Failed to create D3D12 upload/readback buffers"); return false; }
    g_width = w; g_height = h;
    return true;
}

// Everything the model latches. Written once, at CreateFeature - the DLL
// ignores these when they are written only at evaluate time.
static void SetCreateParams(const LatchedParams& p) {
    VSetUInt("CreationNodeMask", 1);
    VSetUInt("VisibilityNodeMask", 1);
    // Both the generic and the DLSSNR-prefixed size keys: the DLL reads one
    // pair and which one is not documented.
    VSetUInt("Width", g_width);
    VSetUInt("Height", g_height);
    VSetUInt("OutWidth", g_width);
    VSetUInt("OutHeight", g_height);
    VSetUInt("PerfQualityValue", 2);
    VSetUInt("DLSSNR.Enabled", 1);
    VSetUInt("DLSSNR.Width", g_width);
    VSetUInt("DLSSNR.Height", g_height);
    VSetUInt("DLSSNR.InputWidth", g_width);
    VSetUInt("DLSSNR.InputHeight", g_height);
    VSetUInt("DLSSNR.OutputWidth", g_width);
    VSetUInt("DLSSNR.OutputHeight", g_height);
    VSetUInt("DLSSNR.Upscaling", 0);
    VSetUInt("DLSSNR.Style", static_cast<unsigned>(p.style));
    VSetUInt("DLSSNR.Hint.Render.Preset", static_cast<unsigned>(p.preset));
    VSetFloat("DLSSNR.Intensity", p.intensity);
    VSetFloat("DLSSNR.LocalToneStrength", p.tone);
    VSetFloat("DLSSNR.LocalStructureStrength", p.structure);
    // Negative means "leave the model default alone".
    if (p.skin >= 0.0f) VSetFloat("DLSSNR.SkinStructureStrength", p.skin);
    VSetUInt("DLSSNR.UseAutoMask", static_cast<unsigned>(p.automask));
    VSetUInt("DLSSNR.UICorrection", 0);
}

// Per-evaluate state: the resources and the frame-to-frame bookkeeping.
static void SetEvalParams(int reset) {
    VSetRes("DLSSNR.Color", g_color.Get());
    VSetRes("DLSSNR.Depth", g_depth.Get());
    VSetRes("DLSSNR.MVec", g_motion.Get());
    VSetRes("DLSSNR.Output", g_output.Get());
    VSetUInt("DLSSNR.Enabled", 1);
    VSetUInt("DLSSNR.Width", g_width);
    VSetUInt("DLSSNR.Height", g_height);
    VSetUInt("DLSSNR.Reset", static_cast<unsigned>(reset));
    VSetUInt("DLSSNR.DepthInverted", 0);
    VSetUInt("DLSSNR.ColorSubrectBaseX", 0);
    VSetUInt("DLSSNR.ColorSubrectBaseY", 0);
    VSetUInt("DLSSNR.ColorSubrectWidth", g_width);
    VSetUInt("DLSSNR.ColorSubrectHeight", g_height);
    VSetUInt("DLSSNR.DepthSubrectWidth", g_width);
    VSetUInt("DLSSNR.DepthSubrectHeight", g_height);
    VSetUInt("DLSSNR.MVecSubrectWidth", g_width);
    VSetUInt("DLSSNR.MVecSubrectHeight", g_height);
    VSetUInt("DLSSNR.OutputSubrectBaseX", 0);
    VSetUInt("DLSSNR.OutputSubrectBaseY", 0);
    VSetUInt("DLSSNR.OutputSubrectWidth", g_width);
    VSetUInt("DLSSNR.OutputSubrectHeight", g_height);
    VSetFloat("DLSSNR.MVecScaleX", 1.0f);
    VSetFloat("DLSSNR.MVecScaleY", 1.0f);
}

static bool EnsureFeature(UINT w, UINT h, const LatchedParams& params) {
    if (g_feature && w == g_width && h == g_height && params == g_latched) return true;

    ReleaseFeatureAndResources();
    if (!AllocateFrameResources(w, h)) return false;
    SetCreateParams(params);

    NGXResult r;
    if (g_nr_create && g_shim_create)
        r = g_shim_create(reinterpret_cast<void*>(g_nr_create), g_cmd.Get(), NR_FEATURE_ID, g_params, &g_feature);
    else
        r = g_core_create(g_cmd.Get(), NR_FEATURE_ID, g_params, &g_feature);
    if (r != NGX_SUCCESS || !g_feature) {
        SetError("CreateFeature(18) failed: 0x%08X. Init_Ext order=%d, param slots uint=%d float=%d. "
                 "Check GPU support, driver, nvngx_dlssnr.dll, and caller shim.",
                 static_cast<unsigned>(r), g_init_order, g_uint_slot, g_float_slot);
        return false;
    }
    g_latched = params;
    return true;
}

static bool LoadNGX() {
    // NGX modules are loaded once and never unloaded, so this is a no-op after
    // the first successful call.
    if (g_core_mod && g_nr_mod && g_shim_mod) return true;

    g_core_mod = LoadCoreNGX(g_runtime_dir);
    if (!g_core_mod) {
        SetError("Could not load NVIDIA NGX core _nvngx.dll. Tried runtime\\_nvngx.dll, normal DLL search, and NVIDIA DriverStore packages matching nv*.inf_*. You can copy the _nvngx.dll from your active NVIDIA DriverStore folder into runtime\\_nvngx.dll as an explicit override.");
        return false;
    }

    const std::wstring nr_path = Join(g_runtime_dir, L"nvngx_dlssnr.dll");
    if (!FileExists(nr_path)) { SetError("nvngx_dlssnr.dll not found in runtime folder"); return false; }

    std::wstring shim_path = Join(Join(g_runtime_dir, L"caller"), L"nvngx.dll_blender.dll");
    if (!FileExists(shim_path)) {
        // Backward-compatible fallback for source-derived builds.
        shim_path = Join(Join(g_runtime_dir, L"caller"), L"nvngx.dll_comfy.dll");
    }
    if (!FileExists(shim_path)) {
        shim_path = Join(Join(g_runtime_dir, L"caller"), L"nvngx.dll");
    }
    if (!FileExists(shim_path)) { SetError("caller shim not found (expected caller\\nvngx.dll_blender.dll)"); return false; }
    g_shim_mod = LoadLibraryW(shim_path.c_str());
    if (!g_shim_mod) { SetError("LoadLibrary(caller shim) failed: Win32 %lu", GetLastError()); return false; }

    // The snippet is loaded BY the shim, so that the module owning the caller's
    // return address during its initialisation is one the snippet accepts.
    g_shim_load_snippet = reinterpret_cast<ShimLoadSnippetFn>(GetProcAddress(g_shim_mod, "DLSSNR_LoadSnippet"));
    if (!g_shim_load_snippet) { SetError("caller shim is missing DLSSNR_LoadSnippet; rebuild it"); return false; }
    unsigned long snippet_error = 0;
    g_nr_mod = reinterpret_cast<HMODULE>(g_shim_load_snippet(nr_path.c_str(), &snippet_error));
    if (!g_nr_mod) { SetError("Loading nvngx_dlssnr.dll via caller shim failed: Win32 %lu", snippet_error); return false; }

    g_core_init_ext = reinterpret_cast<InitExtFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Init_Ext"));
    g_core_init_project = reinterpret_cast<InitProjectIdFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Init_ProjectID"));
    g_alloc_params = reinterpret_cast<AllocParamsFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_AllocateParameters"));
    g_get_caps_params = reinterpret_cast<GetCapsParamsFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    g_core_create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_CreateFeature"));
    g_core_eval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
    g_core_release = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));
    g_core_shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Shutdown"));

    g_nr_init = reinterpret_cast<SnippetInitFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_Init_Ext"));
    g_nr_create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_CreateFeature"));
    g_nr_eval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
    g_nr_release = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));

    g_shim_init = reinterpret_cast<ShimInitFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallInit"));
    g_shim_create = reinterpret_cast<ShimCreateFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallCreate"));
    g_shim_eval = reinterpret_cast<ShimEvaluateFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallEvaluate"));
    g_shim_release = reinterpret_cast<ShimReleaseFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallRelease"));

    if (!g_core_init_ext || (!g_get_caps_params && !g_alloc_params) || !g_core_create || !g_core_eval || !g_core_release || !g_core_shutdown) {
        SetError("Required NGX core exports are missing"); return false;
    }
    if (!g_nr_init || !g_nr_create || !g_nr_eval || !g_nr_release) {
        SetError("Required DLSSNR exports are missing from nvngx_dlssnr.dll"); return false;
    }
    if (!g_shim_init || !g_shim_create || !g_shim_eval || !g_shim_release) {
        SetError("Required caller shim exports are missing"); return false;
    }
    return true;
}

static bool InitNGXSession() {
    const wchar_t* paths[1] = { g_runtime_dir.c_str() };
    NGXPathListInfo pli{ paths, 1 };
    NGXFeatureCommonInfo fci{};
    fci.PathListInfo = pli;
    fci.LoggingInfo.LoggingLevel = NGX_LOG_OFF;

    bool core_ok = false;
    if (g_core_init_project) {
        for (int ver = 0x13; ver <= 0x20 && !core_ok; ++ver) {
            NGXResult r = g_core_init_project(PROJECT_ID, 0, "0.2.0", g_runtime_dir.c_str(), g_device.Get(), ver, nullptr);
            core_ok = (r == NGX_SUCCESS);
        }
    }
    if (!core_ok) {
        for (int ver = 0x13; ver <= 0x20 && !core_ok; ++ver) {
            NGXResult r = g_core_init_ext(APP_ID, g_runtime_dir.c_str(), g_device.Get(), ver, &fci);
            core_ok = (r == NGX_SUCCESS);
        }
    }
    if (!core_ok) { SetError("NGX core initialization failed for API versions 0x13..0x20"); return false; }

    NGXResult first_order_result = 0;
    g_init_order = 0;
    NGXResult sr = g_shim_init(reinterpret_cast<void*>(g_nr_init), APP_ID, g_runtime_dir.c_str(),
                               g_device.Get(), 0x15, &fci, &g_init_order, &first_order_result);
    if (sr != NGX_SUCCESS) {
        wchar_t shim_self[MAX_PATH] = L"<unknown>";
        GetModuleFileNameW(g_shim_mod, shim_self, MAX_PATH);
        char shim_utf8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, shim_self, -1, shim_utf8, static_cast<int>(sizeof(shim_utf8)), nullptr, nullptr);
        // 0xBADC0DE0 is the shim's marker for an attempt that faulted rather
        // than being rejected, which is itself evidence about the argument order.
        SetError("DLSSNR snippet Init_Ext via caller shim failed. Order 1 (version, info): 0x%08X. "
                 "Order 2 (info, version): 0x%08X. Loaded shim=%s",
                 static_cast<unsigned>(first_order_result), static_cast<unsigned>(sr), shim_utf8);
        return false;
    }

    // Feature 18 must be created on the CORE's capability parameters. A freshly
    // allocated block carries none of the snippet/preset callbacks CreateFeature
    // needs, and answers UnableToInitializeFeature (0xBAD0000B).
    g_params = nullptr;
    NGXResult ar = 0;
    if (g_get_caps_params) ar = g_get_caps_params(&g_params);
    if ((ar != NGX_SUCCESS || !g_params) && g_alloc_params) {
        g_params = nullptr;
        ar = g_alloc_params(&g_params);
    }
    if (ar != NGX_SUCCESS || !g_params) {
        SetError("NGX parameter block unavailable (GetCapabilityParameters/AllocateParameters): 0x%08X", static_cast<unsigned>(ar));
        return false;
    }
    DiscoverSetterSlots();
    return true;
}

static void ShutdownUnlocked() {
    ReleaseFeatureAndResources();
    if (g_core_shutdown) g_core_shutdown();
    g_params = nullptr;
    g_device.Reset(); g_queue.Reset(); g_cmd_alloc.Reset(); g_cmd.Reset(); g_fence.Reset();

    // The NGX modules are deliberately left mapped. FreeLibrary on the driver's
    // _nvngx.dll deadlocks: it owns worker threads and the unload never
    // completes, leaving every thread parked in a wait state. Keeping them
    // resident for the process lifetime is how NGX is meant to be used, and it
    // makes a later re-init cheap. Function pointers stay valid with them.


    g_initialized = false;
}

extern "C" {

__declspec(dllexport) const char* __cdecl dlss5nr_version() {
    return "0.4.0-shim-loaded-snippet";
}

// What the runtime actually turned out to want, filled in during init. This is
// the answer to the ABI questions the project could only guess at before.
static std::string g_abi_info = "not initialized";

__declspec(dllexport) const char* __cdecl dlss5nr_abi_info() {
    return g_abi_info.c_str();
}

__declspec(dllexport) const char* __cdecl dlss5nr_gpu_name() {
    return g_gpu_name.c_str();
}

__declspec(dllexport) int __cdecl dlss5nr_init(int gpu_index, const wchar_t* runtime_dir, char* err, int err_cap) {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_last_error.clear();
    if (g_initialized) { CopyError(err, err_cap); return 1; }
    if (!runtime_dir || !*runtime_dir) { SetError("runtime_dir is empty"); CopyError(err, err_cap); return 0; }

    g_gpu_index = gpu_index;
    g_runtime_dir = runtime_dir;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)co; // RPC_E_CHANGED_MODE is harmless for this use.

    if (!SetupD3D12() || !LoadNGX() || !InitNGXSession()) {
        ShutdownUnlocked();
        CopyError(err, err_cap);
        return 0;
    }
    g_initialized = true;
    char abi[256];
    snprintf(abi, sizeof(abi),
             "Init_Ext order=%d (1=version,info 2=info,version); param slots uint=%d float=%d",
             g_init_order, g_uint_slot, g_float_slot);
    g_abi_info = abi;
    CopyError(err, err_cap);
    return 1;
}

__declspec(dllexport) int __cdecl dlss5nr_process(
    const float* rgb_in, float* rgb_out, int width, int height,
    int style, int preset, float intensity, float tone, float structure, float skin,
    int automask, int reset, char* err, int err_cap) {

    std::lock_guard<std::mutex> guard(g_mutex);
    g_last_error.clear();
    if (!g_initialized) { SetError("DLSS5 NR bridge is not initialized"); CopyError(err, err_cap); return 0; }
    if (!rgb_in || !rgb_out || width <= 0 || height <= 0) { SetError("Invalid image buffer/dimensions"); CopyError(err, err_cap); return 0; }
    if (width > 16384 || height > 16384) { SetError("Image dimensions are unreasonably large"); CopyError(err, err_cap); return 0; }

    // A frame whose longer side is small hangs the GPU rather than failing:
    // 64x64 returns DXGI_ERROR_DEVICE_HUNG from the evaluate, while 128x32 with
    // the same 4096 pixels denoises correctly, so the constraint is the longer
    // side and not the area. 96 is the smallest long side observed to work, via
    // 96x96 and 97x61; between 65 and 95 is untested because narrowing it means
    // hanging the GPU again for each probe.
    //
    // Refusing here costs one denoise and Cycles carries on. Letting it through
    // costs a display driver reset, and every later render in the process,
    // because the device never comes back.
    if (std::max(width, height) < kMinLongSide) {
        SetError("DLSS5 NR needs a longer side of at least %d pixels; %dx%d hangs the GPU",
                 kMinLongSide, width, height);
        CopyError(err, err_cap);
        return 0;
    }

    LatchedParams params;
    params.style = style;
    params.preset = preset;
    params.intensity = intensity;
    params.tone = tone;
    params.structure = structure;
    params.skin = skin;
    params.automask = automask;

    const bool rebuilt = !g_feature || static_cast<UINT>(width) != g_width ||
                         static_cast<UINT>(height) != g_height || !(params == g_latched);
    if (!EnsureFeature(static_cast<UINT>(width), static_cast<UINT>(height), params)) {
        CopyError(err, err_cap); return 0;
    }
    SetEvalParams((reset || rebuilt) ? 1 : 0);

    void* mapped = nullptr;
    HRESULT hr = g_upload->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) { SetError("Upload buffer Map failed: 0x%08X", static_cast<unsigned>(hr)); CopyError(err, err_cap); return 0; }
    memset(mapped, 0, static_cast<size_t>(g_total_bytes));
    auto* dst_base = static_cast<uint8_t*>(mapped);
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(dst_base + static_cast<size_t>(y) * g_row_pitch);
        const float* src = rgb_in + static_cast<size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {
            // The model expects display-referred colour; Cycles hands us linear.
            row[x * 4 + 0] = FloatToHalf(LinearToSrgb(TonemapForward(src[x * 3 + 0])));
            row[x * 4 + 1] = FloatToHalf(LinearToSrgb(TonemapForward(src[x * 3 + 1])));
            row[x * 4 + 2] = FloatToHalf(LinearToSrgb(TonemapForward(src[x * 3 + 2])));
            row[x * 4 + 3] = FloatToHalf(1.0f);
        }
    }
    g_upload->Unmap(0, nullptr);

    auto b1 = Barrier(g_color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    g_cmd->ResourceBarrier(1, &b1);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_color.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    src.PlacedFootprint.Footprint.Width = width; src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = g_row_pitch;
    g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto b2 = Barrier(g_color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_cmd->ResourceBarrier(1, &b2);

    NGXResult er = g_shim_eval(reinterpret_cast<void*>(g_nr_eval), g_cmd.Get(), g_feature, g_params, nullptr);
    if (er != NGX_SUCCESS) {
        SetError("DLSSNR EvaluateFeature failed: 0x%08X", static_cast<unsigned>(er));
        // Reset command list to a clean state before returning.
        ExecuteAndWait();
        CopyError(err, err_cap); return 0;
    }

    auto b3 = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_cmd->ResourceBarrier(1, &b3);
    D3D12_TEXTURE_COPY_LOCATION rd{};
    rd.pResource = g_readback.Get(); rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    rd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.PlacedFootprint.Footprint.Width = width; rd.PlacedFootprint.Footprint.Height = height;
    rd.PlacedFootprint.Footprint.Depth = 1; rd.PlacedFootprint.Footprint.RowPitch = g_row_pitch;
    D3D12_TEXTURE_COPY_LOCATION rs{};
    rs.pResource = g_output.Get(); rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g_cmd->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
    auto b4 = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_cmd->ResourceBarrier(1, &b4);

    if (!ExecuteAndWait()) { CopyError(err, err_cap); return 0; }

    void* rmap = nullptr;
    hr = g_readback->Map(0, nullptr, &rmap);
    if (FAILED(hr) || !rmap) { SetError("Readback Map failed: 0x%08X", static_cast<unsigned>(hr)); CopyError(err, err_cap); return 0; }
    const auto* base = static_cast<const uint8_t*>(rmap);
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) * g_row_pitch);
        float* dstf = rgb_out + static_cast<size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {
            // Return the resource channels exactly as stored. Some stock/reference
            // DLSSNR builds have been observed to produce B,G,R,A while patched
            // Ada builds may produce R,G,B,A. Python selects/auto-detects the
            // correct interpretation instead of hard-coding a swap here.
            dstf[x * 3 + 0] = TonemapInverse(SrgbToLinear(std::clamp(HalfToFloat(row[x * 4 + 0]), 0.0f, 1.0f)));
            dstf[x * 3 + 1] = TonemapInverse(SrgbToLinear(std::clamp(HalfToFloat(row[x * 4 + 1]), 0.0f, 1.0f)));
            dstf[x * 3 + 2] = TonemapInverse(SrgbToLinear(std::clamp(HalfToFloat(row[x * 4 + 2]), 0.0f, 1.0f)));
        }
    }
    g_readback->Unmap(0, nullptr);
    CopyError(err, err_cap);
    return 1;
}

__declspec(dllexport) void __cdecl dlss5nr_shutdown() {
    std::lock_guard<std::mutex> guard(g_mutex);
    ShutdownUnlocked();
}

}
