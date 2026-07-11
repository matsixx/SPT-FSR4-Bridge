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
        private const int FSR4F_MV_JITTER_CANCEL = 1 << 8;
        private const int FSR4F_REACTIVE       = 1 << 9;
        private const int FSR4F_MODEL_FIX      = 1 << 10;

        private static bool _triedInit;
        private static bool _initOk;
        private static bool _permaFail;
        private const int FSR4_DEVICE_LOST = 9;   // recoverable status (native rebuilds its device + retries)
        private static int _deviceLostRetries;
        private static IntPtr _renderEventFunc = IntPtr.Zero;

        public static bool SptVrPresent;

        // Per-eye (index 0 = mono/left, 1 = right). Flatscreen only ever touches eye 0.
        private static readonly RenderTexture[] _colorRT  = new RenderTexture[2];
        private static readonly RenderTexture[] _depthRT  = new RenderTexture[2];
        private static readonly RenderTexture[] _motionRT = new RenderTexture[2];

        // Cached native texture pointers. GetNativeTexturePtr SYNCHRONIZES the main thread with the
        // render thread under multithreaded rendering (Unity docs) — the stall lasts as long as the
        // render thread is behind
        private static readonly IntPtr[] _colorPtr  = new IntPtr[2];
        private static readonly IntPtr[] _depthPtr  = new IntPtr[2];
        private static readonly IntPtr[] _motionPtr = new IntPtr[2];
        private static IntPtr _opaquePtr;
        private static readonly RenderTexture[] _destRT  = new RenderTexture[2];
        private static readonly IntPtr[]        _destPtr = new IntPtr[2];

        // One immutable CommandBuffer per (slot,eye) event id — no Clear + re-record per frame.
        private static readonly CommandBuffer[] _eventCmds = new CommandBuffer[8];
        private static int _lastRenderEyeFrame = -1;

        // Main-thread cost profiler (Debug Log only). RenderEye runs on the MAIN thread; its
        // historical dominant cost (GetNativeTexturePtr render-thread syncs) was invisible to the
        // native render-thread phase profiler, so this bucket is the one that shows it.
        private static readonly System.Diagnostics.Stopwatch _mainSw = new System.Diagnostics.Stopwatch();
        private static double _mainAccumMs;
        private static int _mainSamples;

        // Reactive-mask capture: a CommandBuffer at BeforeForwardAlpha blits the opaque-only color into an
        // owned RT each frame (same technique the game's FSR3 wrapper uses), which we hand to the native
        // reactive-mask generator. Flatscreen only for now (single owned RT; VR's per-eye capture is TBD).
        private static RenderTexture _opaqueRT;
        private static CommandBuffer _opaqueCmd;
        private static Camera _opaqueCam;
        private static bool _loggedActiveProvider;
        private static int _debugLogsLeft = 8;
        private static bool _prevDebugLog;
        private static string _activeProviderStr = "FSR";
        private static int _lastRW, _lastRH, _lastOW, _lastOH;
        private static float _lastResLogTime = -999f;

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
            IntPtr colorTex, IntPtr depthTex, IntPtr mvecTex, IntPtr outTex, IntPtr opaqueTex,
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

        // byte[] instead of StringBuilder: StringBuilder marshaling allocates a native buffer and
        // copies both ways on EVERY call; a byte[] pins in place (zero allocation). Same char* ABI.
        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        private static extern int Fsr4GetActiveVersion(byte[] buf, int cap);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        private static extern int Fsr4PopLogLine(byte[] buf, int cap);

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        private static extern void Fsr4InvalidateContexts();

        [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
        private static extern void Fsr4SetVerbose(int on);

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

                Fsr4SetVerbose(Fsr4Config.DebugLog.Value ? 1 : 0);   // route native spam through Debug Log (off by default)
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
            DetachOpaqueCapture();                 // don't leave the capture blit running while off
            _destRT[0] = _destRT[1] = null;        // refetch destination ptr on next use
            if (_initOk)
            {
                try { Fsr4InvalidateContexts(); } catch { }
                DrainLog();
            }
        }

        public static void IdleTick()
        {
            if (_opaqueCam != null && Time.frameCount - _lastRenderEyeFrame > 120)
                DetachOpaqueCapture();
        }

        // Called from Fsr4RenderPatch in place of the game's FSR3 wrapper
        public static bool RenderEye(RenderTexture source, RenderTexture destination, Camera cam)
        {
            if (source == null || destination == null || cam == null)
                return false;
            if (_permaFail || !TryInit())
                return false;

            // Status reflects the previous frame's async result — any hard error falls back to the
            // game's shader FSR3 for the session so we never leave a broken frame up.
            int st = GetStatusSafe();
            if (st == FSR4_DEVICE_LOST)
            {
                // Recoverable: our D3D12 device was removed by a renderer refresh (SteamVR dashboard lowers
                // the game's res in the background). KEEP calling so the native rebuilds it on the next
                // dispatch — usually one glitch frame, then it recovers. Give up only if it never comes back.
                if (++_deviceLostRetries > 30)
                {
                    _permaFail = true;
                    DetachOpaqueCapture();   // stop paying the capture blit once we stop rendering
                    Plugin.MyLog.LogWarning("[FSR4] D3D12 device kept getting lost — falling back to in-engine FSR3 for this session.");
                    DrainLog();
                    return false;
                }
            }
            else
            {
                _deviceLostRetries = 0;
                if (st >= 2)
                {
                    _permaFail = true;
                    DetachOpaqueCapture();   // stop paying the capture blit once we stop rendering
                    Plugin.MyLog.LogWarning($"[FSR4] native status {st} — falling back to in-engine FSR3 for this session.");
                    DrainLog();
                    return false;
                }
            }

            bool vr = SptVrPresent;
            int eye = ((int)cam.stereoActiveEye) & 1;
            _lastRenderEyeFrame = Time.frameCount;

            // Non-OK status (init warmup, device-lost recovery): Unity may have recreated resources
            // behind our cached native pointers — refetch them this frame. 0 on every normal frame.
            bool ptrRefetch = st != 0;

            bool dbg = Fsr4Config.DebugLog.Value;
            if (dbg) _mainSw.Restart();

            // FSR needs render-res depth + motion, which Unity exposes as global textures once the
            // camera has the matching DepthTextureMode. Fetch the actual Texture objects (name-ref'd
            // blit sources don't resolve in a standalone CB) and Blit into owned RTs.
            cam.depthTextureMode |= DepthTextureMode.Depth | DepthTextureMode.MotionVectors;
            Texture camDepth  = Shader.GetGlobalTexture(_camDepthTexId);
            Texture camMotion = Shader.GetGlobalTexture(_camMotionTexId);

            if (camDepth == null || camMotion == null)
                return false; 

            int renderW = source.width;
            int renderH = source.height;
            int outW = destination.width;
            int outH = destination.height;

            // `source` is a pooled temp RT (ptr changes each frame) — copy into an owned RT so the
            // native shared-texture cache stays valid instead of rebuilding every frame.
            EnsureCopyRT(ref _colorRT[eye],  ref _colorPtr[eye],  renderW, renderH, source.format,             "FSR4Color",  eye);
            EnsureCopyRT(ref _depthRT[eye],  ref _depthPtr[eye],  renderW, renderH, RenderTextureFormat.RFloat, "FSR4Depth",  eye);
            EnsureCopyRT(ref _motionRT[eye], ref _motionPtr[eye], renderW, renderH, RenderTextureFormat.RGHalf, "FSR4Motion", eye);
            if (ptrRefetch)
            {
                _colorPtr[eye]  = _colorRT[eye].GetNativeTexturePtr();
                _depthPtr[eye]  = _depthRT[eye].GetNativeTexturePtr();
                _motionPtr[eye] = _motionRT[eye].GetNativeTexturePtr();
            }

            // Same-size same-format color goes through the copy engine (no fullscreen draw);
            // depth/motion stay as Blits (they convert format to RFloat/RGHalf).
            if (source.antiAliasing <= 1 && source.graphicsFormat == _colorRT[eye].graphicsFormat)
                UnityEngine.Graphics.CopyTexture(source, _colorRT[eye]);
            else
                UnityEngine.Graphics.Blit(source, _colorRT[eye]);
            UnityEngine.Graphics.Blit(camDepth,  _depthRT[eye]);
            UnityEngine.Graphics.Blit(camMotion, _motionRT[eye]);

            // destination is the game's persistent output RT — refetch only when its identity changes
            // (a same-object in-place Release+Create would go stale, but no game path does that; any
            // resulting native failure flips status non-OK, which forces a refetch next frame).
            if (!ReferenceEquals(destination, _destRT[eye]) || ptrRefetch)
            {
                _destRT[eye]  = destination;
                _destPtr[eye] = destination.GetNativeTexturePtr();
            }
            IntPtr colorPtr = _colorPtr[eye], outPtr = _destPtr[eye], depthPtr = _depthPtr[eye], motionPtr = _motionPtr[eye];
            if (colorPtr == IntPtr.Zero || outPtr == IntPtr.Zero || depthPtr == IntPtr.Zero || motionPtr == IntPtr.Zero)
                return false;

            // Vertical FOV from the projection matrix (m11 = 1/tan(fovY/2)); near/far from the camera.
            Matrix4x4 proj = cam.projectionMatrix;
            float m11 = Mathf.Abs(proj.m11) < 1e-6f ? 1f : proj.m11;
            float fovY = 2f * Mathf.Atan(1f / m11);
            float camNear = cam.nearClipPlane;
            float camFar  = cam.farClipPlane;

            // Jitter is handled different in VR vs flatscreen
            float jScale = Fsr4Config.JitterScale.Value;
            Vector2 gameJitter = TemporalAntialiasing.jitterPixelSpace;  
            Vector2 vrJitter = Vector2.zero;
            Vector2 jitter;
            float jitterX, jitterY;
            if (vr)
            {
                vrJitter = GetVrJitter();
                if (Fsr4Config.VrUseGameJitter.Value)
                {
                    jitter = gameJitter;
                    jitterX = jitter.x * jScale;
                    jitterY = jitter.y * jScale;
                }
                else
                {
                    // Signs config-driven (default +x, -y = FSR's asymmetric convention). Flip toggles A/B
                    // the residual edge jitter against the render's actual +x,+y projection jitter.
                    jitter  = vrJitter;
                    jitterX = jitter.x * jScale * (Fsr4Config.VrFlipJitterX.Value ? -1f :  1f);
                    jitterY = jitter.y * jScale * (Fsr4Config.VrFlipJitterY.Value ?  1f : -1f);
                }
            }
            else
            {
                jitter  = gameJitter;
                jitterX = jitter.x * jScale;
                jitterY = jitter.y * jScale;
            }

            // MV scale: final scale must be {-1,-1} (the game's FSR3 convention); the native ffx-api
            // divides by the MV target size, so pre-multiply by renderSize.
            float mvScaleX = -renderW;
            float mvScaleY = -renderH;

            float dtMs = Time.deltaTime * 1000f;

            // Depth is reversed-Z + auto-exposure on
            int flags = FSR4F_DYNAMIC_RES | FSR4F_DEPTH_INVERTED | FSR4F_AUTO_EXPOSURE;
            if (Fsr4Config.DepthInfinite.Value)              flags |= FSR4F_DEPTH_INFINITE;
            if (IsHdrFormat(source.format))                  flags |= FSR4F_HDR;
            if (Fsr4Config.Upscaler.Value == EUpscaler.FSR4) flags |= FSR4F_PREFER_FSR4;
            if (Fsr4Config.MvJitterCancel.Value)             flags |= FSR4F_MV_JITTER_CANCEL;
            if (Fsr4Config.ModelFix.Value)                   flags |= FSR4F_MODEL_FIX;

            // Reactive mask (flatscreen + VR). Capture the opaque-only color at BeforeForwardAlpha and hand
            // it to the native reactive-mask generator; the native no-ops if it's null. One capture RT is
            // fine in VR MultiPass: the eyes render sequentially, so each eye's render event copies the RT
            // before the next eye's BeforeForwardAlpha overwrites it (all in render-thread submission order).
            IntPtr opaquePtr = IntPtr.Zero;
            if (Fsr4Config.ReactiveMask.Value)
            {
                EnsureOpaqueCapture(cam, renderW, renderH, source.format);
                if (ptrRefetch && _opaqueRT != null)
                    _opaquePtr = _opaqueRT.GetNativeTexturePtr();
                opaquePtr = _opaquePtr;
                if (opaquePtr != IntPtr.Zero) flags |= FSR4F_REACTIVE;
            }
            else
            {
                DetachOpaqueCapture();
            }

            int slot = Time.frameCount & 3;

            Fsr4SetEyeFrameParams(
                eye, slot,
                colorPtr, depthPtr, motionPtr, outPtr, opaquePtr,
                renderW, renderH, outW, outH,
                jitterX, jitterY, mvScaleX, mvScaleY,
                camNear, camFar, fovY,
                dtMs, Mathf.Clamp01(Fsr4Config.Sharpness.Value), flags);

            int eventId = (slot << 1) | eye;
            CommandBuffer cmd = _eventCmds[eventId];
            if (cmd == null)
            {
                cmd = new CommandBuffer { name = "FSR4Bridge" };
                cmd.IssuePluginEvent(_renderEventFunc, eventId);
                _eventCmds[eventId] = cmd;
            }
            UnityEngine.Graphics.ExecuteCommandBuffer(cmd);

            // Debug Log toggles both the managed per-frame diagnostics AND the native per-texture spam.
            if (dbg != _prevDebugLog)
            {
                if (dbg) _debugLogsLeft = 8;
                Fsr4SetVerbose(dbg ? 1 : 0);
                _prevDebugLog = dbg;
            }
            if (dbg && _debugLogsLeft > 0 && (Time.frameCount % 89 == 0))
            {
                _debugLogsLeft--;
                Plugin.MyLog.LogInfo(
                    $"[FSR4] {(vr ? "VR eye" + eye : "flat")} render {renderW}x{renderH}->{outW}x{outH} " +
                    $"fed=({jitterX:F3},{jitterY:F3}) vrJitter=({vrJitter.x:F3},{vrJitter.y:F3}) " +
                    $"gameJitter=({gameJitter.x:F3},{gameJitter.y:F3}) mvCancel={Fsr4Config.MvJitterCancel.Value}" +
                    (vr ? $" | JITTER-MAG: assumed={GetVrAssumedRenderWidth()}x{GetVrAssumedRenderHeight()} vs actual={renderW}x{renderH}" : ""));
            }

            DrainLog();
            if (!_loggedActiveProvider)
            {
                int n = Fsr4GetActiveVersion(_logBuf, _logBuf.Length);
                if (n > 0 && _logBuf[0] != (byte)'n')   // "none" until the first context creates
                {
                    _activeProviderStr = (Fsr4IsFsr4Active() == 1 ? "FSR4 " : "FSR ") + Encoding.UTF8.GetString(_logBuf, 0, n);
                    Plugin.MyLog.LogInfo($"[FSR4] active — {_activeProviderStr}");
                    _loggedActiveProvider = true;
                }
            }

            // One clean line whenever the resolution changes (quality change / Native AA toggle), throttled
            // (1s) so a transient render-size flip-flop can't spam.
            if ((renderW != _lastRW || renderH != _lastRH || outW != _lastOW || outH != _lastOH)
                && Time.realtimeSinceStartup - _lastResLogTime > 1f)
            {
                _lastRW = renderW; _lastRH = renderH; _lastOW = outW; _lastOH = outH;
                _lastResLogTime = Time.realtimeSinceStartup;
                Plugin.MyLog.LogInfo($"[FSR4] {_activeProviderStr}{(vr ? " (VR)" : "")} — render {renderW}x{renderH} -> {outW}x{outH}");
            }

            if (dbg)
            {
                _mainSw.Stop();
                _mainAccumMs += _mainSw.Elapsed.TotalMilliseconds;
                if (++_mainSamples >= 300)
                {
                    Plugin.MyLog.LogInfo($"[FSR4] main-thread RenderEye avg over {_mainSamples} calls: {_mainAccumMs / _mainSamples:F3} ms");
                    _mainAccumMs = 0.0;
                    _mainSamples = 0;
                }
            }
            return true;
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static Vector2 GetVrJitter()
        {
            return TarkovVR.Patches.Upscalers.VRJitterComponent.CurrentJitter;
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int GetVrAssumedRenderWidth()
        {
            return TarkovVR.Patches.Upscalers.VRJitterComponent.LastScaledWidth;
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int GetVrAssumedRenderHeight()
        {
            return TarkovVR.Patches.Upscalers.VRJitterComponent.LastScaledHeight;
        }

        // -------------------------------------------------------------------------------------------
        // Attach (once) a CommandBuffer that blits the opaque-only color — available at BeforeForwardAlpha,
        // before transparency composites — into an owned RT, for the native reactive-mask generator.
        private static void EnsureOpaqueCapture(Camera cam, int w, int h, RenderTextureFormat fmt)
        {
            bool rebuilt = _opaqueRT == null || _opaqueRT.width != w || _opaqueRT.height != h
                        || _opaqueRT.format != fmt || !_opaqueRT.IsCreated();
            EnsureCopyRT(ref _opaqueRT, ref _opaquePtr, w, h, fmt, "FSR4Opaque", -1);
            if (_opaqueCmd == null)
                _opaqueCmd = new CommandBuffer { name = "FSR4OpaqueCapture" };
            if (rebuilt || _opaqueCam != cam)
            {
                _opaqueCmd.Clear();
                _opaqueCmd.Blit(new RenderTargetIdentifier(BuiltinRenderTextureType.CameraTarget), _opaqueRT);
            }
            if (_opaqueCam != cam)
            {
                if (_opaqueCam != null) _opaqueCam.RemoveCommandBuffer(CameraEvent.BeforeForwardAlpha, _opaqueCmd);
                cam.AddCommandBuffer(CameraEvent.BeforeForwardAlpha, _opaqueCmd);
                _opaqueCam = cam;
            }
        }

        private static void DetachOpaqueCapture()
        {
            if (_opaqueCam != null && _opaqueCmd != null)
                _opaqueCam.RemoveCommandBuffer(CameraEvent.BeforeForwardAlpha, _opaqueCmd);
            _opaqueCam = null;
        }

        private static void EnsureCopyRT(ref RenderTexture rt, ref IntPtr ptr, int w, int h, RenderTextureFormat fmt, string baseName, int eye)
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
                name = eye >= 0 ? baseName + eye : baseName,
                filterMode = FilterMode.Point,
                useMipMap = false,
                autoGenerateMips = false,
                anisoLevel = 0,
            };
            rt.Create();
            ptr = rt.GetNativeTexturePtr();   // the one render-thread sync, at (re)create only
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

        // Reused buffer: this runs every frame, and the old per-call StringBuilder(1024) + its
        // marshaling copies were steady Mono GC churn even with zero pending lines.
        private static readonly byte[] _logBuf = new byte[1024];

        private static void DrainLog()
        {
            try
            {
                int n, guard = 0;
                while ((n = Fsr4PopLogLine(_logBuf, _logBuf.Length)) > 0 && guard++ < 64)
                    Plugin.MyLog.LogInfo(Encoding.UTF8.GetString(_logBuf, 0, n));
            }
            catch { }
        }
    }
}
