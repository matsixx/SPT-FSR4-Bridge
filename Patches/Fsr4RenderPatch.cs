using FSR4Bridge.Source;
using HarmonyLib;
using SPT.Reflection.Patching;
using System;
using System.Reflection;
using UnityEngine;
using UnityEngine.Rendering;

namespace FSR4Bridge.Patches
{
    // Hooks the game's FSR3 upscale entry point. When "Enable FSR4" is on and the native bridge can
    // run this frame, we run FSR4/FSR3.1 instead and skip the in-engine FSR3 wrapper; otherwise we
    // fall through to vanilla FSR3. Same enablement as the game (an FSR quality mode in Graphics
    // settings drives EnableFSR3 + the render-res downscale) — we just swap the upscaler.
    internal class Fsr4RenderPatch : ModulePatch
    {
        protected override MethodBase GetTargetMethod()
        {
            return AccessTools.Method(typeof(SSAAImpl), "TryRenderFSR3",
                new Type[] { typeof(RenderTexture), typeof(RenderTexture), typeof(CommandBuffer) });
        }

        // High priority so we run BEFORE SPT-VR's own FSR3-wrapper prefix on this method. When we
        // handle the frame we return false; SPT-VR's prefix then sees __runOriginal == false and bails.
        [PatchPrefix]
        [HarmonyPriority(Priority.First)]
        static bool Prefix(SSAAImpl __instance, RenderTexture source, RenderTexture destination,
                           CommandBuffer externalCommandBuffer, ref bool __result, bool __runOriginal)
        {
            // Defensive: if some other prefix (e.g. SPT-VR's FSR3 wrapper) already handled this frame,
            // don't render on top of it. With our First priority we normally run before it.
            if (!__runOriginal)
                return false;

            if (Fsr4Config.Enabled.Value)
            {
                Camera cam = __instance.GetComponent<Camera>();
                if (Fsr4Bridge.RenderEye(source, destination, cam))
                {
                    __result = true;
                    return false;   // handled by FSR4 — skip the vanilla FSR3 wrapper
                }
            }
            return true;            // run vanilla FSR3
        }
    }
}
