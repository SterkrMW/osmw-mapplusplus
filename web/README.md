# `web/` — files hosted on osmw.net

Not part of the app. Nothing here is copied into `releases\` by `build.ps1`; these
are uploaded to the website by hand.

## `version.json`

Upload to **`https://osmw.net/mapsplusplus/version.json`** — the URL compiled into
`VERSION_CHECK_URL` (`variables.ahk`). Maps++ fetches it once, a few seconds after
startup, and says nothing unless a newer version exists.

```json
{
  "version": "0.9.0-beta.1",
  "notes": "First public beta."
}
```

| Field | Required | Rules |
|---|---|---|
| `version` | yes | The version now available for download. Must start with a digit and contain only `0-9 A-Z a-z . - +`, max 32 chars. Compared against the running build with the same semver rules the app uses, so `0.9.0-beta.2` is newer than `0.9.0-beta.1`, and `0.9.0` is newer than either. |
| `notes` | no | One short line shown in the update notification. Newlines are stripped and it is truncated at 100 characters. Omit it rather than leaving it empty. |

A bare version string with no JSON at all (a file containing just `0.9.0-beta.1`)
is also accepted, so a mis-uploaded plain-text file still works rather than
failing silently.

### Releasing

1. Bump `APP_VERSION` in `variables.ahk` **and** the `;@Ahk2Exe-SetVersion`
   literal in `main.ahk` — `build.ps1` fails the build if they disagree.
2. `pwsh ./build.ps1`, publish the exes.
3. **Then** update `version` here and upload.

Order matters: this file is what tells every running client an update exists, so
uploading it before the download is live points people at nothing.

### Deliberate omissions

There is **no download URL in the manifest**. Where the "Get the update" tray item
sends people is `VERSION_DOWNLOAD_URL`, compiled into the app. A URL taken from a
network response and handed to `Run()` is a remote-code-execution vector wearing a
hat, and the convenience of changing it server-side is not worth it.

There is also no "critical" or "minimum version" flag, and the app never
downloads or installs anything. The check reports; the user decides.

### Serving notes

- **HTTPS is required.** The request is plain `GET` with `Cache-Control: no-cache`;
  a plain-`http://` or bad-certificate endpoint fails the request, and the app
  treats every failure as "no update" and stays quiet.
- Serve as `application/json` (or `text/plain`; the app does not check the type).
- Keep it small and static. The app never sends anything about the user — no
  query string, no identifiers, no version of their own. It is a plain GET of a
  public file, which is the property that makes it defensible in a tool that
  reads game memory.
