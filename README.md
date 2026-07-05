# FSR4 Bridge — AMD FSR4 for SPT

Routes the game's FSR3 upscaler to **AMD FSR4** (or the latest **FSR 3.1.x** as a fallback) via a
native D3D11→D3D12 bridge. This is built using the latest FSR SDK, so it supports FSR 4.1.1 which works with RDNA3.

If your card does not support FSR4 and you select it, it will auto fallback to 3.1.x.

## Use

1. In the game's **Graphics** settings, select an **FSR3** quality mode
2. In the BepInEx **config** (F12 ConfigurationManager, or edit
   `BepInEx/config/com.matsix.fsr4bridge.cfg`), set **Enable FSR4 = true**.

## Config

- **Enable FSR4** — the master toggle.
- **Sharpness** (0–1) — RCAS post-sharpen. FSR4 is smoother than FSR3 by design (bypassing the game's
  own FSR3 sharpen pass too), so raise this (~0.6–0.9) if edges look soft.

## SPT-VR Support

You will need to be on version 1.3.1+ of SPT-VR for this bridge more to work. I had to add special support for the VR mod since it changes
a lot of upscaler code.

