# appliance/web: the web interface (C2)

`stxweb` is the appliance's configuration and status page. It is a service
that the supervisor starts from the factory configuration, and it is the
only listener the appliance declares (HTTPS on 443).

```
[service-web]
command = /usr/sbin/stxweb
restart = always
control = operate
```

## Pieces

| Path | What |
|---|---|
| `runtime/web/` | the service: JSON, HTTP/1.1, sessions and lockout, the routes, the TLS loop, main |
| `runtime/supervisor/control/` | the control channel between stxweb and the supervisor (fd 3, SOCK_SEQPACKET) |
| `runtime/store/` | turn files: crash-safe small files on the FAT32 user partition |
| `static/` | index.html, app.js, style.css: one page, no inline script, text written with textContent |
| `tools/make_password_turn.py` | makes a provisioned password file on the host |
| `vm/c2_web.sh` | the VM scenario (two boots, driven by `vm/web_check.py`) |

## Running

```
stxweb [--port N] [--state DIR] [--assets DIR] [--config PATH]
```

| Option | Default |
|---|---|
| `--port` | 443 |
| `--state` | /user/stx/web |
| `--assets` | /usr/share/stx/web |
| `--config` | /etc/stx/appliance.conf |

stxweb needs the supervisor's channel on descriptor 3, and it exits when that
channel closes. At start it prints these lines on the console:

- `STXWEB fingerprint sha256=...`: the certificate to expect in the browser;
- `STXWEB setup code=XXXXX-XXXXX`: only while no password is set;
- `STXWEB listening port=N`.

While it runs, it prints one line per security event.

## First password

There are two ways to set it:

- **Provision it.** Make a password file on the host and copy it to
  `<state>/password.0` on the user partition before the first boot:
  ```
  make_password_turn.py --password-file FILE --out password.0
  ```
- **Use the setup code.** Type the code the console shows on the page's
  first screen.

To recover a lost password, delete `password.0` and `password.1` (card out,
or over SSH).

## Settings

Settings live in the user configuration, `/user/stx/config.0` and `.1`,
which the supervisor writes by turns. They take effect at the next boot. The
factory configuration on the read-only root cannot be changed from the page.

The design, its choices and the supervisor interface are in sc2's
`C2-web.md`, which the coordinator files under `notes/planning/stages/`.

## Tests

| Script | Covers |
|---|---|
| `runtime/web/test/run_host_tests.sh` | JSON, HTTP, auth, editor, value rules; the API with a real ControlServer behind a fake backend |
| `runtime/supervisor/control/test/run_host_tests.sh` | the protocol, the server, the turn files |
| `appliance/web/vm/c2_web.sh` | the whole service in a test VM; it needs `stxweb` from `runtime/build_programs.sh` |
