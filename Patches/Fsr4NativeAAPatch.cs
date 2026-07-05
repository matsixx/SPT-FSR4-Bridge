using FSR4Bridge.Source;
using HarmonyLib;
using SPT.Reflection.Patching;
using System;
using System.Reflection;

namespace FSR4Bridge.Patches
{
    // Native AA: the game only RUNS FSR when the render ratio is < 1 (SSState.UPSCALE); at
    // exactly 1.0, FSR never enables. So to run FSR just for AA, we force the ratio to just below
    // 1.0
    internal class Fsr4NativeAAPatch : ModulePatch
    {
        private const float NearNative = 0.99f;

        internal static SSAA LastSsaa;
        internal static float LastOriginalRatio = 1f;

        protected override MethodBase GetTargetMethod()
        {
            return AccessTools.Method(typeof(SSAA), "Switch", new Type[] { typeof(float) });
        }

        [PatchPrefix]
        private static void Prefix(SSAA __instance, ref float superSamplingRatio)
        {
            LastSsaa = __instance;

            if (superSamplingRatio > 0f && superSamplingRatio < 1f
                && Math.Abs(superSamplingRatio - NearNative) > 0.0001f)
                LastOriginalRatio = superSamplingRatio;

            if (!Fsr4Config.NativeAA.Value || Fsr4Bridge.SptVrPresent)
                return;

            // Only override an UPSCALING request (0 < r < 1) — leave native (1.0) and supersampling (>1) alone.
            if (superSamplingRatio > 0f && superSamplingRatio < 1f)
            {
                Plugin.MyLog.LogInfo($"[FSR4] Native AA: render ratio {superSamplingRatio:F3} -> {NearNative}");
                superSamplingRatio = NearNative;
            }
        }

        // Re-run the last Switch so the new toggle state takes effect without changing the quality setting.
        internal static void ReapplyOnToggle()
        {
            if (LastSsaa != null && LastOriginalRatio > 0f && LastOriginalRatio < 1f)
                LastSsaa.Switch(LastOriginalRatio);
        }
    }
}
