using BepInEx;
using BepInEx.Configuration;

namespace FSR4Bridge.Source
{
    // Marker read by the ConfigurationManager UI to fold advanced entries away by default.
    internal sealed class ConfigurationManagerAttributes
    {
        public bool? IsAdvanced;
    }

    internal static class Fsr4Config
    {
        public static ConfigEntry<bool>  Enabled;
        public static ConfigEntry<float> Sharpness;

        // Advanced correctness knobs — defaults follow the game's FSR conventions and normally need no
        // change. Exposed so a bad-looking result can be A/B'd without a rebuild.
        public static ConfigEntry<bool>  PreferFsr4;
        public static ConfigEntry<bool>  AutoExposure;
        public static ConfigEntry<bool>  DepthInverted;
        public static ConfigEntry<bool>  InvertMotionVectors;

        // Jitter tuning — flatscreen reads the game's jitter (TemporalAntialiasing.jitterPixelSpace)
        // instead of controlling it like VR does, so the sign/scale may need to be dialed in. Wrong
        // jitter = a soft, spatial-only upscale (no temporal detail).
        public static ConfigEntry<float> JitterScale;
        public static ConfigEntry<bool>  JitterFlipX;
        public static ConfigEntry<bool>  JitterFlipY;
        public static ConfigEntry<bool>  DebugLog;

        private static ConfigDescription Adv(string desc, AcceptableValueBase range = null)
        {
            return new ConfigDescription(desc, range, new ConfigurationManagerAttributes { IsAdvanced = true });
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

            Enabled = config.Bind("FSR4", "Enable FSR4", true,
                "Route the game's FSR upscaler to AMD FSR4 (or the latest FSR 3.1.x on non-RDNA4 GPUs). " +
                "REQUIRES an FSR quality mode selected in the in-game Graphics settings — that's what turns " +
                "upscaling on; this just swaps the upscaler. Needs an AMD GPU with a recent driver for true FSR4.");

            Sharpness = config.Bind("FSR4", "Sharpness", 0.5f,
                new ConfigDescription("RCAS sharpening applied after upscaling. FSR4's ML model is smoother " +
                    "than FSR3 by design (more stable in motion, softer at rest), and swapping it in bypasses " +
                    "the game's own FSR3 sharpen pass — so raise this (0.6–0.9) if edges look soft. 0 = off.",
                    new AcceptableValueRange<float>(0f, 1f)));

            PreferFsr4 = config.Bind("Advanced", "Prefer FSR4", true,
                Adv("On: use the FSR4 provider when the driver exposes it. Off: force the default FSR 3.1.x " +
                    "provider (useful for A/B comparison)."));
            AutoExposure = config.Bind("Advanced", "Auto Exposure", true,
                Adv("Let FSR compute its own exposure. Turn off only if the image is over/under-exposed."));
            DepthInverted = config.Bind("Advanced", "Depth Inverted", true,
                Adv("Unity uses reversed-Z depth on D3D (default). If you get heavy ghosting / no disocclusion " +
                    "handling, toggle this."));
            InvertMotionVectors = config.Bind("Advanced", "Invert Motion Vectors", true,
                Adv("Negate the motion-vector direction (matches the game's FSR3 convention). If moving/turning " +
                    "smears the image, toggle this."));

            JitterScale = config.Bind("Advanced", "Jitter Scale", 1.0f,
                Adv("Multiplier on the camera jitter fed to FSR. If the image is soft/blurry (temporal detail " +
                    "never builds up), the jitter magnitude may be off — try 0.5 or 2.0.",
                    new AcceptableValueRange<float>(0.1f, 4f)));
            // Default ON: the game's jitterPixelSpace uses the OPPOSITE sign convention to what FSR
            // wants in flatscreen (confirmed in-game — with these off the image is soft/no temporal
            // detail). Left as toggles in case a setup differs.
            JitterFlipX = config.Bind("Advanced", "Jitter Flip X", true,
                Adv("Flip the horizontal jitter sign (A/B if the image is soft or shimmery)."));
            JitterFlipY = config.Bind("Advanced", "Jitter Flip Y", true,
                Adv("Flip the vertical jitter sign (A/B if the image is soft or shimmery)."));
            DebugLog = config.Bind("Advanced", "Debug Log", false,
                Adv("Log the per-frame FSR params (jitter, sizes, dt) to the BepInEx console a few times " +
                    "so you can check the jitter is non-zero and changing each frame."));
        }
    }
}
