using BepInEx;
using BepInEx.Bootstrap;
using BepInEx.Logging;
using FSR4Bridge.Patches;
using FSR4Bridge.Source;

namespace FSR4Bridge
{
    [BepInDependency("com.matsix.sptvr", BepInDependency.DependencyFlags.SoftDependency)]
    [BepInPlugin("com.matsix.fsr4bridge", "FSR4 Bridge", "1.0.1")]
    public class Plugin : BaseUnityPlugin
    {
        public static ManualLogSource MyLog;

        private void Awake()
        {
            MyLog = Logger;
            Fsr4Config.Bind(Config);

            Fsr4Bridge.SptVrPresent = Chainloader.PluginInfos.ContainsKey("com.matsix.sptvr");
            if (Fsr4Bridge.SptVrPresent)
                MyLog.LogInfo("FSR4Bridge: SPT-VR detected — driving VR FSR4 as well as flatscreen.");

            new Fsr4RenderPatch().Enable();
            new Fsr4NativeAAPatch().Enable();

            // Free the FFX contexts + VRAM when the user turns FSR4 off mid-session (they lazily
            // rebuild if re-enabled; kept alive across renderer refreshes otherwise).
            Fsr4Config.Enabled.SettingChanged += (_, __) =>
            {
                if (!Fsr4Config.Enabled.Value)
                    Fsr4Bridge.InvalidateContexts();
            };

            // Apply Native AA live (the game only re-runs Switch on a quality-setting change otherwise).
            Fsr4Config.NativeAA.SettingChanged += (_, __) => Fsr4NativeAAPatch.ReapplyOnToggle();

            MyLog.LogInfo("FSR4Bridge loaded — select an FSR mode in Graphics settings and toggle 'Enable FSR4' in the config.");
        }
    }
}
