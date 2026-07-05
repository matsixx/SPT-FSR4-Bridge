using FSR4Bridge.Source;
using HarmonyLib;
using SPT.Reflection.Patching;
using System;
using System.Reflection;
using UnityEngine;
using UnityEngine.Rendering;

namespace FSR4Bridge.Patches
{
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
            if (!__runOriginal)
                return false;

            if (Fsr4Config.Enabled.Value)
            {
                Camera cam = __instance.GetComponent<Camera>();
                if (Fsr4Bridge.RenderEye(source, destination, cam))
                {
                    __result = true;
                    return false;
                }
            }
            return true;
        }
    }
}
