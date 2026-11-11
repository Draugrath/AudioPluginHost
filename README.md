# Audio Plugin Host

SKSE plugin that hosts VST3 effect plugins inside Skyrim. 
## Features

- Loads 64-bit VST3 effects from `C:\Program Files\Common Files\VST3`, `Data/SKSE/Plugins/AudioPluginHost/VST3` and one folder of your choice
- A chain of plugins: add, remove, switch on or off, drag to reorder; switching is a 10 ms crossfade
- 5.1: Skyrim's master mix always has six channels. Plugins with a 5.1 layout get all six; others run as linked stereo instances for front, center and rear (LFE passes through)
- Stereo master: for stereo devices. The 5.1 mix is folded to stereo with XAudio2 2.7's own coefficients, each plugin runs once in stereo, and the result goes back on the front pair scaled so the engine's fold-down returns it unchanged
- Latency and CPU per plugin, total latency in milliseconds and frames, output peak meter
- The chain and every plugin's state are saved and loaded with the game
- A plugin that faults while processing is switched off instead of taking the game down
- Shares the mastering voice's effect chain with other SKSE plugins that announce their effect the same way, instead of replacing it

## Requirements

- Skyrim Special Edition or Anniversary Edition (built for SE and AE)
- [SKSE](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/118580)
- Your own 64-bit VST3 effect plugins

## Usage

Press F6 in game, or open Audio Plugin Host in the SKSE Menu Framework menu, where the key and the window size can be changed. Pick a plugin, press Add, then Open window. Save writes the chain to `Data/SKSE/Plugins/AudioPluginHost.ini` and each plugin's state to `Data/SKSE/Plugins/AudioPluginHost/SlotN.state`.

If the sound stops while a plugin window has focus, set `bAlwaysActive=1` under `[General]` in `Skyrim.ini`.

## How it works

An XAPO effect on the game's XAudio2 mastering voice runs the chain on every sound the game plays. One thread owns all plugins and their windows; the audio thread only calls their process functions and never waits for it. Skyrim creates the mastering voice with six channels on every runtime, and XAudio2 mixes them to the device afterwards. Each stereo side gets 81/275 of its front channel, 229/1100 of the center, 1/11 of the LFE, 277/1100 of the rear channel on its side and 17/110 of the other rear channel; Stereo master uses these values.

While a plugin window is open, the game's DirectInput mouse is switched from exclusive to shared mode so the window receives clicks.

## Building

Visual Studio 2022 with CMake and vcpkg (`VCPKG_ROOT` set). Get the VST 3 SDK once:

```
git clone --depth 1 --branch v3.8.1_build_84 https://github.com/steinbergmedia/vst3sdk.git extern/vst3sdk
git -C extern/vst3sdk submodule update --init --depth 1 base pluginterfaces public.sdk
```

Then run `BuildRelease.bat`; CommonLibSSE-NG and spdlog are fetched into `extern/`. The package is written to `dist/`.

## License

GPL-3.0 with the exceptions in [EXCEPTIONS.md](EXCEPTIONS.md). See [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) for third-party licenses.

VST is a registered trademark of Steinberg Media Technologies GmbH. This project is not made, endorsed or supported by Steinberg.
