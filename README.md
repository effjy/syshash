# Syshash

![Version](https://img.shields.io/badge/version-2.1.0-blue?style=flat-square)
![Language](https://img.shields.io/badge/language-C11-00599C?style=flat-square&logo=c)
![Hash](https://img.shields.io/badge/hash-SHA3--512-blueviolet?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey?style=flat-square)
![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen?style=flat-square)
![Build](https://img.shields.io/badge/build-passing-brightgreen?style=flat-square&logo=github-actions)

**Syshash** is a lightweight, interactive file integrity monitor written in pure C.  
It recursively hashes every file in a directory using **SHA3-512** (Keccak-f[1600], implemented from scratch — no OpenSSL, no external libraries) and stores the results in a local database. On subsequent runs it detects any file whose content has changed and lets you decide — all at once or file by file — whether to accept or reject the change.

Built to help catch tampering.

---

## Features

- **SHA3-512** from scratch — zero runtime dependencies, single static binary
- Recursively scans the current directory and all subdirectories
- Stores hashes in a human-readable `.syshash.db` flat-file database
- Detects **content changes** only — deletions are intentionally ignored
- **Batch action menu** on verification — list all changes, accept all at once, review one by one, or skip all
- Selective partial updates — rejecting one file never blocks accepting others
- Real-time progress bar with file counts
- Colourful, clean ANSI terminal interface

---

## Table of Contents

- [Installation](#installation)
- [Usage](#usage)
- [Menu Preview](#menu-preview)
- [How It Works](#how-it-works)
- [Database Format](#database-format)
- [Project Structure](#project-structure)
- [Building from Source](#building-from-source)
- [Security Notes](#security-notes)

---

## Installation

```bash
git clone https://github.com/effjy/syshash.git
cd syshash
make
sudo make install      # installs to /usr/local/bin/syshash
```

To uninstall:

```bash
sudo make uninstall
```

To build without installing (run from the project directory):

```bash
make
./syshash
```

> **Requirements:** A C11-capable compiler (`gcc` or `clang`) and POSIX-compatible OS. No libraries beyond libc are needed.

---

## Usage

Navigate to any directory you want to monitor, then run:

```bash
cd /path/to/directory/you/want/to/monitor
syshash
```

The interactive menu will guide you through the rest.

---

## Menu Preview

### Main Menu

```
  ███████╗██╗   ██╗███████╗██╗  ██╗ █████╗ ███████╗██╗  ██╗
  ██╔════╝╚██╗ ██╔╝██╔════╝██║  ██║██╔══██╗██╔════╝██║  ██║
  ███████╗ ╚████╔╝ ███████╗███████║███████║███████╗███████║
  ╚════██║  ╚██╔╝  ╚════██║██╔══██║██╔══██║╚════██║██╔══██║
  ███████║   ██║   ███████║██║  ██║██║  ██║███████║██║  ██║
  ╚══════╝   ╚═╝   ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝
  File Integrity Monitor  ·  SHA3-512  ·  v2.0.1

  ─────────────────────────────────────────────────────────────

  What would you like to do?

  1  →  Create / Rebuild database
  2  →  Verify integrity
  3  →  Exit

  ─────────────────────────────────────────────────────────────

  Choice:
```

### Option 1 — Creating the Database

```
  [ INFO ] Creating database…
  Scanning current directory recursively with SHA3-512.

  [██████████████████████████████████████░░]  38 / 40  src/main.c

  [  OK  ] Database created: 40 files hashed.
           Saved to .syshash.db in the current directory.

  Press [Enter] to return to menu…
```

### Option 2 — Verifying Integrity (all clean)

```
  [ INFO ] Verifying integrity…

  Database created : 2026-06-09T05:31:29Z
  Database updated : 2026-06-09T05:31:29Z
  Entries on record: 40

  Scanning…
  [████████████████████████████████████████]  40 / 40  src/ui.c

  Results:
  [  OK  ]  Unchanged : 40
  [ MOD  ]  Modified  : 0
  [ NEW  ]  New files : 0

  [  OK  ] All files intact. No tampering detected.
```

### Option 2 — Verifying Integrity (changes detected)

```
  [ INFO ] Verifying integrity…

  Database created : 2026-06-09T05:31:29Z
  Database updated : 2026-06-09T05:31:29Z
  Entries on record: 40

  Scanning…
  [████████████████████████████████████████]  40 / 40  src/ui.c

  Results:
  [  OK  ]  Unchanged : 38
  [ MOD  ]  Modified  : 1
  [ NEW  ]  New files : 1

  2 changes detected.

  What would you like to do?

  l  →  List all changes
  r  →  Review one by one
  a  →  Accept all changes
  s  →  Skip all (leave database unchanged)

  Action [lras]: l

  ──────────────────────────────────────────────────
  [ MOD ]  src/main.c
  [ NEW ]  secrets.txt
  ──────────────────────────────────────────────────

  Action [lras]: r

  Reviewing each change…

  ──────────────────────────────────────────────────
  [ MOD ] MODIFIED FILE
    Path   : src/main.c
    Old    : fc955659f16d0bd9e974ac17e0a347c81e658b61cdbcba413ff674857e265a79…
    New    : d164b1c55acd72ac7566f7dc04cf4d3852f56fbc803c0e7d159f2dc8695f58b5…

  Accept this change as legitimate? (update database) [y/n] y
  [  OK  ] Accepted.

  ──────────────────────────────────────────────────
  [ NEW ] NEW FILE
    Path   : secrets.txt
    Hash   : a8cd8b97d3166bf59d0f7501c28248bf9b13ac1aea98a8333cec11c3889a078f…

  Accept this new file into the database? [y/n] n
  [ WARN ] Rejected — database NOT updated for this file.

  ──────────────────────────────────────────────────

  [  OK  ] Database updated successfully.
```

**To accept all changes immediately**, press `a` at the action menu instead of `r`. This is useful when hundreds or thousands of new or modified files are expected (e.g., after a large deployment) and you have already reviewed the list with `l`.

---

## How It Works

```
 cd /target/directory
 syshash
       │
       ▼
 ┌─────────────────────────────────────────────────────┐
 │  Option 1 — Create / Rebuild                        │
 │                                                     │
 │  Walk directory tree recursively                    │
 │    └─ For each regular file (skip .syshash.db):     │
 │         Read file in 64 KB chunks                   │
 │         Compute SHA3-512 (Keccak-f[1600])           │
 │         Store  hex | path  in .syshash.db           │
 └─────────────────────────────────────────────────────┘

       │
       ▼
 ┌─────────────────────────────────────────────────────┐
 │  Option 2 — Verify                                  │
 │                                                     │
 │  Load .syshash.db into memory                       │
 │  Walk directory tree and re-hash every file         │
 │                                                     │
 │  For each file:                                     │
 │    ├─ Hash matches record  →  [  OK  ] unchanged    │
 │    ├─ Hash differs         →  [ MOD ] flag          │
 │    └─ Not in database      →  [ NEW ] flag          │
 │                                                     │
 │  Batch action menu:                                 │
 │    l → list all flagged files, then re-prompt       │
 │    r → review each file individually (y/n per file) │
 │    a → accept all changes at once                   │
 │    s → skip all (leave database unchanged)          │
 │                                                     │
 │  Deletions are silently ignored.                    │
 └─────────────────────────────────────────────────────┘
```

---

## Database Format

The database is stored as `.syshash.db` in the directory where `syshash` is run. It is a plain-text, human-readable file:

```
# syshash v2.0.1
# created: 2026-06-09T05:31:29Z
# updated: 2026-06-09T05:31:29Z
#
# Format: sha3-512-hex|relative-path
# DO NOT EDIT MANUALLY
#
fc955659f16d0bd9e974ac17e0a347c81e658b61...4b4|file1.txt
f73bc13d5c7400e8a066d4b5ff96848572e56e3b...73|subdir/config.cfg
a8cd8b97d3166bf59d0f7501c28248bf9b13ac1a...64|subdir/deep/data.bin
```

- One entry per line: `<sha3-512-hex>|<relative-path>`
- The database file itself is always excluded from scans
- Timestamps are stored in UTC ISO 8601 format

> **Tip:** You can track `.syshash.db` in version control to get a tamper-evident audit trail across commits.

---

## Project Structure

```
syshash/
├── Makefile
└── src/
    ├── main.c      — interactive menu, create and verify commands
    ├── sha3.c/h    — SHA3-512 (Keccak-f[1600]) implemented from scratch
    ├── db.c/h      — database load, save, lookup, upsert
    ├── scan.c/h    — recursive directory walker (opendir / readdir / lstat)
    └── ui.c/h      — ANSI colours, progress bar, y/n prompt helpers
```

---

## Building from Source

```bash
# Default build
make

# Custom install prefix (default: /usr/local)
make install PREFIX=/opt/local

# Uninstall
make uninstall PREFIX=/opt/local

# Clean build artifacts
make clean
```

The Makefile compiles with `-std=c11 -Wall -Wextra -Wpedantic` and produces zero warnings.

---

## Security Notes

- **Store `.syshash.db` safely.** An attacker who can modify both a file and its database entry defeats the check entirely. Consider keeping the database on read-only media, a separate host, or in version control.
- **Deletions are ignored by design.** Syshash is focused on catching content tampering, not auditing file inventory. If you need deletion detection, track `.syshash.db` in git — missing entries become visible in the diff.
- **SHA3-512 has no known practical collision attacks.** The 512-bit output makes birthday-attack collision probability negligible for any realistic file corpus.
- **The database is excluded from its own scan.** This prevents the tool from flagging its own legitimate updates as tampering.

---

## License

MIT © 2026  
SHA3-512 implementation based on the [NIST FIPS 202](https://doi.org/10.6028/NIST.FIPS.202) specification.
