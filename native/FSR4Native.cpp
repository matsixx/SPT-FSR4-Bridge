// SPTVR_FSR4.dll — native FSR4 bridge for SPT-VR.
//
// Tarkov/Unity runs D3D11; AMD's FSR4 (and the FSR 3.1.x providers in the same DLL) run on
// D3D12 only. This plugin bridges the two: per eye, the mod's render-res color/depth/motion
// textures are copied into shared (D3D11<->D3D12) textures, ffxDispatch runs the upscale on a
// private D3D12 queue, and the result is copied back into the mod's output RT. Sync is a single
// shared timeline fence (all GPU-side waits — the CPU never blocks in the frame loop).
//
// Everything executes on Unity's render thread via the IssuePluginEvent callback (the same
// pattern BSG's own DLSSImporter plugin uses); the C# side writes per-frame params into a small
// per-eye slot ring beforehand, so a lagging render thread can never read torn params.
//
// FSR version selection: ffxQuery(GetVersions) enumerates the providers exposed by
// amd_fidelityfx_loader_dx12/amd_fidelityfx_upscaler_dx12 (FSR4 shows up on RDNA4, and on RDNA3 via the
// FSR 4.1+ INT8 model, when the driver ships amdxcffx64). We pick the highest major-4 version when
// preferred, else the default provider (latest 3.1.x). This bridge is architecture-agnostic — the
// INT8-vs-FP8 model choice happens inside the driver's provider — so the same code path works on any GPU.
//
// Built against the MIT-licensed FidelityFX SDK 2.3.0 ffx-api headers (vendored under ffx/).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "ffx/api/include/ffx_api.h"
#include "ffx/api/include/ffx_api_types.h"
#include "ffx/api/include/dx12/ffx_api_dx12.h"
#include "ffx/upscalers/include/ffx_upscale.h"

using Microsoft::WRL::ComPtr;

#define FSR4_EXPORT extern "C" __declspec(dllexport)

// ---------------------------------------------------------------- status codes (mirrored in C#)
enum Fsr4Status : int
{
    FSR4_OK                  = 0,
    FSR4_NOT_INITIALIZED     = 1,
    FSR4_LOADER_MISSING      = 2,
    FSR4_ENTRYPOINTS_MISSING = 3,
    FSR4_DEVICE_FAILED       = 4,
    FSR4_INTEROP_UNSUPPORTED = 5,
    FSR4_NO_VERSIONS         = 6,
    FSR4_CONTEXT_FAILED      = 7,
    FSR4_DISPATCH_FAILED     = 8,
    FSR4_DEVICE_LOST         = 9,   // D3D12 device removed (renderer refresh) — RECOVERABLE: rebuild + retry
};

// context-creation flag bits passed from C# (not the raw FFX flags so C# stays decoupled)
enum Fsr4Flags : int
{
    FSR4F_HDR             = 1 << 0,
    FSR4F_DEPTH_INVERTED  = 1 << 1,
    FSR4F_DEPTH_INFINITE  = 1 << 2,
    FSR4F_AUTO_EXPOSURE   = 1 << 3,
    FSR4F_DEBUG_CHECKING  = 1 << 4,
    FSR4F_DYNAMIC_RES     = 1 << 5,
    FSR4F_PREFER_FSR4     = 1 << 6,
    FSR4F_RESET           = 1 << 7,  // per-frame history reset request
    FSR4F_MV_JITTER_CANCEL = 1 << 8, // motion vectors have the jitter baked in (FSR removes it)
    FSR4F_REACTIVE        = 1 << 9,  // opaque-only color supplied → auto-generate + feed a reactive mask
};

// ---------------------------------------------------------------- logging (drained by C# into BepInEx log)
static std::mutex             gLogMutex;
static std::deque<std::string> gLogLines;

static void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    {
        std::lock_guard<std::mutex> lock(gLogMutex);
        if (gLogLines.size() > 256)
            gLogLines.pop_front();
        gLogLines.emplace_back(buf);
    }
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

static void LogW(const wchar_t* prefix, const wchar_t* msg)
{
    char narrow[900];
    int  n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    narrow[n > 0 ? n : 0] = 0;
    char pre[100];
    n = WideCharToMultiByte(CP_UTF8, 0, prefix, -1, pre, sizeof(pre) - 1, nullptr, nullptr);
    pre[n > 0 ? n : 0] = 0;
    Log("%s%s", pre, narrow);
}

static void FfxMessageCallback(uint32_t type, const wchar_t* message)
{
    LogW(type == FFX_API_MESSAGE_TYPE_ERROR ? L"[ffx ERROR] " : L"[ffx warn] ", message);
}

// ---------------------------------------------------------------- global state
static std::recursive_mutex gRenderMutex;   // guards device/context/eye state across render events, preflight, invalidate
static std::atomic<int>     gStatus{FSR4_NOT_INITIALIZED};

static HMODULE      gLoaderModule   = nullptr;
static HMODULE      gUpscalerModule = nullptr;   // preloaded so the loader's by-name resolution hits it
static HMODULE      gDxilModule     = nullptr;   // the Agility SDK's DXIL validator
static std::wstring gFfxDir;                      // the native/ folder the DLLs live in
static PfnFfxCreateContext  pfnCreateContext  = nullptr;
static PfnFfxDestroyContext pfnDestroyContext = nullptr;
static PfnFfxConfigure      pfnConfigure      = nullptr;
static PfnFfxQuery          pfnQuery          = nullptr;
static PfnFfxDispatch       pfnDispatch       = nullptr;

// The AMD FFX provider (amd_fidelityfx_upscaler_dx12 / the driver's amdxcffx64) can access-violate
// inside these calls on some GPU/driver combos (observed: a non-RDNA4 AMD card during the first
// dispatch). A native AV in a third-party DLL isn't a C++ exception, so it would take the whole game
// down. These SEH shims turn such a crash into a sentinel return code, which the callers treat as a
// normal failure → FSR4 disables and the game falls back to its own FSR3. The shims are object-free
// (no C++ unwinding) so __try/__except is legal here.
static const ffxReturnCode_t FFX_SEH_CRASH = 0x0BADC0DE;

static ffxReturnCode_t SafeFfxQuery(ffxContext* ctx, ffxQueryDescHeader* desc)
{
    __try { return pfnQuery(ctx, desc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return FFX_SEH_CRASH; }
}
static ffxReturnCode_t SafeFfxCreateContext(ffxContext* ctx, ffxCreateContextDescHeader* desc)
{
    __try { return pfnCreateContext(ctx, desc, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return FFX_SEH_CRASH; }
}
static ffxReturnCode_t SafeFfxDispatch(ffxContext* ctx, ffxDispatchDescHeader* desc)
{
    __try { return pfnDispatch(ctx, desc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return FFX_SEH_CRASH; }
}
static void SafeFfxDestroyContext(ffxContext* ctx)
{
    __try { pfnDestroyContext(ctx, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static ComPtr<ID3D11Device>          g11;
static ComPtr<ID3D11Device1>         g11_1;
static ComPtr<ID3D11Device5>         g11_5;
static ComPtr<ID3D11DeviceContext>   gCtx11;
static ComPtr<ID3D11DeviceContext4>  gCtx11_4;
static ComPtr<ID3D12Device>          g12;
static ComPtr<ID3D12CommandQueue>    gQueue;
static ComPtr<ID3D12Fence>           gFence12;
static ComPtr<ID3D11Fence>           gFence11;
static UINT64                        gFenceValue = 0;
static HANDLE                        gFenceEvent = nullptr;

// enumerated upscaler versions (once per device)
static bool                  gVersionsQueried = false;
static std::vector<uint64_t> gVersionIds;
static std::vector<std::string> gVersionNames;
static uint64_t              gVersionIdFsr4 = 0;   // highest "4.x" version id (0 = none)
static uint64_t              gVersionId3x   = 0;   // highest "3.x" version id (0 = none)
static bool                  gForceProvider3x = false; // set after FSR4 crashed on this GPU (e.g. RDNA3)
static bool                  gChosenIsFsr4     = false;
static char                  gActiveVersionName[128] = "none";

// ---------------------------------------------------------------- per-eye state
struct SharedTex
{
    void*                   unityPtr = nullptr;   // last seen ID3D11Texture2D* (identity key)
    UINT                    width = 0, height = 0;
    DXGI_FORMAT             format = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12Resource>  res12;
    ComPtr<ID3D11Texture2D> tex11;
};

struct FrameParams
{
    void* colorTex;
    void* depthTex;
    void* mvecTex;
    void* outTex;
    void* opaqueTex;   // opaque-only color (pre-transparency) for reactive-mask generation; null = disabled
    int   renderW, renderH;
    int   outW, outH;
    float jitterX, jitterY;
    float mvScaleX, mvScaleY;
    float camNear, camFar, fovY;
    float dtMs;
    float sharpness;
    int   flags;
};

static const int PARAM_SLOTS   = 4;
static const int ALLOC_RING    = 3;

struct Eye
{
    FrameParams paramSlots[PARAM_SLOTS] = {};
    std::mutex  paramMutex;

    SharedTex color, depth, mvec, output, opaque;
    ComPtr<ID3D12Resource> reactiveMask;   // D3D12-internal (not shared) UAV target for the reactive mask
    UINT reactiveW = 0, reactiveH = 0;

    ffxContext ctx = nullptr;
    UINT ctxOutW = 0, ctxOutH = 0;
    int  ctxFlags = -1;
    bool firstDispatch = true;

    ComPtr<ID3D12CommandAllocator>    alloc[ALLOC_RING];
    UINT64                            allocFence[ALLOC_RING] = {};
    int                               allocIdx = 0;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
};

static Eye gEyes[2];

// ---------------------------------------------------------------- helpers
static void DestroyEyeContext(Eye& eye)
{
    if (eye.ctx)
    {
        SafeFfxDestroyContext(&eye.ctx);
        eye.ctx = nullptr;
    }
    eye.firstDispatch = true;
}

static void ReleaseEyeResources(Eye& eye)
{
    DestroyEyeContext(eye);
    eye.color  = SharedTex{};
    eye.depth  = SharedTex{};
    eye.mvec   = SharedTex{};
    eye.output = SharedTex{};
    eye.opaque = SharedTex{};
    eye.reactiveMask.Reset();
    eye.reactiveW = eye.reactiveH = 0;
}

// The reactive mask is a D3D12-INTERNAL target (not shared with D3D11): the generate-reactive dispatch
// writes it (UAV) and the upscale dispatch reads it, both on our private device — no cross-API handoff.
static bool EnsureReactiveMask(Eye& e, UINT w, UINT h)
{
    if (e.reactiveMask && e.reactiveW == w && e.reactiveH == h)
        return true;
    e.reactiveMask.Reset();

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_R8_UNORM;   // FSR reactive mask format
    rd.SampleDesc       = {1, 0};
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = g12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&e.reactiveMask));
    if (FAILED(hr))
    {
        Log("[FSR4] reactive mask create failed 0x%08lX", hr);
        return false;
    }
    e.reactiveW = w;
    e.reactiveH = h;
    return true;
}

static void WaitQueueIdle()
{
    if (!gQueue || !gFence12)
        return;
    UINT64 v = ++gFenceValue;
    gQueue->Signal(gFence12.Get(), v);
    if (gFence12->GetCompletedValue() < v)
    {
        gFence12->SetEventOnCompletion(v, gFenceEvent);
        WaitForSingleObject(gFenceEvent, 5000);
    }
}

static bool LoadFfxLibrary(const wchar_t* dir)
{
    if (pfnCreateContext)
        return true;

    gFfxDir = dir;

    wchar_t path[MAX_PATH];

    // Preload the DXIL validator so D3D12Core (Agility SDK) can validate the FSR4 shaders regardless
    // of the process DLL search path. Harmless if it isn't needed.
    swprintf_s(path, L"%s\\dxil.dll", dir);
    gDxilModule = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    // Preload the upscaler provider DLL first so the loader's internal by-name LoadLibrary
    // resolves to it regardless of the process's DLL search path (we live in a BepInEx plugin
    // folder, not next to the exe).
    swprintf_s(path, L"%s\\amd_fidelityfx_upscaler_dx12.dll", dir);
    gUpscalerModule = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!gUpscalerModule)
        Log("[FSR4] warning: amd_fidelityfx_upscaler_dx12.dll not preloaded (err %lu) — loader may still find it", GetLastError());

    swprintf_s(path, L"%s\\amd_fidelityfx_loader_dx12.dll", dir);
    gLoaderModule = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!gLoaderModule)
    {
        Log("[FSR4] FAILED to load amd_fidelityfx_loader_dx12.dll from plugin dir (err %lu)", GetLastError());
        gStatus = FSR4_LOADER_MISSING;
        return false;
    }

    pfnCreateContext  = (PfnFfxCreateContext)GetProcAddress(gLoaderModule, "ffxCreateContext");
    pfnDestroyContext = (PfnFfxDestroyContext)GetProcAddress(gLoaderModule, "ffxDestroyContext");
    pfnConfigure      = (PfnFfxConfigure)GetProcAddress(gLoaderModule, "ffxConfigure");
    pfnQuery          = (PfnFfxQuery)GetProcAddress(gLoaderModule, "ffxQuery");
    pfnDispatch       = (PfnFfxDispatch)GetProcAddress(gLoaderModule, "ffxDispatch");

    if (!pfnCreateContext || !pfnDestroyContext || !pfnQuery || !pfnDispatch)
    {
        // Some packagings export the api from the upscaler DLL instead — try it before giving up.
        if (gUpscalerModule)
        {
            pfnCreateContext  = (PfnFfxCreateContext)GetProcAddress(gUpscalerModule, "ffxCreateContext");
            pfnDestroyContext = (PfnFfxDestroyContext)GetProcAddress(gUpscalerModule, "ffxDestroyContext");
            pfnConfigure      = (PfnFfxConfigure)GetProcAddress(gUpscalerModule, "ffxConfigure");
            pfnQuery          = (PfnFfxQuery)GetProcAddress(gUpscalerModule, "ffxQuery");
            pfnDispatch       = (PfnFfxDispatch)GetProcAddress(gUpscalerModule, "ffxDispatch");
        }
        if (!pfnCreateContext || !pfnDestroyContext || !pfnQuery || !pfnDispatch)
        {
            Log("[FSR4] ffx-api entry points not found in loader/upscaler DLLs");
            gStatus = FSR4_ENTRYPOINTS_MISSING;
            return false;
        }
        Log("[FSR4] ffx-api entry points resolved from amd_fidelityfx_upscaler_dx12.dll");
    }
    else
    {
        Log("[FSR4] ffx-api entry points resolved from amd_fidelityfx_loader_dx12.dll");
    }
    return true;
}

// Create the D3D12 device, opting into the bundled DirectX 12 Agility SDK runtime (D3D12Core.dll)
// FIRST. FSR4 4.1.1 requires Shader Model 6.6, which per AMD requires the DX12 Agility SDK 1.4.9+ —
// the OS's built-in D3D12 runtime is too old, so without this the FSR4 shaders (especially the RDNA3
// INT8 path) access-violate on dispatch. We opt in at runtime via CreateDeviceFactory (works from a
// plugin DLL — the usual exe-export method isn't available to us). Falls back to the OS runtime if the
// Agility core is missing or the opt-in is refused.
static HRESULT CreateD3D12DeviceWithAgility(IDXGIAdapter* adapter)
{
    std::wstring coreDirW = gFfxDir + L"\\D3D12\\";     // folder holding D3D12Core.dll
    char coreDir[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, coreDirW.c_str(), -1, coreDir, sizeof(coreDir) - 1, nullptr, nullptr);

    ComPtr<ID3D12SDKConfiguration1> sdkConfig;
    if (SUCCEEDED(D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdkConfig))))
    {
        ComPtr<ID3D12DeviceFactory> factory;
        // 616 = the Agility SDK version the FSR SDK 2.3 sample ships (well past the required 1.4.9).
        HRESULT hrf = sdkConfig->CreateDeviceFactory(616, coreDir, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hrf) && factory)
        {
            HRESULT hrd = factory->CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g12));
            if (SUCCEEDED(hrd) && g12)
            {
                Log("[FSR4] D3D12 device via Agility SDK v616 (SM 6.6 available for FSR4) — %s", coreDir);
                return S_OK;
            }
            Log("[FSR4] Agility CreateDevice failed (0x%08lX) — falling back to OS D3D12", hrd);
        }
        else
        {
            Log("[FSR4] Agility CreateDeviceFactory failed (0x%08lX, path=%s) — falling back to OS D3D12 "
                "(FSR4 may be unavailable / RDNA3 will crash → 3.1.x). Enable Windows Developer Mode if this persists.", hrf, coreDir);
        }
    }
    else
    {
        Log("[FSR4] D3D12GetInterface(SDKConfiguration) unavailable — OS D3D12 too old for the Agility opt-in");
    }

    return D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g12));
}

// Bring up the D3D11 views + the private D3D12 device/queue/fence from any Unity texture.
static bool EnsureDevices(ID3D11Texture2D* anyUnityTex)
{
    // Re-validate against Unity's CURRENT D3D11 device every call. On a renderer refresh (EFT does one
    // when the SteamVR dashboard opens) the game can recreate its D3D11 device; our cached device,
    // context, shared textures and interop fence are then stranded on the OLD device, and the very next
    // CopyResource runs cross-device → driver crash. If the device changed, fully tear down and rebuild
    // from the new one. Costs the FSR4 model-reload hitch, but ONLY on an actual device change — and a
    // one-time hitch beats a crash. (No-op on the common case where the device is unchanged.)
    ComPtr<ID3D11Device> curDev;
    anyUnityTex->GetDevice(&curDev);

    // Two ways the interop can go stale across a renderer refresh (SteamVR dashboard lowers the game's
    // res in the background): (a) Unity recreates its D3D11 device, or (b) — the one actually observed —
    // OUR private D3D12 device gets DEVICE_REMOVED. Either way, keep going with the dead device = every
    // op fails (0x887A0005) → we'd drop FSR4 for the whole session. Detect both and fully rebuild.
    HRESULT devReason  = g12 ? g12->GetDeviceRemovedReason() : S_OK;
    bool deviceLost    = FAILED(devReason);
    bool d3d11Changed  = g12 && curDev.Get() != g11.Get();

    if (g12 && !deviceLost && !d3d11Changed)
        return true;

    if (g12)
    {
        if (deviceLost)
            Log("[FSR4] D3D12 device REMOVED (reason 0x%08lX) — full rebuild", (unsigned long)devReason);
        else
            Log("[FSR4] Unity D3D11 device changed (renderer refresh) — full rebuild");
        if (!deviceLost)
            WaitQueueIdle();   // only touch the queue/fence while the device is still alive
        for (int e = 0; e < 2; e++)
        {
            ReleaseEyeResources(gEyes[e]);
            for (int i = 0; i < ALLOC_RING; i++) gEyes[e].alloc[i].Reset();
            gEyes[e].cmdList.Reset();
        }
        gFence11.Reset(); gFence12.Reset(); gQueue.Reset(); g12.Reset();
        gCtx11_4.Reset(); gCtx11.Reset(); g11_5.Reset(); g11_1.Reset(); g11.Reset();
        if (gFenceEvent) { CloseHandle(gFenceEvent); gFenceEvent = nullptr; }
        gVersionsQueried = false;
    }

    g11 = curDev;
    g11->GetImmediateContext(&gCtx11);

    if (FAILED(g11.As(&g11_1)) || FAILED(g11.As(&g11_5)) || FAILED(gCtx11.As(&gCtx11_4)))
    {
        Log("[FSR4] D3D11.4 interfaces unavailable (fence interop unsupported on this system)");
        gStatus = FSR4_INTEROP_UNSUPPORTED;
        return false;
    }

    // D3D12 device on the SAME adapter as Unity's D3D11 device.
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(g11.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)))
    {
        Log("[FSR4] failed to resolve DXGI adapter from Unity's device");
        gStatus = FSR4_DEVICE_FAILED;
        return false;
    }

    HRESULT hr = CreateD3D12DeviceWithAgility(adapter.Get());
    if (FAILED(hr))
    {
        Log("[FSR4] D3D12CreateDevice failed (0x%08lX)", hr);
        gStatus = FSR4_DEVICE_FAILED;
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g12->CreateCommandQueue(&qd, IID_PPV_ARGS(&gQueue))))
    {
        Log("[FSR4] failed to create D3D12 command queue");
        gStatus = FSR4_DEVICE_FAILED;
        return false;
    }

    // One shared timeline fence drives all cross-API sync.
    if (FAILED(g12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gFence12))))
    {
        Log("[FSR4] failed to create shared D3D12 fence");
        gStatus = FSR4_DEVICE_FAILED;
        return false;
    }
    HANDLE fenceHandle = nullptr;
    if (FAILED(g12->CreateSharedHandle(gFence12.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle)) ||
        FAILED(g11_5->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&gFence11))))
    {
        if (fenceHandle) CloseHandle(fenceHandle);
        Log("[FSR4] failed to share fence into D3D11 — interop unsupported");
        gStatus = FSR4_INTEROP_UNSUPPORTED;
        return false;
    }
    CloseHandle(fenceHandle);
    gFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    for (int e = 0; e < 2; e++)
    {
        for (int i = 0; i < ALLOC_RING; i++)
            g12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gEyes[e].alloc[i]));
        g12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gEyes[e].alloc[0].Get(), nullptr, IID_PPV_ARGS(&gEyes[e].cmdList));
        gEyes[e].cmdList->Close();
    }

    DXGI_ADAPTER_DESC ad = {};
    adapter->GetDesc(&ad);
    LogW(L"[FSR4] D3D12 device created on adapter: ", ad.Description);
    return true;
}

static void QueryVersionsOnce()
{
    if (gVersionsQueried)
        return;
    gVersionsQueried = true;

    ffxQueryDescGetVersions q = {};
    q.header.type    = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device         = g12.Get();
    uint64_t count   = 0;
    q.outputCount    = &count;

    ffxReturnCode_t rc = SafeFfxQuery(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK || count == 0)
    {
        Log("[FSR4] version query failed (rc=0x%X%s, count=%llu)", rc,
            rc == FFX_SEH_CRASH ? " CRASHED" : "", (unsigned long long)count);
        gStatus = FSR4_NO_VERSIONS;
        return;
    }

    std::vector<uint64_t>    ids(count);
    std::vector<const char*> names(count);
    q.versionIds   = ids.data();
    q.versionNames = names.data();
    rc = SafeFfxQuery(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK)
    {
        Log("[FSR4] version id/name query failed (rc=0x%X)", rc);
        gStatus = FSR4_NO_VERSIONS;
        return;
    }

    gVersionIds.clear();
    gVersionNames.clear();
    int bestFsr4 = -1, best3x = -1;
    for (uint64_t i = 0; i < count; i++)
    {
        const char* name = names[i] ? names[i] : "?";
        gVersionIds.push_back(ids[i]);
        gVersionNames.emplace_back(name);
        Log("[FSR4] available upscaler version: %s (id 0x%016llX)", name, (unsigned long long)ids[i]);

        // Providers report names beginning with the major version, e.g. "4.1.1" / "3.1.5" (maybe with
        // an "FSR " prefix). Track the highest 4.x (FSR4) and the highest 3.x (FSR 3.1.x) separately.
        const char* p = name;
        while (*p && (*p < '0' || *p > '9')) p++;   // skip any "FSR " style prefix
        if (*p == '4' && p[1] == '.')
        {
            if (bestFsr4 < 0 || strcmp(name, gVersionNames[bestFsr4].c_str()) > 0) bestFsr4 = (int)i;
        }
        else if (*p == '3' && p[1] == '.')
        {
            if (best3x < 0 || strcmp(name, gVersionNames[best3x].c_str()) > 0) best3x = (int)i;
        }
    }
    gVersionIdFsr4 = bestFsr4 >= 0 ? gVersionIds[bestFsr4] : 0;
    gVersionId3x   = best3x   >= 0 ? gVersionIds[best3x]   : 0;
    Log("[FSR4] providers: FSR4=%s, 3.1.x=%s",
        bestFsr4 >= 0 ? gVersionNames[bestFsr4].c_str() : "(none)",
        best3x   >= 0 ? gVersionNames[best3x].c_str()   : "(none)");
}

// Picks the version override for a new context: FSR4 when preferred + available + not disabled after a
// crash; otherwise the 3.1.x provider. Returns 0 only if neither is enumerable (falls to ffx default).
static uint64_t ResolveProviderVersionId(bool preferFsr4)
{
    if (preferFsr4 && !gForceProvider3x && gVersionIdFsr4 != 0)
    {
        gChosenIsFsr4 = true;
        return gVersionIdFsr4;
    }
    gChosenIsFsr4 = false;
    return gVersionId3x;   // force the 3.1.x provider (0 → ffx default only if no 3.x enumerated)
}

static bool CreateEyeContext(Eye& eye, UINT outW, UINT outH, int flags)
{
    DestroyEyeContext(eye);

    uint32_t ffxFlags = 0;
    if (flags & FSR4F_HDR)            ffxFlags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (flags & FSR4F_DEPTH_INVERTED) ffxFlags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (flags & FSR4F_DEPTH_INFINITE) ffxFlags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (flags & FSR4F_AUTO_EXPOSURE)  ffxFlags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (flags & FSR4F_DEBUG_CHECKING) ffxFlags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    if (flags & FSR4F_DYNAMIC_RES)    ffxFlags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    if (flags & FSR4F_MV_JITTER_CANCEL) ffxFlags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;

    // maxRenderSize = output size: with dynamic resolution on, any render res up to the output
    // res works without recreating the context — quality-preset changes and transient renderer
    // refreshes (SteamVR dashboard!) then never pay the FSR4 model-reload hitch.
    ffxCreateContextDescUpscale desc = {};
    desc.header.type    = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.flags          = ffxFlags;
    desc.maxRenderSize  = {outW, outH};
    desc.maxUpscaleSize = {outW, outH};
    desc.fpMessage      = &FfxMessageCallback;

    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device      = g12.Get();
    desc.header.pNext   = &backend.header;

    ffxCreateContextDescUpscaleVersion apiVersion = {};
    apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    apiVersion.version     = FFX_UPSCALER_VERSION;
    backend.header.pNext   = &apiVersion.header;

    bool preferFsr4 = (flags & FSR4F_PREFER_FSR4) != 0;
    uint64_t overrideId = ResolveProviderVersionId(preferFsr4);

    ffxOverrideVersion over = {};
    over.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    if (overrideId != 0)
    {
        over.versionId         = overrideId;
        apiVersion.header.pNext = &over.header;
    }

    ffxReturnCode_t rc = SafeFfxCreateContext(&eye.ctx, &desc.header);

    // If creating the FSR4 context itself crashed, permanently drop to the 3.1.x provider and retry.
    if (rc == FFX_SEH_CRASH && gChosenIsFsr4 && !gForceProvider3x)
    {
        Log("[FSR4] FSR4 context creation crashed on this GPU — switching to the FSR 3.1.x provider");
        gForceProvider3x = true;
        overrideId = ResolveProviderVersionId(preferFsr4);
        over.versionId = overrideId;
        apiVersion.header.pNext = overrideId != 0 ? &over.header : nullptr;
        rc = SafeFfxCreateContext(&eye.ctx, &desc.header);
    }
    // A non-crash refusal of the override — retry with the ffx default provider.
    else if (rc != FFX_API_RETURN_OK && rc != FFX_SEH_CRASH && overrideId != 0)
    {
        Log("[FSR4] context creation with version override failed (rc=0x%X) — retrying default provider", rc);
        apiVersion.header.pNext = nullptr;
        gChosenIsFsr4    = false;
        rc = SafeFfxCreateContext(&eye.ctx, &desc.header);
    }
    if (rc != FFX_API_RETURN_OK)
    {
        Log("[FSR4] ffxCreateContext failed (rc=0x%X%s, out %ux%u, flags 0x%X)", rc,
            rc == FFX_SEH_CRASH ? " CRASHED — disabling FSR4, game will fall back to FSR3" : "", outW, outH, ffxFlags);
        eye.ctx = nullptr;
        gStatus = FSR4_CONTEXT_FAILED;
        return false;
    }

    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (SafeFfxQuery(&eye.ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
    {
        strncpy_s(gActiveVersionName, pv.versionName, _TRUNCATE);
        Log("[FSR4] upscale context created: provider %s (out %ux%u, flags 0x%X)", pv.versionName, outW, outH, ffxFlags);
    }
    else
    {
        Log("[FSR4] upscale context created (out %ux%u, flags 0x%X)", outW, outH, ffxFlags);
    }

    eye.ctxOutW = outW;
    eye.ctxOutH = outH;
    eye.ctxFlags = flags & ~FSR4F_RESET;
    eye.firstDispatch = true;
    return true;
}

// D3D11 OpenSharedResource1 rejects TYPELESS formats (E_INVALIDARG), and Unity's HDR render
// targets are frequently created typeless. Map the typeless families to a concrete typed format
// that's copy-compatible (same bit layout) with whatever Unity actually views them as.
static DXGI_FORMAT CoerceTypedFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;
    default:                                return f;
    }
}

// (Re)build one shared D3D12<->D3D11 texture mirroring the given Unity texture.
// Created on our D3D12 device (HEAP_FLAG_SHARED), opened into Unity's D3D11 device via
// OpenSharedResource1 so we can CopyResource against Unity's textures. Sync is fence-based; the
// FSR backend restores every external resource to its declared initial state (COMMON) at the end of
// each dispatch (UnregisterResourcesDX12), so the cross-API handoff stays COMMON-consistent.
//
// OpenSharedResource1 is picky about the D3D12 resource flags (observed: E_INVALIDARG for a plain
// R16G16B16A16_FLOAT on RX 9070 XT). We try a few flag combos, most-compatible first, and use the
// first that actually opens — logging each attempt so a total failure is diagnosable.
static bool EnsureSharedTex(SharedTex& st, ID3D11Texture2D* unityTex, bool needUav, const char* label)
{
    D3D11_TEXTURE2D_DESC ud = {};
    unityTex->GetDesc(&ud);

    if (st.tex11 && st.unityPtr == unityTex && st.width == ud.Width && st.height == ud.Height && st.format == ud.Format)
        return true;

    if (ud.SampleDesc.Count > 1)
    {
        Log("[FSR4] %s texture is MSAA — unsupported", label);
        return false;
    }

    st = SharedTex{};

    DXGI_FORMAT sharedFmt = CoerceTypedFormat(ud.Format);
    Log("[FSR4] %s: unity desc %ux%u fmt %d (->%d) arraySize %u mips %u miscFlags 0x%X",
        label, ud.Width, ud.Height, ud.Format, sharedFmt, ud.ArraySize, ud.MipLevels, ud.MiscFlags);

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    const D3D12_RESOURCE_FLAGS uavFlag = needUav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                 : D3D12_RESOURCE_FLAG_NONE;
    struct Combo { D3D12_RESOURCE_FLAGS flags; const char* name; };
    const Combo combos[] = {
        { D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | uavFlag,                                             "RT" },
        { uavFlag,                                                                                       "plain" },
        { D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | uavFlag | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, "RT+SA" },
    };

    for (const Combo& c : combos)
    {
        ComPtr<ID3D12Resource> res12;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = ud.Width;
        rd.Height           = ud.Height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = sharedFmt;
        rd.SampleDesc       = {1, 0};
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = c.flags;

        HRESULT hr = g12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &rd,
                                                  D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res12));
        if (FAILED(hr))
        {
            Log("[FSR4] %s create[%s] failed 0x%08lX", label, c.name, hr);
            continue;
        }

        HANDLE handle = nullptr;
        hr = g12->CreateSharedHandle(res12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(hr))
        {
            Log("[FSR4] %s CreateSharedHandle[%s] failed 0x%08lX", label, c.name, hr);
            continue;
        }

        ComPtr<ID3D11Texture2D> tex11;
        hr = g11_1->OpenSharedResource1(handle, IID_PPV_ARGS(&tex11));
        CloseHandle(handle);
        if (FAILED(hr))
        {
            Log("[FSR4] %s OpenSharedResource1[%s] failed 0x%08lX", label, c.name, hr);
            continue;
        }

        st.res12    = res12;
        st.tex11    = tex11;
        st.unityPtr = unityTex;
        st.width    = ud.Width;
        st.height   = ud.Height;
        st.format   = ud.Format;
        Log("[FSR4] shared %s texture created via '%s': %ux%u fmt %d%s", label, c.name, ud.Width, ud.Height, sharedFmt, needUav ? " +UAV" : "");
        return true;
    }

    Log("[FSR4] ALL sharing combos failed for %s (%dx%d fmt %d) — cross-API texture sharing unsupported here",
        label, ud.Width, ud.Height, sharedFmt);
    return false;
}

// ---------------------------------------------------------------- the per-eye render event
static void DoRenderEvent(int eventId)
{
    int eye  = eventId & 1;
    int slot = (eventId >> 1) & (PARAM_SLOTS - 1);

    std::lock_guard<std::recursive_mutex> renderLock(gRenderMutex);

    Eye& e = gEyes[eye];
    FrameParams p;
    {
        std::lock_guard<std::mutex> lock(e.paramMutex);
        p = e.paramSlots[slot];
    }

    if (!p.colorTex || !p.depthTex || !p.mvecTex || !p.outTex)
        return;

    ComPtr<ID3D11Texture2D> colorTex, depthTex, mvecTex, outTex;
    ((IUnknown*)p.colorTex)->QueryInterface(IID_PPV_ARGS(&colorTex));
    ((IUnknown*)p.depthTex)->QueryInterface(IID_PPV_ARGS(&depthTex));
    ((IUnknown*)p.mvecTex)->QueryInterface(IID_PPV_ARGS(&mvecTex));
    ((IUnknown*)p.outTex)->QueryInterface(IID_PPV_ARGS(&outTex));
    if (!colorTex || !depthTex || !mvecTex || !outTex)
    {
        Log("[FSR4] a frame texture is not an ID3D11Texture2D — skipping");
        return;
    }

    // Optional opaque-only color for reactive-mask generation. Failure here just disables reactive for
    // the frame — never fails the upscale.
    ComPtr<ID3D11Texture2D> opaqueTex;
    bool wantReactive = (p.flags & FSR4F_REACTIVE) && p.opaqueTex != nullptr;
    if (wantReactive)
        ((IUnknown*)p.opaqueTex)->QueryInterface(IID_PPV_ARGS(&opaqueTex));

    if (!EnsureDevices(colorTex.Get()))
        return;
    QueryVersionsOnce();
    if (gStatus == FSR4_NO_VERSIONS)
        return;

    // Before tearing down / recreating any shared texture or the context on a RESIZE, idle the GPU —
    // freeing a resource the GPU is still using is an AMD driver TDR (device removed). This bites
    // specifically on the SteamVR-dashboard resolution drop at NATIVE AA: there render size == output
    // size, so the OUTPUT shrinks too, forcing a context + output-texture rebuild that the normal
    // upscaling path never triggers (there the output stays fixed and only the render shrinks).
    if (e.ctx && ((UINT)p.outW != e.ctxOutW || (UINT)p.outH != e.ctxOutH
                  || (e.color.tex11 && (UINT)p.renderW != e.color.width)))
    {
        WaitQueueIdle();
    }

    if (!EnsureSharedTex(e.color, colorTex.Get(), false, "color") ||
        !EnsureSharedTex(e.depth, depthTex.Get(), false, "depth") ||
        !EnsureSharedTex(e.mvec, mvecTex.Get(), false, "mvec") ||
        !EnsureSharedTex(e.output, outTex.Get(), true, "output"))
    {
        // If the share failed because OUR D3D12 device was removed (dashboard renderer refresh), flag it
        // RECOVERABLE so C# keeps calling us — next frame EnsureDevices rebuilds the device. Otherwise it's
        // a genuine hard failure (fall back to shader FSR3 for the session).
        gStatus = (g12 && FAILED(g12->GetDeviceRemovedReason())) ? FSR4_DEVICE_LOST : FSR4_DEVICE_FAILED;
        return;
    }

    int ctxFlags = p.flags & ~FSR4F_RESET;
    if (!e.ctx || e.ctxOutW != (UINT)p.outW || e.ctxOutH != (UINT)p.outH || e.ctxFlags != ctxFlags)
    {
        if (!CreateEyeContext(e, p.outW, p.outH, p.flags))
            return;
    }

    // Reactive-mask inputs (opaque-only color + the internal mask target). Any failure just disables
    // reactive for this frame — the upscale below still runs.
    if (wantReactive)
    {
        if (!opaqueTex ||
            !EnsureSharedTex(e.opaque, opaqueTex.Get(), false, "opaque") ||
            !EnsureReactiveMask(e, (UINT)p.renderW, (UINT)p.renderH))
            wantReactive = false;
    }

    // --- D3D11: copy this frame's inputs into the shared textures, then hand off to D3D12.
    gCtx11->CopyResource(e.color.tex11.Get(), colorTex.Get());
    gCtx11->CopyResource(e.depth.tex11.Get(), depthTex.Get());
    gCtx11->CopyResource(e.mvec.tex11.Get(), mvecTex.Get());
    if (wantReactive)
        gCtx11->CopyResource(e.opaque.tex11.Get(), opaqueTex.Get());

    UINT64 handoff = ++gFenceValue;
    gCtx11_4->Signal(gFence11.Get(), handoff);
    gCtx11->Flush();
    gQueue->Wait(gFence12.Get(), handoff);

    // --- D3D12: record + dispatch the upscale.
    int  ai = e.allocIdx;
    e.allocIdx = (e.allocIdx + 1) % ALLOC_RING;
    if (e.allocFence[ai] != 0 && gFence12->GetCompletedValue() < e.allocFence[ai])
    {
        gFence12->SetEventOnCompletion(e.allocFence[ai], gFenceEvent);
        WaitForSingleObject(gFenceEvent, 2000);
    }
    e.alloc[ai]->Reset();
    e.cmdList->Reset(e.alloc[ai].Get(), nullptr);

    // Generate the reactive mask FIRST (opaque-only vs full color → where translucency/particles changed),
    // recorded onto the same command list so it runs right before the upscale reads it. FFX barriers the
    // mask back to COMMON at the end of this dispatch, so the upscale below reads it consistently.
    bool reactiveReady = false;
    if (wantReactive)
    {
        ffxDispatchDescUpscaleGenerateReactiveMask gr = {};
        gr.header.type     = FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK;
        gr.commandList     = e.cmdList.Get();
        gr.colorOpaqueOnly = ffxApiGetResourceDX12(e.opaque.res12.Get(),      FFX_API_RESOURCE_STATE_COMMON);
        gr.colorPreUpscale = ffxApiGetResourceDX12(e.color.res12.Get(),       FFX_API_RESOURCE_STATE_COMMON);
        gr.outReactive     = ffxApiGetResourceDX12(e.reactiveMask.Get(),      FFX_API_RESOURCE_STATE_COMMON);
        gr.renderSize      = {(uint32_t)p.renderW, (uint32_t)p.renderH};
        gr.scale           = 1.0f;
        gr.cutoffThreshold = 0.2f;
        gr.binaryValue     = 0.9f;
        gr.flags           = FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_THRESHOLD | FFX_UPSCALE_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;
        ffxReturnCode_t grc = SafeFfxDispatch(&e.ctx, &gr.header);
        reactiveReady = (grc == FFX_API_RETURN_OK);
        if (!reactiveReady)
            Log("[FSR4] generate-reactive dispatch failed (rc=0x%X) — upscaling without it this frame", grc);
    }

    ffxDispatchDescUpscale d = {};
    d.header.type   = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList   = e.cmdList.Get();
    d.color         = ffxApiGetResourceDX12(e.color.res12.Get(), FFX_API_RESOURCE_STATE_COMMON);
    d.depth         = ffxApiGetResourceDX12(e.depth.res12.Get(), FFX_API_RESOURCE_STATE_COMMON);
    d.motionVectors = ffxApiGetResourceDX12(e.mvec.res12.Get(), FFX_API_RESOURCE_STATE_COMMON);
    d.output        = ffxApiGetResourceDX12(e.output.res12.Get(), FFX_API_RESOURCE_STATE_COMMON);
    if (reactiveReady)
        d.reactive  = ffxApiGetResourceDX12(e.reactiveMask.Get(), FFX_API_RESOURCE_STATE_COMMON);
    d.jitterOffset      = {p.jitterX, p.jitterY};
    d.motionVectorScale = {p.mvScaleX, p.mvScaleY};
    d.renderSize        = {(uint32_t)p.renderW, (uint32_t)p.renderH};
    d.upscaleSize       = {(uint32_t)p.outW, (uint32_t)p.outH};
    d.enableSharpening  = p.sharpness > 0.0f;
    d.sharpness         = p.sharpness;
    d.frameTimeDelta    = p.dtMs;
    d.preExposure       = 1.0f;
    d.reset             = e.firstDispatch || (p.flags & FSR4F_RESET) != 0;
    d.cameraNear        = p.camNear;
    d.cameraFar         = p.camFar;
    d.cameraFovAngleVertical = p.fovY;
    d.viewSpaceToMetersFactor = 1.0f;

    ffxReturnCode_t rc = SafeFfxDispatch(&e.ctx, &d.header);
    if (rc != FFX_API_RETURN_OK)
    {
        // close/submit nothing; leave output untouched (C# falls back once it polls status)
        e.cmdList->Close();

        // FSR4 dispatch crashed on this GPU (e.g. RDNA3's INT8 path) — switch to the native FSR 3.1.x
        // provider and rebuild the contexts. Next frame retries with 3.1.x instead of dropping all the
        // way to the game's shader FSR3. (If 3.1.x ALSO crashes, gForceProvider3x is set so we won't
        // loop — it falls through to the hard-fail below.)
        if (rc == FFX_SEH_CRASH && gChosenIsFsr4 && !gForceProvider3x)
        {
            Log("[FSR4] FSR4 dispatch crashed on this GPU — switching to the FSR 3.1.x provider (retry next frame)");
            gForceProvider3x = true;
            for (int i = 0; i < 2; i++)
                DestroyEyeContext(gEyes[i]);
            gStatus = FSR4_OK;   // not a hard failure — let it retry next frame with 3.1.x
            return;
        }

        Log("[FSR4] ffxDispatch failed (rc=0x%X%s)", rc,
            rc == FFX_SEH_CRASH ? " CRASHED — disabling FSR4, game will fall back to FSR3" : "");
        gStatus = FSR4_DISPATCH_FAILED;
        return;
    }
    e.firstDispatch = false;

    e.cmdList->Close();
    ID3D12CommandList* lists[] = {e.cmdList.Get()};
    gQueue->ExecuteCommandLists(1, lists);

    UINT64 done = ++gFenceValue;
    gQueue->Signal(gFence12.Get(), done);
    e.allocFence[ai] = done;

    // --- D3D11: wait for the upscale on the GPU timeline, then copy into Unity's output RT.
    gCtx11_4->Wait(gFence11.Get(), done);
    gCtx11->CopyResource(outTex.Get(), e.output.tex11.Get());

    if (gStatus == FSR4_DISPATCH_FAILED || gStatus == FSR4_NOT_INITIALIZED)
        gStatus = FSR4_OK;
}

static void __stdcall OnRenderEvent(int eventId)
{
    DoRenderEvent(eventId);
}

// ---------------------------------------------------------------- exports
FSR4_EXPORT int Fsr4Init(const wchar_t* ffxDllDir)
{
    std::lock_guard<std::recursive_mutex> lock(gRenderMutex);
    if (!LoadFfxLibrary(ffxDllDir))
        return gStatus;
    if (gStatus == FSR4_LOADER_MISSING || gStatus == FSR4_ENTRYPOINTS_MISSING)
        gStatus = FSR4_NOT_INITIALIZED;
    Log("[FSR4] bridge initialized (ffx-api loaded)");
    return FSR4_OK;
}

// Optional early validation from the main thread: brings up devices, enumerates versions and
// creates+destroys a small context so the FSR4 model load happens at raid load, not mid-frame.
FSR4_EXPORT int Fsr4Preflight(void* anyUnityTexture, int flags)
{
    std::lock_guard<std::recursive_mutex> lock(gRenderMutex);
    if (!pfnCreateContext)
        return gStatus = FSR4_NOT_INITIALIZED;

    ComPtr<ID3D11Texture2D> tex;
    if (!anyUnityTexture || FAILED(((IUnknown*)anyUnityTexture)->QueryInterface(IID_PPV_ARGS(&tex))))
        return gStatus = FSR4_DEVICE_FAILED;

    if (!EnsureDevices(tex.Get()))
        return gStatus.load();
    QueryVersionsOnce();
    if (gStatus == FSR4_NO_VERSIONS)
        return gStatus.load();

    Eye probe;
    if (!CreateEyeContext(probe, 128, 128, flags))
        return gStatus.load();
    DestroyEyeContext(probe);
    gStatus = FSR4_OK;
    Log("[FSR4] preflight OK — active provider: %s", gActiveVersionName);
    return FSR4_OK;
}

FSR4_EXPORT void Fsr4SetEyeFrameParams(
    int eye, int slot,
    void* colorTex, void* depthTex, void* mvecTex, void* outTex, void* opaqueTex,
    int renderW, int renderH, int outW, int outH,
    float jitterX, float jitterY, float mvScaleX, float mvScaleY,
    float camNear, float camFar, float fovY,
    float dtMs, float sharpness, int flags)
{
    if (eye < 0 || eye > 1)
        return;
    Eye& e = gEyes[eye];
    FrameParams p;
    p.colorTex = colorTex; p.depthTex = depthTex; p.mvecTex = mvecTex; p.outTex = outTex; p.opaqueTex = opaqueTex;
    p.renderW = renderW; p.renderH = renderH; p.outW = outW; p.outH = outH;
    p.jitterX = jitterX; p.jitterY = jitterY; p.mvScaleX = mvScaleX; p.mvScaleY = mvScaleY;
    p.camNear = camNear; p.camFar = camFar; p.fovY = fovY;
    p.dtMs = dtMs; p.sharpness = sharpness; p.flags = flags;
    {
        std::lock_guard<std::mutex> lock(e.paramMutex);
        e.paramSlots[slot & (PARAM_SLOTS - 1)] = p;
    }
}

FSR4_EXPORT void* Fsr4GetRenderEventFunc()
{
    return (void*)&OnRenderEvent;
}

FSR4_EXPORT int Fsr4GetStatus()
{
    return gStatus.load();
}

FSR4_EXPORT int Fsr4IsFsr4Active()
{
    return gChosenIsFsr4 ? 1 : 0;
}

FSR4_EXPORT int Fsr4GetActiveVersion(char* buf, int cap)
{
    if (!buf || cap <= 0)
        return 0;
    strncpy_s(buf, cap, gActiveVersionName, _TRUNCATE);
    return (int)strlen(buf);
}

FSR4_EXPORT int Fsr4PopLogLine(char* buf, int cap)
{
    std::lock_guard<std::mutex> lock(gLogMutex);
    if (gLogLines.empty() || !buf || cap <= 0)
        return 0;
    const std::string& line = gLogLines.front();
    int n = (int)line.size();
    if (n > cap - 1)
        n = cap - 1;
    memcpy(buf, line.c_str(), n);
    buf[n] = 0;
    gLogLines.pop_front();
    return n;
}

// Drops the FFX contexts + shared textures (devices stay). Call when the game tears the SSAA
// pipeline down or the user turns the feature off; everything lazily rebuilds on next use.
FSR4_EXPORT void Fsr4InvalidateContexts()
{
    std::lock_guard<std::recursive_mutex> lock(gRenderMutex);
    WaitQueueIdle();
    for (int e = 0; e < 2; e++)
        ReleaseEyeResources(gEyes[e]);
    if (gStatus == FSR4_OK)
        Log("[FSR4] contexts invalidated");
}

FSR4_EXPORT void Fsr4Shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(gRenderMutex);
    WaitQueueIdle();
    for (int e = 0; e < 2; e++)
    {
        ReleaseEyeResources(gEyes[e]);
        for (int i = 0; i < ALLOC_RING; i++)
            gEyes[e].alloc[i].Reset();
        gEyes[e].cmdList.Reset();
    }
    gFence11.Reset();
    gFence12.Reset();
    gQueue.Reset();
    g12.Reset();
    gCtx11_4.Reset();
    gCtx11.Reset();
    g11_5.Reset();
    g11_1.Reset();
    g11.Reset();
    if (gFenceEvent)
    {
        CloseHandle(gFenceEvent);
        gFenceEvent = nullptr;
    }
    gVersionsQueried = false;
    gStatus = FSR4_NOT_INITIALIZED;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
