# EfzRichPresence

Windows DLL mod that publishes Discord Rich Presence for Eternal Fighter Zero (EFZ).

It reads EFZ state from process memory (efz_streaming-style offsets) and integrates EfzRevival for online status, nicknames, current player side, and set scores. Presence formatting mirrors typical fighting-game semantics and SokuMods’ DiscordIntegration behavior.

## Features

- Offline and online presence with clear details/state.
- Characters and nicknames:
	- Large image = your character; Small image = opponent character.
	- If opponent character can’t be read in netplay, state becomes “Against the <nickname> …” and the small icon is omitted.
	- The in-game character “Unknown” (read as mizuka/mizukab) is treated as a real character and uses its own icon.
- Scores and orientation:
	- Scores come from EfzRevival and are shown from your perspective as “(you-them)”.
	- Score is always displayed online, starting at 0-0.
- Modes and menus:
	- Replay/Auto-Replay: “Watching replay” and “P1 vs P2”.
	- Offline matches: “Playing in <Mode>”, state “As <P1>”.
	- Main Menu shows a generic EFZ icon.
	- Online pre-pick (before your character is chosen) shows a generic EFZ logo as the large image.
- Assets (Discord Application rich presence images):
	- Main menu: `efz_icon` (large).
	- Online pre-pick: `210px-efzlogo` (large).
	- Character icons (small+large):
		- `90px-efz_akane_icon`, `90px-efz_akiko_icon`, `90px-efz_ayu_icon`, `90px-efz_doppel_icon`, `90px-efz_ikumi_icon`,
			`90px-efz_kanna_icon_-_copy`, `90px-efz_kano_icon`, `90px-efz_kaori_icon`, `90px-efz_mai_icon`, `90px-efz_makoto_icon`,
			`90px-efz_mayu_icon`, `90px-efz_minagi_icon`, `90px-efz_mio_icon`, `90px-efz_misaki_icon`, `90px-efz_mishio_icon`,
			`90px-efz_misuzu_icon`, `90px-efz_mizuka_icon`, `90px-efz_nayuki_icon`, `90px-efz_neyuki_icon`, `90px-efz_rumi_icon`,
			`90px-efz_sayuri_icon`, `90px-efz_shiori_icon`, `90px-efz_unknown_icon`.
	- Ensure these asset keys exist in your Discord application; otherwise, images won’t render.

## Build (Visual Studio 2022 + CMake)

1. Ensure you have CMake 3.20+ and MSVC (VS 2022) installed.
2. Configure and build the Win32 (x86) preset:

```powershell
cmake --preset vs2022-Win32
cmake --build --preset vs2022-Win32
```

Artifacts will appear under:

```
out/build/vs2022-Win32/bin/RelWithDebInfo/EfzRichPresence.dll
```

Alternatively, configure manually:

```powershell
cmake -S . -B out/build/vs2022-Win32 -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build out/build/vs2022-Win32 --config RelWithDebInfo --target EfzRichPresence
```

## Configure Discord App ID

Create a `discord_app_id.txt` file next to the built DLL (or next to the injected DLL in the game folder). Put your Discord Application ID inside as a single line. The Rich Presence “header” name is taken from the application’s name in the Discord Developer Portal.

If the file is missing or empty, Discord updates are disabled and the mod will just log.

## Injecting

Place `EfzRichPresence.dll` alongside `EFZ.exe` (or your loader’s mods folder) so it’s picked up automatically, or inject it manually. On attach, the DLL starts a background worker that periodically polls state and updates Discord.

## Runtime behavior (details/state)

- Offline
	- Main Menu: details “Main Menu”; large image `efz_icon`.
	- Replay/Auto-Replay: details “Watching replay”; state “P1 vs P2”; large = P1 character; small = P2 character (tooltip: “Against <P2>”).
	- Matches: details “Playing in <Mode>”; state “As <P1>”; large = your character; small = opponent.
- Online (EfzRevival)
	- Netplay: details “Playing online match (nickname)” if your nickname is known.
	- Tournament: details “Playing tournament match (nickname)”.
	- Spectating: details “Watching online match”.
	- State: “Against <opponentChar> (oppNick) (you-them)”. If the opponent character can’t be read, shows “Against the <nickname> (you-them)” and omits the small icon.
	- Large image: your character; before you pick a character, large image is `210px-efzlogo`.

## Environment toggles

- `EFZDA_DISABLE_REVIVAL=1` — disable EfzRevival reads (treat as offline only), useful for troubleshooting.
- `EFZDA_ENABLE_CONSOLE=1` — optional debug console window and additional logging.

## Notes

- Uses safe ReadProcessMemory against EFZ.exe and EfzRevival.dll within the same process.
- Character name normalization includes special cases (Nayuki variants, Doppel Nanase, Unknown character mapping from mizuka/mizukab, etc.).
- If you upload separate large artwork per character, the mapping can be updated to use those keys.

## Screenshots (placeholders)

Add your screenshots under `docs/screenshots/` and they’ll render below:

![Main menu](docs/screenshots/main-menu.png)
![Offline match](docs/screenshots/offline-match.png)
![Online pre-pick](docs/screenshots/online-pre-pick.png)
![Online match (scores, nicknames)](docs/screenshots/online-match.png)
![Spectating](docs/screenshots/spectating.png)

## License

MIT
