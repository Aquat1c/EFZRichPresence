# EfzRichPresence

Windows DLL mod that publishes Discord Rich Presence for Eternal Fighter Zero (EFZ).

This project integrates EFZ memory reads (efz_streaming style) and mode/online-state (efz-training-mode style).

## Build (Visual Studio 2022 + CMake)

1. Ensure you have CMake 3.20+ and MSVC (VS 2022) installed.
2. Configure and build the Win32 (x86) preset:

```powershell
cmake --preset vs2022-Win32
cmake --build --preset vs2022-Win32
```

Artifacts will appear under `out/build/vs2022-Win32/bin/RelWithDebInfo/efz_rich_presence.dll`.

Alternatively, configure manually:

```powershell
cmake -S . -B out/build/vs2022-Win32 -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build out/build/vs2022-Win32 --config RelWithDebInfo --target EfzRichPresence
```

## Configure Discord App ID

Create `discord_app_id.txt` next to the built DLL (or next to the injected DLL in the game folder) and place your Discord Application ID inside the file (single line).

If the file is missing or empty, the mod runs with Discord disabled and logs only.

## Injecting

Loaders can load any `.dll`; the `efz_*.dll` prefix is just a common convention. Place `efz_rich_presence.dll` (or rename as desired) next to `EFZ.exe` or in your loader’s mods folder so it’s picked up automatically, or inject it manually. On attach, it starts a background thread that polls game state every ~2 seconds and updates Discord.

## Swapping the Discord Stub

Replace `src/discord/discord_client_stub.cpp` with an implementation using Discord IPC or GameSDK.
- Keep the `DiscordClient` interface from `include/discord/discord_client.h`.
- Initialize the SDK in `init`, update/cycle callbacks in `updatePresence`, and clean up in `shutdown`.

Place any third-party libraries under `third_party/` and adjust `CMakeLists.txt` accordingly.

Notes:
- The build links the static MSVC runtime and targets 32-bit to match EFZ.
- A debug console can be enabled/disabled via the `EFZ_ENABLE_CONSOLE` option in CMake.

## License

MIT
