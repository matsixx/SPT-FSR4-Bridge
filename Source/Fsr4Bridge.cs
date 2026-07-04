using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.PostProcessing;

namespace FSR4Bridge.Source
{
    // Managed side of the native FSR4 bridge (FSR4Native.dll) for FLATSCREEN SPT.
    //
    // The native DLL runs FSR4 (or FSR 3.1.x) on a private D3D12 device and copies across a shared
    // fence — see native/FSR4Native.cpp. This side, per frame:
    //   1. loads the native DLL + AMD runtime from plugins/FSR4Bridge/native,
    //   2. materializes the render-res color + depth + motion into owned RTs (the only way to hand
    //      Unity's textures to the D3D12 side as native pointers),
    //   3. feeds the pointers + camera params to the bridge,
    //   4. issues the render-thread plugin event that does the upscale and writes the output.
    //
    // Enablement rides the game's existing FSR3 path (Fsr4RenderPatch hooks SSAAImpl.TryRenderFSR3):
    // pick an FSR quality in Graphics settings, toggle "Enable FSR4" in the config. The FFX context
    // uses dynamic resolution and is kept alive across renderer refreshes so quality changes don't
    // rebuild it.
    internal static class Fsr4Bridge
    {
        private const string DLL = "FSR4Native";

        // ---- context flag bits (mirror Fsr4Flags in the native side) --------------------------
        private const int FSR4F_HDR            = 1 << 0;
        private const int FSR4F_DEPTH_INVERTED = 1 << 1;
        private const int FSR4F_DEPTH_INFINITE = 1 << 2;
        private const int FSR4F_AUTO_EXPOSURE  = 1 << 3;
        private const int FSR4F_DEBUG_CHECKING = 1 << 4;
        private const int FSR4F_DYNAMIC_RES    = 1 << 5;
        private const int FSR4F_PREFER_FSR4    = 1 << 6;
        private const int FSR4F_RESET          = 1 << 7;

        private static bool _triedInit;
        private static bool _initOk;
        private static bool _permaFail;
        private static IntPtr _renderEventFunc = IntPtr.Zero;

        // Set by Plugin when the SPT-VR mod is loaded → we drive VR (stereo, per-eye) FSR4 as well as
        // flatscreen. Gates the VR-typed jitter helper so it's only JITted when SPT-VR is present.
        public static bool SptVrPresent;

        // Per-eye (index 0 = mono/left, 1 = right). Flatscreen only ever touches eye 0.
        private static readonly RenderTexture[] _colorRT  = new RenderTexture[2];
        private static readonly RenderTexture[] _depthRT  = new RenderTexture[2];
        private static readonly RenderTexture[] _motionRT = new RenderTexture[2];
        private static CommandBuffer _cmd;
        private static bool _loggedActiveProvider;
        private static int _debugLogsLeft = 6;

        private static readonly int _camDepthTexId  = Shader.PropertyToID("_CameraDepthTexture");
        private static readonly int _camMotionTexId = Shader.PropertyToID("_CameraMotionVectorsTexture");

        // -------------------------------------------------------------------------------------------
        // P/Invoke surface (matches native FSR4Bridge.cpp exports)
        // -------------------------------------------------------------------------------------------
        [DllImport(DLL, CharSet = CharSet.Unicode, CallingConvention = CallingConvention.Cdecl)]
        private static extern int Fsr4Init(string ffxDllDir);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        private static extern void Fsr4SetEyeFrameParams(
            int eye, int slot,
            IntPtr colorTex, IntPtr depthTex, IntPtr mvecTex, IntPtr outTex,
            int renderW, int renderH, int outW, int outH,
            float jitterX, float jitterY, float mvScaleX, float mvScaleY,
            float camNear, float camFar, float fovY,
            float dtMs, float sharpness, int flags);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr Fsr4GetRenderEventFunc();

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        private static extern int Fsr4GetStatus();

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        private static extern int Fsr4IsFsr4Active();

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        private static extern int Fsr4GetActiveVersion(StringBuilder buf, int cap);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        private static extern int Fsr4PopLogLine(StringBuilder buf, int cap);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        private static extern void Fsr4InvalidateContexts();

        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibrary(string path);

        // -------------------------------------------------------------------------------------------
        private static bool TryInit()
        {
            if (_triedInit)
                return _initOk;
            _triedInit = true;

            try
            {
                string modDir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                string nativeDir = Path.Combine(modDir, "native");
                string nativePath = Path.Combine(nativeDir, DLL + ".dll");

                if (!File.Exists(nativePath))
                {
                    Plugin.MyLog.LogWarning($"[FSR4] {DLL}.dll not found at {nativePath} — FSR4 unavailable.");
                    return false;
                }

                if (LoadLibrary(nativePath) == IntPtr.Zero)
                {
                    Plugin.MyLog.LogError($"[FSR4] LoadLibrary failed for {nativePath} (err {Marshal.GetLastWin32Error()})");
                    return false;
                }

                int rc = Fsr4Init(nativeDir);
                DrainLog();
                if (rc != 0)
                {
                    Plugin.MyLog.LogWarning($"[FSR4] native init returned {rc} — FSR4 unavailable.");
                    return false;
                }

                _renderEventFunc = Fsr4GetRenderEventFunc();
                _initOk = _renderEventFunc != IntPtr.Zero;
                if (_initOk)
                    Plugin.MyLog.LogInfo("[FSR4] native bridge loaded.");
                return _initOk;
            }
            catch (Exception ex)
            {
                Plugin.MyLog.LogError($"[FSR4] init exception: {ex}");
                return false;
            }
        }

        public static void InvalidateContexts()
        {
            _permaFail = false;
            if (_initOk)
            {
                try { Fsr4InvalidateContexts(); } catch { }
                DrainLog();
            }
        }

        // Called from Fsr4RenderPatch in place of the game's FSR3 wrapper. Returns true if it drove
        // the upscale (caller then skips the original), false to fall through to vanilla FSR3.
        public static bool RenderEye(RenderTexture source, RenderTexture destination, Camera cam)
        {
            if (source == null || destination == null || cam == null)
                return false;
            if (_permaFail || !TryInit())
                return false;

            // Status reflects the previous frame's async result — any hard error → fall back to the
            // game's shader FSR3 for the session so we never leave a broken frame up.
            int st = GetStatusSafe();
            if (st >= 2)
            {
                _permaFail = true;
                Plugin.MyLog.LogWarning($"[FSR4] native status {st} — falling back to in-engine FSR3 for this session.");
                DrainLog();
                return false;
            }

            // VR mode whenever SPT-VR is loaded (it's a VR-only mod, and TryRenderFSR3 only fires for
            // the upscaled main cam) — per-eye contexts + the VR mod's own jitter (it owns the
            // projection jitter, so the game's jitterPixelSpace is wrong there). This matches the VR
            // mod's original FSR4 path exactly. Flatscreen: eye 0, game jitter.
            bool vr = SptVrPresent;
            int eye = ((int)cam.stereoActiveEye) & 1;

            // FSR needs render-res depth + motion, which Unity exposes as global textures once the
            // camera has the matching DepthTextureMode. Fetch the actual Texture objects (name-ref'd
            // blit sources don't resolve in a standalone CB) and Blit into owned RTs.
            cam.depthTextureMode |= DepthTextureMode.Depth | DepthTextureMode.MotionVectors;
            Texture camDepth  = Shader.GetGlobalTexture(_camDepthTexId);
            Texture camMotion = Shader.GetGlobalTexture(_camMotionTexId);
            if (camDepth == null || camMotion == null)
                return false;   // not ready yet (mode just enabled) — vanilla FSR3 covers this frame

            int renderW = source.width;
            int renderH = source.height;
            int outW = destination.width;
            int outH = destination.height;

            // `source` is a pooled temp RT (ptr changes each frame) — copy into an owned RT so the
            // native shared-texture cache stays valid instead of rebuilding every frame.
            EnsureCopyRT(ref _colorRT[eye],  renderW, renderH, source.format,             "FSR4Color" + eye);
            EnsureCopyRT(ref _depthRT[eye],  renderW, renderH, RenderTextureFormat.RFloat, "FSR4Depth" + eye);
            EnsureCopyRT(ref _motionRT[eye], renderW, renderH, RenderTextureFormat.RGHalf, "FSR4Motion" + eye);
            UnityEngine.Graphics.Blit(source,    _colorRT[eye]);
            UnityEngine.Graphics.Blit(camDepth,  _depthRT[eye]);
            UnityEngine.Graphics.Blit(camMotion, _motionRT[eye]);

            IntPtr colorPtr  = _colorRT[eye].GetNativeTexturePtr();
            IntPtr outPtr    = destination.GetNativeTexturePtr();
            IntPtr depthPtr  = _depthRT[eye].GetNativeTexturePtr();
            IntPtr motionPtr = _motionRT[eye].GetNativeTexturePtr();
            if (colorPtr == IntPtr.Zero || outPtr == IntPtr.Zero || depthPtr == IntPtr.Zero || motionPtr == IntPtr.Zero)
                return false;

            // Vertical FOV from the projection matrix (m11 = 1/tan(fovY/2)); near/far from the camera.
            Matrix4x4 proj = cam.projectionMatrix;
            float m11 = Mathf.Abs(proj.m11) < 1e-6f ? 1f : proj.m11;
            float fovY = 2f * Mathf.Atan(1f / m11);
            float camNear = cam.nearClipPlane;
            float camFar  = cam.farClipPlane;

            // Jitter. VR: the VR mod's own jitter, negated, no flip (confirmed convention). Flatscreen:
            // the game's jitterPixelSpace with the tunable sign/scale (flips default ON — see config).
            Vector2 jitter;
            float jitterX, jitterY;
            if (vr)
            {
                jitter  = GetVrJitter();
                jitterX = -jitter.x;
                jitterY = -jitter.y;
            }
            else
            {
                jitter  = TemporalAntialiasing.jitterPixelSpace;
                float jScale = Fsr4Config.JitterScale.Value;
                jitterX = (Fsr4Config.JitterFlipX.Value ? 1f : -1f) * jitter.x * jScale;
                jitterY = (Fsr4Config.JitterFlipY.Value ? 1f : -1f) * jitter.y * jScale;
            }

            // MV scale: final scale must be {sign, sign}; the native ffx-api divides by the MV target
            // size, so pre-multiply by renderSize. Game FSR3 uses {-1,-1} (InvertMotionVectors on).
            float mvSign = Fsr4Config.InvertMotionVectors.Value ? -1f : 1f;
            float mvScaleX = mvSign * renderW;
            float mvScaleY = mvSign * renderH;

            float dtMs = Time.deltaTime * 1000f;

            int flags = FSR4F_DYNAMIC_RES;
            if (IsHdrFormat(source.format))       flags |= FSR4F_HDR;
            if (Fsr4Config.DepthInverted.Value)   flags |= FSR4F_DEPTH_INVERTED;
            if (Fsr4Config.AutoExposure.Value)    flags |= FSR4F_AUTO_EXPOSURE;
            if (Fsr4Config.PreferFsr4.Value)      flags |= FSR4F_PREFER_FSR4;

            int slot = Time.frameCount & 3;

            Fsr4SetEyeFrameParams(
                eye, slot,
                colorPtr, depthPtr, motionPtr, outPtr,
                renderW, renderH, outW, outH,
                jitterX, jitterY, mvScaleX, mvScaleY,
                camNear, camFar, fovY,
                dtMs, Mathf.Clamp01(Fsr4Config.Sharpness.Value), flags);

            if (_cmd == null)
                _cmd = new CommandBuffer { name = "FSR4Bridge" };
            _cmd.Clear();
            _cmd.IssuePluginEvent(_renderEventFunc, (slot << 1) | eye);
            UnityEngine.Graphics.ExecuteCommandBuffer(_cmd);

            // Diagnostic: is the jitter sane (non-zero, changing each frame)? A stuck/zero jitter =
            // a soft, spatial-only upscale. Logs a handful of frames spaced out, then stops.
            if (Fsr4Config.DebugLog.Value && _debugLogsLeft > 0 && (Time.frameCount % 90 == 0))
            {
                _debugLogsLeft--;
                Plugin.MyLog.LogInfo(
                    $"[FSR4] params: {(vr ? "VR eye" + eye : "flat")} render {renderW}x{renderH} -> {outW}x{outH} " +
                    $"jitterPx=({jitter.x:F3},{jitter.y:F3}) fed=({jitterX:F3},{jitterY:F3}) " +
                    $"mvScale=({mvScaleX:F0},{mvScaleY:F0}) dt={dtMs:F2}ms hdr={IsHdrFormat(source.format)} sharp={Fsr4Config.Sharpness.Value:F2}");
            }

            DrainLog();
            if (!_loggedActiveProvider)
            {
                var sb = new StringBuilder(128);
                if (Fsr4GetActiveVersion(sb, sb.Capacity) > 0 && sb.Length > 0 && sb[0] != 'n')
                {
                    Plugin.MyLog.LogInfo($"[FSR4] active provider: {sb} (FSR4={(Fsr4IsFsr4Active() == 1)})");
                    _loggedActiveProvider = true;
                }
            }
            return true;
        }

        // Typed access to SPT-VR's jitter. NoInlining + the SptVrPresent gate mean this method is only
        // ever JITted when SPT-VR is loaded, so the flatscreen-only case never tries to resolve the VR
        // type (would throw at JIT). Mirrors the VR mod's own soft-dep pattern (see OrbitSupport).
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static Vector2 GetVrJitter()
        {
            return TarkovVR.Patches.Upscalers.VRJitterComponent.CurrentJitter;
        }

        // -------------------------------------------------------------------------------------------
        private static void EnsureCopyRT(ref RenderTexture rt, int w, int h, RenderTextureFormat fmt, string name)
        {
            if (rt != null && rt.width == w && rt.height == h && rt.format == fmt && rt.IsCreated())
                return;
            if (rt != null)
            {
                if (rt.IsCreated()) rt.Release();
                UnityEngine.Object.Destroy(rt);
            }
            rt = new RenderTexture(w, h, 0, fmt, RenderTextureReadWrite.Linear)
            {
                name = name,
                filterMode = FilterMode.Point,
                useMipMap = false,
                autoGenerateMips = false,
                anisoLevel = 0,
            };
            rt.Create();
        }

        private static bool IsHdrFormat(RenderTextureFormat fmt)
        {
            return fmt == RenderTextureFormat.ARGBHalf
                || fmt == RenderTextureFormat.ARGBFloat
                || fmt == RenderTextureFormat.RGB111110Float
                || fmt == RenderTextureFormat.DefaultHDR;
        }

        private static int GetStatusSafe()
        {
            try { return Fsr4GetStatus(); } catch { return 1; }
        }

        private static void DrainLog()
        {
            try
            {
                var sb = new StringBuilder(1024);
                int guard = 0;
                while (Fsr4PopLogLine(sb, sb.Capacity) > 0 && guard++ < 64)
                    Plugin.MyLog.LogInfo(sb.ToString());
            }
            catch { }
        }
    }
}
