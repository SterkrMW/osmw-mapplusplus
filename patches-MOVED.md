# The native mods moved

`patches/` is gone from this repository. The nine DLL mods now live in the
osMW server repo:

    mw-tools/tools/client-mods/

## Why

The mods and the server share a wire protocol - the `0x7F000000` packet range,
whose server half is `PacketType.ts`. Keeping the two halves in separate repos
let them drift, and it had already happened: the (now scrapped) fishing mod
shipped three packet types the server had never heard of, and nothing could
have noticed.

Sources, the server that answers them, and the design notes in
`tools/client-dll/` are now in one history.

## What this means for a release

`build.ps1` treats `patches/` as optional and will ship `Lib/` as-is without
it, which is why nothing here breaks. But the staleness check that lived in
`patches/common/modhash.py` went with the sources, and that check exists
because `Lib/` and the sources drifted once before: a fix landed in the C
source while `Lib/` kept shipping the previous build, so a fresh install
silently got the old behaviour.

**So before cutting a release, rebuild the DLLs in the server repo and copy
them into `Lib/` here:**

    cd <mw-tools>
    python tools/client-mods/core/tools/build.py --test
    python tools/client-mods/<mod>/tools/build.py --test     # for each mod
    copy build\client-mods\*.dll  <maps++>\Lib\
    copy build\client-mods\*.exe  <maps++>\Lib\
    copy build\client-mods\osmw_core_api.ini  <maps++>\Lib\

`build/client-mods/` in the server repo is generated and untracked; `Lib/`
here stays tracked, because it is what a release ships.

## The scrapped fishing mod

`osmw_fishing.dll` was dropped for quality. Its sources and MDA are archived in
the server repo under `archive/scrapped-fishing-mod/`, and every reference to
it has been removed from the core loader list, the build lists, the mod-hash
registry and the scan tests. The server-side fishing minigame is a separate,
live feature and was not touched.

## History

The sources' history up to the move is in this repository. `git log -- patches`
still works here.
