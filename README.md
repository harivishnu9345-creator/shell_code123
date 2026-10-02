# hsh - a bash-style shell for native Windows

**Version v1 (prints as `hsh 0.1`) - early test release.**

hsh is a small Linux/bash-style shell written in C with the Win32 API. It runs
directly on Windows 10/11 (64-bit): no WSL, no virtual machine.

> **Message to testers:** this is **v1**. It works for the core features, but
> bugs are expected. If something breaks, please report it (see
> [If setup fails](#if-setup-fails)). Fixes and new features are planned for
> **v2** and **v3** (see the [roadmap](#roadmap)).

## What it can do

- Bash-style pipes, redirection and background jobs: `|`  `<`  `>`  `>>`  `2>`  `2>&1`  `&`
- Kali-style two-line prompt with colours (red `#` when elevated)
- Runs any installed Windows program by name (`notepad`, `python`, `ipconfig`, ...)
- Built-in commands: `cd pwd echo export env history jobs fg bg which type help`
  and Linux-style `ls cat cp mv rm mkdir touch head tail wc grep chmod`
- `sudo` / `su` using the normal Windows UAC prompt (hsh never asks for passwords)

## Install (no administrator rights needed)

1. Download this project (**Code > Download ZIP**) and extract it, or get the
   ready-made `hsh.exe` from the **Releases** page and put it next to `setup.bat`.
2. Double-click **`setup.bat`** and choose:
   - **1** - install the ready-made `hsh.exe` (fast), or
   - **2** - build from source (installs CMake and the MinGW compiler with
     `winget` if you do not have them; the first time takes a few minutes).
3. **Close all terminal windows** (every Windows Terminal window, not just one
   tab), open a new cmd / PowerShell / Windows Terminal, and type:

```
hsh
```

Setup copies `hsh.exe` to `%LOCALAPPDATA%\hsh` and adds that folder to your
**user PATH** once, so `hsh` works from cmd, PowerShell and Windows Terminal.
You can also run it non-interactively: `setup.bat install`, `setup.bat build`,
`setup.bat uninstall`.

### Manual alternative (one time)

If you prefer not to use `setup.bat`, put `hsh.exe` in a folder such as
`C:\hsh` and run this once in PowerShell, then open a new terminal:

```
$p = [Environment]::GetEnvironmentVariable("Path","User")
[Environment]::SetEnvironmentVariable("Path", ($p.TrimEnd(';') + ";C:\hsh"), "User")
```

## Try it

```
echo hello | grep hello
ls -l
cat < notes.txt > copy.txt
notepad &
jobs
sudo whoami
```

## Known limits in v1

- `dir`, `type FILE`, `cls`, `start` (cmd.exe internals) are not available yet.
- Linux-only programs are not installed by Windows, so `file`, `ip a`, `ifconfig`
  say "command not found". Use the Windows equivalents (`ipconfig`, ...) for now.
- `sudo` / `su` open a **new elevated window** (Windows cannot elevate a running
  process in place).
- No `&&`, `||`, `;`, and no scripting (`if`, `for`, `while`).
- `--help` is not supported by the built-in commands yet.
- Windows may show **SmartScreen** ("Windows protected your PC") because the
  exe is not code-signed: click **More info > Run anyway** only if you trust the
  source. Antivirus may also flag a shell that can start other programs.

## Roadmap

Planned, not promised:

- **v2:** aliases and a `.hshrc` file (`ip a`, `ifconfig`, ...), `dir`/`type`/`cls`
  fallback, optional Git-for-Windows Linux tools, `/mnt/c`-style drive paths and a
  `drives` command, `--help` for built-ins, `sudo` in the same tab, UI fixes
  (Ctrl+D, non-ASCII typing).
- **v3:** `&&` / `||` / `;`, scripting, better tab completion, Start Menu app lookup.

## If setup fails

1. Read the message the script printed; most are explained there.
2. Common fixes:
   - **`hsh` is not recognized:** you did not open a *new* terminal. Close all
     terminal windows and open a new one.
   - **"winget was not found":** install *App Installer* from the Microsoft
     Store, or install CMake (cmake.org) and WinLibs MinGW (winlibs.com) by hand.
   - **"Could not copy hsh.exe":** a running hsh is locking the file. Close it.
   - **Build error:** scroll up to the *first* error, not the last.
3. Report it: open a GitHub **Issue** and paste the full text from the setup
   window, your Windows version (`winver`), and which option (1 or 2) you chose.
   A screenshot is fine too.

## Uninstall

```
setup.bat uninstall
```

## Security note

`sudo` relies on the standard Windows **UAC** consent prompt. hsh never reads or
stores your password and never tries to bypass UAC.

## Build from source by hand

```
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
.\build\hsh.exe
```

Parser unit tests: `cmake --build build --target test_parser` then
`.\build\test_parser.exe`.

This is a student project (Simple Shell). Use it at your own risk.
