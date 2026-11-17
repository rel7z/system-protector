# system-protector

Keep a Linux program **alive even when it's killed**. If your program exits,
crashes, or is killed (`kill -9` included), it is launched again automatically —
usually within a second.

Comes with an **interactive, self-installing CLI** (`system-protector`) that
builds the watchdog, symlinks it onto your `PATH`, and walks you through
protecting a program.

## How it protects your program (two layers)

```
systemd ─(Restart=always)─▶ protector ─(respawn on exit/kill)─▶ your app
   │                            │                                   │
   │ if the watchdog is killed, │ if your app is killed,            │
   └─ systemd restarts it       └─ protector restarts it in ~1s     └─ runs
```

- **`protector`** — a tiny C watchdog (one file, no deps beyond libc) that runs
  your app as a child and respawns it with exponential backoff.
- **systemd** — runs the watchdog with `Restart=always`, so if the *watchdog
  itself* is killed, PID 1 brings it back. (On systems without systemd, a cron
  fallback revives the watchdog instead.)

> **Honest limitation:** nothing on Linux can make a process truly un-killable —
> `root` can always `kill -9` anything, by design. This makes your program
> **come back fast** after any kill. To keep it down, something would have to
> kill the app *and* the watchdog *and* stop systemd.

## Install (self-installing CLI)

Copy this folder to your Linux machine, then:

```bash
sudo ./install.sh
```

That builds the watchdog and **symlinks** both `protector` and the
`system-protector` CLI into `/usr/local/bin`. (Without `sudo` it installs for
the current user into `~/.local/bin` and uses the cron fallback.)

Then just run the interactive menu:

```bash
system-protector
```

```
system-protector — keep a program alive even when it's killed
  status: installed
  backend: systemd

  1) Set up a new protected program
  2) List protected programs
  3) Status of a program
  4) View logs
  5) Remove a protected program
  6) (Re)install system-protector onto PATH
  7) Uninstall system-protector
  0) Exit
```

Pick **1** and answer three questions (name, command, working directory) — it
creates the service, starts it, and shows you the status.

## Non-interactive subcommands

Everything the menu does is also a direct command:

```bash
system-protector self-install        # build + symlink onto PATH
system-protector add                 # set up a program (prompts you)
system-protector list                # list protected programs + state
system-protector status <name>       # show status
system-protector logs <name>         # tail the log
system-protector remove <name>       # stop + remove a program
system-protector uninstall           # remove the symlinks/unit (keeps configs)
```

## Symlinks it creates

| Symlink | Points to |
|---|---|
| `/usr/local/bin/protector` | `<repo>/protector` (built binary) |
| `/usr/local/bin/system-protector` | `<repo>/system-protector` (this CLI) |
| `/etc/systemd/system/system-protector@.service` | `<repo>/systemd/system-protector@.service` |

Because they're symlinks, rebuilding (`system-protector self-install`) updates
everything in place. **Keep this project folder** where you installed it from —
the symlinks point back to it.

## Testing that it works

```bash
system-protector add            # protect e.g. `sleep 1000`, name it "demo"
system-protector status demo    # note it's running
pkill -9 sleep                  # kill the protected program
system-protector logs demo      # watch it get restarted in ~1s
```

With systemd you can also `sudo kill -9` the `protector` process and watch
systemd bring the watchdog right back.

## Updating

To pull the latest version from GitHub and rebuild **safely** (a broken update
can't take your running programs down):

```bash
cd system-protector
sudo ./update.sh          # or: system-protector update
```

It fast-forwards the repo, builds the new watchdog to a **temp file and
smoke-tests it first**, and only swaps it in if that works (keeping the old one
as `protector.bak`). Then it refreshes the symlinks and restarts each running
instance onto the new binary. If the build or test fails, nothing changes and
your programs keep running on the old binary.

## Restart behavior

- A program that was running fine and then **exits or is killed** is respawned
  **immediately**, with no delay — forever.
- A program that dies almost instantly (a real crash loop) is retried with a
  short, growing backoff (up to `--max-backoff`) so it can't peg the CPU; as
  soon as it stays up again, instant respawns resume.

## Run the watchdog directly (no install)

```bash
make
./protector --name myapp --logfile ./myapp.log -- python3 ./app.py
```

| Option | Meaning | Default |
|---|---|---|
| `--name NAME` | label used in logs | `protected` |
| `--pidfile PATH` | write the watchdog's own PID | — |
| `--logfile PATH` | append watchdog + child output | stderr |
| `--min-backoff SECS` | first restart delay | `1` |
| `--max-backoff SECS` | cap on restart delay | `30` |
| `--healthy SECS` | child must run this long to reset backoff | `15` |
| `--daemon` | detach into the background | off |

Everything after `--` is the command to protect, with its arguments.

## Files

| File | Purpose |
|---|---|
| `system-protector` | the interactive self-installing CLI (main entry point) |
| `protector.c` | the watchdog itself |
| `systemd/system-protector@.service` | systemd template unit (layer 2) |
| `config/example.conf` | example program config |
| `install.sh` / `uninstall.sh` | thin wrappers around the CLI |
| `update.sh` | safe self-update from GitHub (build-test-swap-restart) |
| `Makefile` | `make`, `make install`, `make clean` |
