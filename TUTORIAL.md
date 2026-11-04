# osMW Maps++ — Player Guide

**osMW Maps++** is a companion app for [MythWar Online](https://github.com/osMW) when playing on the **osMW** private server. It adds a detailed minimap overlay — with points of interest, party markers and waypoints — and quality-of-life tools for multi-boxing (running several game clients at once).

Maps++ runs in the background and appears in your system tray as **osMW Maps++**.

---

## What Maps++ Does

| Feature | Description |
|--------|-------------|
| **Maps++ minimap** | A high-resolution map over the game window, with POIs, party markers and waypoints. Where the game has its own minimap, **Tab** opens that one, and Maps++'s is a key or a menu item away. |
| **Live position marker** | A dot on the map tracks your character as you move. |
| **Launching** | Start your clients from the tray menu or a hotkey, centred on the display you choose. |
| **Launch profiles** | Name what a launch does — which accounts, how many clients, which display, which layout — and bind one to each launch hotkey. |
| **Auto-login** | Save your accounts once and launched clients log themselves in, as a named group or whichever accounts are free. |
| **Skill hotkeys** | Give each character any number of persistent battle-skill shortcuts, including different skills on the same key. |
| **Multi-box helpers** | Arrange windows, toggle chat, open inventory, and confirm battle actions on every fighting client at once. |
| **Action ring** | **Ctrl+Alt+A** puts your common actions in a ring around the cursor; choose **Clients** there to jump between alts. One click each. |
| **Works with the client mods** | Edits the Chat mod settings, shows which client mods are running, and keeps its overlays and clicks lined up when the mods scale the game 2× or 3×. |

Maps++ reads information from the running game client (`main.exe`). It never patches `main.exe`, and it never installs, updates or loads the **osMW client mods** (`osmw_core.dll` and its features — chat window, auto-login, the in-game F12 settings panel, display scaling and more). Those are a separate download that the client loads itself. Maps++ works with or without them; when they are there, it reads their status and edits a few of their settings, but only when you ask it to.

---

## Getting Started

### 1. Install and run

1. Download the release that matches how you play:
   - **Full** — everything: minimap, map POIs, launching and auto-login, window layout, chat and inventory keys, skill hotkeys, battle action, the client roster, party health and party markers, the Character vendor and Discord status.
   - **Lite** — minimap, map POIs, launching and auto-login, window layout, the Action ring and the Chat mod settings.
   - **Battle** — minimap, launching and auto-login, chat keys, skill hotkeys, battle action, view mode, the client roster, party health and party markers, the Action ring and the Chat mod settings (no inventory, map POIs or window layout).
2. Extract the folder somewhere convenient (Desktop, a games folder, etc.).
3. Run **`mapsplusplus.exe`**.

The folder must include:

- `mapsplusplus.exe`
- `marker.png` (your position dot on the map)
- `maps\` (custom map images, one `.jpg` per supported zone)

### 2. The setup guide

The first time Maps++ starts, a short setup guide opens. It asks the few things Maps++ cannot work
out for itself, and leaves everything else at sensible defaults:

| Step | What it asks |
|------|--------------|
| **Welcome** | Whether you play **one character** or **several at once**. One character keeps things simple: launching starts a single client, and the multi-boxing features (client roster, party health and markers, window layout, chat toggles, battle action) start switched off. |
| **Game** | Where `main.exe` is. Maps++ lists any copy it finds beside itself or already running, so usually you just pick one. You can skip it: the minimap works without it, and the first launch asks again. |
| **Minimap** | What **Tab** should open (see *Tab: the game's minimap or Maps++'s* below), and optionally a key for Maps++'s own minimap. |
| **Launching** | *Several characters only.* How many clients to start and on which display, an optional second team, the layout to arrange them in, and whether to log them in automatically — with a button to save your accounts there and then. |
| **Features** | Every feature in your build with a line on what it does. Turn off what you won't use; its menu items and shortcuts go with it. |
| **Finish** | The keys worth remembering, the look of Maps++, whether it starts with Windows, and an option to launch the game straight away. |

**Skip setup** leaves everything at its defaults. Either way you can run the guide again at any time
from **Tray → Help → Setup guide…**, and every choice in it is also in **Settings** (**Ctrl+Alt+,**).

> Already using Maps++? Updating does not show the guide: your settings carry over as they were, and
> a one-time notice points you at **Help → Setup guide** in case you want to walk through it.

### 3. Launch the game

- Double-click the tray icon, or right-click it → **Launch _profile name_**
- Or press **Ctrl+Alt+L** (Launch primary) / **Ctrl+Alt+K** (Launch secondary)

Each launch runs a **launch profile** — a named bundle of *which accounts, how many clients, which
display, which layout*, edited under **Settings → Launching**. The tray shows each launch by the name
of its profile, so **Launch Main team** says what it does. If you play one character, Launch secondary
is set to run nothing and leaves the tray menu; give it a profile under **Settings → Launching** if
you start a second team.

If the client mods' display scaling is on, each new client grows to 2× or 3× a moment after its
window appears. Maps++ waits for that (a few seconds at most) before centring the clients or
applying the layout, so they land where you expect at their final size.

To have a client start with Maps++, turn on **Settings → Game & startup → Launch the game when Maps++ starts**.

To skip the stock login screen, save your accounts under **Tray → Clients → Accounts…** and leave **Settings → Launching → Log launched clients in automatically** on. Maps++ clicks through the intro, then the client mods' auto-login fills the account (it needs the client mods installed). Passwords stay on this Windows user and are never stored in `config.ini`. Turn that setting off when you want the stock login screen.

If a profile names no accounts, each client claims the next enabled account on its own — which is
fine for one batch, but arbitrary once you run more clients than one batch holds. Give the profile a
**group** of accounts instead and it launches exactly those characters, every time.

---

## Using the Maps++ Minimap

### Open and close

| Action | How |
|--------|-----|
| **Open / toggle minimap** | **Tab** (while the game window is focused) — on a map where the game has no minimap of its own |
| **Open the Maps++ minimap anywhere** | **Open Maps++ minimap** — unbound until you give it a key (in the setup guide, or under **Settings → Hotkeys**); also **Tray → Minimap → Open Maps++ minimap** and the **Maps++ map** spoke of the Action ring |
| **Close minimap** | **Tab** again, **Right-click** on the overlay, or switch away from the game |

The overlay is centered on the game window and stays on top. Clicks on the minimap do **not** steal focus from the game, so you can keep playing normally.

### Tab: the game's minimap or Maps++'s

The game now has a minimap of its own on every map Maps++ ships an image for, Oranus included.
So by default **Tab goes to the game** wherever the game has a minimap, and opens the Maps++
overlay only where it does not. The first time that happens, Maps++ tells you once how to reach
its own map instead.

Change it under **Settings → Minimap → Tab key** (the setup guide asks the same question):

| Choice | Tab does |
|--------|----------|
| **The game's minimap where it has one, Maps++ elsewhere** *(default)* | Opens the game's own minimap when the map has one; the Maps++ overlay otherwise |
| **Maps++ wherever it has a map** | Opens the Maps++ overlay on every map it has an image for, as before |
| **The game's minimap only** | Always goes to the game; Maps++ never takes Tab |

Whichever you pick, **Open Maps++ minimap** opens the overlay — with its POIs, party markers and
waypoints — even on a map where the game has a minimap too. Tab always closes the overlay while it
is open.

> A map counts as having a game minimap only when the game would really open one: the map must
> come with one, your character must have minimaps enabled by the server, and for the newer
> minimaps (Oranus and others past the first 52) the client must have the minimap patch and
> `smallmap04.mda`. If any of that is missing, Tab opens Maps++ as it always did.

### Sizing and placing the minimap

**Settings → Minimap** controls how the overlay looks:

- **Size** — 50–200 % of the standard 400×300. Map calibration is unaffected, so the marker stays accurate at any size.
- **Grow with the game's display scale** — on by default. When the client mods show the game at 2× or 3×, the minimap is enlarged by the same amount, so it keeps its size relative to the game. Turn it off to keep the overlay at exactly the Size above.
- **Opacity** — 30–100 %, for when you want to see the game through the map.
- **Position** — centered (the default) or pinned to a corner of the game picture. With the client mods' Resizable or Borderless display, that is the corner of the game picture itself, not of the black bars around it.
- **Nudge X / Y** — a pixel offset from that position. You can also **hold Ctrl and drag the overlay**; where you drop it is saved automatically. (Ctrl is required so that mousing over the minimap to read marker labels never moves it by accident.)
- **Double-click the overlay** to snap it straight back to the centre of the game window — handy if you dragged it somewhere awkward, or onto a display you no longer have.
- **Keep the minimap open when the game loses focus** — by default the overlay hides when you Alt+Tab away. Tick this to keep it up. It still closes for battles and unsupported zones.

### The position marker

When the minimap is open, a small icon (`marker.png`) shows where your character is on the map. It updates in real time as you move.

If the marker is missing, check that `marker.png` exists next to the executable.

### Coordinates under the cursor

Put your mouse over the minimap and the **in-game coordinates** under the pointer appear in the
bottom-left corner — the same numbers the game shows you, so you can read a location straight off the
map and call it out to someone. They update as you move the mouse and disappear when you move it away.

Turn it off under **Settings → Minimap → Show coordinates under the cursor** if you would rather have
the clear corner.

> This is also the quickest way to tell whether a map is calibrated correctly: hover your own marker
> and the coordinates should match what the game reports for your position.

### Waypoints

**Shift+click** anywhere on the minimap to drop a temporary marker. It shows **how far away it is and
which way to walk** — `42 SW` means forty-two paces to the south-west — and the distance updates as
you move. **Shift+click it again** to remove it.

A waypoint is a quick "meet me here", not a saved place:

- Only one exists at a time.
- It is **never written to disk** and does not survive a restart.
- It clears itself when you leave the zone.

For somewhere you want to keep, use **Map POIs** instead — those are permanent, named, per-map, and
shareable.

> The other minimap gestures are unchanged: **Ctrl+drag** moves the overlay, **double-click**
> re-centres it, **right-click** closes it, and a plain hover reveals marker labels.

### When the minimap is available

The custom map works only when **all** of these are true:

- You are in the **overworld** (exploring, not in a menu-only screen).
- You are **not in battle**.
- Your current zone has a matching image in the `maps\` folder (e.g. `MAP301.jpg` for that map).

If you enter battle, load into an unsupported zone, or the game cannot read the current map name, the overlay closes automatically. It opens again once you return to a supported area — with **Tab** if the map has no game minimap (or **Tab key** is set to *Maps++ wherever it has a map*), otherwise with **Open Maps++ minimap**.

### Adding a zone Maps++ doesn't have

**Tray → Minimap → Calibrate this map…** walks you through it.

1. Put the zone's image in the `maps\` folder, named after its map file —
   `MAP301.jpg`, `MAP329.jpg` and so on. The name Maps++ is looking for is shown at the top of
   the calibration window while you stand in that zone.
2. Open the Maps++ minimap and the calibration window, then stand somewhere you can recognise on
   the image — a shop door, a bridge, a corner. (On a map where Tab opens the game's own
   minimap, open Maps++'s with **Open Maps++ minimap** or **Tray → Minimap**.)
3. Move the mouse to that same spot **on the minimap** and press **Ctrl+Alt+1**.
4. Walk somewhere **diagonally away** and repeat with **Ctrl+Alt+2**.
5. Press **Apply calibration**.

Capturing is a keyboard shortcut rather than a button because it reads where your mouse is
sitting over the minimap — clicking a button would move the mouse off the point you are capturing.

Two points is all it needs; everything between and beyond them is worked out from those. Pick
them far apart and diagonal to each other — two spots close together, or in a line, give a much
less accurate result, and Maps++ will refuse points that share an X or Y position.

After applying, the window shows what your own position now reads as. **Compare it with the
coordinates on the game's own display** — if they match, the map is calibrated correctly.

### Changing zones

When you walk into a different supported map, the overlay image swaps to the new zone without you needing to reopen it.

---

## System Tray Menu

Right-click the tray icon for the main menu. It is never more than one level deep, and groups
that have nothing in your build or with your features switched off simply do not appear:

```
Launch Main team                   Ctrl+Alt+L     (double-clicking the icon does this)
Launch Alts                        Ctrl+Alt+K     (only when Launch secondary runs a profile)
Launch other                    ▸  your other profiles, and each saved group of accounts
───────
Minimap                         ▸  Open Maps++ minimap · Calibrate this map…
                                   Show POIs · Add POI here · Manage POIs…
                                   Party markers
Clients                         ▸  Client roster · Party health
                                   Arrange windows · Arrange on second display · Undo arrange · Window layouts…
                                   Toggle chat on other clients · on this client · chat size
                                   Accounts…
Actions                         ▸  Action ring
                                   Send Enter until ready · Battle action · Open inventory · Toggle view mode
                                   Character vendor… · Skill hotkeys…
───────
Settings…                          Ctrl+Alt+,
Help                            ▸  Setup guide… · Copy diagnostics · Open log folder
                                   Client mods status… · Client mod logging · About Maps++…
───────
Exit                               Ctrl+Alt+Q
```

Switches such as **Show POIs** and **Party markers** show whether they are on: a check mark in the
low-memory interface, an **ON**/**OFF** label in the enhanced one. Shortcuts are shown beside the
items that have one.

**Configuration lives in Settings…** (**Ctrl+Alt+,**). The tray keeps only things you *do*; lists that
used to hang off it in deep submenus now live in the windows that manage them:

| Looking for… | It is now in |
|--------------|--------------|
| Layout presets (Grid 2×2, Center focus…) and your saved layouts | **Clients → Window layouts…** — under *Built-in*, with **Apply** and **Make default** |
| Launching one particular account, or a batch from one account on | **Clients → Accounts…** — select the account, then **Launch this account** or **Launch from here** |
| Exporting, importing and game-marker export of POIs | **Settings → Map POIs** — the *Sharing* buttons |
| What Tab opens | **Settings → Minimap → Tab key** |
| Enhanced or low-memory interface | **Settings → Appearance → Interface** |

### Settings

The Settings window groups its pages the way the tray does:

| Group | Pages |
|-------|-------|
| **General** | **Game & startup** (game path, start with Windows, launch on start, updates) · **Launching** (which profile each launch runs, the profiles themselves, auto-login, delay between clients) · **Appearance** (interface, accent colour, developer tools) · **Hotkeys** |
| **Map** | **Minimap** · **Map POIs** · **Party markers** |
| **Multi-boxing** | **Client roster** · **Party health** · **Window layout** |
| **Tools** | **Action ring** · **Character vendor** · **Chat mod** · **Discord** |
| | **Features** — every feature in your build, with what it does, to switch on or off |

Pages for features you have switched off are not shown. Turning a feature on or off asks to
restart Maps++ when you save, since a feature only fully starts or stops on a restart. Settings on
feature pages show a small **Reset** beside any value you have changed from its default.

Under **Settings → Launching**, each profile has a **Layout after launch**:

- **None — center clients only** — launch the batch without organising it
- **Default — _layout name_** — follow the current default from **Settings → Window layout**
- A named preset or custom layout — pin that profile independently, so your two teams can use
  different arrangements

**Settings → Appearance → Interface** selects the look of Maps++. **Enhanced** gives the panels you
see in Settings and keeps the tray menu and the radial rings ready for an instant response. **Low
memory** uses standard Windows menus and windows, does not load WebView2, opens the client roster as
a list and the Action ring as a popup menu. Maps++ restarts when you save a change here.

The enhanced interface needs the **Microsoft Edge WebView2 Runtime**. It comes with Windows 11
and most up-to-date Windows 10 installs, but some editions and "debloated" or gaming Windows
builds leave it out or remove it. If it is missing, Maps++ uses the low-memory interface instead and
tells you once at startup, and **Enhanced** under **Settings → Appearance** says it needs the runtime.
To get the enhanced interface back, install the
[Evergreen WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703) and restart Maps++.
Your choice stays saved, so you never need to switch modes by hand.

Confirmations, name prompts and diagnostic read-outs follow the same choice. In enhanced mode
they are Maps++'s own dialogs, matching the panel that raised them; in low-memory mode they are
plain Windows message boxes. Either way **Esc** cancels, **Enter** accepts, and a dialog that
asks before deleting something starts with the safe answer selected. Read-outs such as **Verify
signatures** put their table in a scrollable **Details** panel with a **Copy** button.

> **Start Maps++ when Windows starts** is a per-user setting (no admin needed). When enabled, Maps++
> registers itself to launch at login; disabling removes that entry. You can also remove it
> from **Task Manager → Startup apps** — the checkbox reflects the real state either way.

### Developer tools

The tools for checking Maps++ after a game update — **Debug state**, **Calibrate signatures**,
**Verify signatures**, **Reload Maps++**, and a few feature-specific checks — are kept out of the
everyday menu. **Shift+right-click** the tray icon to see them in a **Developer** group for that
one open, or turn on **Settings → Appearance → Show developer tools in the tray menu** to keep it
there. Their fixed shortcuts (**Ctrl+Alt+D**, **Ctrl+Alt+S**, **Ctrl+Alt+V**, **Ctrl+Alt+R**) work
either way.

### Reporting a problem

**Tray → Help → Copy diagnostics** puts everything needed to investigate an issue on your
clipboard, ready to paste into a bug report: the version you are running, which memory offsets
resolved and how, your display layout, which features are on or off, the clients that were running,
and the recent log. It also saves a copy next to the log file. Please include it — without the
version and offset information, most reports cannot be acted on.

**Tray → Help → Open log folder** opens the folder containing `mapsplusplus.log`. Maps++ writes
one line per event there — startup, errors, and anything a feature reported — so a problem that
happened five minutes ago is still recoverable. The log is capped at about 1 MB and the previous
one is kept as `mapsplusplus.log.1`; nothing else is retained.

**Tray → Help → Client mods status…** shows what the osMW client mods report about themselves:
which features are running, which are switched off, and which stood down and why — a feature
whose signature no longer matches after a client update, say, or one **blocked by Windows**
(Smart App Control refuses unsigned DLLs; they are not damaged). It is the same report as the
in-game **F12 → Status** page, read from `osmw_status.ini` in the game folder. That one file is
shared by every client in the folder, so it describes whichever client last started or stopped.
Copy diagnostics includes the same table. Auto-login shows as idle in a client you started by
hand — it only runs in clients Maps++ launches with auto-login on — and that is normal.

When Windows blocks one or two features, the game tells you itself, once, as it starts. When it
blocks `osmw_core.dll`, the file that loads all the others, the game cannot: it just runs without
any mods, after a Windows "Bad Image" box. Maps++ notices this the first time you switch to such a
client and says so once, including whether Smart App Control is the cause. It does not repeat the
message until a client has loaded the mods again. Client mods status then lists the clients
running without the mods first, since the report below it can only come from an earlier client.

**Tray → Help → Client mod logging** is off, and should stay off until someone asks for it. The
client mods can each write a tagged log — `osmw_core.<pid>.log` and one per feature, one set per
client per launch, in the game folder. That is a handful of files every launch for something only
useful while a problem is being chased, so nothing is written unless you turn this on. The switch
is `[Debug] ModLogging` in the mods' own `osmw.ini`; this menu item is a shortcut to it.

If a client-mod problem (chat, auto-login, display, …) is being investigated: switch it on,
**relaunch your clients** (each mod reads the setting once, as the client starts, so windows
already open are unaffected), reproduce the problem, then send the `osmw_*.log` files from the game
folder alongside your diagnostics. Switch it back off afterwards. The diagnostics report says which
state you are in, so an empty game folder is never mistaken for a mod that failed to load.

> **If a feature keeps failing, Maps++ turns it off.** Rather than notifying you once a second
> forever, a repeatedly-failing feature is disabled for the rest of the session and named in one
> notification. This is not saved — restarting Maps++ gives it another chance. Every failure is in
> the log regardless of whether you saw a notification.

### Update notifications

**Settings → Game & startup → Check now** checks whenever you ask, and tells you the answer either way —
up to date, a newer version with a **Get the update** button, or why the check could not be
completed. It works even with automatic checking switched off.

**Check for a newer Maps++ on startup** (on by default) does the same thing once, a few seconds
after launch, but only speaks up when there is something to report. If a newer version exists you
get one notification, the tray menu gains a **Get the update** entry at the top, and **About Maps++…**
shows it. That tray entry lasts for the session only — after a restart it returns once the next check
completes, which is what the **Check now** button is for.

It never downloads or installs anything — it reads a version number and tells you. Maps++ makes no
other network requests, sends nothing about you or your characters (no identifiers, no query string,
not even your own version), and treats every failure as "no update" without saying a word. Turn the
setting off and it makes no requests at all.

---

## Hotkey Reference

Everyday shortcuts can be changed under **Settings → Hotkeys** (click a shortcut, press new keys, then Save). They are grouped the way the tray is — General, Launching, Minimap, Clients, Actions. Choose **Unbind** when an action should have no direct shortcut. Changes apply immediately — no restart needed. The tables below list the **default** bindings.

The default set is intentionally small: core controls and the **Ctrl+Alt+A** Action ring are bound, and so are Chat toggle, Inventory, Party health and Battle action, which keep the chords they have always had; most other actions start unbound. This avoids claiming common game and Windows shortcuts just because a feature is installed; use the ring or tray menu, or bind only the actions you want.

**Keys the client mods use in game.** With the osMW client mods installed, some keys already do
something in game: **F12** opens the settings panel, **F11** cycles fight speed, **F9** shows the
timer panel, plus a leaderboard key and the Chat mod's clear/tab hotkeys if you set them. Maps++ still
lets you bind those, since you may want both — but it warns you when you pick one, and again when
you save, so a key that suddenly does two things is never a surprise. **Copy diagnostics** lists
any overlaps too. (Maps++ reads which keys from the mods' own settings, so a key you moved in the
F12 panel is the one it checks.)

Existing saved bindings are left alone during an update. Use **Reset** (or **Reset all shortcuts to defaults**) if you want the revised defaults.

Global shortcuts work while the game or minimap overlay is focused.

### Core (all variants)

| Hotkey | Action |
|--------|--------|
| **Tab** | Toggle the Maps++ minimap — on a supported map where the game has no minimap of its own (see **Settings → Minimap → Tab key**); otherwise Tab reaches the game |
| **Not bound** | **Open Maps++ minimap** — the overlay even where the game has a minimap too |
| **Right-click** | Close minimap (while overlay is open) |
| **Ctrl+Alt+L** | Launch primary — runs the profile bound to it |
| **Ctrl+Alt+K** | Launch secondary — runs the profile bound to it, if any |
| **Ctrl+Alt+E** | Send Enter to clients still at intro/login until each one is ready |
| **Ctrl+Alt+A** | Open the Action ring |
| **Ctrl+Alt+,** | Open the Settings window |
| **Ctrl+Alt+R** | Reload Maps++ |
| **Ctrl+Alt+Q** | Exit Maps++ |

These are fixed and cannot be rebound — Reload and Exit have to work even when something else is
broken, and the calibration captures read where your mouse is over the minimap. The last three are
the developer tools (see *Developer tools* above):

| Hotkey | Action |
|--------|--------|
| **Ctrl+Alt+1** / **Ctrl+Alt+2** | Capture calibration point 1 / 2 (mouse over the minimap — see *Adding a zone Maps++ doesn't have*) |
| **Ctrl+Alt+3** | Apply calibration from the two points |
| **Ctrl+Alt+4** | Export the current map's calibration |
| **Ctrl+Alt+D** | Debug state |
| **Ctrl+Alt+S** | Calibrate signatures |
| **Ctrl+Alt+V** | Verify signatures |

### Window Layout (Full / Lite)

Arranges your open game windows on one monitor. Window **size is never changed**—only position.
**Minimized clients are left alone**, so a client you deliberately tucked away stays that way —
and so are **maximized** and **Borderless** ones (the client mods' Borderless display covers its
whole monitor, so there is nowhere to move it). Slots are sized for the largest client, so a mix
of 1× and 2× clients does not overlap.

| Hotkey | Action |
|--------|--------|
| **Not bound** | Arrange windows — apply your default layout on the **primary** display |
| **Not bound** | Arrange windows on the **second** display |
| **Not bound** | Open the **Window layouts** window |
| **Not bound** | **Undo** the last arrangement |

**Tray → Clients** has **Arrange windows** (and **Arrange on second display** when you have two),
**Undo arrange** and **Window layouts…**. The window lists your saved layouts and, under
**Built-in**, the presets — each with **Apply** and **Make default** — so any layout is a click away
there rather than in a submenu. **Capture** in that window saves where your clients sit right now.

The Window layout **configuration** lives in **Settings → Window layout**:

- **Default layout** — which layout **Arrange windows** uses. Presets *and* your own custom
  layouts both appear here
- **Main character** — which character’s window is centered or brought to front
- **Target display** — which monitor presets target when you have multiple screens

**Layout presets at a glance:**

| Preset | Good for |
|--------|----------|
| **Grid 2×2** | Four clients in a 2×2 grid |
| **Grid 3×2** | Up to six clients in a 3×2 grid |
| **Center focus** | Main character centered; others in corners |
| **Dice, left / Dice, right** | Five windows in a dice pattern on one side of the screen |
| **Centered** | One window centered |
| **Stack top-left** | Move all windows to the top-left of the work area |

More clients than a preset has slots? The extras no longer land exactly on top of each other —
they **cascade** a little further down and right on each pass, and Maps++ tells you how many.

#### Custom layouts

Presets stop at six slots and treat every client as interchangeable. A **custom layout** stores
one slot per client — up to all eleven — and remembers **which character belongs in which slot**,
so re-applying it always puts the same client back in the same place.

Two ways to make one, and they work together:

1. **Capture.** Drag your clients wherever you want them, then **Capture** in **Tray → Clients →
   Window layouts…** and give it a name. This is the fast path, and it is exact.
2. **Edit.** **Tray → Clients → Window layouts…** opens the editor (or bind **Open Window layouts**): a
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
| **Set as default** | Make **Arrange windows** use this layout |

Drag a box to move it, or select it and use the **arrow keys** (**Shift** for bigger steps).
**Ctrl+S** saves. Boxes are drawn at the client window size recorded when the layout was
captured — applying a layout still never resizes anything.

**Low-memory mode** gets the same layouts without the canvas: **Window layouts…** opens a list of
your layouts and the built-in ones, with Capture, Apply, Rename, Delete, Set as default and Undo.
Capture is the main way to author a layout either way; switch to **Enhanced** under
**Settings → Appearance** for visual editing.

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

Made a mess? **Undo arrange** puts every window back where the last arrangement found it.

### Chat (Full / Battle)

| Hotkey | Action |
|--------|--------|
| **Ctrl+Shift+C** | Toggle chat on **all other** clients (keeps active window’s chat as-is) |
| **Ctrl+C** | Toggle chat on the **active** client only |
| **Ctrl+Alt+C** | Toggle **mini chat size** on all clients |

Useful when multi-boxing: hide chat on alts to reduce clutter, or shrink chat globally. These only
act while a game window is focused, and are also under **Tray → Clients**. **Ctrl+Shift+C** is also
one of the clear-chat keys the Chat mod offers — if you give it to the Chat mod, rebind or unbind
this one, or one of the two will not register. With the Chat mod's *Let me resize* on, the chat
window's size is the mod's, so the chat-size toggle may not stick.

### Chat mod (all variants)

Configured from **Settings → Chat mod**.

`osmw_chat.dll` is one of the osMW client mods, installed with the client rather than by Maps++. This panel is how you configure it, when your game folder has them; when it does not, the panel says so and changes nothing. It improves the game's own chat window rather than drawing a new one:

- A readable font, chosen from four **tested presets** — *Default* (the client's own Courier New), *Crisp*, *Large* and *Wide* — or set by hand with *Custom*. **18 pixels is a hard ceiling**: above it the client cuts the bottom of every line, underscores first. Nothing can be set to avoid that, which is why the presets stop where they do. Pick *Default* to put the chat back to stock.
- Channel tabs with a clickable strip, so you can watch trade without the rest. **Edit tabs…** opens an editor where you name each tab and tick the channels it shows — there is no syntax to learn, and it refuses anything the client would silently cut.
- Highlighting for messages that name you, and for any PM.
- A **blocked-names** list that hides a character's chat on this PC — every character you log in, and every multi-boxed client sharing the game folder. **Ctrl+click** a sender name in the chat window to add them; **Edit blocked names…** is where you remove them. New messages vanish from the chat window and the in-game overlay; leftover overlay lines age out of history. System and Info announcements are never hidden.
- Inline timestamps, mouse-wheel scrolling, and a cooldown message that says how many seconds are left instead of “Please do not flood !”.
- Moving and resizing the chat window freely. The position is remembered **relative to the game window**, so with several clients running each chat window comes back beside its own client instead of all of them stacking in one place. With nothing remembered, the chat window docks against the right edge of its client, exactly where the unmodified game puts it. **Reset chat window position** puts it back there at any time.

Changes apply within about two seconds — there is nothing to restart. Settings are stored under `[Chat]` in the mods' own `osmw.ini` in the game folder, the same file the in-game **F12** panel edits. Maps++ writes **only the settings you changed** when you press Save, so saving something else in Maps++ never undoes a change you made in game. **Reset to Defaults** leaves this tab alone — those settings belong to the client mods — and it does not clear the blocked-names list either; unblocking is always explicit.

Two of its settings are **system-wide hotkeys registered by the mod, not by Maps++**: *Clear chat* and *Cycle tabs*. Leave the matching entries in `[Hotkeys]` alone — binding the same chord in both places means one of them silently fails to register. Maps++ warns you if you try.

Turning **Chat mod** off in **Settings → Features** only hides this page in Maps++. The chat mod itself keeps running; the **Enable the chat mod in game** switch at the top of this tab is what turns it off.

### Inventory (Full only)

| Hotkey | Action |
|--------|--------|
| **Alt+E** | Open inventory on the active client (clicks the inventory button); also **Tray → Actions → Open inventory** |
| **Alt+Shift+E** | Send **Alt+I** to open inventory |

**Alt+E steps aside where the game does better.** In a fight, Alt+E is the game's own **Skill**
command, so Maps++ lets it through. And on a client with the inventory hotkey patch, the game opens
the bag on Alt+E itself, so Maps++ lets it through there too rather than opening the bag and
closing it again. Rebind the action to another key and it works everywhere, as before. The click
lands on the bag button even when the client mods show the game at 2× or 3×.

### Character vendor (Full only)

| Hotkey | Action |
|--------|--------|
| **Not bound** | Open the Character vendor pricing panel for the active client; also **Tray → Actions → Character vendor…** |

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

**Verify vendor slots…** (a developer tool — Shift+right-click the tray icon) dumps the raw price and
item-id arrays next to each other. Use it after a game update to confirm the grid still lines up
with your real inventory.

### Battle action (Full / Battle)

| Hotkey | Action |
|--------|--------|
| **Alt+Shift+Q** | For every client **currently in battle** with a pending action, set battle action 9 for both character and pet; also **Tray → Actions → Battle action** |

Use this to confirm or repeat the same battle command across all fighting characters at once.
Nothing is typed into the clients — the action value is written straight into each one, so it
works on every fighting client at once regardless of which window has focus.

### Skill hotkeys (Full / Battle)

Choose **Tray → Actions → Skill hotkeys…** to create character profiles and bind their
battle skills. Profiles are created only when you ask, so bank characters and other mules do
not clutter the editor. A saved profile remains editable while its character is offline.

1. Choose **Create**, then select a running character or enter its exact name and class.
2. Select that profile and choose one of its four skill families and tiers I–IV.
3. Press the shortcut when prompted. Add as many bindings as you need, then choose
   **Save hotkeys**.

The same shortcut can mean something different for each character. For example, **F1** can
select **Stun IV** while one Human is active and **Heal Other IV** while a Centaur is active.
It never switches clients: the character's own `main.exe` window must already be active, and
the shortcut only acts while that client is in battle. Pressing another skill hotkey before
the turn resolves replaces the previously selected action.

Player skills are supported now. Pet skill/action offsets are reserved in the addon for future
support, but pet bindings are not shown until Maps++ can reliably identify a pet's four skills.

### View mode (Full / Battle)

| Hotkey | Action |
|--------|--------|
| **Not bound** | Cycle view mode on **all** clients (0→3, wrap) |
| **Not bound** | Toggle view mode 0/1 on **all** clients (0→1; 1/2/3→0); also **Tray → Actions → Toggle view mode** |

Echoes the in-game **F2** view-mode cycle across every open client without focusing each window.

### Auto-login (Full / Lite / Battle)

| Hotkey | Action |
|--------|--------|
| **Not bound** | Open **Accounts** |

**Accounts** is the list of *accounts to log in as* — not the Client roster, which shows the
clients already running.

Choose **Tray → Clients → Accounts…**. Add a row per character: account name, password, character (`#1`–`#5` for the slot on that account, or the numeric display ID), and an optional **group**. Up to 16 rows.

**Duplicate** is the fast way to add the other characters on one account: select a row, click it, and you get a new row with the same account, the same group, the stored password already carried across, and the character slot advanced by one. You never retype a password you cannot read back — which is what made a typo in five identical rows so easy before.

**Groups** tag the rows that launch together (`Main`, `Alts`, whatever you like). A launch profile or **Tray → Launch other** can then run a whole group, so eleven characters split cleanly across two teams instead of depending on which client happens to finish loading first.

**Launch this account** starts one client logged in as the selected row, and **Launch from here**
starts a batch beginning with it, as many as Launch primary's profile starts. The dropdown beside
them picks whose display they borrow. Both use the saved accounts, so save new rows first.

Passwords are write-only. After save the field is empty and the hint says the password is stored. They are encrypted for **this Windows user** in `login_roster.dat` next to `main.exe`. Another Windows account on the same PC cannot read them, and they never appear in `config.ini`, `mapsplusplus.log`, or the roster page after save. Group names are *not* in that file — they live in Maps++'s own `config.ini`, so the roster file's format is unchanged.

**Settings → Launching → Log launched clients in automatically** is what the launch shortcuts honour. On (the default) clicks through the intro on the new windows only and lets each client claim the next roster row. Off leaves the stock login — already-playing clients are not sent Enter. An empty roster (or every row disabled) also leaves the stock login. Turning **Auto-login** off in **Settings → Features** only hides Accounts and the group launches in Maps++; the Launching setting above is what turns auto-login off. Auto-login itself is a client mod (`osmw_login.dll`), so it needs the client mods installed.

**Auto-login only ever applies to clients Maps++ itself launched.** Consent travels to the mod in an environment variable that exists only for the moment Maps++ spawns the clients, so a client you start yourself — running `main.exe` from Explorer, say — always gets the stock login screen, whatever your Maps++ settings say. That is also why **Client Mods Status** lists auto-login as idle in such a client. With **Tray → Help → Client mod logging** on, the tagged `osmw_core.<pid>.log` in the game folder names which gate stopped the managed login feature, if either did.

Use a distinct account per window. If the client says the account is already logged in, that is a server rule — Maps++ will not work around it. For the same reason, **an account you pick explicitly is never swapped for another**: if a live client already holds it, the new window simply stays at the stock login screen — and says `requested row … unavailable` in its `osmw_login.<pid>.log`, if client mod logging is on.

The osMW client mods are installed and updated separately -- Maps++ neither ships nor deploys them, and everything except the Chat mod page works the same on a client without them. Browse in **Settings → Game & startup** points at `main.exe` (or at a launcher, if your client needs one to load the mods); overlay attach is still `main.exe`. The line under the path, and **Tray → Help → Copy diagnostics**, say whether the mods are present in your game folder.

---

### Launch profiles and Launch other (all variants)

A **launch profile** answers everything about one launch:

| Field | Meaning |
|-------|---------|
| **Name** | What it is called in Settings and in the tray (**Launch _name_**) |
| **Accounts** | *Next free roster row* (the old behaviour), a **group**, a batch **starting at** one account, or exactly **one** account |
| **Clients** | How many windows. A group always launches exactly as many clients as it has rows, and one account exactly one, so the field is fixed for those |
| **Display** | Which monitor the new windows are centered on |
| **Layout** | Layout applied after the batch (Full / Lite — needs Window Layout) |

Edit them under **Settings → Launching**, where two dropdowns say which profile **Launch primary** and **Launch secondary** run (secondary can run **None**). A typical eleven-character setup is one profile per team:

| Profile | Accounts | Display | Bound to |
|---------|----------|---------|----------|
| Main team | Group: Main (5) | 1 | Primary — **Ctrl+Alt+L** |
| Alt team | Group: Alts (5) | 2 | Secondary — **Ctrl+Alt+K** |
| Mule | Only: Kilo (#3) | 1 | — |

Anything **not** bound to a hotkey is still one click away under **Tray → Launch other**:

```
Launch other                  ▸
   Mule — 1 client
   ──────────────────
   Group: Main — 5 accounts
   Group: Alts — 5 accounts
```

Main team and Alt team are absent from that list on purpose — they already have their own tray
items and hotkeys just above it. A group launched from here uses Launch primary's display.

To launch one particular account, or a batch starting from one, use **Launch this account** and
**Launch from here** in **Tray → Clients → Accounts…** — those change no setting. Launch other does
not appear at all when there is nothing to put in it — no saved groups and no unbound profiles,
which is what a fresh install looks like.

### Client roster (Full / Battle)

| Hotkey | Action |
|--------|--------|
| **Not bound** | Show / hide the client roster |

By default the roster is a **ring of your running clients around the cursor** — click one to bring
it to the front. **Settings → Client roster** can open it as a list instead: a small always-on-top
window listing every running client with its character, current zone and status (**Playing**,
**In battle**, **Loading**, **Not ready**), where **double-clicking a row** brings that client to
the front. The low-memory interface always uses the list.

It only appears when you ask for it — choose **Clients** in the Action ring, bind the action, or use
**Tray → Clients → Client roster**. If you would rather the list opened itself whenever two or more
clients are running, tick that in **Settings → Client roster**; closing it by hand still keeps it
closed for that session.

Use it with **Ctrl+Alt+E** (*Send Enter Until Ready*): the roster is what shows you which alt is
still sitting at a login prompt.

#### Health and mana

The list view shows each client's **HP and MP** as a bar with the figures beside it, so you can see
at a glance which alt is in trouble without switching to it. Below a quarter health the HP bar turns
red — but the numbers are always there, so you are never relying on the colour alone.

Each client shows **its own** health, whoever is leading the party — and it keeps updating during a
fight, which is the moment it matters.

A client still at a login or character-select screen shows **—**: there is no character loaded to
have health. The same **—** appears in the rare case where a fight is running and Maps++ cannot work
out which combatant you are — two party members built identically enough to be indistinguishable.
Showing nothing is deliberate there: the alternative is a bar that might belong to somebody else,
and a health bar you cannot trust is worse than no health bar.

### Party health (Full / Battle)

A strip along the bottom of the client you are playing, with one cell per character, for checking
on your alts without switching to them.

| Hotkey | Action |
|--------|--------|
| **Ctrl+Alt+H** | Peek at party health — shows the strip for a few seconds; also **Tray → Clients → Party health** |

By default it is **not** on screen permanently. Press the key, read it, and it takes itself away
after three seconds; press again to dismiss it sooner.

**Clicking another window also dismisses it.** A peek belongs to the client you opened it on — it is
answering a question about *that* character's fight — so switching away ends it rather than dragging
the strip onto the new window. Press the key again on the client you have moved to and you get its
fight, freshly read. Left up all the time it is just another gauge
competing with the ones the game already draws.

Each cell shows the character name, then its health, mana and — during a fight — its pet, each as a
figure with a hairline bar under it. The health bar runs **green through amber to red** as health
falls, so a character sliding into trouble looks different from one that is merely not full. The
numbers are always there, so colour is never the only signal.

**It sits on the window you are playing.** The strip is centred along the bottom edge of the active
client's game picture and follows you when you switch windows, so in a multi-box layout it is always
on the client you are looking at. It is never wider than that client: cells share the available
width and shrink as more characters appear.

**It grows with the game.** When the client mods show the game at 2× or 3×, the strip is drawn 2× or
3× as well, bars and text alike, so it keeps the same size next to the game's own gauges. In a
Resizable or Borderless window it sits on the picture, above any black bar.

There is no saved position and nothing to drag, deliberately — a fixed coordinate would be wrong
the moment you alt-tabbed to a window somewhere else on screen.

**It never intercepts your clicks.** The HUD is click-through, so a click that lands on it goes to
the game underneath as though it were not there. It also never takes focus, so it can appear in the
middle of a fight without pulling you out of the game.

**Settings → Party health** changes when it appears:

| Setting | Meaning |
|---------|---------|
| Only when I press the key, for a few seconds | *(default)* Never appears on its own |
| Automatically while a client is in battle | Appears when a fight starts and disappears when it ends, listing **only the clients actually fighting** |
| Always, whenever a client is running | Permanently on screen, listing every client |

**Seconds a peek lasts** sets how long the key holds it up, between 1 and 60.

**It shows one fight.** If a battle is running, the strip lists the characters in the fight that the
window you are playing is in — at most five, which is a full party. Boxing six clients across two
separate battles, you get the battle you are looking at rather than both parties' bars in one row;
switch to a client in the other fight and peek again, and the strip follows.

Peeking while nothing is fighting shows every running client instead, since checking on idle alts is
the other reason to press the key. The automatic battle mode shows nothing at all out of combat, and
leaves an idle alt out of a fight it is not in — rows you do not need push the ones you do further
from where you are looking.

Cells show **—** for a client with no character loaded, or in the rare case where two party members
are built identically enough that Maps++ cannot tell which combatant you are.

To turn the strip off entirely, switch **Party health** off in **Settings → Features**.

### Action ring (all variants)

| Hotkey | Action |
|--------|--------|
| **Ctrl+Alt+A** | Open / close the Action ring; also **Tray → Actions → Action ring** |

The same ring as the client roster, but the spokes are shortcuts instead of characters: view
mode, battle action, inventory, chat, window layout (primary or second display), POIs, the
Character vendor pricing panel, and settings. Press the hotkey and the ring
appears **at your cursor** — move onto a spoke and left-click to fire it. Right-click, **Esc**,
a click in the gaps, or the hotkey again all dismiss it, and a click in the gaps does *not* also
reach the game.

It works from anywhere, including with the game unfocused: actions that act on "the active
client" use whichever client you were last in, and Maps++ brings that window back to the front
before firing. If no client is running at all you get a tray notification rather than a
misfire.

Spokes for **Minimap**, **POIs** and **Party markers** carry a small dot showing whether that
layer is currently on. View mode and chat size have no dot — their state lives in the game's
memory, and reading it on every open would make the ring feel slow.

**Settings → Action ring** chooses which shortcuts appear and in what order (up to ten), using
checkboxes and the ↑/↓ buttons. Only actions from features you actually have are listed, and a
spoke for a feature you switch off drops out, so the ring is always correct for your build — the
Lite variant, for instance, starts with Arrange windows, POIs and Settings. The centre of the ring
opens that settings page.

### Map POIs (Full / Lite)

Marks NPCs, shops, portals, quest spots and notes on the minimap.

| Hotkey | Action |
|--------|--------|
| **Not bound** | Add a POI where you are standing |
| **Not bound** | Show / hide the POI layer (remembered between sessions) |
| **Not bound** | Append an NPC entry for your position to `npc_generated.txt` |

Adding one works the way mapping already worked: **stand where the thing is and choose Add POI
here** in **Tray → Minimap** or the Action ring, or assign it a shortcut. The position is read from game memory, so it lands exactly where you stood — no
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
coordinates are `(raw X + 400) ÷ 16` and `(raw Y + 300) ÷ 8`, applied for display only:

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

**Tray → Minimap** has **Show POIs**, **Add POI here** and **Manage POIs…** (list and delete for the
current map). Contributors to the server repo have one more, among the developer tools
(Shift+right-click the tray icon): **Export this map's POIs (server format)** writes every POI in
the server repo's NPC entry format — the same shape the **Write an NPC entry for here** action
produces for a single position — into `npc_generated.txt` and onto the clipboard:

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

#### Sharing POIs with other players

That export is for the **server repo**. To share with another *player*, use
**Export POI pack…** under **Settings → Map POIs** — it writes every POI you have, across every
map, as an ordinary `pois.ini`-shaped file you can send to someone. The sharing buttons act at
once; they do not wait for Save.

**Import POI pack…** reads one back. It **merges**: your own POIs are never removed, and
anything already present — same map, same spot, same name — is left alone rather than
duplicated, so importing the same pack twice does nothing the second time. Two different NPCs
standing on the same spot are still treated as two POIs, since the name is part of the match.

Imported labels go through the same cleaning as ones you type, so a pack from elsewhere cannot
corrupt your file. Anything unusable is skipped and counted in the summary rather than
silently dropped.

#### Putting POIs on the game's own minimap

The client mods give the game's minimap a marker layer of its own. **Settings → Map POIs →
Export to game markers…** copies every POI you have, across every map, onto it, so they show without the Maps++
overlay open. They go into `map-markers.tsv` in the game folder, which every client there shares,
and running clients pick them up within a couple of seconds.

- **Your in-game markers are safe.** Maps++ only ever replaces markers it exported itself.
  Export again after changing your POIs and its markers are updated, and markers for POIs you
  deleted are taken out. Anything you placed in the game is left exactly as it was.
- **Remove exported game markers…** takes all of Maps++'s markers back out. Maps++ remembers
  which markers are its own in `map_markers_export.ini`, next to its other files.
- An export is a snapshot. Adding a POI in Maps++ doesn't change the game's markers until you
  export again. If you edit or delete one of Maps++'s markers in the game, the next export puts it
  back the way Maps++ has it.
- **Limits are the game's.** It keeps at most 64 markers per map, counting your own, and 2048 in
  all. Labels are cut to 31 characters (fewer for letters outside your Windows language).
  Anything that does not fit is counted in the summary.
- **The marker layer ships switched off** (`[Minimap] MarkerLayer=0` in `osmw.ini`). Exporting
  still works, and the export tells you, but the markers only show in game once the layer is on.
- It needs the client mods installed in your game folder. Without them there is no marker layer,
  and Maps++ says so instead of writing anything.

### Party markers (Full / Battle)

| Hotkey | Action |
|--------|--------|
| **Not bound** | Show / hide the party marker layer (remembered between sessions); also **Tray → Minimap → Party markers** |

Every *other* client running on the same map as you appears
as a coloured dot, labelled with the character name, alongside your own standard marker. Alts
that are loading, in battle, or in a different zone are not drawn (their position would be
stale).

Each client keeps the **same colour** for as long as it is running, so you can learn which dot
is which. An alt zoning out or dropping into a battle does not renumber the others; the colour
is only released once that client closes.

Name labels are shown while you play and **hide while the mouse is over the minimap**, so the
map underneath stays readable when you look at it directly. **Settings → Party markers → Show
names** switches this to *Always* or *Never*.

### Discord (Full only)

Shows the map you are on as your Discord status ("Rich Presence"), for friends who want to know
where to find you, with your character's name underneath (or *In battle* during a fight). It is
**off** until you turn on **Settings → Discord → Enable Rich Presence**, and it needs the Discord
desktop app running on the same PC: Maps++ talks to that app on your own machine, and sends nothing
over the network itself.

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
LaunchAutoLogin=1
MultiClientCount=1
MultiClientDelay=0
PrimaryMonitor=0
SecondaryMonitor=0
PrimaryLaunchLayout=__default__
SecondaryLaunchLayout=__default__
PrimaryProfile=1
SecondaryProfile=2

[LaunchProfileIndex]
1=Main team
2=Alt team

[LaunchProfile.1]
Accounts=group:Main
Count=5
Monitor=1
Layout=__default__

[LaunchProfile.2]
Accounts=group:Alts
Count=5
Monitor=2
Layout=__default__

[Minimap]
Source=game
Scale=100
FollowGameScale=1
Opacity=100
Anchor=Center
OffsetX=0
OffsetY=0
KeepOpenOnFocusLoss=0
ShowHoverCoords=1

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
ConfirmHigh=0
WarnAbove=10000000
PresetClears=0
IconBase=0

[PartyHealth]
Mode=peek
PeekSeconds=3
ShowPets=1

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
AutoLogin=1
ChatMod=1
```

| Setting | Meaning |
|---------|---------|
| `GamePath` | Full path to the executable a launch starts: `main.exe`, or a mod launcher if your client needs one. Maps++ runs exactly what is set here |
| `GameArgs` | Extra command-line arguments passed when launching the game |
| `LaunchOnStartup` | `1` = launch a game client when Maps++ starts |
| `LaunchAutoLogin` | `1` = launch shortcuts skip the intro and log in from the roster; `0` = stock login |
| `MultiClientCount` | Client count for a profile saved before profiles carried their own (`Count=0`). New profiles always store a count; installs from before this version keep the `5` they had |
| `MultiClientDelay` | Milliseconds between each client in a launch batch |
| `PrimaryProfile` / `SecondaryProfile` | Which `[LaunchProfile.<id>]` **Ctrl+Alt+L** / **Ctrl+Alt+K** runs. `SecondaryProfile=none` = Launch secondary runs nothing and is left out of the tray |
| `PrimaryMonitor` / `SecondaryMonitor` | Mirror of the bound profiles' `Monitor`, kept for the startup single-client launch. `0` = OS primary (or first non-primary for secondary) |
| `PrimaryLaunchLayout` / `SecondaryLaunchLayout` | Mirror of the bound profiles' `Layout` |

Launch profiles use two sections, the way `layouts.ini` does — `[LaunchProfileIndex]` maps a
numeric id to the display name, and one `[LaunchProfile.<id>]` section holds the rest, so renaming
a profile never breaks the bindings above. On first run after upgrading, two profiles are created
from your existing `Primary*` / `Secondary*` keys, so nothing about your launches changes.

| Profile key | Meaning |
|-------------|---------|
| `Accounts` | Blank = next free enabled roster row. `group:<name>` = every row in that group. `start:<rowId>` = `Count` rows from that one onward. `only:<rowId>` = exactly that row |
| `Count` | Clients to launch. `0` (older profiles only) = use `MultiClientCount`; a `group:` always launches its own row count |
| `Monitor` | `0` = the target's default display; `1`, `2`, … = a specific one |
| `Layout` | Blank = center only, `__default__` = follow `DefaultLayout`, or a preset/custom layout name |

`[AutoLoginGroups]` maps a roster row id to its group name. It lives here rather than in
`login_roster.dat` because the auto-login mod has no use for group names, and changing that file's
format would stop an older already-deployed `osmw_login.dll` from reading it at all.
| `Source` | What **Tab** opens: `game` (the game's own minimap where it has one, Maps++ elsewhere — the default), `mapspp` (Maps++ wherever it has a map) or `off` (Tab always reaches the game). Set from **Settings → Minimap → Tab key** or the setup guide |
| `SourceNoticeShown` | `1` once the one-time "Tab now opens the game's own minimap" notice has appeared. Clear it to see it again |
| `Scale` | Minimap size, 50–200 % of the standard 400×300 |
| `FollowGameScale` | `1` (the default) = multiply `Scale` by the game's own display scale when the client mods show it at 2× or 3×; `0` = always exactly `Scale` |
| `Opacity` | Minimap opacity, 30–100 % |
| `Anchor` | Where the minimap sits on the game picture: `Center`, `TopLeft`, `TopRight`, `BottomLeft`, `BottomRight` |
| `OffsetX` / `OffsetY` | Pixel nudge from the anchor (negative allowed). Dragging the minimap writes these for you |
| `KeepOpenOnFocusLoss` | `1` = keep the minimap open when you Alt+Tab away |
| `ShowHoverCoords` | `1` = show in-game coordinates in the corner while the mouse is over the minimap |
| `LabelMode` | When marker labels are drawn: `autohide` (shown, except while the mouse is over the minimap), `always`, or `never`. Applies to both POI labels and party marker names |
| `LayerVisible` | `0` = that layer stays hidden. Set by the POI/party **Show/hide layer** actions, so a layer you switch off stays off next session |
| `DefaultKind` | Type pre-selected when adding a POI: `npc`, `shop`, `portal`, `quest`, `note` |
| `Actions` | Action ring contents: hotkey action ids, comma-separated, in ring order (first at 12 o'clock, then clockwise). Ids for features this build doesn't have, or that are switched off, are ignored, so one file works across variants. Set from **Settings → Action ring** |
| `Prewarm` | `1` = load the Action ring at startup so the first press is instant; `0` = build it on first use, saving memory |
| `Mode` | When the party health HUD appears: `peek` (only while its hotkey holds it up), `combat` (while a client is fighting, listing just those clients), or `always` |
| `PeekSeconds` | How long a peek lasts, 1–60 |
| `ShowPets` | `1` = include a pet health bar in each cell during a fight |
| `DefaultLayout` | Layout **Arrange windows** uses — a preset key (`Grid2x2`, `Grid3x2`, `CenterFocus`, `DiceLeft`, `DiceRight`, `Single`, `Reset`), or the name of one of your custom layouts |
| `MainCharacter` | Character name for center-focus layouts |
| `TargetMonitor` | `0` = primary; `1`, `2`, … = specific display |
| `MainCharacterAsked` | `1` once the first-run "which is your main character?" prompt has been shown. Set whether you answered or dismissed it, so it is only ever asked once — clear it to be asked again |
| `VersionCheck` | `0` disables the startup update check, after which Maps++ makes no network requests at all |
| `ConfirmHigh` | `1` = ask for confirmation before applying a large vendor price. Off by default; the panel already colours prices by magnitude and never opens your vendor for you |
| `WarnAbove` | The price at which that confirmation kicks in, once `ConfirmHigh` is on |
| `PresetClears` | `1` = loading a shop preset also clears prices on slots the preset doesn’t mention. Off by default, so presets only add |
| `IconBase` | Whether the client’s item id is `0`- or `1`-based, which decides how thumbnails are matched. Set it from **Settings → Character vendor** after comparing one known item |
| `ItemIdOffset` | Override for the inventory item-id address, e.g. `0x2E2028`. Only needed if a game update moves it; blank uses the built-in value (or whatever signature calibration resolves), `0` turns thumbnails off. When set, it wins over signature calibration — which is the point, since the case it exists for is calibration landing on the wrong address |
| `[Hotkeys]` | One entry per rebindable action, in AutoHotkey chord syntax (`^`=Ctrl, `!`=Alt, `+`=Shift). A blank value means **Not bound**; rebind or unbind from **Settings → Hotkeys** |
| `[Addons]` | `0` = disabled, `1` = enabled (also toggled from **Settings → Features**) |
| `[UI] DeveloperTools` | `1` = always show the tray's **Developer** group; `0` (the default) = only on Shift+right-click |
| `[Setup] Completed` | How this install met the setup guide: `1` finished, `skipped`, or `upgrade` (an install from before the guide, never shown it). Delete it to see the guide at the next start; **Help → Setup guide…** runs it any time |
| `[ClientMods] CoreRefusedNoticeShown` | `1` once the "client mods did not load" message has appeared; Maps++ sets it back to `0` when it next sees a client with the mods loaded. Clear it to see the message again |
| `[Debug] ModLogging` | Not in this file. `1` = the client mods write their own per-client `osmw_*.<pid>.log` files into the game folder; `0` (the default) = they write nothing. Like `[Chat]` below, this key belongs to the mods and lives in their own **`osmw.ini` next to `main.exe`**. Toggle it from **Tray → Help → Client mod logging** |

After editing `config.ini`, restart Maps++ — **Ctrl+Alt+R** reloads it.

### Chat mod settings (`[Chat]`)

These belong to `osmw_chat.dll`, not to Maps++, and live in the client mods' own **`osmw.ini` next to `main.exe`** — never in Maps++'s `config.ini`, even when the two files sit side by side. Set them from **Settings → Chat mod** or the in-game **F12** panel; the mod re-reads the file about every two seconds and applies changes live, so there is nothing to reload or restart. Maps++ writes only the keys you change, one at a time, and never on its own.

| Setting | Meaning |
|---------|---------|
| `FontFace` | Chat font. Empty = the client's own Courier New |
| `FontSize` | Pixel height. `0` = the client's own (16). **18 is the hard ceiling** — above it the client clips every line |
| `FontWidth` | Forced character cell width. `-1` lets GDI choose; `8` matches the client's own cell |
| `Tabs` | Tab set, `Name:channels` separated by `\|`. `*` is every channel, a leading `-` removes one. Channels: `local party pm guild market world system info fd gm` |
| `TabStrip` | `1` = draw the clickable tab strip across the top of the chat pane |
| `TabHotkey` | System-wide hotkey that cycles tabs. Empty = unbound |
| `Timestamps` | `1` = show the time each message arrived |
| `TimestampFormat` | `%H` `%M` `%S` plus the client's colour markup, e.g. `#c808080[%H:%M]#N` |
| `WheelScroll` | `1` = scroll the chat with the mouse wheel |
| `WheelScrollLines` | Lines per notch. `0` follows your Windows mouse setting |
| `CooldownMessage` | `1` = say how many seconds are left instead of "Please do not flood !" |
| `CooldownFormat` | The wording. `%d` seconds, then `%s` plural, then `%s` channel — **in that order** |
| `HighlightMentions` | `1` = mark messages naming you, and every PM |
| `MyName` | Override for your character name; normally read from the game window caption |
| `BlockedNames` | Comma-separated character names this client hides. Shared by every client using this game folder. **Ctrl+click** a sender name to add; **Edit blocked names…** to remove. System/Info are never dropped. Overlay leftovers age out of the 1024-slot ring. **Reset to Defaults does not clear this list** |
| `FreePosition` | `1` = stop the chat window snapping to the client's edge. It is still placed once when it first appears |
| `UnlockSize` | `1` = allow resizing |
| `RememberGeometry` | `1` = persist position and size across restarts. Position is stored as an offset from the game window, so each client's chat returns beside its own client. Turning it off re-docks the window |
| `ResetPosition` | A command, not a setting. `1` = forget the saved position and re-dock; the mod writes it back to `0` once done |
| `ClearHotkey` | System-wide hotkey that empties the chat. Empty = unbound |

`ClearHotkey` and `TabHotkey` are registered by the mod itself, not through `[Hotkeys]` — do not bind the same chord in both places or one will silently fail to register.

`[Chat]` also holds keys this panel deliberately never touches: `WinRelX`/`WinRelY`/`WinW`/`WinH`, which the mod writes for you when you finish dragging or resizing, the chip and tab-strip font keys, `MentionPrefix`, and the `Probe*` diagnostics. `BlockedNames` is edited from **Edit blocked names…** (and by Ctrl+click in chat), not from the Settings Save of the other fields, so a Save cannot clobber a name you just blocked. Every one of them is documented inline in `defaults.ini`.

`WinX`/`WinY` are the older absolute position pair. The mod still reads them, converts them to a `WinRelX`/`WinRelY` offset the first time it restores the window, and then removes them — so upgrading keeps your chat window where it was, and nothing needs redoing by hand.

### Skill hotkey profiles (`better_hotkeys.ini`)

The Skill hotkeys editor owns this file and stores it beside `config.ini` (or in the same
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

### Accounts (`login_roster.dat`)

Owned by the Auto-login editor, stored **next to `main.exe`** (not beside Maps++ and not in AppData). Binary, DPAPI-bound to the current Windows user — do not copy this file between machines or Windows accounts. Edit it from **Tray → Clients → Accounts…**. Passwords are never written to `config.ini`. Whether launch shortcuts use it is **Settings → Launching → Log launched clients in automatically** (`LaunchAutoLogin` in Maps++ `config.ini`), not this file.

Row **groups** are the one part of a row that is *not* here — they are in Maps++'s `config.ini` under `[AutoLoginGroups]`, keyed by row id. Copying the roster to another Windows account would not work anyway (DPAPI), and keeping this file's format fixed means an older `osmw_login.dll` already deployed in a game folder can still read it.

### Shop presets (`shop_presets.ini`)

Character vendor presets live beside `config.ini` in the same way. The editor writes this for
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

1. Start **Maps++** (tray icon appears). The first time, the setup guide asks how you play —
   choose **Several characters at once** and set your client count, displays and accounts there.
2. **Ctrl+Alt+L** — Launch primary starts your main team.
3. **Ctrl+Alt+K** — Launch secondary starts your second team on the other display (if you set one up).
4. **Settings → Window layout → Main character** — choose your main.
5. Choose **Layout** in the Action ring (or **Tray → Clients → Arrange windows**) to snap everyone
   into your default grid. Once you have your windows exactly where you like them, **Capture** in
   **Tray → Clients → Window layouts…** saves that arrangement, and **Set as default** makes
   Arrange windows restore it every session.
6. In game on a supported map, press **Tab** on your main window for the minimap.
7. Choose **Chat (others)** in the Action ring to hide chat on alts during farming.
8. In battle, choose **Battle action** to confirm the same action on every fighting client.

---

## Troubleshooting

### Minimap does not open when I press Tab

- **Tab may be opening the game's own minimap instead.** On a map where the game has a minimap,
  Tab goes to the game by default. Use **Open Maps++ minimap** (bind it under Settings → Hotkeys,
  or Tray → Minimap), or set **Settings → Minimap → Tab key** to *Maps++ wherever it has a
  map*. **Debug state** (**Ctrl+Alt+D**) shows the *Tab source* line, saying where Tab goes right
  now and why.
- Make sure the **game window is focused** (click it first).
- Confirm you are **not in battle** and not on a loading screen.
- Check that your current zone has a map file in `maps\` (supported maps ship with the release).
- If you run **multiple** `main.exe` instances, Maps++ tracks whichever game window you last focused.

### "Client mods did not load"

Windows refused `osmw_core.dll`, so that client is running without any of the client mods. Maps++
is unaffected. If the message says Smart App Control is on, that is the cause: the mods are not
code-signed, and Smart App Control refuses unsigned files however they were obtained. Otherwise,
reinstall the client mods first; on a work or school PC, a security policy may be what blocks
them. **Copy Diagnostics** shows each running client's state under *Minimap → client mods*.

### Position marker is wrong or stuck

- Maps are calibrated per zone. If a map was recently added or updated, report it to the Maps++ / osMW community.
- Ensure `marker.png` is present and Maps++ was not blocked by antivirus from reading game memory.

### Maps++ asks where the game is when I launch

It does not know where `main.exe` is yet (the setup guide was skipped, or the game moved). Choose
**Choose main.exe…** in that message, or set it under **Settings → Game & startup → Browse…**
(**Ctrl+Alt+,**). It is remembered from then on.

### “Map folder missing” notification

The `maps\` folder must sit next to `mapsplusplus.exe`. Re-extract the full release package.

### Hotkeys do nothing

- Another program may be using the same shortcuts.
- Check **Settings → Features** — the feature may be switched off.
- Check **Settings → Hotkeys** — the shortcut may have been rebound or unbound.
- Restart Maps++ after editing `config.ini` by hand.

### Only one monitor / secondary launch centers on primary

With a single display, **Ctrl+Alt+K** still launches a client but centers it on the only monitor available. If **Ctrl+Alt+K** only says *Launch secondary isn't set up*, choose a profile for it under **Settings → Launching**.

### More than two monitors / wrong displays used

By default **Ctrl+Alt+L** uses your OS primary display and **Ctrl+Alt+K** uses the first non-primary one. If you have three or more screens and want different ones, pick the **Display** of each launch profile under **Settings → Launching**. A display that isn't currently connected falls back to the default behaviour.

---

## Tips

- The minimap closes if you **Alt+Tab** to another application, so it stays out of the way when you are not playing (**Settings → Minimap** can keep it open instead).
- You can **switch off features** you do not use from **Settings → Features**: their menu items and hotkeys go with them.
- **Lite** is ideal if you only want the minimap, launching and window layout without combat or inventory shortcuts.
- **Battle** is a smaller build focused on combat multi-boxing and chat control.

---

## Supported Maps

Custom images are provided for many osMW zones (files in `maps\`, named like `MAP301.jpg`, `MAP302.jpg`, etc.). If you enter an area without a custom image, Tab will not open the overlay until you reach a supported map.

New maps are added over time; keep your `maps\` folder updated with the latest release if you play newer content.

---

## Need Help?

Maps++ is built for the osMW community. If something breaks after a **game client update**, offsets may need refreshing—check osMW forums or Discord for an updated Maps++ build.

For bugs or feature requests, contact the Maps++ maintainers through your usual osMW community channels.
