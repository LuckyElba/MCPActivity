# MCP Activity

## Author and origin

**D. Matthew Pegg — PeggSoft**

Original source:

    https://github.com/LuckyElba/MCPActivity

MCP Activity is released under the **PeggSoft Permissive Attribution License**.
It may be used, modified, distributed, and used commercially for any lawful
purpose, provided attribution to D. Matthew Pegg / PeggSoft and a reference to
the original source repository are retained. See `LICENSE` for the full terms.

MCP Activity is a native x64 Windows tray application for supervising local MCP (Model Context Protocol) helper applications.

It provides a compact status popup and a larger management window for starting, stopping, restarting, and monitoring MCP processes without leaving command-prompt or PowerShell windows open.

## Features

- Native Win32 C++ application
- Dark UI with per-monitor DPI awareness
- Compact popup showing MCP health
- Notification-area status icon
  - Green: running/healthy
  - Orange: active
  - Red: error
  - Gray: stopped/not loaded
- Start, Stop, Restart, Start All and Stop All controls
- Auto-start and restart-on-failure settings per MCP application
- Activity/error log
- Startup with Windows option
- Config editing through the Windows default `.ini` editor
- `Ctrl+Alt+Shift+M` fallback hotkey to restore the popup
- Automatic tray-icon recovery after Explorer/taskbar restarts
- Hidden child-process launching with no console window
- WMI-based process discovery and adoption of already-running instances

The current default configuration supervises Desktop Commander Remote and the
WindowCaptureMCP screenshot service.

## Companion applications

### WindowCaptureMCP

[WindowCaptureMCP](https://github.com/LuckyElba/WindowCaptureMCP) is a read-only
Windows MCP screenshot service by D. Matthew Pegg / PeggSoft. It lets an MCP
client enumerate Windows application windows and capture PNG screenshots.

It is a natural companion to MCP Activity: MCP Activity can start it automatically,
monitor whether it is still running, restart it after a failure, and keep its
console window hidden.

Repository:

    https://github.com/LuckyElba/WindowCaptureMCP

### Desktop Commander Remote

Desktop Commander Remote provides a remote MCP connection between AI clients such
as ChatGPT and the Windows PC. MCP Activity can keep its local Remote Device
process running alongside WindowCaptureMCP.

Desktop Commander can be found at:

- Website: https://desktopcommander.app/
- Remote Desktop Commander GitHub:
  https://github.com/desktop-commander/remote-desktop-commander

The Remote Device can be launched with:

    npx @wonderwhy-er/desktop-commander@latest remote

Before relying on MCP Activity to auto-start Desktop Commander, run Desktop
Commander Remote manually at least once and complete its account/device pairing
if required.

Together, a useful setup is:

    ChatGPT / other AI
           |
           v
    Desktop Commander Remote
           |
           v
    Windows PC
       |        |
       |        +--> WindowCaptureMCP
       |
       +--> other local tools

    MCP Activity monitors and controls both local processes.

## Build

Requirements:

- Windows 10 or later
- Visual Studio with Desktop development with C++
- x64 MSVC toolchain

From a Developer Command Prompt, open `MCPActivity.sln` and build `Release|x64`.

The included `build.bat` also builds the Release x64 configuration using MSBuild.

## Configuration

MCP Activity reads `mcp-activity.ini` from the same directory as
`MCPActivity.exe`.

The runtime INI is intentionally not committed because commands and working
directories are machine-specific. If the file does not exist, MCP Activity creates
a default configuration for Desktop Commander Remote and WindowCaptureMCP.

The current version loads these two section names specifically:

    [Desktop Commander]
    [ScreenShot]

Keep those section names unchanged.

### Desktop Commander configuration

A working Desktop Commander Remote configuration is:

    [Desktop Commander]
    Command=cmd.exe /d /s /c "npx @wonderwhy-er/desktop-commander@latest remote"
    WorkingDirectory=C:\Users\YourWindowsName
    Match1=desktop-commander
    Match2= remote
    AutoStart=1
    RestartOnFailure=1

Set `WorkingDirectory` to a valid directory for the Windows account running MCP
Activity. When MCP Activity creates a new default INI itself, it uses that user's
Windows profile directory automatically.

The important fields are:

- `Command` starts the Desktop Commander Remote Device without requiring a
  permanently visible command window.
- `Match1` and `Match2` are case-insensitive pieces of process command-line
  text used by MCP Activity to find and adopt an already-running instance.
- `AutoStart=1` starts Desktop Commander when MCP Activity starts if it is not
  already running.
- `RestartOnFailure=1` asks MCP Activity to start it again if the wanted process
  disappears unexpectedly.

Desktop Commander Remote should be paired/authenticated normally before using
MCP Activity as its supervisor.

### WindowCaptureMCP configuration

First install WindowCaptureMCP. The example below assumes it is installed in:

    C:\WindowCaptureMCP

and that its Python virtual environment has been created by its `install.bat`.

Use:

    [ScreenShot]
    Command=cmd.exe /d /s /c ""C:\WindowCaptureMCP\.venv\Scripts\python.exe" "C:\WindowCaptureMCP\server.py""
    WorkingDirectory=C:\WindowCaptureMCP
    Match1=windowcapturemcp
    Match2=server.py
    AutoStart=1
    RestartOnFailure=1

If WindowCaptureMCP is installed somewhere else, change both paths in `Command`
and change `WorkingDirectory` to match.

`Match1=windowcapturemcp` and `Match2=server.py` allow MCP Activity to identify
the WindowCaptureMCP server process from its command line. The section is called
`[ScreenShot]` because that is the section name currently expected by MCP
Activity.

WindowCaptureMCP repository:

    https://github.com/LuckyElba/WindowCaptureMCP

### Complete example

A complete configuration for both applications looks like this:

    [General]
    RefreshMilliseconds=1200

    [Desktop Commander]
    Command=cmd.exe /d /s /c "npx @wonderwhy-er/desktop-commander@latest remote"
    WorkingDirectory=C:\Users\YourWindowsName
    Match1=desktop-commander
    Match2= remote
    AutoStart=1
    RestartOnFailure=1

    [ScreenShot]
    Command=cmd.exe /d /s /c ""C:\WindowCaptureMCP\.venv\Scripts\python.exe" "C:\WindowCaptureMCP\server.py""
    WorkingDirectory=C:\WindowCaptureMCP
    Match1=windowcapturemcp
    Match2=server.py
    AutoStart=1
    RestartOnFailure=1

After editing `mcp-activity.ini`, restart MCP Activity so it reloads the
configuration. You can also use the Config control in the Details window to open
the INI in the Windows default editor.

## Source formatting

The project uses Allman brace formatting. The included `.clang-format` contains the formatting rules used for `main.cpp`.
