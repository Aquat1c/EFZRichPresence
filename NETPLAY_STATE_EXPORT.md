# EFZ Netplay State Export — Inter-Mod Interface

## Overview

The **EFZ Netplay State Export** provides a stable, read-only interface for
other mods (e.g. [EFZRichPresence](https://github.com/Aquat1c/EFZRichPresence))
to consume the netplay session state from `efz_netplay_mod.dll` without
reverse-engineering memory layouts or depending on internal globals.

The export mechanism publishes a fixed-layout C struct (`EFZNetplayState`)
containing:

| Field | Type | Description |
|---|---|---|
| `sessionMode` | `int32_t` | `EFZNetplaySessionMode` — whether we're hosting, joining, spectating, in a tournament, or idle |
| `sessionPhase` | `int32_t` | `EFZNetplaySessionPhase` — connection lifecycle (idle → connecting → delay setup → connected → ended) |
| `localSide` | `int32_t` | `0` = P1 (host), `1` = P2 (joiner), `-1` = unknown / spectator |
| `p1Wins` | `int32_t` | P1 win count within current set (absolute, not perspective-adjusted). Sourced from Revival session object; backed by high-water-mark latch. Works for online, spectator, and tournament sessions. |
| `p2Wins` | `int32_t` | P2 win count within current set |
| `matchCounter` | `int32_t` | Current match index within the set |
| `localNickname` | `char[32]` | Our chosen nickname (from `EfzRevival.ini`) |
| `p1Name` | `char[64]` | P1 nickname (from Revival session object — online and spectator sessions) |
| `p2Name` | `char[64]` | P2 nickname (from Revival session object — online and spectator sessions) |
| `pingMs` | `int32_t` | Round-trip ping in ms (`-1` = unavailable) |
| `rollbackFrames` | `int32_t` | Agreed input-delay frames (`-1` = not set) |
| `inNetplayMenu` | `uint8_t` | Non-zero when the netplay menu overlay is open *(v2)* |
| `netplayMenuScreen` | `uint8_t` | `EFZNetplayMenuScreen` — which sub-menu page is active *(v2)* |
| `revivalVersion` | `char[16]` | EfzRevival.dll version tag (e.g. `"1.02e"`, `"1.02i"`) *(v2)* |
| `inNetplayCharacterSelect` | `uint8_t` | Non-zero during online character-select screen *(v3)* |
| `inNetplayMatch` | `uint8_t` | Non-zero during active online match / round *(v3)* |
| `capabilityFlags` | `uint32_t` | Bitmask of `EFZ_CAP_*` — which field groups are populated *(v4)* |
| `stateSeq` | `uint32_t` | Monotonically increasing per-tick sequence number *(v4)* |
| `sessionId` | `uint32_t` | Incremented each new session (0 before first) *(v4)* |
| `setId` | `uint32_t` | Incremented each new set / score reset (0 before first) *(v4)* |
| `activityPhase` | `uint8_t` | `EFZNetplayActivityPhase` — current high-level activity *(v4)* |
| `endReason` | `uint8_t` | `EFZNetplayEndReason` — why the last session ended *(v4)* |
| `p1CharId` | `uint8_t` | P1 selected character ID (`0xFF` = unknown) *(v4)* |
| `p2CharId` | `uint8_t` | P2 selected character ID (`0xFF` = unknown) *(v4)* |
| `p1Locked` | `uint8_t` | Non-zero if P1 has locked their pick *(v4)* |
| `p2Locked` | `uint8_t` | Non-zero if P2 has locked their pick *(v4)* |
| `localCursorCharId` | `uint8_t` | Character under local cursor (`0xFF` = unavailable) *(v4)* |
| `stageId` | `uint8_t` | Stage / background ID (`0xFF` = unknown) *(v4)* |
| `roundIndex` | `uint8_t` | Current round within match, 0-based (`0xFF` = unknown) *(v4)* |
| `isRoundActive` | `uint8_t` | Non-zero when a round is in progress *(v4)* |
| `roundTimerFrames` | `uint16_t` | Round timer in frames (`0xFFFF` = unknown) *(v4)* |

---

## How To Consume (for mod authors)

Both DLLs live inside the same `EFZ.exe` process.  Two access methods are
provided — use whichever is more convenient.

### Method 1: Named Shared Memory (recommended)

```c
#include "efz_netplay_state.h"          // copy this header into your project

HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "EFZNetplay_State");
if (hMap) {
    const EFZNetplayState* state = (const EFZNetplayState*)
        MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(EFZNetplayState));
    if (state && state->magic == EFZ_NETPLAY_STATE_MAGIC) {
        // state->sessionMode, state->p1Name, etc. are ready to use
    }
    // Keep mapped for the lifetime of your mod; unmap on shutdown.
}
```

### Method 2: DLL Export (`EFZNetplay_GetState`)

```c
#include "efz_netplay_state.h"

typedef const EFZNetplayState* (__cdecl *GetStateFn)(void);

HMODULE mod = GetModuleHandleA("efz_netplay_mod");
if (mod) {
    GetStateFn fn = (GetStateFn)GetProcAddress(mod, "EFZNetplay_GetState");
    if (fn) {
        const EFZNetplayState* state = fn();
        if (state && state->magic == EFZ_NETPLAY_STATE_MAGIC) {
            // use state->sessionMode, etc.
        }
    }
}
```

### Validation

Always check before reading:

1. **magic** must equal `EFZ_NETPLAY_STATE_MAGIC` (`0x4E5A4645`).
2. **version** — your code should handle the version it was compiled against;
   future versions only _add_ fields (never remove/reorder).
3. **structSize** — use this for forward compatibility; only read up to
   `min(sizeof(YourStruct), state->structSize)` bytes.

---

## Enum Reference

### `EFZNetplaySessionMode`

| Value | Name | Meaning |
|---|---|---|
| 0 | `EFZ_SESSION_NONE` | No active netplay session (offline / idle) |
| 1 | `EFZ_SESSION_HOSTING` | Hosting an online match (P1 / host side) |
| 2 | `EFZ_SESSION_JOINING` | Joined an online match (P2 / client side) |
| 3 | `EFZ_SESSION_SPECTATING` | Spectating an online match |
| 4 | `EFZ_SESSION_TOURNAMENT` | Tournament mode session |

### `EFZNetplaySessionPhase`

| Value | Name | Meaning |
|---|---|---|
| 0 | `EFZ_PHASE_IDLE` | No session |
| 1 | `EFZ_PHASE_CONNECTING` | Waiting for peer / connecting |
| 2 | `EFZ_PHASE_DELAY_SETUP` | Input-delay negotiation prompt |
| 3 | `EFZ_PHASE_CONNECTED` | Fully connected, match active |
| 4 | `EFZ_PHASE_FAILED` | Connection attempt failed |
| 5 | `EFZ_PHASE_SESSION_ENDED` | Session ended gracefully |

### `EFZNetplayMenuScreen`

Only meaningful when `inNetplayMenu` is non-zero.

| Value | Name | Meaning |
|---|---|---|
| 0 | `EFZ_MENU_MAIN` | Top-level: Host / Join / Nickname / Lobby / Back |
| 1 | `EFZ_MENU_HOST` | Host settings sub-menu |
| 2 | `EFZ_MENU_JOIN` | Join settings sub-menu |
| 3 | `EFZ_MENU_NICKNAME` | Nickname edit sub-menu |
| 4 | `EFZ_MENU_LOBBY` | Lobby browser |

### `EFZNetplayActivityPhase` *(v4)*

High-level description of the local player's current netplay activity.
More granular than the v3 boolean flags — distinguishes connecting, delay
setup, loading, and idle as separate states.

| Value | Name | Meaning |
|---|---|---|
| 0 | `EFZ_ACTIVITY_IDLE` | Not in any netplay flow |
| 1 | `EFZ_ACTIVITY_MENU` | Netplay menu overlay is open |
| 2 | `EFZ_ACTIVITY_CONNECTING` | Waiting for peer / connecting |
| 3 | `EFZ_ACTIVITY_DELAY_SETUP` | Input-delay negotiation prompt |
| 4 | `EFZ_ACTIVITY_CHAR_SELECT` | Online character-select screen |
| 5 | `EFZ_ACTIVITY_LOADING` | Post-charselect loading screen |
| 6 | `EFZ_ACTIVITY_MATCH` | Active online match / round |
| 7 | `EFZ_ACTIVITY_RESULTS` | Post-match results screen (reserved) |

### `EFZNetplayEndReason` *(v4)*

Why the most recent session ended.  The value latches until the next session
starts (phase transitions from Idle to Connecting).

| Value | Name | Meaning |
|---|---|---|
| 0 | `EFZ_END_NONE` | No end event (session active or never started) |
| 1 | `EFZ_END_GRACEFUL` | Session ended normally |
| 2 | `EFZ_END_DISCONNECT` | Peer disconnected / connection lost |
| 3 | `EFZ_END_CANCELLED` | Local player cancelled the session |
| 4 | `EFZ_END_CONNECT_FAILED` | Connection attempt failed (timeout / refused) |
| 5 | `EFZ_END_PEER_PROCESS_DIED` | Revival peer process exited unexpectedly |
| 255 | `EFZ_END_UNKNOWN` | Unknown / unclassified end reason |

### Capability Bits (`capabilityFlags`) *(v4)*

Each bit indicates a field group is actively populated on this tick.
Consumers should check the relevant bit before interpreting the corresponding
fields — a zero bit means the data source is unavailable (e.g. the charselect
screen object is not allocated, or the game system pointer is null).

| Bit | Constant | Fields guarded |
|---|---|---|
| 0 | `EFZ_CAP_SESSION` | `sessionMode`, `sessionPhase`, `localSide` |
| 1 | `EFZ_CAP_SCORES` | `p1Wins`, `p2Wins`, `matchCounter` |
| 2 | `EFZ_CAP_NICKNAMES` | `localNickname`, `p1Name`, `p2Name` |
| 3 | `EFZ_CAP_NETWORK` | `pingMs`, `rollbackFrames` |
| 4 | `EFZ_CAP_MENU` | `inNetplayMenu`, `netplayMenuScreen` |
| 5 | `EFZ_CAP_REVIVAL` | `revivalVersion` |
| 6 | `EFZ_CAP_GAME_FLOW` | `inNetplayCharacterSelect`, `inNetplayMatch` |
| 7 | `EFZ_CAP_ACTIVITY` | `activityPhase`, `endReason` |
| 8 | `EFZ_CAP_CHAR_SELECT` | `p1CharId`, `p2CharId`, `p1Locked`, `p2Locked`, `localCursorCharId` |
| 9 | `EFZ_CAP_MATCH_CONTEXT` | `stageId`, `roundIndex`, `roundTimerFrames`, `isRoundActive` |

---

## Internal Architecture

### Data flow

```
  g_netplayRole ──────────────┐
  g_localRoleFlag ────────────┤
  NetbridgeStatus         ├──▶  state_export::Update()  ──▶  EFZNetplayState
  Revival session memory   ┤      (called every frame)         ├── shared memory
  Game memory (win counts) ┤                                   └── DLL export ptr
  Game memory (charselect) ┤
  g_netplayMenuState      ┤
  g_activeRevival ─────────┘
```

Revival session memory is refreshed every frame by `RefreshRuntimeStatus()`
(called from both `session_bridge::Tick()` and `TickExportOnly()`), which
reads the session pointer, validates it, and populates `NetbridgeStatus`
fields including wins, nicknames, ping, delay, and active player.

For **spectator sessions**, `RefreshRuntimeStatus()` reads wins and
nicknames from spectator-specific offsets (+128/+132 for wins, +154/+282
for raw wchar_t[64] names) rather than the online session offsets.

### Source files

| File | Role |
|---|---|
| `include/efz_netplay_state.h` | **Public header** — struct & enum definitions. Copy this to consumer projects. |
| `include/netplay/bridge/netplay_state_export.h` | Internal header — `Initialize()` / `Shutdown()` / `Update()` declarations |
| `src/netplay/bridge/netplay_state_export.cpp` | Implementation — shared memory lifecycle, score reading, state population |
| `src/dllmain.cpp` | DLL export: `EFZNetplay_GetState()` |
| `src/netplay/bridge/session_bridge.cpp` | Integration: calls `state_export::Initialize()`, `Update()`, `Shutdown()` |

### Lifecycle

1. **DLL_PROCESS_ATTACH** → `session_bridge::Initialize()` → `state_export::Initialize()`
   - Creates named shared memory `"EFZNetplay_State"`
   - Writes initial idle state
2. **Every frame** → `session_bridge::Tick()` or `TickExportOnly()` → `state_export::Update(g_status)`
   - `Tick()` runs from title/menu hooks; `TickExportOnly()` runs from the
     per-frame hook during match/loading/charselect and first calls
     `RefreshRuntimeStatus()` to re-read volatile session fields (wins,
     nicknames, ping, delay, activePlayer) from Revival memory
   - Reads `g_netplayRole`, `g_localRoleFlag` from the takeover module
   - Reads win counts from Revival session memory (primary) and EFZ game
     memory (fallback), merged via high-water-mark latch
   - Copies nicknames and network metrics from `NetbridgeStatus`
   - Reads `g_netplayMenuState.active` and `.menuId` for menu tracking
   - Reads `g_activeRevival->versionTag` for the EfzRevival version
   - Writes the complete struct to shared memory atomically
3. **DLL_PROCESS_DETACH** → `session_bridge::Shutdown()` → `state_export::Shutdown()`
   - Clears magic field (consumers detect stale data)
   - Unmaps and closes the shared memory handle

### Session mode resolution

The `sessionMode` field is derived from three sources, checked in priority order:

1. **`g_netplayRole`** (set after init handshake):

| `g_netplayRole` | → `sessionMode` |
|---|---|
| `kNetplayRoleHost` (1) | `EFZ_SESSION_HOSTING` |
| `kNetplayRoleClient` (2) | `EFZ_SESSION_JOINING` |
| `kNetplayRoleSpectator` (3) | `EFZ_SESSION_SPECTATING` |

2. **`status.role`** (NetbridgeRole — set at `StartSession` time, available during connecting phase before `g_netplayRole` is resolved):

| `NetbridgeRole` | → `sessionMode` |
|---|---|
| `Host` (0) | `EFZ_SESSION_HOSTING` |
| `Join` (1) | `EFZ_SESSION_JOINING` |
| `Spectate` (2) | `EFZ_SESSION_SPECTATING` |
| `JoinSpectate` (3) | `EFZ_SESSION_JOINING` |

3. **`g_localRoleFlag`** (fallback for tournament):

| `g_localRoleFlag` | → `sessionMode` |
|---|---|
| `kLocalRoleTournament` (3) | `EFZ_SESSION_TOURNAMENT` |
| anything else | `EFZ_SESSION_NONE` |

This means `sessionMode` correctly reflects **HOSTING** or **JOINING** even
during the connecting phase, before the init handshake resolves the
active-player field.

### Score reading

Win counts (`p1Wins`, `p2Wins`) are sourced from multiple data paths and
merged to ensure reliable values across all screen transitions:

**Primary source: EfzRevival session object**

The Revival session object maintains authoritative win counters that persist
across screen transitions (charselect → loading → match → charselect).
Offsets depend on the session type:

| Session type | P1 wins offset | P2 wins offset | Notes |
|---|---|---|---|
| Online (mode 0) | +1224 (1.02e–h) / +1232 (1.02i) | +1228 (1.02e–h) / +1236 (1.02i) | Version-specific offsets from `RevivalAddressProfile` |
| Spectator (mode 1) | **+128** | **+132** | Same across all Revival versions (1.02e–i) |

These are read every frame via `RefreshRuntimeStatus()` (called from both
`Tick()` and `TickExportOnly()`) and exposed through
`NetbridgeStatus.sessionP1Wins` / `.sessionP2Wins`.

**Secondary source: EFZ game-system struct**

As a fallback (e.g. before the Revival session is fully initialised), wins
are read from the EFZ.exe game-system struct:

- Screen object table at VA `0x00790110`
- Active screen index byte at VA `0x00790148`
- Game system pointer at screen object + `0x1C`
- P1 wins at game system + `4920` (uint32)
- P2 wins at game system + `4924` (uint32)
- Match counter at game system + `4952` (uint8)

All reads are SEH-guarded.

**High-water-mark latch**

During screen transitions the session pointer may briefly fail validation
(the pointer is re-validated each frame via `IsLikelySessionPointer`), and
the game-system struct resets when screen objects are destroyed.  To prevent
wins from flickering to 0 during these gaps, a high-water-mark latch
(`g_latchedP1Wins`, `g_latchedP2Wins`) records the highest win total seen
within the current session.  If the live value drops below the latch (due
to a transient read failure), the latched value is used instead.  The latch
resets when a new session begins (phase Idle → Connecting).

**Merge logic** (in `state_export::Update()`):

1. Read live wins from Revival session (`sessionP1Wins` / `sessionP2Wins`).
2. If both are 0 but game-system wins are non-zero, use game-system values
   as a transient bridge.
3. Update the high-water-mark latch if the live total exceeds it.
4. Export whichever has the higher total: live reads or latch.

### Netplay menu tracking

The EFZ game engine does not expose a "netplay menu" screen — from its
perspective the player is still on the title screen.  The netplay mod hooks
the title screen and renders its own overlay.

The `inNetplayMenu` flag reads `g_netplayMenuState.active` (set to `true` by
`EnterNetplayMenu()`, cleared by `LeaveNetplayMenu()` and match-handoff
functions).  The `netplayMenuScreen` field maps directly from the internal
`NetplayMenuId` enum:

| Internal `NetplayMenuId` | Exported `EFZNetplayMenuScreen` | Meaning |
|---|---|---|
| `Main` (0) | `EFZ_MENU_MAIN` (0) | Top-level menu |
| `Host` (1) | `EFZ_MENU_HOST` (1) | Hosting settings |
| `Join` (2) | `EFZ_MENU_JOIN` (2) | Join settings |
| `Nickname` (3) | `EFZ_MENU_NICKNAME` (3) | Nickname editor |
| `Lobby` (4) | `EFZ_MENU_LOBBY` (4) | Lobby browser |

Note: A host or join session can be **connecting** while the menu is still
open (`inNetplayMenu = 1` and `sessionPhase = EFZ_PHASE_CONNECTING`).
The menu closes only when a successful handoff to a match occurs.

### Game-flow flags (v3)

Three mutually-exclusive `uint8_t` flags track the online session lifecycle:

| Flag | Value=1 when | EFZ screen index |
|---|---|---|
| `inNetplayMenu` | Netplay menu overlay is open | 0 (title) |
| `inNetplayCharacterSelect` | Online character-select or loading screen | 1 or 2 |
| `inNetplayMatch` | Active online match / round | 3 (battle) |

All three are zero when offline, idle, or during non-netplay screens.

**Transition flow (normal match):**

```
Menu(1,0,0) → CharSelect(0,1,0) → Match(0,0,1) → Menu(1,0,0)
```

**Transition flow (disconnect during match):**

```
Match(0,0,1) → Menu(1,0,0)   [immediate, skips charselect]
```

**Implementation detail:**  `inNetplayCharacterSelect` and `inNetplayMatch`
are derived from the combination of:
- `g_returnToNetplayAfterMatch` — `true` after handoff until the game
  returns to the title screen (set by `HandoffConnectedSessionToVsHumanState`
  and `HandoffSpectateSession`, cleared on menu re-entry or disconnect)
- The EFZ screen index at `0x00790148` — `1` = charselect, `2` = loading,
  `3` = battle

This ensures the flags accurately track the current visual state without
requiring hooks in the charselect or battle code paths.

### Activity phase and end reason (v4)

The `activityPhase` field is a more granular replacement for the three
boolean flow flags.  It's derived from the same inputs plus the session
phase:

| Priority | Condition | `activityPhase` |
|---|---|---|
| 1 | `inNetplayMenu` | `EFZ_ACTIVITY_MENU` (1) |
| 2 | `sessionPhase == Connecting` | `EFZ_ACTIVITY_CONNECTING` (2) |
| 3 | `sessionPhase == DelaySetup` | `EFZ_ACTIVITY_DELAY_SETUP` (3) |
| 4 | `inNetplayCharacterSelect` + screenIdx==2 | `EFZ_ACTIVITY_LOADING` (5) |
| 5 | `inNetplayCharacterSelect` | `EFZ_ACTIVITY_CHAR_SELECT` (4) |
| 6 | `inNetplayMatch` | `EFZ_ACTIVITY_MATCH` (6) |
| 7 | (default) | `EFZ_ACTIVITY_IDLE` (0) |

The v3 boolean flags remain populated for backward compatibility — consumers
compiled against v3 continue to work.  The relationship is:

| `activityPhase` | `inNetplayMenu` | `inNetplayCharacterSelect` | `inNetplayMatch` |
|---|---|---|---|
| IDLE | 0 | 0 | 0 |
| MENU | 1 | 0 | 0 |
| CONNECTING | 0 | 0 | 0 |
| DELAY_SETUP | 0 | 0 | 0 |
| CHAR_SELECT | 0 | 1 | 0 |
| LOADING | 0 | 1 | 0 |
| MATCH | 0 | 0 | 1 |

`endReason` is set when the session phase transitions into `Failed` or
`SessionEnded`.  It latches (persists) until the next session starts.
The value is derived from `NetbridgeStatus.errorMsg`:

| Error message contains | `endReason` |
|---|---|
| `"process ended"` | `EFZ_END_PEER_PROCESS_DIED` |
| `"start canceled"` | `EFZ_END_CANCELLED` |
| `"peer disconnected"` | `EFZ_END_DISCONNECT` |
| (other, phase=Failed) | `EFZ_END_CONNECT_FAILED` |
| (other, phase=SessionEnded) | `EFZ_END_GRACEFUL` |

### Sequence and identity counters (v4)

- **`stateSeq`** — Incremented every call to `Update()`.  Consumers can
  compare two reads to detect whether the state has been refreshed.
- **`sessionId`** — Incremented when `sessionPhase` transitions from
  `Idle` to `Connecting`.  Zero before the first session.
- **`setId`** — Incremented when win counts reset from non-zero to 0-0.
  Zero before the first set.  Useful for detecting rematch / new set.

### Character-select context (v4)

During `EFZ_ACTIVITY_CHAR_SELECT` / `EFZ_ACTIVITY_LOADING`, the following
fields are populated by reading the charselect screen object (screen index 1):

| Field | Source offset | Meaning |
|---|---|---|
| `p1CharId` | +1340 | Currently highlighted P1 character |
| `p2CharId` | +1341 | Currently highlighted P2 character |
| `p1Locked` | +1344 (timer ≠ 0) | P1 has confirmed their pick |
| `p2Locked` | +1346 (timer ≠ 0) | P2 has confirmed their pick |
| `localCursorCharId` | gridMap[row*3+col] | Character under the local player's grid cursor |

The lock state is inferred from the per-player countdown timer: when a player
confirms their character, EFZ starts a short timer animation.  A non-zero
timer indicates locked.

`localCursorCharId` is read from the grid map using the local player's cursor
row/col (determined by `localSide`: 0=P1, 1=P2).

### Match context (v4)

During `EFZ_ACTIVITY_MATCH`, the following fields are populated from the
game-system struct:

| Field | Source | Meaning |
|---|---|---|
| `roundIndex` | game system + 4952 | Current round within the match (0-based) |
| `isRoundActive` | 1 if game system is readable | A round is in progress |
| `stageId` | 0xFF (not yet sourced) | Reserved for stage ID |
| `roundTimerFrames` | 0xFFFF (not yet sourced) | Reserved for round timer |

`stageId` and `roundTimerFrames` are reserved — their game-memory offsets
have not yet been identified, so they always report sentinel values.

### EfzRevival version detection

The `revivalVersion` field contains the version tag from the detected
`RevivalAddressProfile`.  Detection uses the PE `TimeDateStamp` from the
loaded `EfzRevival.dll` COFF header.

| Version | PE Timestamp | Supported by Netplay Mod | Supported by RichPresence |
|---|---|---|---|
| 1.02e | `0x5EA876B0` | ✅ Yes | ✅ Yes |
| 1.02f | `0x5F8C58A3` | ✅ Yes | ❌ No |
| 1.02g | `0x6240CE73` | ✅ Yes | ❌ No |
| 1.02h | `0x62929371` | ✅ Yes | ✅ Yes |
| 1.02i | `0x63BF27EA` | ✅ Yes | ✅ Yes |

The netplay mod supports all five versions.  EFZRichPresence currently only
has offset tables for 1.02e, 1.02h, and 1.02i.  By consuming the state
export instead of raw memory reads, the Rich Presence mod gets version-
independent data for all versions the netplay mod supports.

---

## Relationship to EFZRichPresence

The [EFZRichPresence](https://github.com/Aquat1c/EFZRichPresence) mod
currently reads EfzRevival.dll memory directly to determine online state.
Its `OnlineState` enum only distinguishes:

| Value | Name |
|---|---|
| 0 | Netplay (host **and** join combined) |
| 1 | Spectating |
| 2 | Offline |
| 3 | Tournament |

The new `EFZNetplayState.sessionMode` splits `Netplay` into `HOSTING` vs
`JOINING`, giving the Rich Presence mod the information it needs to display
distinct statuses like "Hosting online match" vs "Playing online match".

Additionally, the `inNetplayMenu` flag lets the Rich Presence mod show
"In Netplay Menu" as a Discord status — something that was previously
impossible since the game engine only sees the title screen.

The `revivalVersion` field provides the loaded EfzRevival version.
This means the Rich Presence mod can properly support 1.02f and 1.02g
(which it currently cannot) by reading state from the export instead of
using version-specific memory offsets.

### Migration path for EFZRichPresence

1. Copy `include/efz_netplay_state.h` into the Rich Presence project.
2. In the state provider, attempt to open `"EFZNetplay_State"` shared memory.
3. If available and valid (`magic == 0x4E5A4645`):
   - Use `sessionMode` to distinguish hosting / joining / spectating.
   - Use `inNetplayMenu` to detect the netplay menu overlay (show "In Netplay Menu").
   - Use `p1Name` / `p2Name` for nicknames (already sanitised).
   - Use `p1Wins` / `p2Wins` for scores.
   - Use `localSide` to determine perspective (0=P1, 1=P2).
   - Use `revivalVersion` to know which EfzRevival is loaded (works for
     all versions including 1.02f and 1.02g that RichPresence can't detect).
4. Fall back to the existing EfzRevival.dll memory reads if the shared memory
   is not present (netplay mod not loaded).

---

## Version History

| Version | Changes |
|---|---|
| 1 | Initial release — session mode, phase, side, scores, nicknames, ping, delay |
| 2 | Added `inNetplayMenu`, `netplayMenuScreen`, `revivalVersion`. Improved `sessionMode` resolution during connecting phase (uses intended role from `StartSession` before handshake completes). |
| 3 | Added `inNetplayCharacterSelect`, `inNetplayMatch`. These three game-flow flags (`inNetplayMenu`, `inNetplayCharacterSelect`, `inNetplayMatch`) are mutually exclusive and track the full online session lifecycle: menu → charselect → match → menu. Transition logging added for validation. |
| 4 | Added capability bits (`capabilityFlags`), sequence/identity counters (`stateSeq`, `sessionId`, `setId`), granular activity phase (`activityPhase`), session end reason (`endReason`), character-select context (`p1CharId`, `p2CharId`, `p1Locked`, `p2Locked`, `localCursorCharId`), and match context (`stageId`, `roundIndex`, `isRoundActive`, `roundTimerFrames`). v3 flags remain populated for backward compatibility. |
| 4.1 | **Win counter fix**: `TickExportOnly()` now calls `RefreshRuntimeStatus()` every frame so wins (and other volatile session fields) are re-read from Revival memory during match/charselect — previously they were only read during title-screen ticks, causing 0-0 display at character select. Added high-water-mark latch (`g_latchedP1Wins` / `g_latchedP2Wins`) as safety net against transient read gaps during screen transitions. |
| 4.2 | **Spectator session support**: Wins and nicknames are now read from spectator session objects (mode 1) using spectator-specific offsets: P1 wins at +128, P2 wins at +132, P1 name at +154, P2 name at +282. These offsets are consistent across all Revival versions (1.02e–i). Previously, spectator sessions exported empty nicknames and 0-0 wins because the online session offsets are out of bounds for the smaller 1088-byte spectator object. |
