using HarmonyLib;
using SPT.Reflection.Patching;
using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;

namespace FSR4Bridge.Patches
{
    // WindowsManager hands SSAAImpl the RT holding the color right after forward transparents (every frame),
    // and SSAAImpl only forwards it to an FSR wrapper that already exists — which never happens while the
    // bridge handles FSR3. Remember it so the bridge's reactive mask compares the same images the game's does.
    internal class Fsr4AfterTransparentPatch : ModulePatch
    {
        private static readonly Dictionary<SSAAImpl, RenderTexture> _rts = new Dictionary<SSAAImpl, RenderTexture>();
        private static readonly List<SSAAImpl> _dead = new List<SSAAImpl>();

        protected override MethodBase GetTargetMethod()
        {
            return AccessTools.Method(typeof(SSAAImpl), "SetAfterTransparentRT", new Type[] { typeof(RenderTexture) });
        }

        [PatchPostfix]
        private static void Postfix(SSAAImpl __instance, RenderTexture source)
        {
            if (!_rts.ContainsKey(__instance))
                PruneDestroyed();   // only on a new SSAAImpl (raid load), not per frame
            _rts[__instance] = source;
        }

        internal static RenderTexture Get(SSAAImpl impl)
        {
            return _rts.TryGetValue(impl, out RenderTexture rt) && rt != null ? rt : null;
        }

        private static void PruneDestroyed()
        {
            _dead.Clear();
            foreach (var kv in _rts)
                if (kv.Key == null) _dead.Add(kv.Key);
            foreach (var k in _dead)
                _rts.Remove(k);
        }
    }
}
