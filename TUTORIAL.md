# osMW Maps++ — Player Guide

**osMW Maps++** is a companion app for [MythWar Online](https://github.com/osMW) when playing on the **osMW** private server. It replaces the in-game minimap with detailed custom maps and adds quality-of-life tools for multi-boxing (running several game clients at once).

Maps++ runs in the background and appears in your system tray as **osMW Maps++**.

---

## What Maps++ Does

| Feature | Description |
|--------|-------------|
| **Custom minimap** | Press **Tab** to overlay a high-resolution map on top of the game window. |
| **Live position marker** | A dot on the map tracks your character as you move. |
| **Game launcher** | Launch new game clients from the tray menu or a hotkey, centered on your chosen monitor. |
| **Multi-box helpers** | Optional addons arrange windows, toggle chat, open inventory, and send battle commands to all fighting characters. |

Maps++ reads information from the running game client (`main.exe`). It does not modify game files and works alongside the normal client.

---

## Getting Started

### 1. Install and run

1. Download the release that matches how you play:
   - **Full** — minimap, launcher, window layout, chat, inventory, battle helpers, map POIs, and the multi-client roster and party markers.
   - **Lite** — minimap, map POIs, launcher, and window layout only.
   - **Battle** — minimap, launcher, chat, battle helpers, roster and party markers (no inventory or window layout).
2. Extract the folder somewhere convenient (Desktop, a games folder, etc.).
3. Run **`mapsplusplus.exe`**.

The folder must include:

- `mapsplusplus.exe`
- `marker.png` (your position dot on the map)
- `maps\` (custom map images, one `.jpg` per supported zone)

On first run, Maps++ may ask you to locate **`main.exe`** (the game client). You can also set this later from **Tray → Settings… → Launcher → Browse…** (**Ctrl+Alt+,**).

### 2. Optional: place the game next to Maps++

If `main.exe` sits in the same folder as Maps++, the app finds it automatically and you will not be prompted.

### 3. Launch the game

- From the tray icon: right-click → **Launch Game**
- Or press **Ctrl+Alt+L** (primary monitor) / **Ctrl+Alt+K** (secondary monitor)

If **Launch on startup** is enabled in `config.ini`, Maps++ opens a game client for you when the app starts.

---

## Using the Custom Minimap

### Open and close

| Action | How |
|--------|-----|
| **Open / toggle minimap** | **Tab** (while the game window is focused) |
| **Close minimap** | **Tab** again, **Right-click** on the overlay, or switch away from the game |

The overlay is centered on the game window and stays on top. Clicks on the minimap do **not** steal focus from the game, so you can keep playing normally.

### Sizing and placing the minimap

**Settings → Minimap** controls how the overlay looks:

- **Size** — 50–200 % of the standard 400×300. Map calibration is unaffected, so the marker stays accurate at any size.
- **Opacity** — 30–100 %, for when you want to see the game through the map.
- **Position** — centered (the default) or pinned to a corner of the game window.
- **Nudge X / Y** — a pixel offset from that position. You can also **hold Ctrl and drag the overlay**; where you drop it is saved automatically. (Ctrl is required so that mousing over the minimap to read marker labels never moves it by accident.)
- **Double-click the overlay** to snap it straight back to the centre of the game window — handy if you dragged it somewhere awkward, or onto a display you no longer have.
- **Keep the minimap open when the game loses focus** — by default the overlay hides when you Alt+Tab away. Tick this to keep it up. It still closes for battles and unsupported zones.

### The position marker

When the minimap is open, a small icon (`marker.png`) shows where your character is on the map. It updates in real time as you move.

If the marker is missing, check that `marker.png` exists next to the executable.

### When the minimap is available

The custom map works only when **all** of these are true:

- You are in the **overworld** (exploring, not in a menu-only screen).
- You are **not in battle**.
- Your current zone has a matching image in the `maps\` folder (e.g. `MAP301.jpg` for that map).

If you enter battle, load into an unsupported zone, or the game cannot read the current map name, the overlay closes automatically. **Tab** will work again once you return to a supported area.

### Changing zones

When you walk into a different supported map, the overlay image swaps to the new zone without you needing to reopen it.

---

## System Tray Menu

Right-click the tray icon for the main menu:

| Item | Purpose |
|------|---------|
| **Launch Game** | Start a client on the primary monitor |
| **Launch Game (Secondary)** | Start a client on the secondary monitor |
| **Window Layout** | Apply window arrangements (Full / Lite) |
| **Chat** | Chat panel shortcuts (Full / Battle) |
| **Inventory** | Open inventory shortcuts (Full only) |
| **Send Alt+Q to Fighting** | Battle command helper (Full / Battle) |
| **Client Roster** | Open the radial client picker, or the native list in low-memory mode (Full / Battle) |
| **Map POIs** | Add, manage and export minimap points of interest (Full / Lite) |
| **Interface** | Switch between Native (low memory) and WebView2 (enhanced); Maps++ reloads automatically |
| **Settings…** | Open the settings window (game path, launcher, monitors, Window Layout, addons) |
| **Reload** | Restart Maps++ (picks up config changes) |
| **Exit** | Quit the app |

All configuration now lives in **Settings…** (or **Ctrl+Alt+,**): game path and arguments,
**start Maps++ automatically when Windows starts**, launch-a-client-on-startup, multi-client
count/delay, primary/secondary monitors, the Window Layout defaults, and enabling/disabling
addons. The tray keeps only the quick **actions**.

**Tray → Interface** selects the GUI used by Maps++. **WebView2 (enhanced)** keeps the polished
tray and radial client picker prewarmed for immediate response. **Native (low memory)** uses
standard Windows/AHK controls, does not preload WebView2, and opens the client roster as a list.
Your selection is remembered in `config.ini`.

> **Run on Windows start-up** is a per-user setting (no admin needed). When enabled, Maps++
> registers itself to launch at login; disabling removes that entry. You can also remove it
> from **Task Manager → Startup apps** — the checkbox reflects the real state either way.

---

## Hotkey Reference

Everyday shortcuts can be changed under **Settings → Hotkeys** (click a shortcut, press new keys, then OK). Changes apply immediately — no Reload needed. The tables below list the **default** bindings.

Global shortcuts work while the game or minimap overlay is focused.

### Core (all variants)

| Hotkey | Action |
|--------|--------|
| **Tab** | Toggle custom minimap (in game, on a supported map) |
| **Right-click** | Close minimap (while overlay is open) |
| **Ctrl+Alt+L** | Launch game on primary monitor |
| **Ctrl+Alt+K** | Launch game on secondary monitor |
| **Ctrl+Alt+,** | Open the Settings window |
| **Ctrl+Alt+R** | Reload Maps++ |
| **Ctrl+Alt+Q** | Exit Maps++ |

### Window Layout (Full / Lite)

Arranges all open game windows on one monitor. Window **size is never changed**—only position.

| Hotkey | Action |
|--------|--------|
| **Ctrl+Shift+L** | Apply your default layout on the **primary** monitor |
| **Ctrl+Shift+K** | Apply your default layout on the **secondary** monitor |

**Tray → Window Layout** offers one-time **Apply Preset** layouts: Reset, Single, Grid2x2,
Grid3x2, CenterFocus, DiceLeft, DiceRight.

The Window Layout **configuration** lives in **Settings… → Window Layout**:

- **Default layout** — which preset **Ctrl+Shift+L/K** uses
- **Main character** — which character’s window is centered or brought to front
- **Target display** — which monitor presets target when you have multiple screens

**Layout presets at a glance:**

| Preset | Good for |
|--------|----------|
| **Grid2x2** | Four clients in a 2×2 grid |
| **Grid3x2** | Up to six clients in a 3×2 grid |
| **CenterFocus** | Main character centered; others in corners |
| **DiceLeft / DiceRight** | Five windows in a dice pattern on one side of the screen |
| **Single** | One window centered |
| **Reset** | Move all windows to the top-left of the work area |

### Chat (Full / Battle)

| Hotkey | Action |
|--------|--------|
| **Shift+Ctrl+C** | Toggle chat on **all other** clients (keeps active window’s chat as-is) |
| **Ctrl+C** | Toggle chat on the **active** client only |
| **Alt+Ctrl+C** | Toggle **mini chat size** on all clients |

Useful when multi-boxing: hide chat on alts to reduce clutter, or shrink chat globally.

### Inventory (Full only)

| Hotkey | Action |
|--------|--------|
| **Alt+E** | Open inventory on the active client (clicks the inventory button) |
| **Alt+Shift+E** | Send **Alt+I** to open inventory |

### Battle Send (Full / Battle)

| Hotkey | Action |
|--------|--------|
| **Shift+Alt+Q** | For every client **currently in battle** with a pending action, queue **Alt+Q** (action 9) for both character and pet |

Use this to confirm or send the same battle command across all fighting characters at once.

### View Mode (Full / Lite / Battle)

| Hotkey | Action |
|--------|--------|
| **Alt+2** | Cycle view mode on **all** clients (0→3, wrap) |
| **Alt+1** | Toggle view mode 0/1 on **all** clients (0→1; 1/2/3→0) |

Echoes the in-game **F2** view-mode cycle across every open client without focusing each window.

### Client Roster (Full / Battle)

| Hotkey | Action |
|--------|--------|
| **Ctrl+Alt+M** | Show / hide the client roster |

A small always-on-top window listing every running client with its character, current zone and
status (**Playing**, **In battle**, **Loading**, **Not ready**). **Double-click a row** to bring
that client to the front.

It only appears when you ask for it — press **Ctrl+Alt+M** or use the tray menu. If you would
rather it opened itself whenever two or more clients are running, tick that in
**Settings → Client Roster**; closing it by hand still keeps it closed for that session.

Use it with **Ctrl+Alt+E** (*Send Enter Until Ready*): the roster is what shows you which alt is
still sitting at a login prompt.

### Map POIs (Full / Lite)

Marks NPCs, shops, portals, quest spots and notes on the minimap.

| Hotkey | Action |
|--------|--------|
| **Ctrl+Alt+P** | Add a POI where you are standing |
| **Ctrl+Alt+O** | Show / hide the POI layer (remembered between sessions) |
| **Ctrl+Alt+N** | Append an NPC entry for your position to `npc_generated.txt` |

Adding one works the way mapping already worked: **stand where the thing is and press the
hotkey**. The position is read from game memory, so it lands exactly where you stood — no
clicking on the map, no eyeballing. You give it a label and a type, and it appears on the
minimap in that type's colour.

Labels are shown while you play and **hide while the mouse is over the minimap**, so you can put
the cursor there to see the map art underneath them. The coloured dots are always there.
**Settings → Map POIs → Show labels** switches this to *Always* or *Never*.

Maps++ shows you the **zone name** ("Stillreach") and **in-game coordinates** — the same numbers
the game puts on your screen — wherever it talks about a POI.

POIs are stored per map in **`maps\pois.ini`**, next to `calibration.ini`, so a curated set can
ship with a release and anyone can add their own on top. The file is keyed by map **id** and
holds **raw** positions, because that is what the game and the server repo use — in-game
coordinates are raw X ÷ 16 and Y ÷ 8, applied for display only:

```ini
[MAP302]
1=1089|559|Grocery|shop
2=705|386|Rift Plains portal|portal
```

**Tray → Map POIs** has the rest: **Manage POIs…** (list, delete for the current map) and
**Export This Map's POIs**, which writes every POI in the server repo's NPC entry format —
the same shape **Ctrl+Alt+N** produces for a single position — into `npc_generated.txt` and
onto the clipboard:

```ts
	{
		id: 0x80020040,
		name: 'Grocery',
		file: 135,
		map: MapID.MAP302,
		point: { x: 1000, y: 2000 },
		direction: Direction.South,
	},
```

### Party Markers (Full / Battle)

| Hotkey | Action |
|--------|--------|
| **Ctrl+Alt+I** | Show / hide the party marker layer (remembered between sessions) |

Every *other* client running on the same map as you appears
as a coloured dot, labelled with the character name, alongside your own standard marker. Alts
that are loading, in battle, or in a different zone are not drawn (their position would be
stale).

Name labels are shown while you play and **hide while the mouse is over the minimap**, so the
map underneath stays readable when you look at it directly. **Settings → Party Markers → Show
names** switches this to *Always* or *Never*.

---

## Configuration (`config.ini`)

Maps++ creates or updates `config.ini` next to the executable (or in your user AppData folder if the install directory is not writable).

Everything below can be set from **Settings** (**Ctrl+Alt+,**) — editing the file by hand is only needed for unattended setups.

Common settings:

```ini
[Launcher]
GamePath=C:\Path\To\Your\main.exe
GameArgs=
LaunchOnStartup=0
MultiClientCount=5
MultiClientDelay=0
PrimaryMonitor=0
SecondaryMonitor=0

[Minimap]
Scale=100
Opacity=100
Anchor=Center
OffsetX=0
OffsetY=0
KeepOpenOnFocusLoss=0

[WindowLayout]
DefaultLayout=Grid2x2
MainCharacter=YourCharName
TargetMonitor=0

[Hotkeys]
toggleMinimap=Tab
launchPrimary=^!l
openSettings=^!,

[MapPois]
LabelMode=autohide
DefaultKind=npc
LayerVisible=1

[Addons]
WindowLayout=1
ChatToggle=1
BattleSend=1
InventoryToggle=1
ClientRoster=1
PartyMarkers=1
MapPois=1
```

| Setting | Meaning |
|---------|---------|
| `GamePath` | Full path to `main.exe` |
| `GameArgs` | Extra command-line arguments passed when launching the game |
| `LaunchOnStartup` | `1` = launch a game client when Maps++ starts |
| `MultiClientCount` | How many clients **Ctrl+Alt+5** launches |
| `MultiClientDelay` | Milliseconds between each of those launches |
| `PrimaryMonitor` | Display for **Ctrl+Alt+L**. `0` = OS primary; `1`, `2`, … = specific display |
| `SecondaryMonitor` | Display for **Ctrl+Alt+K**. `0` = first non-primary; `1`, `2`, … = specific display |
| `Scale` | Minimap size, 50–200 % of the standard 400×300 |
| `Opacity` | Minimap opacity, 30–100 % |
| `Anchor` | Where the minimap sits on the game window: `Center`, `TopLeft`, `TopRight`, `BottomLeft`, `BottomRight` |
| `OffsetX` / `OffsetY` | Pixel nudge from the anchor (negative allowed). Dragging the minimap writes these for you |
| `KeepOpenOnFocusLoss` | `1` = keep the minimap open when you Alt+Tab away |
| `LabelMode` | When marker labels are drawn: `autohide` (shown, except while the mouse is over the minimap), `always`, or `never`. Applies to both POI labels and party marker names |
| `LayerVisible` | `0` = that layer stays hidden. Set by **Ctrl+Alt+O** (POIs) and **Ctrl+Alt+I** (party markers), so a layer you switch off stays off next session |
| `DefaultKind` | Type pre-selected when adding a POI: `npc`, `shop`, `portal`, `quest`, `note` |
| `DefaultLayout` | Preset used by **Ctrl+Shift+L/K** |
| `MainCharacter` | Character name for center-focus layouts |
| `TargetMonitor` | `0` = primary; `1`, `2`, … = specific display |
| `[Hotkeys]` | One entry per rebindable action, in AutoHotkey chord syntax (`^`=Ctrl, `!`=Alt, `+`=Shift). Only actions you have rebound appear here; rebind from **Settings → Hotkeys** |
| `[Addons]` | `0` = disabled, `1` = enabled (also toggled from **Settings → Addons**) |

After editing `config.ini`, use **Reload** from the tray menu or **Ctrl+Alt+R**.

---

## Multi-Boxing Workflow (Example)

A typical setup with the **Full** variant:

1. Start **Maps++** (tray icon appears).
2. **Ctrl+Alt+L** — launch main character on primary monitor.
3. **Ctrl+Alt+K** — launch alts on secondary monitor (if you have two displays).
4. **Settings → Window Layout → Main character** — choose your main.
5. **Ctrl+Shift+L** — snap everyone into your default grid.
6. In game on a supported map, press **Tab** on your main window for the custom minimap.
7. Use **Shift+Ctrl+C** to hide chat on alts during farming.
8. In battle, **Shift+Alt+Q** to send commands to all fighting clients.

---

## Troubleshooting

### Minimap does not open when I press Tab

- Make sure the **game window is focused** (click it first).
- Confirm you are **not in battle** and not on a loading screen.
- Check that your current zone has a map file in `maps\` (supported maps ship with the release).
- If you run **multiple** `main.exe` instances, Maps++ tracks whichever game window you last focused.

### Position marker is wrong or stuck

- Maps are calibrated per zone. If a map was recently added or updated, report it to the Maps++ / osMW community.
- Ensure `marker.png` is present and Maps++ was not blocked by antivirus from reading game memory.

### “Game path not configured”

Open **Settings → Launcher → Browse…** (**Ctrl+Alt+,**) and select your osMW `main.exe`.

### “Map folder missing” notification

The `maps\` folder must sit next to `mapsplusplus.exe`. Re-extract the full release package.

### Hotkeys do nothing

- Another program may be using the same shortcuts.
- Check **Settings → Addons** — the feature may be disabled.
- Check **Settings → Hotkeys** — the shortcut may have been rebound.
- Reload Maps++ after config changes.

### Only one monitor / secondary launch centers on primary

With a single display, **Ctrl+Alt+K** still launches a client but centers it on the only monitor available.

### More than two monitors / wrong displays used

By default **Ctrl+Alt+L** uses your OS primary display and **Ctrl+Alt+K** uses the first non-primary one. If you have three or more screens and want different ones, set `PrimaryMonitor` and `SecondaryMonitor` in `config.ini` to the 1-based display index you want (e.g. `SecondaryMonitor=3`), then **Reload**. An index that isn't currently connected falls back to the default behaviour.

---

## Tips

- The minimap closes if you **Alt+Tab** to another application, so it stays out of the way when you are not playing (**Settings → Minimap** can keep it open instead).
- You can **disable addons** you do not use from **Settings → Addons** to avoid accidental hotkey triggers.
- **Lite** is ideal if you only want the minimap and window layout without combat or inventory shortcuts.
- **Battle** is a smaller build focused on combat multi-boxing and chat control.

---

## Supported Maps

Custom images are provided for many osMW zones (files in `maps\`, named like `MAP301.jpg`, `MAP302.jpg`, etc.). If you enter an area without a custom image, Tab will not open the overlay until you reach a supported map.

New maps are added over time; keep your `maps\` folder updated with the latest release if you play newer content.

---

## Need Help?

Maps++ is built for the osMW community. If something breaks after a **game client update**, offsets may need refreshing—check osMW forums or Discord for an updated Maps++ build.

For bugs or feature requests, contact the Maps++ maintainers through your usual osMW community channels.
