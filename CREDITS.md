# Credits

## Original project

This is a community fork of [**opencoduo/coduomp**](https://github.com/opencoduo/coduomp) —
a reconstructed source for **Call of Duty: United Offensive** multiplayer
client and dedicated-server components.

**Original authors:** OpenCoDUO Recovery team

All original reconstruction work, structural recovery, code comments,
and documentation remain the intellectual property of the original
authors. This fork does not claim ownership of any of it.

## What this fork adds

Community-requested features on top of the original reconstruction:

### Server-side
- **`sv_fastDownload`** — up to 16x faster mod downloads by sending
  multiple 2KB blocks per snapshot frame
- **`sv_welcome`** — fully customizable welcome message with placeholders:
  - `%name` / `%s` — player name
  - `%ip` — player IP address
  - `%num` — client slot number
  - `%city` — city from IP geolocation
  - `%region` — region from IP geolocation
  - `%country` — country from IP geolocation
  - `%loc` — combined location (format controlled by `sv_welcomeLocation`)
- **`sv_welcomeLocation`** — control what `%loc` expands to:
  - `0` = disabled
  - `1` = country only
  - `2` = city, country
  - `3` = region, city, country
- **`sv_maxConnectionsPerIP`** — limits simultaneous connections from
  a single IP address (anti-flood / anti-spam)
- **`sv_downloadNotifications`** — notify players when downloads start
- **`sv_downloadLog`** — append download events to `downloads.log`
- **`sv_debug`** — verbose logging for custom features
- **`say2`** — secondary admin broadcast command
- **Colored console output** — ANSI escape codes for readability
- **Fixed download blockBudget calculation** in `SV_WriteDownloadToClient`

### Technical details
- Linked `libcurl` for IP geolocation lookups via `ip-api.com`
- Fixed-size geolocation cache (256 entries) to avoid API hammering
- All new code marked with `NOT_FROM_ORIGINAL_SOURCE` comments
  for traceability
- `sv_welcome` supports full customization of colors, text, and
  placeholders directly from `dedicated.cfg`

## How to use

See the [README.md](README.md) for installation and configuration.

## Not affiliated with Activision

**Call of Duty®** and **Call of Duty®: United Offensive®** are registered
trademarks of **Activision Publishing, Inc.**

This project is **not affiliated with, endorsed by, or sponsored by
Activision**. It contains **no retail game assets** and requires a legally
owned copy of the game to run.

## License status

The original coduomp repository does not currently include an explicit
`LICENSE` file. This fork is distributed under the same unclear licensing
status as the original, with the following additional terms:

1. The original attribution to OpenCoDUO must be preserved in any
   redistribution of this fork.
2. No retail Activision game assets may be included in any
   redistribution of this fork.
3. This fork may not be sold commercially without explicit permission
   from the original authors.

If you are an original author and wish to clarify the licensing of this
fork, please open an issue.
