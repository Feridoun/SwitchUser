# SwitchUser

Switch between signed-in Windows users instantly — with a hotkey, a tray-icon click, or a
pop-up picker — instead of going through **Start → account picture → Switch user → pick a tile**.

Unlike other tools, SwitchUser does **not** ask for the other account's password. A small
Windows service running as `SYSTEM` moves the already-signed-in session onto the screen with
`WTSConnectSession`, which is the only way Windows allows a passwordless switch to a *different*
user. You land straight on the other desktop.

| Trigger | What happens |
| --- | --- |
| **Ctrl + Alt + U** | Switch to the other signed-in user. If nobody else is signed in, show the sign-in screen; if several others are, open the picker. |
| **Ctrl + Alt + Shift + U** | Open the picker at the mouse pointer (press `1`–`9` to choose, or `S` for the sign-in screen). |
| **Tray icon — left-click** | Same as Ctrl + Alt + U. |
| **Tray icon — right-click** | Open the picker. |

## Install

Download `SwitchUser.exe` from the [latest release](../../releases/latest) and run:

```
SwitchUser.exe install
```

This asks for admin rights once (UAC), copies itself to `C:\Program Files\SwitchUser\`, registers
the `SwitchUserSvc` service and a per-user Run entry, and starts the tray icon in every session
that is already signed in. The first switch to an account that isn't signed in yet drops you at the
sign-in screen; pick its tile once, and from then on switching both ways is instant.

To remove everything:

```
SwitchUser.exe uninstall
```

## How it works

- **`SwitchUserSvc`** — a `SYSTEM` service. It lists sessions with `WTSEnumerateSessions`, then
  calls `WTSConnectSession` to bring the target user to the console, or `WTSDisconnectSession` to
  show the sign-in screen when the target isn't signed in. It listens on a local named pipe and
  **only obeys the user currently on the console**.
- **The tray helper** — one lightweight process per signed-in user (started from an `HKLM` Run
  entry). It owns the hotkeys, tray icon and picker, and asks the service to do the switch.

Everything is a single ~100 KB executable with no dependencies, plain Win32 C++.

## Security note

Because the switch needs no password, **anyone sitting at your PC can reach your other signed-in
accounts** with a keypress. That is the right trade-off when every account on the machine is yours.
Do **not** install it on a shared or multi-person computer where accounts are meant to be private.

## Build

Needs Visual Studio 2019+ with the "Desktop development with C++" workload.

```
build.cmd
```

Produces `SwitchUser.exe`. Released builds are code-signed with a Certum certificate.

## License

MIT — see [LICENSE](LICENSE).
