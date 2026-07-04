using BepInEx;
using BepInEx.Bootstrap;
using BepInEx.Logging;
using FSR4Bridge.Patches;
using FSR4Bridge.Source;

namespace FSR4Bridge
{
    // The single FSR4 upscaler mod for SPT. Routes the game's FSR path to a native D3D11->D3D12 FSR4
    // (or FSR 3.1.x) bridge. Works flatscreen standalone; when the SPT-VR mod is present it ALSO drives
    // VR (stereo, per-eye, reading the VR mod's own jitter) — SPT-VR no longer ships FSR4 itself.
    //
    // The soft dependency makes BepInEx load us AFTER SPT-VR when present, so the PluginInfos check in
    // Awake is reliable regardless of on-disk order, and lets us reference its (publicized) types.
    [BepInDependency("com.matsix.sptvr", BepInDependency.DependencyFlags.SoftDependency)]
    [BepInPlugin("com.matsix.fsr4bridge", "FSR4 Bridge", "1.0.0")]
    public class Plugin : BaseUnityPlugin
    {
        public static ManualLogSource MyLog;

        private void Awake()
        {
            MyLog = Logger;
            Fsr4Config.Bind(Config);

            // If SPT-VR is loaded we drive VR too (its FSR4 patch stands down for us). This flag gates
            // the VR-typed jitter helper so it's only JITted when SPT-VR is actually present.
            Fsr4Bridge.SptVrPresent = Chainloader.PluginInfos.ContainsKey("com.matsix.sptvr");
            if (Fsr4Bridge.SptVrPresent)
                MyLog.LogInfo("FSR4Bridge: SPT-VR detected — driving VR FSR4 as well as flatscreen.");

            new Fsr4RenderPatch().Enable();

            // Free the FFX contexts + VRAM when the user turns FSR4 off mid-session (they lazily
            // rebuild if re-enabled; kept alive across renderer refreshes otherwise).
            Fsr4Config.Enabled.SettingChanged += (_, __) =>
            {
                if (!Fsr4Config.Enabled.Value)
                    Fsr4Bridge.InvalidateContexts();
            };

            MyLog.LogInfo("FSR4Bridge loaded — select an FSR mode in Graphics settings and toggle 'Enable FSR4' in the config.");
        }
    }
}
