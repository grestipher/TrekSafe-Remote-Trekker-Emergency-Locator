# The bridge

Carries frames between the mesh and the console, and serves the console. Its own
process. Decision D5.

It holds no opinion about what a frame means. Parsing lives in the console and
in the firmware, both of which are tested. This moves bytes, stores what crossed
the wire, and says what it is doing.

## Why it is a separate process

The original reason was licensing: the Meshtastic Python library is GPL-3.0-only,
and importing it into the console would have made the console a derivative work
on distribution. That reason went away with D10, when TrekSafe stopped being a
Meshtastic module and got its own firmware.

The decision stands on what it turned out to buy instead:

- **It outlives the browser.** That is what makes persistence possible at all,
  D6. Incidents survive a reload, a crash, and closing the laptop lid.
- **A wedged radio driver cannot take the screen down with it.** The thing
  talking to hardware and the thing a warden is looking at are separate
  processes.
- **The console stays a static page.** No build step, no server-side rendering,
  and it still opens from disk as a fallback.

## Running it

```bash
python3 bridge/treksafe_bridge.py --source sim --open
```

Serves the console at http://127.0.0.1:8730/ and opens a browser there. Runs
from any directory: its default paths are anchored to the repository, not the
working directory.

## Sources

| `--source` | What is real | What is simulated |
| --- | --- | --- |
| `sim[:speed]` | nothing | everything, including the beacon. No hardware needed |
| `node[:port]` | **the beacon firmware**, over USB | repeaters, gateway, signal readings |
| `gateway[:port]` | everything. Needs a board flashed as a gateway | nothing |
| `replay:<file>` | nothing | a scripted file of bare payload frames |
| `stdin` | nothing | frames typed or piped in |
| `none` | nothing | nothing. Serves the console and the stored record only |

`sim:10` runs the scripted incident ten times faster, so a demonstration does
not need thirty seconds of standing about to show a repeat.

Everything a simulated source produces is labelled `simulated`, and that label
survives the store and a restart. The console shows it as `SIMULATED, not
measured` and keeps it out of the coverage survey.

`meshtastic` and `meshtastic-tcp` were removed with D10. Typing either gets a
message saying what replaced it rather than "unknown source". The old code is in
`archive-meshtastic/`, unbuilt and unimported.

## Options

| Flag | Meaning |
| --- | --- |
| `--source` | see above. Default `none` |
| `--serial-port` | the device for `node` and `gateway`, autodetected if omitted |
| `--listen` | port, default 8730 |
| `--console-dir` / `--no-console` | serve the console from here, or do not |
| `--store` / `--no-store` | the SQLite file, D6 |
| `--replay-hours` | how far back an attaching console is rebuilt. Default 12 |
| `--journal` | append every frame to a file before anything interprets it |
| `--open` | open a browser once it is up |

## Endpoints

| Path | |
| --- | --- |
| `GET /` | the console |
| `GET /stream` | Server-Sent Events: `packet`, `heartbeat`, `replay`, `replay-done` |
| `GET /status` | what it is attached to, and counts |
| `GET /export.csv` | the whole stored record, for an incident review |
| `POST /send` | a frame for the mesh. The only thing the console sends is an acknowledgement |
| `POST /action` | an operator decision that leaves no trace on the radio |

Loopback only, and it checks. The `Host` header must name this machine, which is
what stops a name somebody else controls being pointed at 127.0.0.1, and a
request carrying an `Origin` this bridge does not serve is refused. Without
those, any page in the operator's browser could read `/export.csv`, which is
every stored frame including the position of a person in trouble, or POST a
forged acknowledgement and tell a beacon help was coming when nobody had looked.

## Things it does deliberately

- **It stores an outbound frame only once the radio has taken it.** A send the
  radio refused is stored as an operator action instead, never as a frame, so
  the console cannot rebuild an acknowledgement that never happened.
- **It sends no heartbeat when there is no radio.** A heartbeat means a gateway
  is alive. Beating with nothing attached is the same lie as beating over a dead
  one.
- **It kills itself if its source stops reading.** The alternative is quietly
  worse than a crash: it would keep telling every console the feed was healthy
  while nothing was listening.
- **It notices a console that has gone.** The stream writes a comment line on an
  idle timeout, so a closed tab is dropped rather than counted forever.

## Testing

```bash
python3 -m unittest discover -s bridge -p 'test_*.py'
```

89 tests, standard library only, no board needed. They cover the parts that can
be wrong in a way that loses a frame, and the security checks above.

## What has never been done

No radio has ever been attached to this. The `gateway` source is written and
tested against fakes and has never seen one. The `node` source has been run
against a real board over a real cable, which exercises the serial layer, but
the mesh behind it was software.
