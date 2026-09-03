// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ComfyUI-DLSS5-NR contributors

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

using NGXResult = int;

struct NGXHandle { unsigned int Id; };
struct NGXParameter;

// Init_Ext argument order.
//
//   order 1, the public NGX signature, and what video2dlssnr's forwarder uses:
//     (app, path, device, version, FeatureCommonInfo*)
//   order 2, what this project used to assume:
//     (app, path, device, FeatureCommonInfo*, version)
//
// MEASURED: order 1 is correct. Confirmed against nvngx_dlssnr.dll 310.8.0 on
// driver 591.86, Blackwell. The old order-2 assumption was simply wrong.
//
// Order 1 is tried first and order 2 kept as a fallback for other runtime
// builds. Guessing wrong puts an int where the callee expects a pointer, which
// can fault on dereference, so each attempt runs under SEH and a fault counts
// as "wrong order" rather than taking Blender down with it.
using SnippetInitVersionFirst = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using SnippetInitInfoFirst = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);
using CreateFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using EvalFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ReleaseFn = NGXResult(__cdecl*)(NGXHandle*);
using ShutdownFn = NGXResult(__cdecl*)();

// The NR runtime validates the module that owns its RETURN ADDRESS. A trivial
// wrapper built with /O2 can be tail-call-optimized into a JMP, which would
// leave the return address in dlss5nr_bridge.dll and trigger 0xBAD00002.
// This observable post-call store forces a real CALL/RET through this DLL.
static volatile LONG g_post_call_sink = 0;
static __forceinline NGXResult FinishCall(NGXResult r) {
    g_post_call_sink = static_cast<LONG>(r);
    return r;
}

// Load the snippet from THIS module rather than from the bridge.
//
// The snippet resolves the module owning its caller's return address and
// rejects any whose path does not contain "nvngx.dll", and it does so during
// its own initialisation too, not only at Init_Ext. Loading it directly from
// dlss5nr_bridge.dll was observed to deadlock the process: every thread parked
// in a wait state inside LoadLibraryW with no CPU and no I/O.
//
// LOAD_WITH_ALTERED_SEARCH_PATH additionally makes the snippet's own
// dependencies resolve from its directory, which is how the reference
// forwarder loads it.
extern "C" __declspec(dllexport) __declspec(noinline) void* __cdecl DLSSNR_LoadSnippet(
    const wchar_t* path, unsigned long* out_last_error) {
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module && out_last_error) *out_last_error = GetLastError();
    g_post_call_sink = module ? 1 : 0;
    return reinterpret_cast<void*>(module);
}

// NGX_SUCCESS. Anything else means the call was rejected, not that it faulted.
static constexpr NGXResult kNgxSuccess = 1;
// Returned when an attempt faulted, so the caller can tell it apart from a
// genuine NGX rejection code.
static constexpr NGXResult kFaulted = static_cast<NGXResult>(0xBADC0DE0);

// Each attempt is its own noinline function so the snippet's return-address
// check still resolves to this module.
__declspec(noinline) static NGXResult TryInitVersionFirst(
    void* real_fn, unsigned long long app_id, const wchar_t* path,
    ID3D12Device* device, int version, const void* common_info) {
    NGXResult r;
    __try {
        r = reinterpret_cast<SnippetInitVersionFirst>(real_fn)(app_id, path, device, version, common_info);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        r = kFaulted;
    }
    return FinishCall(r);
}

__declspec(noinline) static NGXResult TryInitInfoFirst(
    void* real_fn, unsigned long long app_id, const wchar_t* path,
    ID3D12Device* device, int version, const void* common_info) {
    NGXResult r;
    __try {
        r = reinterpret_cast<SnippetInitInfoFirst>(real_fn)(app_id, path, device, common_info, version);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        r = kFaulted;
    }
    return FinishCall(r);
}

extern "C" {

// out_order reports which argument order was used: 1 = version before the
// info pointer (public NGX signature), 2 = info pointer before version,
// 0 = neither worked. May be null. out_first_result carries the result of
// order 1 when order 2 is the one that succeeded, so a caller reporting a
// failure can show what both attempts said.
__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallInit(
    void* real_fn, unsigned long long app_id, const wchar_t* path,
    ID3D12Device* device, int version, const void* common_info,
    int* out_order, NGXResult* out_first_result) {
    if (out_order) *out_order = 0;

    const NGXResult first = TryInitVersionFirst(real_fn, app_id, path, device, version, common_info);
    if (out_first_result) *out_first_result = first;
    if (first == kNgxSuccess) {
        if (out_order) *out_order = 1;
        return first;
    }

    const NGXResult second = TryInitInfoFirst(real_fn, app_id, path, device, version, common_info);
    if (second == kNgxSuccess && out_order) *out_order = 2;
    return second;
}

__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallCreate(
    void* real_fn, ID3D12GraphicsCommandList* list, int feature_id,
    NGXParameter* params, NGXHandle** handle) {
    NGXResult r = reinterpret_cast<CreateFn>(real_fn)(list, feature_id, params, handle);
    return FinishCall(r);
}

__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallEvaluate(
    void* real_fn, ID3D12GraphicsCommandList* list, const NGXHandle* handle,
    const NGXParameter* params, void* callback) {
    NGXResult r = reinterpret_cast<EvalFn>(real_fn)(list, handle, params, callback);
    return FinishCall(r);
}

__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallRelease(void* real_fn, NGXHandle* handle) {
    NGXResult r = reinterpret_cast<ReleaseFn>(real_fn)(handle);
    return FinishCall(r);
}

__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallShutdown(void* real_fn) {
    NGXResult r = reinterpret_cast<ShutdownFn>(real_fn)();
    return FinishCall(r);
}

}
