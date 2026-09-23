# parental-control-recovered

Восстановленная реализация родительского контроля для OpenWrt 25.12.x.

## Компоненты

- `src/parental-control.c` — основной daemon: устройства, учёт активности, дневные/сессионные лимиты, отдых, ночная и ручная блокировка, DHCP и nftables.
- `src/parental-control-web.c` — web UI и WebSocket-шлюз к Unix socket daemon.
- `files/usr/share/parental-control/www/` — интерфейс.
- `files/etc/parental-control/devices.json` — пользовательский список устройств.
- `/var/lib/parental-control/state.json` — runtime-состояние; формат совпадает с `/api/state` внутри daemon.
- `/var/lib/parental-control/history.json` — история действий.

## Web UI

По умолчанию web-сервис слушает `0.0.0.0:5000`. Значения берутся из:

```json
{"web_port":5000,"bind_address":"0.0.0.0"}
```

HTTP `/api/*` на web-сервере намеренно отключён. Интерфейс обращается к daemon через `/ws`; web-сервер проксирует разрешённые API-команды в Unix socket `/var/run/parental-control.sock`.

## Состояние

`state.json` создаётся сразу при запуске daemon и атомарно обновляется при изменении runtime-состояния. Список устройств берётся из `/etc/parental-control/devices.json`.

Пользовательские `devices.json` и `settings.json` объявлены conffiles пакета и должны сохраняться при обновлении APK.

## Сборка

GitHub Actions собирает APK для OpenWrt 25.12.5 x86-64. Перед cross-build выполняются:

- проверка синхронности `src/` и `openwrt-package/src/`;
- проверка синхронности `files/`;
- `node --check` для frontend JS;
- проверка JSON и shell-скриптов;
- нативная сборка C с `-Wall -Wextra -Werror`.

Для локальной нативной сборки нужен `json-c`:

```sh
make
```

## Безопасный тест на OpenWrt

Скрипты в `test-openwrt/` запускают тестовые бинарники из `/tmp/parental-control-v10`, не перезаписывая `/usr/sbin/parental-control*`.

`check-api.sh` проверяет загрузку UI, запрет старого HTTP API и WebSocket handshake.
