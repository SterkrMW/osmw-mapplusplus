# The native mods are a separate product

`patches/` is gone from this repository. The DLL mods live in the osMW server
repo:

    mw-tools/tools/client-mods/

## Why

The mods and the server share a wire protocol - the `0x7F000000` packet range,
whose server half is `PacketType.ts`. Keeping the two halves in separate repos
let them drift, and it had already happened: the (now scrapped) fishing mod
shipped three packet types the server had never heard of, and nothing could
have noticed.

## Maps++ no longer ships or deploys them

Maps++ used to carry the built DLLs in `Lib/` and copy them into the game
folder before every launch. That was a temporary measure from early
development, and it is gone: the mods reach a game folder by their own route,
`Lib/` holds only the WebView2 libraries, and `build.ps1` has no native payload
step, no `-SkipMods` switch and no module manifest to keep in step.

The two products are also no longer allowed to share settings files:

| File (game folder) | Owner |
|---|---|
| `osmw.ini`, `osmw_defaults.ini` | the mods |
| `config.ini`, `defaults.ini`, `signatures.ini`, `offsets_cache.ini` | Maps++ |

They used to be one pair, and it went wrong in both directions: `[Addons]`
mixed Maps++ addon toggles with module gates, `[Minimap]` mixed the overlay's
keys with the minimap module's, and Maps++ rewrote `[Addons] ChatMod` on every
launch over whatever the in-game F12 panel had set. The mods carry a pre-split
`config.ini` across once, on their first start after the rename, and then leave
it alone.

Maps++ may still *edit* `osmw.ini` - the Chat Mod settings tab, and Tray ->
Debug -> Client Mod Logging - but only when the user asks, and only through
`native_mods.ahk`, which is the whole of the contract. Nothing here requires
the mods to be installed: with no `osmw_core.dll` beside `main.exe`, the tab
says so and everything else works as before.

## History

The sources' history up to the move is in this repository. `git log -- patches`
still works here.
