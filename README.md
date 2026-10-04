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

The current default configuration supervises Desktop Commander Remote and the WindowCaptureMCP screenshot service.

## Build

Requirements:

- Windows 10 or later
- Visual Studio with Desktop development with C++
- x64 MSVC toolchain

From a Developer Command Prompt, open `MCPActivity.sln` and build `Release|x64`.

The included `build.bat` also builds the Release x64 configuration using MSBuild.

## Configuration

MCP Activity reads `mcp-activity.ini` from the same directory as `MCPActivity.exe`.

The runtime INI is intentionally not committed because commands and working directories are machine-specific.

## Source formatting

The project uses Allman brace formatting. The included `.clang-format` contains the formatting rules used for `main.cpp`.
