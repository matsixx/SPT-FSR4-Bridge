# FSR4 Bridge — AMD FSR4 for Single Player Tarkov (flatscreen)

Routes the game's FSR upscaler to **AMD FSR4** (or the latest **FSR 3.1.x** on non-RDNA4 GPUs) via a
native D3D11→D3D12 bridge. Tarkov/Unity render on D3D11; FSR4 and the modern FidelityFX providers are
D3D12-only, so the bridge stands up a private D3D12 device, shares the render-res textures across a
timeline fence, runs `ffxDispatch`, and copies the result back — all on the render thread, no CPU stall.

## Install

Drop the `FSR4Bridge` folder into `BepInEx/plugins/`:

```
BepInEx/plugins/FSR4Bridge/
  FSR4Bridge.dll
  native/
    FSR4Native.dll
    amd_fidelityfx_loader_dx12.dll
    amd_fidelityfx_upscaler_dx12.dll
```

## Use

1. In the game's **Graphics** settings, select an **FSR** quality mode (this is what enables upscaling
   and the render-res downscale — FSR4 Bridge just swaps the upscaler).
2. In the BepInEx **config** (F12 ConfigurationManager, or edit
   `BepInEx/config/com.matsix.fsr4bridge.cfg`), set **Enable FSR4 = true**.

True **FSR4** requires an **AMD RDNA4 GPU** (e.g. RX 9070) with a recent driver — it's delivered by the
driver's `amdxcffx64.dll`, which the loader finds at runtime. On other GPUs it transparently uses the
bundled native **FSR 3.1.x** provider. Watch the BepInEx log for `[FSR4] active provider: ...`.

## Config

- **Enable FSR4** — the master toggle.
- **Sharpness** (0–1) — RCAS post-sharpen. FSR4 is smoother than FSR3 by design (bypassing the game's
  own FSR3 sharpen pass too), so raise this (~0.6–0.9) if edges look soft.
- *Advanced:* **Prefer FSR4** (off = force 3.1.x for A/B), **Auto Exposure**, **Depth Inverted**,
  **Invert Motion Vectors**, and **Jitter Scale / Flip X / Flip Y** — correctness knobs. Defaults are
  tuned for EFT; the jitter flips default ON because the game's jitter uses the opposite sign FSR wants.

## Coexistence with SPT-VR

The SPT-VR mod ships its own VR-tuned FSR4 integration. If it's installed, **FSR4 Bridge detects it and
stands down** (via a soft dependency) so the two never double-patch the upscaler — VR keeps working,
FSR4 Bridge only owns flatscreen. (The VR mod is VR-only, so there's no practical "flatscreen with the
VR mod" case; to run FSR4 Bridge, use a build without the VR mod loaded.)

## Building

- Managed: `dotnet build -c Debug` (auto-deploys to the game via the csproj `DeployToGame` target).
- Native: `native/build.bat` (MSVC, static CRT → `native/build/FSR4Native.dll`); the deploy target
  copies it + the AMD runtime into the plugin folder.

The native bridge is generic D3D interop (no VR, no Unity) — the same source as the SPT-VR mod's
`SPTVR_FSR4.dll`, built here as `FSR4Native.dll` so the managed mod (`FSR4Bridge.dll`) and the native
helper never share a base name.
