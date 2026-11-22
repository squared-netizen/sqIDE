# Installing and starting apps from sqterm (draft)

Status: agreed (2026-11-22). Nothing here is built yet.

## What it is for

`sqmake install` and `sqmake run` on the phone go through `tools/device.sh`,
which needs adb: Wireless debugging on, paired, connected, and connected
again each time Wi-Fi drops. Without adb, `install` hands the APK to
`termux-open`, which sqterm does not have, and `run` cannot work at all.

sqterm is itself an Android app, and every command typed in it runs as that
app's own Linux user. So sqterm can do what adb is used for here, through
Android's own APIs, with nothing to pair:

- install an APK (Android asks the person to confirm, as for any installer)
- start an installed app
- uninstall one (Android asks)

Out of scope: reading another app's log. Android gives that only to adb (or
to the system); a debug build's own log already reaches sqterm through
DEV_LOG and the Console, without logcat.

## The parts

```
sqmake run ─► squared/build/android.lua ─► sqterm-install ─► sqterm (the app)
                (verbs: what run means)     sqterm-launch     PackageInstaller,
                                            (commands)        startActivity
```

1. **The app's side**, in sqterm (`sq_android/`): a small service on a Unix
   socket, `$PREFIX/var/run/sqterm.sock`. Only sqterm's own user can open it
   (it sits in the app's private files), which is exactly the user every
   shell and tool in sqterm runs as; nothing else on the phone can reach it.
   It answers one JSON request with one JSON response, as the SDK's tools do
   (squared's `docs/developer/sdk-tools.md`), through sq.data.
2. **The commands**, one job each, named as Termux names its own
   (`termux-open`, `termux-usb`): each sends one request to the socket and
   prints what comes back, and each is useful without sqmake.

   | command | does |
   |---|---|
   | `sqterm-open PATH\|URL` | hands a file or link to Android (an APK: the installer, as `termux-open`) |
   | `sqterm-install APK` | installs, and waits for Android's answer |
   | `sqterm-uninstall PACKAGE` | Android asks, then it is gone |
   | `sqterm-launch PACKAGE` | starts an installed app (its launcher activity) |
   | `sqterm-usb` | a USB device, by Android's permission prompt (as `termux-usb`) |
   | `sqterm-adb` | adb to a USB device through `sqterm-usb` (as `termux-adb`): logcat with no Wi-Fi |

   One static C++23 program, packaged like sqedit, installed under each
   name (it reads `argv[0]`), so the family stays one package.
3. **The platform file** (`squared/build/android.lua`): inside sqterm
   (`SQTERM=1`, the commands installed) `install` is `sqterm-install`,
   `run` is `sqterm-install` then `sqterm-launch`, `uninstall` is
   `sqterm-uninstall`; `logcat` and `devices` stay adb's (`tools/device.sh`,
   which can reach adb through `sqterm-adb`). Elsewhere, `tools/device.sh`
   as now. A project's `app.device` still wins. Nothing else changes: not
   sqmake, not the template.

## The requests

```json
{"sq": 1, "command": "open", "args": {"target": "/abs/path/file.pdf"}}
{"sq": 1, "command": "install", "args": {"apk": "/abs/path/build/game.apk"}}
{"sq": 1, "command": "launch", "args": {"package": "dev.me.game"}}
{"sq": 1, "command": "uninstall", "args": {"package": "dev.me.game"}}
{"sq": 1, "command": "usb", "args": {"device": "/dev/bus/usb/001/002"}}
{"sq": 1, "command": "status", "args": {}}
```

`install` answers when Android has finished: `{"ok": true, "result":
{"package": ..., "version_code": ...}}`, or an error with Android's own
status and message (`STATUS_FAILURE_CONFLICT`: signed with another key;
`STATUS_FAILURE_ABORTED`: the person said no). `status` says whether
sqterm may install at all (below), so `sqmake devices` can say what to do.
`usb` answers with a file descriptor for the device, passed over the socket
(SCM_RIGHTS), after Android's permission prompt: what `sqterm-adb` and
libusb need.

## How the app does each, with no Java

squared's apps have no Java: the activity is a NativeActivity from the shim
dex (`squared/tools/dexgen`). Native code reaches Android through JNI, on
the activity's VM.

- **Install**: `PackageInstaller` sessions. Create a session, write the
  APK's bytes into it (`openWrite`, from native code: no file has to be
  shared, so no FileProvider and no content:// URI), then commit with an
  IntentSender. The IntentSender is a PendingIntent for SquaredActivity
  itself: the result arrives as a new intent (`onNewIntent`, which the shim
  passes to native code). `STATUS_PENDING_USER_ACTION` carries Android's
  confirmation screen, which sqterm starts; the final status arrives the
  same way, and the request is answered then.
- **Open**: an ACTION_VIEW intent. A file is offered through a small
  ContentProvider in the shim dex (one class, read-only, for the one file
  named), since Android gives other apps no file:// paths.
- **USB**: `UsbManager.requestPermission`, then `openDevice`; its file
  descriptor goes back over the socket.
- **Start**: `PackageManager.getLaunchIntentForPackage`, then
  `startActivity`. sqterm is in front when the person types, so Android
  lets it start another app.
- **Uninstall**: `PackageInstaller.uninstall` with the same IntentSender;
  Android asks.

What the shim dex needs: SquaredActivity's `onNewIntent` passing the
intent's extras to native code (dexgen writes it; a few hundred bytes).
What the manifest needs: `REQUEST_INSTALL_PACKAGES` (and
`REQUEST_DELETE_PACKAGES`). The first time, Android sends the person to
Settings, "Install unknown apps", to allow sqterm; `status` reports
whether that has been done.

## The one real limit: a prompt on every install

Android 12 lets an installer update an app it installed itself without
asking again (`setRequireUserAction(USER_ACTION_NOT_REQUIRED)`), but only
for an installer that targets a recent SDK (30 or later; 33 and 34 tighten
it). sqterm targets 28, so every `sqmake install` shows Android's "Update
this app?" once, one tap. That is still no adb, nothing to pair, and works
offline; moving sqterm's target SDK up removes the tap, and is its own
decision (the foreground service rules, storage access).

## Order of work

1. The shim's `onNewIntent` (squared, dexgen), and the manifest permissions.
2. The socket service in `sq_android/`, with `status` and `launch` first
   (no confirmation flow), then `install` and `uninstall`.
3. The commands (one package), `sqterm-launch` and `sqterm-install` first,
   and `squared/build/android.lua` using them inside sqterm.
4. On the phone: `sqmake run` with Wireless debugging off.
5. Later: `sqterm-open` (the ContentProvider), then `sqterm-usb` and
   `sqterm-adb`.

## Decisions

- Decided: the commands are named as Termux's are, `sqterm-*`, one job each.
- Decided: one tap per install is fine for now; raising sqterm's target
  SDK is its own decision, later.
