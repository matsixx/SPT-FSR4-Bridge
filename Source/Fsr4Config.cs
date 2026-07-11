using BepInEx;
using BepInEx.Configuration;

namespace FSR4Bridge.Source
{
    internal sealed class ConfigurationManagerAttributes
    {
        public int? Order;
        public bool? IsAdvanced;
    }

    public enum EUpscaler { FSR4, FSR3_1 }

    internal static class Fsr4Config
    {
        public static ConfigEntry<EUpscaler> Upscaler;
        public static ConfigEntry<bool>      Enabled;
        public static ConfigEntry<float>     Sharpness;
        public static ConfigEntry<float>     JitterScale;
        public static ConfigEntry<bool>      DepthInfinite;
        public static ConfigEntry<bool>      ReactiveMask;
        public static ConfigEntry<bool>      NativeAA;
        public static ConfigEntry<bool>      ModelFix;

        public static ConfigEntry<bool>      DebugLog;
        public static ConfigEntry<bool>      VrUseGameJitter;
        public static ConfigEntry<bool>      MvJitterCancel;
        public static ConfigEntry<bool>      VrFlipJitterX;
        public static ConfigEntry<bool>      VrFlipJitterY;

        private static ConfigDescription Ordered(string desc, int order, AcceptableValueBase range = null)
        {
            return new ConfigDescription(desc, range, new ConfigurationManagerAttributes { Order = order });
        }

        private static ConfigDescription Advanced(string desc, int order = 0, AcceptableValueBase range = null)
        {
            return new ConfigDescription(desc, range, new ConfigurationManagerAttributes { Order = order, IsAdvanced = true });
        }

        public static void Bind(ConfigFile config)
        {
            string currentVersion = MetadataHelper.GetMetadata(typeof(Plugin)).Version.ToString();
            var version = config.Bind("Internal", "ConfigVersion", "", "Do not modify");
            if (version == null || version.Value != currentVersion)
            {
                config.Clear();
                System.IO.File.WriteAllText(config.ConfigFilePath, "");
                config.Reload();
                version = config.Bind("Internal", "ConfigVersion", currentVersion, "Do not modify");
                version.Value = currentVersion;
                config.Save();
                Plugin.MyLog.LogInfo($"Config reset for version {currentVersion}");
            }

            Upscaler = config.Bind("FSR4", "Upscaler", EUpscaler.FSR4,
                Ordered("Choose which upscaler you would like to use, 3.1.x will use your current default in Adrenalin.", 4));

            ModelFix = config.Bind("FSR4", "FSR4 Model Stability Fix", true,
                Ordered("Forces FSR4's stable ML model at Quality-range ratios (1.29x+), where the driver's own model " +
                    "selection is unstable and makes edges wobble/shimmer. No effect on FSR 3.1.x or at native ratios.", 5));

            Enabled = config.Bind("FSR4", "Enable FSR4", true,
                Ordered("Enables FSR4, if this is disabled, the game will switch back to base FSR3.0", 3));

            Sharpness = config.Bind("FSR4", "Sharpness", 0.5f,
                Ordered("RCAS sharpening applied after upscaling.", 2, new AcceptableValueRange<float>(0f, 1f)));

            JitterScale = config.Bind("FSR4", "Jitter Scale", 1.0f,
                Ordered("Multiplier on the camera jitter fed to FSR. Leave at 1.0 unless the image is soft " +
                    "(temporal detail never builds up) — then try 0.5 or 2.0.", 1,
                    new AcceptableValueRange<float>(0.1f, 4f)));

            NativeAA = config.Bind("FSR4", "Native AA", false,
                Ordered("Render at native resolution and use FSR4 for AA", 0));

            DepthInfinite = config.Bind("Debug", "Depth Infinite Far Plane", true,
                Advanced("This tells FSR the far plane is at infinity — fixes edge shimmer on objects silhouette"));

            ReactiveMask = config.Bind("Debug", "Reactive Mask", true,
                Advanced("Auto-generate a reactive mask each frame (captures the opaque-only color and compares) " +
                "Can improve clarity over moving particles and edges"));

            DebugLog = config.Bind("Debug", "Debug Log", false,
                Advanced("Log per-frame FSR jitter/sizes to the BepInEx console."));

            VrUseGameJitter = config.Bind("Debug", "VR Only - Disable jitter", false,
                Advanced("Feed FSR the game's TemporalAntialiasing.jitterPixelSpace which is zero'd in SPT-VR"));

            MvJitterCancel = config.Bind("Debug", "MV Jitter Cancellation", false,
                Advanced("Experiment: tell FSR the motion vectors already contain the jitter (it subtracts it). "));

            VrFlipJitterX = config.Bind("Debug", "VR Only - Jitter Flip X", false,
                Advanced("Flips jitter on the X axis"));

            VrFlipJitterY = config.Bind("Debug", "VR Only - Jitter Flip Y", true,
                Advanced("Flips jitter on the Y axis"));
        }
    }
}
