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
| **Better Hotkeys** | Give each character any number of persistent battle-skill shortcuts, including different skills on the same key. |
| **Multi-box helpers** | Optional addons arrange windows, toggle chat, open inventory, and send battle commands to all fighting characters. |
| **Radial menus** | **Ctrl+Alt+A** rings your common actions around the cursor; choose **Clients** there to jump between alts. One click each. |

Maps++ reads information from the running game client (`main.exe`). It does not modify game files and works alongside the normal client.

---

## Getting Started

### 1. Install and run

1. Download the release that matches how you play:
   - **Full** — minimap, launcher, window layout, chat, inventory, Better Hotkeys, battle helpers, map POIs, and the multi-client roster and party markers.
   - **Lite** — minimap, map POIs, launcher, and window layout only.
   - **Battle** — minimap, launcher, chat, Better Hotkeys, battle helpers, roster and party markers (no inventory or window layout).
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

- From the tray icon: right-click → **Launch (Primary)** or **Launch (Secondary)**
- Or press **Ctrl+Alt+L** (primary monitor) / **Ctrl+Alt+K** (secondary monitor)

Each action starts the number of clients configured under **Settings → Launcher**. The same
page can leave the new windows centered, apply the current default layout, or assign a different
preset/custom layout to each display.

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
| **Launch (Primary)** | Start the configured clients on the primary target, then optionally apply its layout |
| **Launch (Secondary)** | Start the configured clients on the secondary target, then optionally apply its layout |
| **Quick Actions** | Open the action ring and access broadcast/game controls such as Send Enter, battle send, chat, inventory and view mode |
| **Clients & Windows** | Open the client roster, arrange game windows and access character tools available in the current build |
| **Map & Overlay** | Add or manage map POIs and toggle map-related overlay features available in the current build |
| **Interface** | Switch between Native (low memory) and WebView2 (enhanced); Maps++ reloads automatically |
| **Settings…** | Open the settings window (game path, launcher, monitors, Window Layout, addons) |
| **Reload** | Restart Maps++ (picks up config changes) |
| **Debug** | Diagnostics and memory-signature tools — see below |
| **About Maps++…** | Version, interface mode and how many addons are active |
| **Exit** | Quit the app |

All configuration now lives in **Settings…** (or **Ctrl+Alt+,**): game path and arguments,
**start Maps++ automatically when Windows starts**, launch-a-client-on-startup, clients per
launch and delay, primary/secondary displays and launch layouts, Window Layout defaults, and enabling/disabling
addons. The tray keeps only the quick **actions**.

Under **Settings → Launcher**, each launch target has a **Layout after launch** dropdown:

- **None — center clients only** — launch the batch without organising it
- **Default — _layout name_** — follow the current default from **Settings → Window Layout**
- A named preset or custom layout — pin that target independently, so Primary and Secondary can
  use different arrangements

**Tray → Interface** selects the GUI used by Maps++. **WebView2 (enhanced)** keeps the polished
tray and the radial rings prewarmed for immediate response. **Native (low memory)** uses
standard Windows/AHK controls, does not preload WebView2, opens the client roster as a list and
Quick Actions as a popup menu. Your selection is remembered in `config.ini`.

> **Run on Windows start-up** is a per-user setting (no admin needed). When enabled, Maps++
> registers itself to launch at login; disabling removes that entry. You can also remove it
> from **Task Manager → Startup apps** — the checkbox reflects the real state either way.

### Reporting a problem

**Tray → Debug → Copy Diagnostics** puts everything needed to investigate an issue on your
clipboard, ready to paste into a bug report: the version you are running, which memory offsets
resolved and how, your display layout, which addons are on or off, the clients that were running,
and the recent log. It also saves a copy next to the log file. Please include it — without the
version and offset information, most reports cannot be acted on.

**Tray → Debug → Open Log Folder** opens the folder containing `mapsplusplus.log`. Maps++ writes
one line per event there — startup, errors, and anything an addon reported — so a problem that
happened five minutes ago is still recoverable. The log is capped at about 1 MB and the previous
one is kept as `mapsplusplus.log.1`; nothing else is retained.

> **If an addon keeps failing, Maps++ turns it off.** Rather than notifying you once a second
> forever, a repeatedly-failing addon is disabled for the rest of the session and named in one
> notification. This is not saved — restarting Maps++ gives it another chance. Every failure is in
> the log regardless of whether you saw a notification.

### Update notifications

**Settings → Launcher → Check for a newer Maps++ on startup** (on by default) makes a single request
to `osmw.net` a few seconds after launch. If a newer version exists you get one notification, and
the tray menu gains a **Get the update** entry that opens the download page; **About Maps++…** shows
it too. If you are up to date, nothing happens at all.

It never downloads or installs anything — it reads a version number and tells you. Maps++ makes no
other network requests, sends nothing about you or your characters (no identifiers, no query string,
not even your own version), and treats every failure as "no update" without saying a word. Turn the
setting off and it makes no requests at all.

---

## Hotkey Reference

Everyday shortcuts can be changed under **Settings → Hotkeys** (click a shortcut, press new keys, then Save). Choose **Unbind** when an action should have no direct shortcut. Changes apply immediately — no Reload needed. The tables below list the **default** bindings.

The default set is intentionally small: core controls and the **Ctrl+Alt+A** Quick Actions ring are bound, while addon actions start unbound. This avoids claiming common game and Windows shortcuts just because an addon is installed; use the ring or tray menu, or bind only the addon actions you want.

Existing saved bindings are left alone during an update. Use **Reset** (or **Reset all shortcuts to defaults**) if you want the revised defaults.

Global shortcuts work while the game or minimap overlay is focused.

### Core (all variants)

| Hotkey | Action |
|--------|--------|
| **Tab** | Toggle custom minimap (in game, on a supported map) |
| **Right-click** | Close minimap (while overlay is open) |
| **Ctrl+Alt+L** | Launch the configured clients on the primary target |
| **Ctrl+Alt+K** | Launch the configured clients on the secondary target |
| **Ctrl+Alt+E** | Send Enter to clients until each one reaches the game |
| **Ctrl+Alt+A** | Open the Quick Actions ring |
| **Ctrl+Alt+,** | Open the Settings window |
| **Ctrl+Alt+R** | Reload Maps++ |
| **Ctrl+Alt+Q** | Exit Maps++ |

### Window Layout (Full / Lite)

Arranges your open game windows on one monitor. Window **size is never changed**—only position.
**Minimized clients are left alone**, so a client you deliberately tucked away stays that way.

| Hotkey | Action |
|--------|--------|
| **Not bound** | Apply your default layout on the **primary** monitor |
| **Not bound** | Apply your default layout on the **secondary** monitor |
| **Not bound** | Open the **Window Layouts** window |
| **Not bound** | **Undo** the last layout you applied |

**Tray → Window Layout** offers one-time **Apply Preset** layouts (Reset, Single, Grid2x2,
Grid3x2, CenterFocus, DiceLeft, DiceRight), an **Apply Custom** list of your own saved layouts,
**Capture Current As…**, **Layouts…**, and **Undo Last Apply**.

The Window Layout **configuration** lives in **Settings… → Window Layout**:

- **Default layout** — which layout the **Apply default layout** actions use. Presets *and* your own custom
  layouts both appear here
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

More clients than a preset has slots? The extras no longer land exactly on top of each other —
they **cascade** a little further down and right on each pass, and Maps++ tells you how many.

#### Custom layouts

Presets stop at six slots and treat every client as interchangeable. A **custom layout** stores
one slot per client — up to all eleven — and remembers **which character belongs in which slot**,
so re-applying it always puts the same client back in the same place.

Two ways to make one, and they work together:

1. **Capture.** Drag your clients wherever you want them, then **Tray → Window Layout →
   Capture Current As…** and give it a name. This is the fast path, and it is exact.
2. **Edit.** **Tray → Window Layout → Layouts…** opens the editor (or bind **Open window layouts**): a
   scale drawing of your monitor with a draggable box per client. Use it to line things up
   precisely, or to design a layout for alts that aren’t even launched yet.

In the editor:

| Control | What it does |
|---------|--------------|
| **Capture current** | Replaces the selected layout’s boxes with where your clients sit right now |
| **Add slot** / **Remove slot** | Change how many clients the layout arranges |
| **Snap** | **Guides** aligns to other boxes’ edges and centres; **8/16/32 px** snaps to a grid |
| **Display** | Which monitor the layout targets |
| **Character** | Pin a slot to one character, or leave it as **Any client** |
| **Focus this window after applying** | The one client that gets activated at the end |
| **Apply** / **Save** | Try it out, or store it |
| **Set as default** | Make the primary/secondary **Apply default layout** actions use this layout |

Drag a box to move it, or select it and use the **arrow keys** (**Shift** for bigger steps).
**Ctrl+S** saves. Boxes are drawn at the client window size recorded when the layout was
captured — applying a layout still never resizes anything.

**Native (low memory) mode** gets the same layouts without the canvas: the tray menu opens a
list with Capture, Apply, Rename, Delete, Set as default and Undo. Capture is the main way to
author a layout either way; switch to **Tray → Interface → WebView2 (enhanced)** for visual editing.

**How clients are matched to slots when you apply:**

1. A slot naming a character claims that character’s window.
2. Whatever is left fills the unnamed slots, top-to-bottom then left-to-right.
3. Any client beyond the last slot cascades instead of stacking.
4. Slots whose character isn’t running are simply skipped.

Character names are only readable once a client has logged in. Capturing at the login screen
still works — those slots are saved as **Any client**, and Maps++ says so.

Layouts are stored as **fractions of the monitor’s work area**, not fixed pixels, so they survive
a resolution change. Each one also records the display it was authored on, and Maps++ finds that
same physical monitor again even if Windows renumbers your displays. If it has to rescale or fall
back to another screen, it tells you.

Made a mess? **Undo Last Apply** puts every window back where the last apply found it.

### Chat (Full / Battle)

| Hotkey | Action |
|--------|--------|
| **Not bound** | Toggle chat on **all other** clients (keeps active window’s chat as-is) |
| **Not bound** | Toggle chat on the **active** client only |
| **Not bound** | Toggle **mini chat size** on all clients |

Useful when multi-boxing: hide chat on alts to reduce clutter, or shrink chat globally.

### Inventory (Full only)

| Hotkey | Action |
|--------|--------|
| **Not bound** | Open inventory on the active client (clicks the inventory button) |
| **Not bound** | Send **Alt+I** to open inventory |

### Character Vendor (Full only)

| Hotkey | Action |
|--------|--------|
| **Not bound** | Open the Character Vendor pricing panel for the active client |

Setting up a player shop normally means typing a price for each item one at a time, into a
client box that shows no thousands separators — so `1000000` and `100000` look almost
identical. This panel shows your 24 inventory slots as a 6×4 grid with item thumbnails and
prices formatted as you type (`1,500,000`), coloured by magnitude so a missing or extra zero
changes the colour.

**It only writes prices.** It never opens your shop — after applying, you click **Open**
in-game yourself so you can check every price first.

Using it:

- **Type a price** into any slot. It formats itself as you go; `1.2m` and `850k` are accepted
  and expand when you leave the field.
- **Select several slots** — click, Ctrl+click to add one, Shift+click for a range, or drag a
  box across the grid. Then type a price in the toolbar and press **Set on selected** to price
  them all at once.
- **Nothing is written until you press Apply.** Changed slots are marked with a gold bar and
  show the old price struck through. **Revert** puts everything back.
- After applying, Maps++ reads the values back and checks each one. If any price did not take,
  it says so rather than reporting success.

**Presets** save the prices you have in the grid under a name, for shops you re-list often.
Presets are keyed by **slot position**, not by item — so if your inventory has been reordered
since you saved, the prices would land on different items. Loading a preset therefore always
shows a preview first, flagging any slot that now holds a different item or is empty, and
"Load into grid" only fills in the grid: you still press Apply yourself.

Presets are stored in `shop_presets.ini` next to `config.ini`.

**The grid follows the game.** While the panel is open it re-reads your client about once a
second, so prices you change in the game’s own shop window appear in the grid. That makes a
useful workflow: set one base price across a group of similar items from the panel, apply it,
then nudge individual ones up or down in game and watch the grid keep up.

Anything you have typed but not applied is kept — the slot stays marked as changed, and the
value it will replace updates underneath it. The one exception is a slot where the *item*
itself changed: that price was meant for something that is no longer there, so it is dropped
and the panel tells you which slots.

**Close your vendor before pricing.** While the vendor window is open in-game the client owns
the price table, so applying is blocked and the header shows *“Your vendor is open”*. Close it
and the panel unblocks within a second. You can still read and edit the grid meanwhile —
only writing is held back.

The panel writes to the client it was opened from — shown in the title bar — and will not
follow your focus to another window. Use the **Client** dropdown in the header to switch
deliberately. Applying is blocked while that client is in battle, not logged in, or has the
vendor open.

**Tray → Character Vendor → Verify Slot Mapping…** dumps the raw price and item-id arrays
next to each other. Use it after a game update to confirm the grid still lines up with your
real inventory.

### Battle Send (Full / Battle)

| Hotkey | Action |
|--------|--------|
| **Not bound** | For every client **currently in battle** with a pending action, set battle action 9 for both character and pet |

Use this to confirm or repeat the same battle command across all fighting characters at once.
Nothing is typed into the clients — the action value is written straight into each one, so it
works on every fighting client at once regardless of which window has focus.

### Better Hotkeys (Full / Battle)

Choose **Tray → Quick Actions → Better Hotkeys…** to create character profiles and bind their
battle skills. Profiles are created only when you ask, so bank characters and other mules do
not clutter the editor. A saved profile remains editable while its character is offline.

1. Choose **Create**, then select a running character or enter its exact name and class.
2. Select that profile and choose one of its four skill families and tiers I–IV.
3. Press the shortcut when prompted. Add as many bindings as you need, then choose
   **Save hotkeys**.

The same shortcut can mean something different for each character. For example, **F1** can
select **Stun IV** while one Human is active and **Heal Other IV** while a Centaur is active.
It never switches clients: the character's own `main.exe` window must already be active, and
the shortcut only acts while that client is in battle. Pressing another Better Hotkey before
the turn resolves replaces the previously selected action.

Player skills are supported now. Pet skill/action offsets are reserved in the addon for future
support, but pet bindings are not shown until Maps++ can reliably identify a pet's four skills.

### View Mode (Full / Lite / Battle)

| Hotkey | Action |
|--------|--------|
| **Not bound** | Cycle view mode on **all** clients (0→3, wrap) |
| **Not bound** | Toggle view mode 0/1 on **all** clients (0→1; 1/2/3→0) |

Echoes the in-game **F2** view-mode cycle across every open client without focusing each window.

### Client Roster (Full / Battle)

| Hotkey | Action |
|--------|--------|
| **Not bound** | Show / hide the client roster |

A small always-on-top window listing every running client with its character, current zone and
status (**Playing**, **In battle**, **Loading**, **Not ready**). **Double-click a row** to bring
that client to the front.

It only appears when you ask for it — choose **Clients** in Quick Actions, bind the action, or use the tray menu. If you would
rather it opened itself whenever two or more clients are running, tick that in
**Settings → Client Roster**; closing it by hand still keeps it closed for that session.

Use it with **Ctrl+Alt+E** (*Send Enter Until Ready*): the roster is what shows you which alt is
still sitting at a login prompt.

### Quick Actions (all variants)

| Hotkey | Action |
|--------|--------|
| **Ctrl+Alt+A** | Open / close the Quick Actions ring |

The same ring as the client picker, but the spokes are shortcuts instead of characters: view
mode, battle send, inventory, chat, window layout (primary or secondary display), POIs, the
Character Vendor pricing panel, and settings. Press the hotkey and the ring
appears **at your cursor** — move onto a spoke and left-click to fire it. Right-click, **Esc**,
a click in the gaps, or the hotkey again all dismiss it, and a click in the gaps does *not* also
reach the game.

It works from anywhere, including with the game unfocused: actions that act on "the active
client" use whichever client you were last in, and Maps++ brings that window back to the front
before firing. If no client is running at all you get a tray notification rather than a
misfire.

Spokes for **Minimap**, **POIs** and **Party Markers** carry a small dot showing whether that
layer is currently on. View mode and chat size have no dot — their state lives in the game's
memory, and reading it on every open would make the ring feel slow.

**Settings → Quick Actions** chooses which shortcuts appear and in what order (up to ten), using
checkboxes and the ↑/↓ buttons. Only actions from addons you actually have are listed, so the
ring is always correct for your build — the Lite variant, for instance, offers the minimap,
layout, POI and launcher entries. The centre of the ring opens that settings page.

### Map POIs (Full / Lite)

Marks NPCs, shops, portals, quest spots and notes on the minimap.

| Hotkey | Action |
|--------|--------|
| **Not bound** | Add a POI where you are standing |
| **Not bound** | Show / hide the POI layer (remembered between sessions) |
| **Not bound** | Append an NPC entry for your position to `npc_generated.txt` |

Adding one works the way mapping already worked: **stand where the thing is and run Add POI at
my position** from Quick Actions, or assign it a shortcut. The position is read from game memory, so it lands exactly where you stood — no
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

`|` separates the fields, so a label you type containing one has it replaced with a space, and
labels are capped at 48 characters. Accented and non-Latin labels are stored as you typed them.

A map with more than **120** POIs draws only the first 120, and says so once when you enter it —
each one is a control on the overlay window, and there is a point past which the overlay pays
for markers nobody can pick out anyway.

**Tray → Map POIs** has the rest: **Manage POIs…** (list, delete for the current map) and
**Export This Map's POIs**, which writes every POI in the server repo's NPC entry format —
the same shape the **Generate NPC entry** action produces for a single position — into `npc_generated.txt` and
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
| **Not bound** | Show / hide the party marker layer (remembered between sessions) |

Every *other* client running on the same map as you appears
as a coloured dot, labelled with the character name, alongside your own standard marker. Alts
that are loading, in battle, or in a different zone are not drawn (their position would be
stale).

Each client keeps the **same colour** for as long as it is running, so you can learn which dot
is which. An alt zoning out or dropping into a battle does not renumber the others; the colour
is only released once that client closes.

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
PrimaryLaunchLayout=__default__
SecondaryLaunchLayout=__default__

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
MainCharacterAsked=1

[Hotkeys]
toggleMinimap=Tab
launchPrimary=^!l
openSettings=^!,
clientRosterToggle=

[MapPois]
LabelMode=autohide
DefaultKind=npc
LayerVisible=1

[ShopPrices]
ConfirmHigh=1
WarnAbove=10000000
PresetClears=0
IconBase=0

[QuickActions]
Actions=viewModeToggle,battleSend,inventoryOpenClick,chatToggleAll,windowLayoutPrimary,clientRosterToggle,poiToggleLayer,openSettings
Prewarm=1

[Addons]
WindowLayout=1
ChatToggle=1
BattleSend=1
InventoryToggle=1
ClientRoster=1
PartyMarkers=1
MapPois=1
QuickActions=1
ShopPrices=1
BetterHotkeys=1
```

| Setting | Meaning |
|---------|---------|
| `GamePath` | Full path to `main.exe` |
| `GameArgs` | Extra command-line arguments passed when launching the game |
| `LaunchOnStartup` | `1` = launch a game client when Maps++ starts |
| `MultiClientCount` | How many clients **Ctrl+Alt+L/K** launches |
| `MultiClientDelay` | Milliseconds between each client in a launch batch |
| `PrimaryMonitor` | Display for **Ctrl+Alt+L**. `0` = OS primary; `1`, `2`, … = specific display |
| `SecondaryMonitor` | Display for **Ctrl+Alt+K**. `0` = first non-primary; `1`, `2`, … = specific display |
| `PrimaryLaunchLayout` / `SecondaryLaunchLayout` | Layout applied after that launch: blank = center only, `__default__` = follow `DefaultLayout`, or a preset/custom layout name |
| `Scale` | Minimap size, 50–200 % of the standard 400×300 |
| `Opacity` | Minimap opacity, 30–100 % |
| `Anchor` | Where the minimap sits on the game window: `Center`, `TopLeft`, `TopRight`, `BottomLeft`, `BottomRight` |
| `OffsetX` / `OffsetY` | Pixel nudge from the anchor (negative allowed). Dragging the minimap writes these for you |
| `KeepOpenOnFocusLoss` | `1` = keep the minimap open when you Alt+Tab away |
| `LabelMode` | When marker labels are drawn: `autohide` (shown, except while the mouse is over the minimap), `always`, or `never`. Applies to both POI labels and party marker names |
| `LayerVisible` | `0` = that layer stays hidden. Set by the POI/party **Show/hide layer** actions, so a layer you switch off stays off next session |
| `DefaultKind` | Type pre-selected when adding a POI: `npc`, `shop`, `portal`, `quest`, `note` |
| `Actions` | Quick Actions ring contents: hotkey action ids, comma-separated, in ring order (first at 12 o'clock, then clockwise). Ids for addons this build doesn't have are ignored, so one file works across variants. Set from **Settings → Quick Actions** |
| `Prewarm` | `1` = load the Quick Actions ring at startup so the first press is instant; `0` = build it on first use, saving memory |
| `DefaultLayout` | Layout used by the primary/secondary **Apply default layout** actions — a preset name, or the name of one of your custom layouts |
| `MainCharacter` | Character name for center-focus layouts |
| `TargetMonitor` | `0` = primary; `1`, `2`, … = specific display |
| `MainCharacterAsked` | `1` once the first-run "which is your main character?" prompt has been shown. Set whether you answered or dismissed it, so it is only ever asked once — clear it to be asked again |
| `VersionCheck` | `0` disables the startup update check, after which Maps++ makes no network requests at all |
| `ConfirmHigh` | `1` = ask for confirmation before applying a large vendor price |
| `WarnAbove` | The price at which that confirmation kicks in |
| `PresetClears` | `1` = loading a shop preset also clears prices on slots the preset doesn’t mention. Off by default, so presets only add |
| `IconBase` | Whether the client’s item id is `0`- or `1`-based, which decides how thumbnails are matched. Set it from **Settings → Character Vendor** after comparing one known item |
| `ItemIdOffset` | Override for the inventory item-id address, e.g. `0x2E2028`. Only needed if a game update moves it; blank uses the built-in value (or whatever signature calibration resolves), `0` turns thumbnails off. When set, it wins over signature calibration — which is the point, since the case it exists for is calibration landing on the wrong address |
| `[Hotkeys]` | One entry per rebindable action, in AutoHotkey chord syntax (`^`=Ctrl, `!`=Alt, `+`=Shift). A blank value means **Not bound**; rebind or unbind from **Settings → Hotkeys** |
| `[Addons]` | `0` = disabled, `1` = enabled (also toggled from **Settings → Addons**) |

After editing `config.ini`, use **Reload** from the tray menu or **Ctrl+Alt+R**.

### Better Hotkeys profiles (`better_hotkeys.ini`)

The Better Hotkeys editor owns this file and stores it beside `config.ini` (or in the same
writable AppData fallback). Profiles use numbered sections so character names never need to
be used as INI keys:

```ini
[Meta]
Version=1
Count=1

[Profile.1]
Name=YourCharName
Class=0
BindingCount=2
Binding1Chord=F1
Binding1Actor=player
Binding1Skill=19
Binding2Chord=^2
Binding2Actor=player
Binding2Skill=11
```

`Class` is the client class ID (`0`–`7`), `Skill` is the server/client skill enum, and `Chord`
uses AutoHotkey syntax. Edit through the visual editor where possible so class restrictions and
conflicts with Maps++ shortcuts are checked before saving.

### Shop presets (`shop_presets.ini`)

Character Vendor presets live beside `config.ini` in the same way. The editor writes this for
you, but the format is plain text:

```ini
[Index]
1=Weekend Prices

[Preset.1]
Saved=20260803143012
Char=Sterkr
Slot1=1000000|37
Slot7=250000|118
```

| Field | Meaning |
|-------|---------|
| `[Index]` | One `id=name` line per preset. Renaming only touches this section |
| `Saved` | When the preset was captured |
| `Char` | The character whose grid it was captured from |
| `Slot<N>` | `price|item id at save time`. Slots with no line are left alone when the preset is loaded |

The item id is only used to warn you when a slot now holds something different — presets match
by **slot position**, which is why loading one always shows a preview first.

### Custom layouts (`layouts.ini`)

Custom window layouts live in their own file beside `config.ini` (or in `%AppData%\osMW Maps++\`
when that folder isn’t writable). You never need to edit it by hand — the editor and
**Capture Current As…** write it for you — but it is plain text if you want to back it up or
share it:

```ini
[Index]
1=Six Box — Main Left

[Layout.1]
Monitor=1|3840x2160|3840x2120|0,0
WinSize=1024x768
Slot1=0.0000|0.0000|Sterkr|1
Slot2=0.2667|0.0000|Alt2|0
Slot3=0.5333|0.0000||0
```

| Field | Meaning |
|-------|---------|
| `[Index]` | One `id=name` line per layout. Renaming only touches this section |
| `Monitor` | `index|screen size|work-area size|top-left` — how Maps++ recognises the display this layout was made on |
| `WinSize` | Client window size at capture time. Used to draw boxes to scale in the editor; **never applied to a window** |
| `Slot<N>` | `x|y|character|flags` — position as a fraction of the work area (`0.0`–`1.0`), the character pinned to the slot (blank = any client), and `1` if this window should be focused after applying |

A malformed `Slot` line is skipped rather than breaking the layout.

---

## Multi-Boxing Workflow (Example)

A typical setup with the **Full** variant:

1. Start **Maps++** (tray icon appears).
2. **Ctrl+Alt+L** — launch main character on primary monitor.
3. **Ctrl+Alt+K** — launch alts on secondary monitor (if you have two displays).
4. **Settings → Window Layout → Main character** — choose your main.
5. Choose **Layout** in Quick Actions to snap everyone into your default grid. Once you have your windows exactly
   where you like them, **Tray → Window Layout → Capture Current As…** saves that arrangement,
   and **Set as default** makes the Layout action restore it every session.
6. In game on a supported map, press **Tab** on your main window for the custom minimap.
7. Choose **Chat (others)** in Quick Actions to hide chat on alts during farming.
8. In battle, choose **Battle send** to send commands to all fighting clients.

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
