# Installing and starting apps from sqterm (draft)

Status: a proposal, for review. Nothing here is built yet.

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
sqmake run ─► squared/build/android.lua ─► device tool ─► sqterm (the app)
                (verbs: what run means)     `sqterm-device`   PackageInstaller,
                                            (a command)       startActivity
```

1. **The app's side**, in sqterm (`sq_android/`): a small service on a Unix
   socket, `$PREFIX/var/run/sqterm.sock`. Only sqterm's own user can open it
   (it sits in the app's private files), which is exactly the user every
   shell and tool in sqterm runs as; nothing else on the phone can reach it.
   It answers one JSON request with one JSON response, as the SDK's tools do
   (squared's `docs/developer/sdk-tools.md`), through sq.data.
2. **The command**, `sqterm-device`: the device tool's interface, the same
   commands as `tools/device.sh` (`install APK`, `run APK PACKAGE`,
   `uninstall PACKAGE`, `devices`; `logcat TAG` says it needs adb and hands
   over to `tools/device.sh`). It sends requests to the socket and prints
   what comes back. A static C++23 program, packaged like sqedit.
3. **The platform file** (`squared/build/android.lua`) picks the device
   tool: `sqterm-device` when it runs inside sqterm (`SQTERM=1`) and the
   command is installed, else `tools/device.sh`. A project's `app.device`
   still wins. Nothing else changes: not sqmake, not the template.

## The requests

```json
{"sq": 1, "command": "install", "args": {"apk": "/abs/path/build/game.apk"}}
{"sq": 1, "command": "start", "args": {"package": "dev.me.game"}}
{"sq": 1, "command": "uninstall", "args": {"package": "dev.me.game"}}
{"sq": 1, "command": "status", "args": {}}
```

`install` answers when Android has finished: `{"ok": true, "result":
{"package": ..., "version_code": ...}}`, or an error with Android's own
status and message (`STATUS_FAILURE_CONFLICT`: signed with another key;
`STATUS_FAILURE_ABORTED`: the person said no). `status` says whether
sqterm may install at all (below), so `sqmake devices` can say what to do.

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
2. The socket service in `sq_android/`, with `status` and `start` first
   (no confirmation flow), then `install` and `uninstall`.
3. `sqterm-device` (a package), and `squared/build/android.lua` choosing it.
4. On the phone: `sqmake run` with Wireless debugging off.

## Questions for review

- The command's name: `sqterm-device`, or a general `sqterm` command with
  more uses later (`sqterm open FILE`, `sqterm share`)?
- Is one tap per install acceptable for now, or should raising sqterm's
  target SDK come first?
