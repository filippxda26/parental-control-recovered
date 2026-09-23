## v6 update

See `RECOVERY_V6.md` for temporary-unblock state and DHCP hostname/MAC reconciliation.

## v4 update

Traffic activity is now reconstructed from nftables byte counters, including `activity_threshold_bytes`, `idle_timeout_seconds`, `previous_inbound`, `previous_outbound`, `counters_seen`, and `last_active`. See `RECOVERY_V4.md`.

# parental-control — recovered source

Functional reconstruction from the surviving OpenWrt binaries, web assets and configuration.
It is **not byte-for-byte original source**. The original frontend is preserved unchanged.

## Components
- `src/parental-control.c` — Unix-socket API daemon, device CRUD, DHCP lookup, night/manual/break blocking and nftables application.
- `src/parental-control-web.c` — HTTP server on port 5000; serves the original frontend and proxies `/api/*` to the daemon.
- `files/usr/share/parental-control/www/` — recovered original frontend.
- `files/etc/init.d/` — recovered original procd init scripts.
- `files/etc/parental-control/` — settings and a sanitized/recovery example device file.

## Recovered API surface
`GET /api/health`, `/api/status`, `/api/state`, `/api/devices`, `/api/discovered`; `POST /api/devices`; `PUT/DELETE /api/devices/<id>`; `POST /api/devices/<id>/{block,unblock,break,bonus}`.

The binary strings also show original support for daily/session accounting, history, reset-today and temporary state. Those deeper accounting paths are documented by the recovery evidence but are not yet reproduced exactly in this first reconstruction.

## Build
Requires musl/OpenWrt toolchain and json-c. For a native test build: `make`.

## Second recovery pass
`BINARY_API.md` and the two `*.strings.txt` files contain the additional forensic recovery of runtime persistence, daily/session accounting, nftables counters, reset-today, history, and temporary-state behavior. These are preserved separately so later implementation work can be checked against evidence from the original binaries.

## v5 additions
See `RECOVERY_V5.md`: DHCP discovery, persistent history endpoint/events, and stricter validation were added from binary evidence.

## v8 machine-code xref pass

See `XREF_V8.md`. This pass maps original ELF string addresses to executable references and confirms persistent `temporary_unblock`, the runtime-state field layout, DHCP MAC-update success/failure branches, history persistence, and nftables-result-aware manual blocking.

## v10 safe test
See `RECOVERY_V10.md` and `test-openwrt/`. v10 compiles cleanly with `-Wall -Wextra -Werror`. The test procedure runs reconstructed binaries from `/tmp` and does not replace the recovered originals in `/usr/sbin`.
