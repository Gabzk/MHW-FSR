# mhwharness.py: driving MHW into the world, reading the player, measuring input latency

`tools/live/mhwharness.py` starts Monster Hunter: World (Iceborne 15.23, Steam) and gets it into the game world with the player controllable, with no human input. It also reads the player's position and sends keyboard and mouse input. Its `react` command measures how long a key press takes to move the player in the simulation. It uses only Python's standard library with ctypes (ReadProcessMemory, SendInput, PrintWindow), and it never writes game memory.

```
uv run --quiet python -I tools\live\mhwharness.py <command> [args]      (--help on each command)
```

| command | what it does |
| --- | --- |
| `launch [--timeout S]` | Starts `steam://rungameid/582010` unless the game runs; waits for the `MT FRAMEWORK` window. |
| `to-game [--until STATE] [--slot N] [--timeout S]` | From any state (launches if needed) into the world, then proves control by holding W for 0.3 s (the player must move at least 5 units). Prints each state change and a stage-timing table. On failure it saves a screenshot to `%TEMP%\mhwharness\` (or `%MHWHARNESS_SHOTS%`) and exits 1. `--until title` and similar stop at that state (used for tests). |
| `state [--watch SEC]` | Prints the detected state plus pid, process age, fps (frame counter) and plugin-log signals. `--watch` prints each change. |
| `pos [--watch SEC] [-v]` | Prints the player's `x y z`. Exits 1 when there is no player. |
| `key NAME [ms]`, `hold NAME ms`, `mouse DX DY [--steps N]` | Scan-code key tap or hold, or a relative mouse move. The game window is brought to the front first. `f9` is refused (SharpPluginLoader overlay). Key names: letters, digits, `space`, `enter`, `esc`, `up`/`down`/`left`/`right`, `lshift`, `f1`..`f12`, ... (`SCAN` table). |
| `react NAME [--count N] [--csv F]` | Key press → first change of the player's position. Prints min/median/mean/max over N tries. Needs the `in-game` state (`--force` overrides). |
| `shot PATH [--scale F]` | Screenshot with PrintWindow, without stealing focus (all-black captures are retried). |
| `kill [--wait S] [--force]` | `Stop-Process -Name MonsterHunterWorld -Force`, but first waits up to `--wait` s while a system message or the autosave icon is shown. |

Exit codes: 0 ok, 1 failure or timeout, 2 usage error (unknown or reserved key), 3 game not running.

A latency session:

```
mhwharness.py to-game                         # about 42 s from a cold start
mhwharness.py react w --count 30 --csv a.csv  # alternates W and S so the hunter stays in place
mhwharness.py kill
```

The game reads input through DirectInput, which only sees it while its window is in the foreground. So every input command calls `SetForegroundWindow` (with `AttachThreadInput`) and waits 0.3 s before sending. This was tested with Notepad in front: the game came back and W moved the player. `react` checks the foreground before every try and brings the game back if needed.

## Menu flow and keys (observed 2026-10-14, user's save and bindings)

| # | screen | state (GUI unit) | harness action | typical time |
| --- | --- | --- | --- | --- |
| 1 | Capcom/middleware logos | `logos` (`uGUILogo`) | wait. Space does not shorten them (6.1 s with Space, 7.2 s without). | 7 s |
| 2 | "Loading data. Please do not exit the game." | `message` (`uGUICommonMessageWindow`, system list) | wait. **Never kill here.** | 2.3 s |
| 3 | save data load | `loading` (`uGUICommonSaveLoad`) | wait | 1.4 s |
| 4 | "Starting the game in offline mode. ... [Close]" (MHW-SteamOffline) | `offline-dialog` (`uGUINetworkErrorWindow`) | **Space** (Enter does nothing) | |
| 5 | "Checking for add-on content..." (first run: "Saving data. Please do not exit the game.") | `message` | wait. **Never kill here.** | 1.6-4 s |
| 6 | autosave explanation, "[F] Next" | `autosave-notice` (`uGUITitleSaveInfo`) | **F** (it also advances by itself after about 6 s) | 2 s |
| 7 | logo intro | `title-busy` phase 0 | wait | 2.7 s |
| 8 | "Press Any Button" | `title` (`uGUITitle`, phase 2) | **Space** | |
| 9 | main menu: Start Game, Gallery, Steam Store, Options, Credits, Support, Quit | `main-menu` (phase 4, index 0..6) | parks the mouse, Up until index 0, then **F** once the index has held for 0.4 s | 1.3 s |
| 10 | Select Save Data: slot 1 "Faenas" (HR 4), slots 2-3 "New Game" | `save-select` (`uGUITitleDataSelect`, slot index) | waits 1.5 s, Up/Down to the slot (`--slot`, default 1), then **F** once the index has held for 0.4 s | 2.4 s |
| 11 | loading screen ("Hunting Tips") | `loading` (`uGUICommonSaveLoad`) | wait | 2.9 s (warm file cache) |
| 12 | Astera, "Continuing the game in offline mode. ... [Close]" | `in-game-notice` (HUD + `uGUINetworkErrorWindow`) | **Space** | 1 s |
| 13 | Astera, HUD only | `in-game` | 1 s settle, then the W probe | |

The button guides show **F = Confirm** and **right mouse button = Back** (main menu and save select). On the save select, **R = Delete Save Data** and **N = Re-edit Character**: the harness only ever sends Up, Down and F there. The menus wrap (Up on slot 1 selects slot 3), so the harness steers by the index in memory, never by counting key presses.

**Mouse hover moves the selection.** The game draws its own cursor, and the item under it becomes selected even after keyboard input. Seen in practice: the main menu opened with "Quit" selected, and a parked cursor selected "New Game" in the save select. On the first menu screen, `to-game` therefore moves the cursor into the bottom-right corner (where nothing is selectable on these screens). It only confirms once the index has been right for 0.4 s. Both cases were tested: the cursor was placed on "Quit" and on a "New Game" slot, and `to-game` still loaded slot 1.

Timing of a cold start (`kill`, then `to-game`, 2026-10-14). Columns: seconds since `to-game` started, seconds since the process started, and time spent in the stage:

```
starting           6.1     2.3   (0.2)    Steam start: process after 4.1 s, window after 6.1 s
logos              8.9     5.1   (7.4)
message           16.7    12.9   (2.3)    Loading data
loading           19.4    15.6   (1.4)
offline-dialog    20.8    17.0   (0.9)    Space
message           21.7    17.9   (3.4)    Checking for add-on content
autosave-notice   25.1    21.3   (2.0)    F
title-busy        27.1    23.3   (2.7)    intro
title             29.8    26.0   (0.7)    Space
main-menu         31.8    28.0   (1.3)    park mouse, F
save-select       33.6    29.8   (2.4)    F
loading           36.4    32.6   (2.9)
in-game-notice    39.2    35.4   (1.0)    Space
in-game           40.2    36.4   (1.8)    settle + W probe
controllable      42.1    38.3
```

Three complete cold starts took 44.1 s, 42.1 s and 40.2 s. The slowest lost 6 s because an F pressed 0.6 s after the save select appeared was ignored; since then the harness waits 1.5 s there. Starting `to-game` from the offline dialog, the main menu (cursor on "Quit") or the save select (cursor on "New Game") also worked: 20.2 s, 12.9 s and 8.9 s to control.

## State detection (memory only, `detect()`)

The state comes from four signals, checked in this order:

1. **Active GUI units.** The harness walks `sMhUnit`'s GUI unit list (+0x13e0) and the system list (+0x15d0), reads each unit's MT class name, and treats a unit as shown when `unit+0x14` has bit 0x10.
2. **The local player**, found the same way the game's getter finds it.
3. **The HUD:** any `uGUIHud*` unit active.
4. **Title GUI fields:** `uGUITitle` phase and menu index, `uGUITitleDataSelect` slot index.

The rules, in order:

- `uGUICommonSaveLoad` active → `loading`. During a load the HUD is already active, so this check comes first.
- HUD and player present →
  - `in-game` when no other `uGUI*` unit is active. The chat, cursor, button-assign and saving-icon units do not count as other units.
  - `in-game-notice` when the only other unit is `uGUINetworkErrorWindow`.
  - `in-game-ui` otherwise. Examples: the Esc start menu shows as `uGUIStartMenu`, `uGUIStatusSideBar`, `uGUIMoney`, `uGUIPoint`, `uGUICommonTextOperation`.
- `uGUICommonMessageWindow` → `message`.
- `uGUITitleDataSelect` → `save-select`.
- `uGUITitle` with phase 2 → `title`, with phase 4 → `main-menu`, any other phase → `title-busy`.
- `uGUITitleSaveInfo` → `autosave-notice`.
- `uGUINetworkErrorWindow` → `offline-dialog`.
- `uGUILogo` → `logos`.
- Anything else → `transition`.

A player alone does not mean in-game. On the title screen the getter already returns a `uPlayer` for the title scene (position 85, 0, -55), so the HUD is required as well.

**Controllable** = state `in-game` **and** holding W for 0.3 s moves the player at least 5 units. In the tests it moved 78-100 units. `to-game` ends on this probe (at most 3 tries). The probe walks the hunter about 1 m, which is allowed. Without moving the player, `state` = `in-game` is the passive form of the check. It was correct in every observed case, but it is a GUI-based rule, so a GUI that blocks control without being a `uGUI*` unit would not be caught.

## Memory map and evidence

Binary: `MonsterHunterWorld.exe` 15.23.00 (window title `MONSTER HUNTER: WORLD(421810)`, title screen "Ver. 15.23.00"). As installed (SteamStub) its SHA-256 is `c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea`. The unpacked copy `re\MonsterHunterWorld.exe.unpacked.exe` has SHA-256 `fe50d8468881ff8cb094c5f9a599a5d6b2965950331a22ee34148c612651c521`. Image base 0x140000000, no ASLR, so the addresses below are absolute VAs. Live checks were made in 8 game processes on 2026-10-14. The player chain resolved in each one, with a different `uPlayer` address each time.

Labels: **observed** = seen in memory, disassembly or a trace; **inferred** = concluded from observations; **unknown** = not established.

| what | where | evidence | label |
| --- | --- | --- | --- |
| MT class name of an object | `[[obj]+0x20]` is `getDTI`: `48 8D 05 rel32 C3` (`lea rax,[DTI]; ret`); the name is the `char*` at `DTI+8` | Live: `sMhRender` vtable 0x1431e2e70, slot +0x20 = 0x140fba650 → DTI 0x144f87f60 "sMhRender". `sPlayer` getDTI 0x141b41ed0 → DTI 0x14500ece0. `uPlayer` getDTI 0x142038d80 → DTI 0x14502df90. The DTI name pointers are written at run time and are not in the file. There is no MSVC RTTI (2 `.?AV` strings in the whole exe). | observed |
| Singletons (static pointer → object; name checked through the DTI) | `sPlayer` 0x14500eca0, `sMhUnit` 0x1451238c8, `sMhKeyboard` 0x1451c53b0, `sMhMouse` 0x1451c53f0, `sMhRender` 0x1451c4480, `sMhGUI` 0x1451c4640, `sMhArea` 0x1451c42b8, `sMhCamera` 0x1451c4400, `sMhScene` 0x1451c4368, `sMhMain` 0x1451c4148, `sQuest` 0x14500ed30, `sFade` 0x14500ded0, `sField` 0x14500df10 | Scan of `.data` (0x143e78000, size 0x139ded4) for pointers to named objects | observed |
| Local player getter | function 0x141b42010 | Disassembly: 24 slots of 0x740 bytes from `sPlayer+0x48`. Each slot: key (u64) at +0, two i32 ids at +8/+0xc (the slot is used when either is ≥ 0), `uPlayer*` at +0x10. The function returns the player of the first used slot whose key equals `[sPlayer+0xae40]`, if `byte [player+0xc] & 0xe` is non-zero. 1146 call sites call it right after `mov rcx,[0x14500eca0]`. The harness ports this function (`player()`). | observed |
| Local player, short chain | `[[0x14500eca0]+0x58]` = slot 0's `uPlayer*` | Offline the local key is 0 and slot 0 is the player (live). `[sPlayer+0xae48]` held the same pointer (its purpose is unknown). Online sessions were not tested; there the local player may sit in another slot, which `player()` handles. | observed (offline) |
| **Player position** | `uPlayer+0x160`: x, y, z float32 (cm, y is up). Chain: `[[0x14500eca0]+0x58]+0x160` | Live: x went 5225.3 → 5501.8 and z went −1732.1 → −1793.0 while W was held for 0.8 s. The value stays put while idle. The spawn point in Astera is 5225.3, 1781.9, −1732.1 in every run. | observed |
| Rotation, scale | `uPlayer+0x170` quaternion (x, y, z, w), `+0x180` scale (1, 1, 1) | Live: a unit-length 4-vector that changes when the hunter turns and flickers by 1 ulp every frame (renormalisation). This matches the MT `uCoord` layout. The function that writes the position is unknown. | observed / layout inferred |
| Unit lists | heads in `sMhUnit` at +0x270 + n·0xf8: player list +0xa30, GUI list +0x13e0, post-filter and system-message list +0x15d0 | Live list walks. The stride is inferred from the heads at +0x270, +0x368, +0xa30, +0x13e0 and +0x15d0. | observed (heads), inferred (stride) |
| Next unit | `unit+0x30` | Live (the plugin uses the same offset) | observed |
| Unit shown | `unit+0x14` bit 0x10 | Live: set exactly on the GUIs of the visible screen (for example `uGUITitle` 0x13 on the title while hidden GUIs are 0x03/0x0b; `uGUITitleDataSelect` 0x13 only on the save select). Which code sets it is unknown. | observed |
| Title phase | `uGUITitle+0x28f0`: 0 intro, 2 "Press Any Button", 6 opening the menu, 4 main menu, 8 save select open | Live, across key presses | observed (values), inferred (meaning of 0/6/8) |
| Main menu index | `uGUITitle+0x28f8`: 0 Start Game … 6 Quit | Live: 5→4→3→2→1→0 with Up | observed |
| Save slot index | `uGUITitleDataSelect+0x2938`: 0 = slot 1 | Value scan: 2,0,1,0,1,0 with Down, Down, Up, Down, Up. The code that accesses +0x28f8 and +0x2938 sits at 0x141f00556-0x141f07b4d and 0x141f02bc8-0x141f06707, next to the getDTI functions of `uGUITitle` (0x141f00d60, vtable 0x1434870e8) and `uGUITitleDataSelect` (0x141f05220, vtable 0x143487740); that it belongs to these classes is inferred. | observed (memory) |
| Key state | `sMhKeyboard+0x138`: 256 bits indexed by virtual-key code (W 0x57 → +0x142 bit 0x80, S 0x53 → +0x142 bit 0x08). `+0x158` holds the same bits one frame later. | Live. The per-frame update 0x140485d90 (vtable 0x142f86d08, slot +0x30) passes `this+0x138` to 0x140485470 and 0x1404848a0 and then fills the action block at +0x13e0. It skips that when `+0x2c24` bit 0 is set or `+0xa00` is 0 (meaning unknown). The bit appears in the same poll as the frame counter's increment. | observed |
| Frame counter | `sMhRender+0x1f210` | From the plugin's notes; live, it increments at the same moment the key bit appears | observed |
| Autosave icon | `uGUICommonSaving` | Name only; never seen active (no autosave happened in the tests). `kill` waits while it is shown. | inferred |

How these were found, for re-doing it after a game update:

- MT names come from the DTI pattern above.
- Singletons came from scanning `.data`.
- The position came from reading `uPlayer+0x160` while holding W, then searching the singletons for pointers to that `uPlayer` (`sPlayer+0x58`, `sPlayer+0xae48`, `sMhUnit+0xa30`, ...), then reading the getter.
- Menu fields came from diffing GUI objects across key presses, plus a value scan (numpy over the 2.1 GB of private read/write memory) for the slot index.

The helper scripts are in the job scratch folder, not in the repo.

**Cheat Engine caution:** a first scan for a common value (exact dword 0) over all memory wrote 56 GB of temp files and failed with "Stream write error". Use selective scans (rare values, address ranges), or a numpy scan of the private read/write regions.

## react: method and results

For each try:

1. Wait until the player stands still for 300 ms (position ±0.001 units, rotation ±1e-4), then wait a random 0-17 ms more.
2. `t0` = QueryPerformanceCounter, then `SendInput` the key down (scan code).
3. Poll as fast as possible with ReadProcessMemory: `uPlayer+0x160..+0x17f`, the frame counter, and the key's byte in `sMhKeyboard+0x138`.
4. Record when `SendInput` returned, when the game read the key (its bit appears), and the first position change larger than 0.001 units.
5. Release the key 150 ms after `t0`. Tries alternate W and S so the hunter stays in place.

Results on 2026-10-14 in Astera:

- Settings: plugin Mode DLAA, Reflex Off, frame limit 0, the game's Frame Rate at No Limit, control panel V-Sync off.
- Frame rate: about 135 fps (7.3-7.4 ms per frame). The plugin's line read "input to GPU done 19.1-19.4 ms".
- The title screen runs at 60-240 fps, so measure in the world.

| run | tries | key press → position change | → game reads key | game read → position | SendInput call |
| --- | --- | --- | --- | --- | --- |
| A | 30 | median 13.17 ms (min 9.62, mean 13.33, max 17.03) | median 5.49 (2.25-9.41) | 7.60 (6.98-7.83) | 2.20 (1.92-2.90) |
| B (after a restart) | 20 | median 13.95 ms (10.39-17.67) | median 6.43 (2.82-9.23) | 7.57 (7.42-8.43) | 2.22 (1.87-2.70) |

Reading:

- The game picks up the key at the start of the first frame after it arrives. That wait spreads over one frame (observed).
- The position changes exactly **one frame after** that read, in every try: the read→position time equals the frame interval. Whether the movement is computed in the read frame and applied at the start of the next one, or first computed in the next one, is not established; either way it costs one frame.
- So key → simulation = (wait for the next frame start) + 1 frame, about 1.0-2.3 frames at 135 fps.
- `SendInput` itself takes about 2 ms on this machine (synchronous, probably because of low-level input hooks; the cause is unknown). A frame can start during the call, so the latency is measured from the call's start. `react` prints the call time separately.

## Failure modes and cautions

- **Saving:** `message` states ("Loading data", "Saving data", "Checking for add-on content") and the autosave icon mean the game may be writing the save. Use `kill`, which waits for them, instead of a blind Stop-Process.
- **First confirm ignored:** the save select ignores F for about a second after it appears. The harness waits 1.5 s there and repeats a press only after 4-6 s.
- **Mouse hover** selects menu items (see above). The harness parks the cursor and checks the index before confirming. If the index does not follow 4 key presses, `to-game` fails with a screenshot instead of guessing.
- **Unknown GUI in the world** (start menu, NPC dialog, a pop-up): state `in-game-ui`. `to-game` waits `--ui-timeout` (15 s), then fails. It does not press Esc on its own.
- **Foreground:** input only works with the game in front. The harness steals focus (agreed for tests). A user clicking elsewhere during `react` costs a re-focus, which `react` handles between tries.
- **PrintWindow** sometimes returns an all-black frame; `shot` retries up to 4 times. Loading screens are legitimately dark.
- **Untested:** an online session (local player in another slot: `player()` follows the game's getter, but this was not exercised); a save that starts somewhere other than Astera (the detector does not depend on the place); a cold file cache (loads were about 3 s, the files were cached); Steam not running or not logged in (launch would hit its timeout); in-game autosave.
- **Other keys:** Enter does not close the offline dialog. Space works for the dialogs and "Press Any Button". F is the menu confirm, as the button guide shows. `--confirm` and `--dialog-key` exist in case the bindings change.
- **Window:** the game window has class `MT FRAMEWORK`. A loader console (`ConsoleWindowClass`) also reports the game's pid; the harness ignores it. The window is 2560x1421 for its first seconds, then 2560x1440.
