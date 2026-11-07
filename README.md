# lcdsmartie-jellyfin

A plugin for [LCD Smartie](https://github.com/LCD-Smartie/LCDSmartie) that shows what's
playing on your [Jellyfin](https://jellyfin.org) server: title, artist, series and episode,
elapsed/total time, a progress bar, and more.

```
Digital Bath
Deftones
> 01:04/04:15
█████_______________
```

- Works with **64-bit LCD Smartie** on **Windows 7 and later**.
- One DLL with nothing else to install. The C runtime is built in, and it only uses
  `KERNEL32.dll` and `WINHTTP.dll`, which ship with Windows.
- Polls the server on a background thread, so a slow or unreachable server never stalls
  the display.

## Install

1. Download `jellyfin.dll` from the [latest release](https://github.com/Spooky098529/lcdsmartie-jellyfin/releases/latest).
2. Copy it into `LCD Smartie\plugins\`. Close LCD Smartie first if you're replacing an older
   version, because Windows locks a DLL while it's loaded.
3. In Jellyfin, create an API key under **Dashboard → API Keys → +**.
4. Start LCD Smartie once. It creates `plugins\jellyfin.ini`. Edit it:

```ini
[Jellyfin]
Server=http://192.168.1.10:8096     ; base URL (a reverse-proxy sub-path like /jellyfin is fine)
ApiKey=xxxxxxxx                     ; from step 3
PollInterval=2000                   ; ms between server polls (min 500)
DefaultFilter=                      ; user/device/client to show when a line gives no filter
IdleText=Nothing playing            ; shown by "display" and templates when nothing plays
BarEmpty=_                          ; empty character for the progress bar
```

The plugin re-reads the ini on every poll, so you don't need to restart LCD Smartie after
editing it.

## Usage

| Line | Shows |
|---|---|
| `$dll(jellyfin,1,display,)` | `Artist - Title`, `Series S01E02 Title` or `Movie (Year)` |
| `$dll(jellyfin,1,<field>,<filter>)` | one field (below) |
| `$dll(jellyfin,2,20,)` | progress bar, 20 characters wide |
| `$dll(jellyfin,3,,)` | number of sessions currently playing |
| `$dll(jellyfin,4,{icon} {time},)` | template: each `{field}` is replaced with its value |
| `$dll(jellyfin,20,,)` | last error, or `OK` |

Fields:

| Field | Example |
|---|---|
| `title` `artist` `album` `series` | `Digital Bath`, `Deftones`, `White Pony`, … |
| `season` `episode` `se` | `3`, `7`, `S03E07` |
| `year` `type` | `2000`, `Audio` / `Episode` / `Movie` |
| `user` `device` `client` | who is playing, and where |
| `state` `icon` | `Playing` / `Paused` / `Stopped`, `>` / `\|\|` |
| `pct` | `25` (percent played) |
| `count` | number of matching sessions |
| `display` | smart one-line summary (see above) |
| `error` | last error, empty when OK |
| `pos` `dur` `remain` `time` `timeleft` | time fields (below) |

### Time fields

`pos` (elapsed), `dur` (total), `remain` (time left), `time` (elapsed/total) and
`timeleft` (remaining/total). Add a suffix to choose the format:

| Suffix | 6-min song, `time` | 2h12 movie, `time` |
|---|---|---|
| (none) | `1:30/5:54` | `1:05:09/2:12:00` |
| `_mm` | `01:30/05:54` | `65:09/132:00` |
| `_m` | `1:30/5:54` | `65:09/132:00` |
| `_hms` | `0:01:30/0:05:54` | `1:05:09/2:12:00` |
| `_hhmmss` | `00:01:30/00:05:54` | `01:05:09/02:12:00` |

For example, `$dll(jellyfin,1,timeleft_mm,)` gives `-04:23/05:54`, and
`$dll(jellyfin,4,{pos_mm} {remain_mm},)` gives `01:30 -04:23`.

The position keeps counting between polls, so the clock ticks every second even with a
long `PollInterval`.

### Filters

The optional `<filter>` matches a Jellyfin **user name, device name or client name**. It
ignores case but must match the whole name. With no filter, `DefaultFilter` from the ini
is used; if that's empty too, all sessions count. When several sessions match, a playing
one is shown before a paused one.

```
$dll(jellyfin,1,display,alice)        what alice is playing
$dll(jellyfin,1,display,Living Room)  what's playing on the "Living Room" device
```

### Example 20x4 screen

```
$dll(jellyfin,1,title,)
$dll(jellyfin,1,artist,)
$dll(jellyfin,4,{icon} {time_mm},)
$dll(jellyfin,2,20,)
```

### Switching to the Jellyfin screen when playback starts

LCD Smartie's **Actions** tab can use the plugin as a condition. An action runs once when
its condition changes from false to true, not on every check. `$dll(jellyfin,3,,)` is the
number of active sessions (paused ones included, `DefaultFilter` applied), so `> 0` means
"something started playing". Unpausing doesn't count as a new start.

In these rules, `N` is the number of your Jellyfin screen.

**Jump to it when playback starts** (normal screen rotation carries on afterwards):

| Variable | Condition | Value | Action |
|---|---|---|---|
| `$dll(jellyfin,3,,)` | `>` | `0` | `GotoScreen(N)` |

**Jump to it and stay there until playback stops:**

| Variable | Condition | Value | Action |
|---|---|---|---|
| `$dll(jellyfin,3,,)` | `>` | `0` | `GotoScreen(N)` |
| `$dll(jellyfin,3,,)` | `>` | `0` | `FreezeScreen` |
| `$dll(jellyfin,3,,)` | `=` | `0` | `UnfreezeScreen` |

**Hide the screen while nothing is playing** (combines with either of the above):

| Variable | Condition | Value | Action |
|---|---|---|---|
| `$dll(jellyfin,3,,)` | `=` | `0` | `DisableScreen(N)` |

`DisableScreen` works in both directions: it disables the screen while the condition is true
and enables it again as soon as playback starts.

The switch happens within about `PollInterval` (2 s by default) of pressing play. If the
server can't be reached, the count is `0`, so the rules treat that as idle.

## Troubleshooting

When nothing is playing, `display` and templates show the error instead of `IdleText`.
`$dll(jellyfin,20,,)` always shows it.

| Message | Meaning |
|---|---|
| `Set Server and ApiKey in plugins\jellyfin.ini` | the ini still has its defaults |
| `Unauthorized - check ApiKey` | the server rejected the API key |
| `Cannot connect to server` / `Server timed out` | wrong address or port, or the server is down |
| `Request failed (12175)` or similar | usually an https/TLS problem (see below) |

**https on Windows 7:** WinHTTP on Windows 7 only supports TLS 1.0 unless update
[KB3140245](https://support.microsoft.com/kb/3140245) is installed. Most https servers
require TLS 1.2, so on Windows 7 use plain `http://` on your LAN, or install that update.

## Building from source

The DLL is cross-compiled on Linux with clang and lld-link against Microsoft's CRT and
Windows SDK, which you download with [xwin](https://github.com/Jake-Shadle/xwin). Running
xwin accepts Microsoft's license.

```sh
xwin --accept-license --arch x86_64 splat --use-winsysroot-style --preserve-ms-arch-notation --output xwin-sdk
make                          # or: make WINSYSROOT=/path/to/xwin-sdk
make check                    # list imports/exports
```

`make check` lets you confirm the DLL imports only functions that exist on Windows 7. You
can put `WINSYSROOT = /path/to/xwin-sdk` in a `config.mk` (git-ignored) instead of passing it
each time.

## License

MIT. See [LICENSE](LICENSE).
