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
   - **Full** — minimap, launcher, window layout, chat, inventory, and battle helpers.
   - **Lite** — minimap, launcher, and window layout only.
   - **Battle** — minimap, launcher, chat, and battle helpers (no inventory or window layout).
2. Extract the folder somewhere convenient (Desktop, a games folder, etc.).
3. Run **`mapsplusplus.exe`**.

The folder must include:

- `mapsplusplus.exe`
- `marker.png` (your position dot on the map)
- `maps\` (custom map images, one `.jpg` per supported zone)

On first run, Maps++ may ask you to locate **`main.exe`** (the game client). You can also set this later from the tray menu: **Set Game Path…**

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
| **Settings…** | Open the settings window (game path, launcher, monitors, Window Layout, addons) |
| **Reload** | Restart Maps++ (picks up config changes) |
| **Exit** | Quit the app |

All configuration now lives in **Settings…** (or **Ctrl+Alt+,**): game path and arguments,
**start Maps++ automatically when Windows starts**, launch-a-client-on-startup, multi-client
count/delay, primary/secondary monitors, the Window Layout defaults, and enabling/disabling
addons. The tray keeps only the quick **actions**.

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

---

## Configuration (`config.ini`)

Maps++ creates or updates `config.ini` next to the executable (or in your user AppData folder if the install directory is not writable).

Common settings:

```ini
[Launcher]
GamePath=C:\Path\To\Your\main.exe
GameArgs=
LaunchOnStartup=0
PrimaryMonitor=0
SecondaryMonitor=0

[WindowLayout]
DefaultLayout=Grid2x2
MainCharacter=YourCharName
TargetMonitor=0

[Addons]
WindowLayout=1
ChatToggle=1
BattleSend=1
InventoryToggle=1
```

| Setting | Meaning |
|---------|---------|
| `GamePath` | Full path to `main.exe` |
| `GameArgs` | Extra command-line arguments passed when launching the game |
| `LaunchOnStartup` | `1` = launch a game client when Maps++ starts |
| `PrimaryMonitor` | Display for **Ctrl+Alt+L**. `0` = OS primary; `1`, `2`, … = specific display |
| `SecondaryMonitor` | Display for **Ctrl+Alt+K**. `0` = first non-primary; `1`, `2`, … = specific display |
| `DefaultLayout` | Preset used by **Ctrl+Shift+L/K** |
| `MainCharacter` | Character name for center-focus layouts |
| `TargetMonitor` | `0` = primary; `1`, `2`, … = specific display |
| `[Addons]` | `0` = disabled, `1` = enabled (also toggled from tray **Addons**) |

After editing `config.ini`, use **Reload** from the tray menu or **Ctrl+Alt+R**.

---

## Multi-Boxing Workflow (Example)

A typical setup with the **Full** variant:

1. Start **Maps++** (tray icon appears).
2. **Ctrl+Alt+L** — launch main character on primary monitor.
3. **Ctrl+Alt+K** — launch alts on secondary monitor (if you have two displays).
4. **Tray → Window Layout → Set Main Character** — choose your main.
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

Use **Tray → Set Game Path…** and select your osMW `main.exe`.

### “Map folder missing” notification

The `maps\` folder must sit next to `mapsplusplus.exe`. Re-extract the full release package.

### Hotkeys do nothing

- Another program may be using the same shortcuts.
- Check **Tray → Addons** — the feature may be disabled.
- Reload Maps++ after config changes.

### Only one monitor / secondary launch centers on primary

With a single display, **Ctrl+Alt+K** still launches a client but centers it on the only monitor available.

### More than two monitors / wrong displays used

By default **Ctrl+Alt+L** uses your OS primary display and **Ctrl+Alt+K** uses the first non-primary one. If you have three or more screens and want different ones, set `PrimaryMonitor` and `SecondaryMonitor` in `config.ini` to the 1-based display index you want (e.g. `SecondaryMonitor=3`), then **Reload**. An index that isn't currently connected falls back to the default behaviour.

---

## Tips

- The minimap closes if you **Alt+Tab** to another application, so it stays out of the way when you are not playing.
- You can **disable addons** you do not use from the tray to avoid accidental hotkey triggers.
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
