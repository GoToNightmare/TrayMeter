# TrayMeter

A lightweight CPU and memory meter for the Windows notification area, styled after Task Manager's tray icon.

- **Left bar, CPU:** blue.
- **Right bar, memory:** green below 70 %, yellow from 70 %, red from 90 %.

Hover the icon for exact values. Right-click for options.

## Requirements

- Windows 10 version 1703 or later.
- To build: Visual Studio 2019 or later, or Build Tools, with the C++ x64 tools.

## Build

```
build.cmd
```

Output: `bin\TrayMeter.exe`, a single executable with no dependencies beyond Windows.

## Usage

Run `TrayMeter.exe`. Only one instance runs per user session.

| Menu item | Effect |
|---|---|
| Open Task Manager | Starts Task Manager. |
| Match Task Manager's CPU % | On by default. See [CPU measurement](#cpu-measurement). |
| Update every | 1, 2 or 5 seconds. |
| Start with Windows | Adds or removes a per-user entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. |
| Exit | Removes the icon and quits. |

To start TrayMeter for every user, place a shortcut named `TrayMeter.lnk` in the all-users Startup folder
(`shell:common startup`). The **Start with Windows** item then shows *(all users)*, ticked and greyed out.

Settings are stored per user in `HKCU\Software\TrayMeter` (`MatchTaskManager`, `IntervalMs`).

## CPU measurement

- **Match Task Manager's CPU % on:** reads the `% Processor Utility` performance counter, capped at 100. It accounts
  for boost clocks. `pdh.dll` is loaded only in this mode, adding about 4 MB of memory.
- **Off:** uses `GetSystemTimes`, the share of time the processors were busy. It is lighter, but reads lower than
  Task Manager on CPUs that boost above their base clock.

Memory is the share of physical memory in use, from `GlobalMemoryStatusEx`.

## Resource usage

TrayMeter reads two system-wide totals per tick and never enumerates processes. Explorer gets a new icon only when the
bars' pixels change, and tooltip text only while the pointer is over the icon. Sampling pauses while the session is
locked.

At a 1-second interval it typically uses about 0.02 % of one CPU core. Private memory is about 5.5 MB with **Match Task
Manager's CPU %** on (the default) and 1.5 MB with it off.
