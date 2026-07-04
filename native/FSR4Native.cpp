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
// amd_fidelityfx_loader_dx12/amd_fidelityfx_upscaler_dx12 (FSR4 shows up only on RDNA4 with a
// driver that ships amdxcffx64). We pick the highest major-4 version when preferred, else the
// default provider (latest 3.1.x) — so the same code path degrades gracefully on any GPU.
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
static PfnFfxCreateContext  pfnCreateContext  = nullptr;
static PfnFfxDestroyContext pfnDestroyContext = nullptr;
static PfnFfxConfigure      pfnConfigure      = nullptr;
static PfnFfxQuery          pfnQuery          = nullptr;
static PfnFfxDispatch       pfnDispatch       = nullptr;

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
static uint64_t              gChosenVersionId  = 0;   // 0 = no override (provider default)
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

    SharedTex color, depth, mvec, output;

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
        pfnDestroyContext(&eye.ctx, nullptr);
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

    wchar_t path[MAX_PATH];

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

// Bring up the D3D11 views + the private D3D12 device/queue/fence from any Unity texture.
static bool EnsureDevices(ID3D11Texture2D* anyUnityTex)
{
    if (g12)
        return true;

    anyUnityTex->GetDevice(&g11);
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

    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g12));
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

static void QueryVersionsOnce(bool preferFsr4)
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

    ffxReturnCode_t rc = pfnQuery(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK || count == 0)
    {
        Log("[FSR4] version query failed (rc=%u, count=%llu)", rc, (unsigned long long)count);
        gStatus = FSR4_NO_VERSIONS;
        return;
    }

    std::vector<uint64_t>    ids(count);
    std::vector<const char*> names(count);
    q.versionIds   = ids.data();
    q.versionNames = names.data();
    rc = pfnQuery(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK)
    {
        Log("[FSR4] version id/name query failed (rc=%u)", rc);
        gStatus = FSR4_NO_VERSIONS;
        return;
    }

    gVersionIds.clear();
    gVersionNames.clear();
    int bestFsr4 = -1;
    for (uint64_t i = 0; i < count; i++)
    {
        const char* name = names[i] ? names[i] : "?";
        gVersionIds.push_back(ids[i]);
        gVersionNames.emplace_back(name);
        Log("[FSR4] available upscaler version: %s (id 0x%016llX)", name, (unsigned long long)ids[i]);

        // FSR4 providers report names beginning with "4." (e.g. "4.0.2"); pick the first/highest.
        const char* p = name;
        while (*p && (*p < '0' || *p > '9')) p++;   // skip any "FSR " style prefix
        if (*p == '4' && p[1] == '.')
        {
            if (bestFsr4 < 0 || strcmp(name, gVersionNames[bestFsr4].c_str()) > 0)
                bestFsr4 = (int)i;
        }
    }

    if (preferFsr4 && bestFsr4 >= 0)
    {
        gChosenVersionId = gVersionIds[bestFsr4];
        gChosenIsFsr4    = true;
        Log("[FSR4] selected FSR4 provider: %s", gVersionNames[bestFsr4].c_str());
    }
    else
    {
        gChosenVersionId = 0;   // provider default (latest 3.1.x)
        gChosenIsFsr4    = false;
        if (preferFsr4)
            Log("[FSR4] no FSR4 provider available (needs RDNA4 GPU + recent AMD driver) — using default provider");
        else
            Log("[FSR4] FSR4 not preferred — using default provider");
    }
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

    ffxOverrideVersion over = {};
    if (gChosenVersionId != 0)
    {
        over.header.type       = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        over.versionId         = gChosenVersionId;
        apiVersion.header.pNext = &over.header;
    }

    ffxReturnCode_t rc = pfnCreateContext(&eye.ctx, &desc.header, nullptr);
    if (rc != FFX_API_RETURN_OK && gChosenVersionId != 0)
    {
        // FSR4 provider refused (e.g. dynamic-res unsupported) — retry with the default provider.
        Log("[FSR4] context creation with version override failed (rc=%u) — retrying default provider", rc);
        apiVersion.header.pNext = nullptr;
        gChosenVersionId = 0;
        gChosenIsFsr4    = false;
        rc = pfnCreateContext(&eye.ctx, &desc.header, nullptr);
    }
    if (rc != FFX_API_RETURN_OK)
    {
        Log("[FSR4] ffxCreateContext failed (rc=%u, out %ux%u, flags 0x%X)", rc, outW, outH, ffxFlags);
        eye.ctx = nullptr;
        gStatus = FSR4_CONTEXT_FAILED;
        return false;
    }

    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (pfnQuery(&eye.ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
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

    if (!EnsureDevices(colorTex.Get()))
        return;
    QueryVersionsOnce((p.flags & FSR4F_PREFER_FSR4) != 0);
    if (gStatus == FSR4_NO_VERSIONS)
        return;

    if (!EnsureSharedTex(e.color, colorTex.Get(), false, "color") ||
        !EnsureSharedTex(e.depth, depthTex.Get(), false, "depth") ||
        !EnsureSharedTex(e.mvec, mvecTex.Get(), false, "mvec") ||
        !EnsureSharedTex(e.output, outTex.Get(), true, "output"))
    {
        gStatus = FSR4_DEVICE_FAILED;
        return;
    }

    int ctxFlags = p.flags & ~FSR4F_RESET;
    if (!e.ctx || e.ctxOutW != (UINT)p.outW || e.ctxOutH != (UINT)p.outH || e.ctxFlags != ctxFlags)
    {
        if (!CreateEyeContext(e, p.outW, p.outH, p.flags))
            return;
    }

    // --- D3D11: copy this frame's inputs into the shared textures, then hand off to D3D12.
    gCtx11->CopyResource(e.color.tex11.Get(), colorTex.Get());
    gCtx11->CopyResource(e.depth.tex11.Get(), depthTex.Get());
    gCtx11->CopyResource(e.mvec.tex11.Get(), mvecTex.Get());

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

    ffxDispatchDescUpscale d = {};
    d.header.type   = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList   = e.cmdList.Get();
    d.color         = ffxApiGetResourceDX12(e.color.res12.Get(), FFX_API_RESOURCE_STATE_COMMON);
    d.depth         = ffxApiGetResourceDX12(e.depth.res12.Get(), FFX_API_RESOURCE_STATE_COMMON);
    d.motionVectors = ffxApiGetResourceDX12(e.mvec.res12.Get(), FFX_API_RESOURCE_STATE_COMMON);
    d.output        = ffxApiGetResourceDX12(e.output.res12.Get(), FFX_API_RESOURCE_STATE_COMMON);
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

    ffxReturnCode_t rc = pfnDispatch(&e.ctx, &d.header);
    if (rc != FFX_API_RETURN_OK)
    {
        // close/submit nothing; leave output untouched (C# falls back once it polls status)
        e.cmdList->Close();
        Log("[FSR4] ffxDispatch failed (rc=%u)", rc);
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
    QueryVersionsOnce((flags & FSR4F_PREFER_FSR4) != 0);
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
    void* colorTex, void* depthTex, void* mvecTex, void* outTex,
    int renderW, int renderH, int outW, int outH,
    float jitterX, float jitterY, float mvScaleX, float mvScaleY,
    float camNear, float camFar, float fovY,
    float dtMs, float sharpness, int flags)
{
    if (eye < 0 || eye > 1)
        return;
    Eye& e = gEyes[eye];
    FrameParams p;
    p.colorTex = colorTex; p.depthTex = depthTex; p.mvecTex = mvecTex; p.outTex = outTex;
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
