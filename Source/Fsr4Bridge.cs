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
        private const int FSR4F_OPTIC          = 1 << 11;

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
        private static IntPtr _afterAlphaPtr;
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

        // Reactive-mask capture: two CommandBuffers bracket the forward-transparent pass, the same way the
        // game's FSR3 wrapper does it — opaque-only color at BeforeForwardAlpha, color right after the
        // transparents at AfterForwardAlpha. The native reactive-mask generator compares the two.
        private static RenderTexture _opaqueRT, _afterAlphaRT;
        private static CommandBuffer _opaqueCmd, _afterAlphaCmd;
        private static Camera _captureCam;
        private static RenderTexture _afterAlphaSrcBound;

        // Depth/motion capture for the immediate path: a CommandBuffer on OUR camera, so the buffers we feed
        // FSR are always the ones our camera rendered (see the comment at the call site).
        private static CommandBuffer _dmCmd;
        private static Camera _dmCam;
        private static RenderTexture _dmDepthBound, _dmMotionBound;
        private static bool _loggedDepthSizeMismatch;
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
            IntPtr colorTex, IntPtr depthTex, IntPtr mvecTex, IntPtr outTex, IntPtr opaqueTex, IntPtr afterAlphaTex,
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
            DetachReactiveCapture();               // don't leave the capture blits running while off
            DetachDepthCapture();
            _destRT[0] = _destRT[1] = null;        // refetch destination ptr on next use
            if (_initOk)
            {
                try { Fsr4InvalidateContexts(); } catch { }
                DrainLog();
            }
        }

        public static void IdleTick()
        {
            if (Time.frameCount - _lastRenderEyeFrame > 120)
            {
                if (_captureCam != null) DetachReactiveCapture();
                if (_dmCam != null) DetachDepthCapture();
            }
        }

        // Called from Fsr4RenderPatch in place of the game's FSR3 wrapper.
        // externalCommandBuffer: flatscreen's PostProcessLayer builds its command buffer in OnPreCull
        // (SSAAPropagator.FillCommandBuffer) and runs it later at BeforeImageEffects; VR has no PostProcessLayer
        // and calls from OnRenderImage with null. When a buffer is given, everything that reads this frame's
        // images must be RECORDED into it — doing it immediately feeds FSR the previous frame's color/depth/motion
        // against this frame's jitter, and shows the result a frame late.
        public static bool RenderEye(RenderTexture source, RenderTexture destination, Camera cam,
                                     CommandBuffer externalCommandBuffer, bool opticOrCollimator, RenderTexture afterTransparentRT)
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
                // Recoverable: D3D12 device was removed by a renderer refresh (SteamVR dashboard lowers
                // the game's res in the background). KEEP calling so the native rebuilds it on the next
                // dispatch — usually one glitch frame, then it recovers. Give up only if it never comes back.
                if (++_deviceLostRetries > 30)
                {
                    _permaFail = true;
                    DetachReactiveCapture();   // stop paying the capture blits once we stop rendering
                    DetachDepthCapture();
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
                    DetachReactiveCapture();   // stop paying the capture blits once we stop rendering
                    DetachDepthCapture();
                    Plugin.MyLog.LogWarning($"[FSR4] native status {st} — falling back to in-engine FSR3 for this session.");
                    DrainLog();
                    return false;
                }
            }

            bool vr = SptVrPresent;
            bool record = externalCommandBuffer != null;
            int eye = ((int)cam.stereoActiveEye) & 1;
            _lastRenderEyeFrame = Time.frameCount;

            // Non-OK status (init warmup, device-lost recovery): Unity may have recreated resources
            // behind our cached native pointers — refetch them this frame. 0 on every normal frame.
            bool ptrRefetch = st != 0;

            bool dbg = Fsr4Config.DebugLog.Value;
            if (dbg) _mainSw.Restart();

            // FSR needs render-res depth + motion, which Unity only produces once the camera has the matching
            // DepthTextureMode.
            cam.depthTextureMode |= DepthTextureMode.Depth | DepthTextureMode.MotionVectors;

            int renderW = source.width;
            int renderH = source.height;
            int outW = destination.width;
            int outH = destination.height;

            // The immediate path's fallback source: the global textures (fetched as Texture objects — name-ref'd
            // blit sources don't resolve in a standalone CB).
            Texture camDepth = null, camMotion = null;
            if (!record)
            {
                camDepth  = Shader.GetGlobalTexture(_camDepthTexId);
                camMotion = Shader.GetGlobalTexture(_camMotionTexId);
                if (camDepth == null || camMotion == null)
                    return false;

                // A global that doesn't match our render size belongs to a different camera — the tell for the
                // optic-camera clobber below. Worth a one-time shout because it silently degrades the upscale.
                if (!_loggedDepthSizeMismatch && (camDepth.width != renderW || camDepth.height != renderH))
                {
                    _loggedDepthSizeMismatch = true;
                    Plugin.MyLog.LogWarning(
                        $"[FSR4] _CameraDepthTexture is {camDepth.width}x{camDepth.height} but our render is " +
                        $"{renderW}x{renderH} — another camera owns the depth/motion globals.");
                }
            }

            // `source` may be a pooled temp RT (ptr changes each frame) — copy into an owned RT so the
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

            // Same-size same-format color goes through the copy engine (no fullscreen draw); depth/motion
            // stay as Blits either way (they convert format to RFloat/RGHalf).
            bool copyColor = source.antiAliasing <= 1 && source.graphicsFormat == _colorRT[eye].graphicsFormat;

            if (record)
            {
                if (copyColor) externalCommandBuffer.CopyTexture(source, _colorRT[eye]);
                else           externalCommandBuffer.Blit(source, _colorRT[eye]);

                // The post-process buffer is a camera-event buffer, so the builtin identifiers resolve to OUR
                // camera's depth/motion — same sources the game's FSR3 binds.
                DetachDepthCapture();
                externalCommandBuffer.Blit(new RenderTargetIdentifier(BuiltinRenderTextureType.ResolvedDepth), _depthRT[eye]);
                externalCommandBuffer.Blit(new RenderTargetIdentifier(BuiltinRenderTextureType.MotionVectors), _motionRT[eye]);
            }
            else
            {
                if (copyColor) UnityEngine.Graphics.CopyTexture(source, _colorRT[eye]);
                else           UnityEngine.Graphics.Blit(source, _colorRT[eye]);

                // The _CameraDepthTexture/_CameraMotionVectorsTexture globals belong to whichever camera rendered
                // last, and aiming an optic puts a SECOND camera in the frame: OpticComponentUpdater copies the
                // main camera's antialiasingMode onto the scope camera's PostProcessLayer, and a TAA-mode layer
                // forces Depth|MotionVectors on its camera — so the scope rebinds both globals from its own square
                // 1024 render at the scope's magnified FOV. Capture through a CommandBuffer on our own camera
                // instead. VR stays on the globals — the CB bakes in one eye's RT and MultiPass renders both eyes
                // through the same camera.
                bool useCameraScoped = !vr;
                bool captured = useCameraScoped && EnsureDepthMotionCapture(cam, eye);
                if (!useCameraScoped)
                    DetachDepthCapture();
                if (!captured)
                {
                    // Globals path, and the bootstrap for the frame the CB above is (re)recorded on — it is
                    // attached before image effects, so it has not run yet when we get here that frame.
                    UnityEngine.Graphics.Blit(camDepth,  _depthRT[eye]);
                    UnityEngine.Graphics.Blit(camMotion, _motionRT[eye]);
                }
            }

            // Vertical FOV from the projection matrix (m11 = 1/tan(fovY/2)); near/far from the camera.
            Matrix4x4 proj = cam.projectionMatrix;
            float m11 = Mathf.Abs(proj.m11) < 1e-6f ? 1f : proj.m11;
            float fovY = 2f * Mathf.Atan(1f / m11);
            float camNear = cam.nearClipPlane;
            float camFar  = cam.farClipPlane;

            // FSR takes the NEGATED game jitter: the game's own FSR3Wrapper feeds -jitterPixelSpace, and SPT-VR's
            // stereo FSR3 feeds -CurrentJitter. The flip toggles A/B each axis against that.
            float jScale = Fsr4Config.JitterScale.Value;
            Vector2 gameJitter = TemporalAntialiasing.jitterPixelSpace;
            Vector2 vrJitter = vr ? GetVrJitter() : Vector2.zero;
            Vector2 jitter = vr && !Fsr4Config.VrUseGameJitter.Value ? vrJitter : gameJitter;
            float jitterX = -jitter.x * jScale * (Fsr4Config.FlipJitterX.Value ? -1f : 1f);
            float jitterY = -jitter.y * jScale * (Fsr4Config.FlipJitterY.Value ? -1f : 1f);

            // MV scale: final scale must be {-1,-1} (the game's FSR3 convention); the native ffx-api
            // divides by the MV target size, so pre-multiply by renderSize.
            float mvScaleX = -renderW;
            float mvScaleY = -renderH;

            float dtMs = Time.deltaTime * 1000f;

            // Depth is reversed-Z + auto-exposure on. Dynamic resolution is VR-only: it keeps one context alive
            // across the SteamVR dashboard's render-size changes, but it also makes FSR4 pick its generalist DRS
            // model (the native model fix remaps that). Flatscreen gets a context sized to the real render res,
            // rebuilt on quality changes, so FSR4 selects the model for the actual ratio.
            int flags = FSR4F_DEPTH_INVERTED | FSR4F_AUTO_EXPOSURE | FSR4F_MODEL_FIX;
            if (vr)                                          flags |= FSR4F_DYNAMIC_RES;
            if (Fsr4Config.DepthInfinite.Value)              flags |= FSR4F_DEPTH_INFINITE;
            if (IsHdrFormat(source.format))                  flags |= FSR4F_HDR;
            if (Fsr4Config.Upscaler.Value == EUpscaler.FSR4) flags |= FSR4F_PREFER_FSR4;
            if (Fsr4Config.MvJitterCancel.Value)             flags |= FSR4F_MV_JITTER_CANCEL;

            // Reactive mask (flatscreen + VR). One pair of capture RTs is fine in VR MultiPass: the eyes render
            // sequentially, so each eye's render event copies them before the next eye's pass overwrites them.
            IntPtr opaquePtr = IntPtr.Zero, afterAlphaPtr = IntPtr.Zero;
            if (Fsr4Config.ReactiveMask.Value)
            {
                EnsureReactiveCapture(cam, renderW, renderH, source.format, afterTransparentRT);
                if (ptrRefetch)
                {
                    if (_opaqueRT != null)     _opaquePtr     = _opaqueRT.GetNativeTexturePtr();
                    if (_afterAlphaRT != null) _afterAlphaPtr = _afterAlphaRT.GetNativeTexturePtr();
                }
                opaquePtr = _opaquePtr;
                afterAlphaPtr = _afterAlphaPtr;
                if (opaquePtr != IntPtr.Zero) flags |= FSR4F_REACTIVE;
                if (opticOrCollimator)        flags |= FSR4F_OPTIC;
            }
            else
            {
                DetachReactiveCapture();
            }

            int slot = Time.frameCount & 3;

            Fsr4SetEyeFrameParams(
                eye, slot,
                colorPtr, depthPtr, motionPtr, outPtr, opaquePtr, afterAlphaPtr,
                renderW, renderH, outW, outH,
                jitterX, jitterY, mvScaleX, mvScaleY,
                camNear, camFar, fovY,
                dtMs, Mathf.Clamp01(Fsr4Config.Sharpness.Value), flags);

            int eventId = (slot << 1) | eye;
            if (record)
            {
                externalCommandBuffer.IssuePluginEvent(_renderEventFunc, eventId);
                externalCommandBuffer.SetRenderTarget(destination);   // leave the buffer targeting the output, like the game's FSR3
            }
            else
            {
                CommandBuffer cmd = _eventCmds[eventId];
                if (cmd == null)
                {
                    cmd = new CommandBuffer { name = "FSR4Bridge" };
                    cmd.IssuePluginEvent(_renderEventFunc, eventId);
                    _eventCmds[eventId] = cmd;
                }
                UnityEngine.Graphics.ExecuteCommandBuffer(cmd);
            }

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
                    $"gameJitter=({gameJitter.x:F3},{gameJitter.y:F3}) recorded={record} optic={opticOrCollimator} " +
                    $"afterAlphaSrc={(afterTransparentRT != null ? afterTransparentRT.name : "CameraTarget")}" +
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
        // Attach (once) the two reactive-mask captures: opaque-only color at BeforeForwardAlpha, and the color
        // after forward transparents at AfterForwardAlpha. The after-alpha source is the RT the game hands
        // SSAAImpl.SetAfterTransparentRT (WindowsManager's), else the camera target — exactly what the game's
        // FSR3 wrapper compares. Using the final input color instead would count every later image effect as
        // "reactive" and throw away history there.
        private static void EnsureReactiveCapture(Camera cam, int w, int h, RenderTextureFormat fmt, RenderTexture afterSrc)
        {
            bool opaqueRebuilt = NeedsRecreate(_opaqueRT, w, h, fmt);
            bool afterRebuilt  = NeedsRecreate(_afterAlphaRT, w, h, fmt);
            EnsureCopyRT(ref _opaqueRT,     ref _opaquePtr,     w, h, fmt, "FSR4Opaque",     -1);
            EnsureCopyRT(ref _afterAlphaRT, ref _afterAlphaPtr, w, h, fmt, "FSR4AfterAlpha", -1);

            if (_opaqueCmd == null)
                _opaqueCmd = new CommandBuffer { name = "FSR4OpaqueCapture" };
            if (_afterAlphaCmd == null)
                _afterAlphaCmd = new CommandBuffer { name = "FSR4AfterAlphaCapture" };

            if (opaqueRebuilt || _captureCam != cam)
            {
                _opaqueCmd.Clear();
                _opaqueCmd.Blit(new RenderTargetIdentifier(BuiltinRenderTextureType.CameraTarget), _opaqueRT);
            }
            if (afterRebuilt || _captureCam != cam || !ReferenceEquals(afterSrc, _afterAlphaSrcBound))
            {
                _afterAlphaCmd.Clear();
                _afterAlphaCmd.Blit(afterSrc != null ? new RenderTargetIdentifier(afterSrc)
                                                     : new RenderTargetIdentifier(BuiltinRenderTextureType.CameraTarget), _afterAlphaRT);
                _afterAlphaSrcBound = afterSrc;
            }
            if (_captureCam != cam)
            {
                DetachReactiveCapture();
                cam.AddCommandBuffer(CameraEvent.BeforeForwardAlpha, _opaqueCmd);
                cam.AddCommandBuffer(CameraEvent.AfterForwardAlpha, _afterAlphaCmd);
                _captureCam = cam;
            }
        }

        private static void DetachReactiveCapture()
        {
            if (_captureCam != null)
            {
                if (_opaqueCmd != null)     _captureCam.RemoveCommandBuffer(CameraEvent.BeforeForwardAlpha, _opaqueCmd);
                if (_afterAlphaCmd != null) _captureCam.RemoveCommandBuffer(CameraEvent.AfterForwardAlpha, _afterAlphaCmd);
            }
            _captureCam = null;
        }

        // Attach (and keep in sync) a CommandBuffer that blits OUR camera's depth + motion into the owned RTs
        // before image effects. Returns false on the frame it is (re)recorded — the CB's event has already
        // passed by then, so the caller must fill the RTs itself that once.
        private static bool EnsureDepthMotionCapture(Camera cam, int eye)
        {
            if (_dmCmd != null && _dmCam == cam
                && ReferenceEquals(_dmDepthBound, _depthRT[eye]) && ReferenceEquals(_dmMotionBound, _motionRT[eye]))
                return true;

            if (_dmCmd == null)
                _dmCmd = new CommandBuffer { name = "FSR4DepthMotionCapture" };
            _dmCmd.Clear();
            _dmCmd.Blit(new RenderTargetIdentifier(BuiltinRenderTextureType.ResolvedDepth), _depthRT[eye]);
            _dmCmd.Blit(new RenderTargetIdentifier(BuiltinRenderTextureType.MotionVectors), _motionRT[eye]);
            _dmDepthBound  = _depthRT[eye];
            _dmMotionBound = _motionRT[eye];

            if (_dmCam != cam)
            {
                if (_dmCam != null) _dmCam.RemoveCommandBuffer(CameraEvent.BeforeImageEffects, _dmCmd);
                cam.AddCommandBuffer(CameraEvent.BeforeImageEffects, _dmCmd);
                _dmCam = cam;
            }
            return false;
        }

        private static void DetachDepthCapture()
        {
            if (_dmCam != null && _dmCmd != null)
                _dmCam.RemoveCommandBuffer(CameraEvent.BeforeImageEffects, _dmCmd);
            _dmCam = null;
            _dmDepthBound = _dmMotionBound = null;
        }

        private static bool NeedsRecreate(RenderTexture rt, int w, int h, RenderTextureFormat fmt)
        {
            return rt == null || rt.width != w || rt.height != h || rt.format != fmt || !rt.IsCreated();
        }

        private static void EnsureCopyRT(ref RenderTexture rt, ref IntPtr ptr, int w, int h, RenderTextureFormat fmt, string baseName, int eye)
        {
            if (!NeedsRecreate(rt, w, h, fmt))
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
