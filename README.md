# storage

A compiled Bend terminal dashboard for local Linux storage. Run `storage`, or
`storage ~/projects` to open the folder browser immediately. No Python or child
processes run as part of the command.

```sh
storage --plain
storage --json
storage --all
storage --jobs 4 ~/projects
storage --interval 10
```

`--plain` is automatic when output is piped. A directory argument adds its
allocated-size report to plain/JSON output. `--all` includes RAM-backed and
other virtual filesystems; network filesystems are excluded to avoid waiting
on remote servers. `--jobs` selects 1–32 scanner workers (default 4).

Use Tab or **1 / 2 / 3** for filesystems, disks, and folders. Arrows or **j / k**
select entries. **Enter** opens a directory or mounted partition; **h** or
Backspace goes to the parent; **H** opens home. **r** refreshes the current view,
**a** toggles virtual filesystems, **c** copies the selected full path, **?** shows
help, and **q / Esc / Ctrl+C** quits.
The terminal needs at least 64 columns by 20 rows. `NO_COLOR` disables colors.

Copy uses the highlighted row: the file/folder itself (including symlinks), the
mount point in Overview, or `/dev/<name>` in Disks. Empty lists leave the
clipboard unchanged. Paths are copied in full, even when shortened on screen.
The terminal must allow [OSC 52 clipboard writes](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html).
The status reports that the path was sent; terminals that block OSC 52 will not
update the clipboard. No clipboard helper commands are required.

## What makes it fast

- Filesystem, inode, disk, and swap information comes directly from Linux APIs,
  `/proc`, and `/sys`. The command does not invoke `df`, `findmnt`, or `lsblk`.
- A bounded worker pool scans independent directories concurrently. Results are
  visible while the scan runs; `+` means an entry's total is still growing.
- The current scan retains its directory tree in memory. Navigation within that
  tree reads cached results, including hidden entries, without another walk.
- Unchanged terminal frames are not sent again on idle ticks.

**Cached folder sizes are a snapshot.** New, removed, or changed files appear
after **r**. Switching tabs or navigating within the scanned tree does not
refresh it. Opening a directory outside the cached tree starts a new scan;
quitting or replacing the scan cancels its workers. Only one tree is retained.
There is no persistent disk cache. The home scan used about 132 MiB of peak RAM
for roughly 716,000 entries in the benchmark.

Sizes are allocated blocks, not apparent file lengths. Sparse files therefore
report allocated space. Hard links are counted once across the complete scan;
which containing folder gets a shared file's blocks can vary with traversal
order. Totals across the tree are consistent. Symlinks are displayed but their
targets are not scanned. Scans do not cross mount boundaries; explicitly open
the mounted directory to scan it. Permission errors are reported as partial
results. Cached entries may still appear after their underlying files change.

## Source, proofs, and build

| File | Role |
| --- | --- |
| `app.bend` | Application state machine, refresh policy, selection and rendering |
| `main.bend` | Bend IO loop and native-effect declarations |
| `LAWS.bend` | Ten behavioral laws for refresh, quit, and navigation |
| `PROOF.bend` | Proofs importing all laws |
| `native.c` | Linux effects, CLI/output, and terminal boundary |
| `scan.h` | Concurrent, cancellable filesystem traversal and in-memory tree |
| `snapshot.h` | Local filesystem/device statistics and output encoding |
| `entry.c` | Separates application arguments from Bend runtime flags |

Bend owns the application logic and terminal rendering. The native effect layer
uses POSIX filesystem and terminal APIs, which Bend's small standard library
does not expose directly. Pure laws are checked without foreign code or unsafe
definitions in their import graph. These laws cover the state machine; they
do not prove Linux calls or the C scanner correct. Integration tests and native
sanitizers check those boundaries.

Build with Bend **2.0.27** and Clang 18 (or another compatible Clang):

```sh
cd /path/to/storage
./build.sh
python3 test_storage.py
```

`build.sh` runs `bend PROOF.bend` before compiling. The version is pinned because
Bend's native effect representation is compiler-specific. `BEND_BIN` and
`CLANG_BIN` override executable locations. The script also looks for a local
Bend at `build/toolchain/bend/bin/bend` and Clang at
`build/toolchain/clang/usr/bin/clang-18`, then checks the home toolchain locations
and PATH (including `clang-18`). On Ubuntu, Clang can be installed with
`sudo apt install clang-18`. Bend 2.0.27 archives are available from the
[pinned release](https://github.com/bendlang/bend/releases/tag/v2.0.27);
extract the matching archive into `build/toolchain/` to keep it separate from
a newer Bend installation. Generated C, local toolchains, and binaries stay
in `build/`.

Build, test, and install atomically to `~/.local/bin`:

```sh
./build.sh --install
```

Plain `./build.sh` only builds; it does not install. Use `INSTALL_DIR` to change
the install directory, and ensure that directory is on your PATH.

Before any commit, run `bend PROOF.bend` with Bend 2.0.27 and the integration tests.
Do not weaken a law or add `@unsafe` to make a failed proof pass.

## Measured on this machine

These are elapsed times, with both implementations running on the same device.
Full scans necessarily vary with the filesystem cache and concurrent activity.

| Operation | Previous Python utility | Native Bend utility |
| --- | --- | --- |
| JSON snapshot, median of 15 runs | 119.7 ms | 4.3 ms |
| Home scan, interleaved runs | 6.12 s, 3.85 s | 2.12 s, 2.00 s |

The matching final scans returned the same allocated total. The native scan
shows partial results immediately, and subsequent navigation reuses that scan.
An earlier first traversal by `du` took 23.94 seconds; this is not used as a
like-for-like speedup claim because filesystem-cache conditions differed.

The old Python executable, tests, and documentation remain in
`~/.local/share/storage/previous-python/`. Restore it with:

```sh
install -m 755 ~/.local/share/storage/previous-python/storage ~/.local/bin/storage.previous
mv ~/.local/bin/storage.previous ~/.local/bin/storage
```
