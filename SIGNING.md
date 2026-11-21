# Signing

sqterm's releases are signed with **the squared key**: an ECDSA P-256 key
and certificate made by `apkpack keygen` (APK Signature Scheme v2). Android
installs an update only when it is signed with the same key as the app
already on the phone, so every release from GitHub and every build signed
with this key updates the one before it in place.

The key's certificate:

    certificate SHA-256: e92c41b93ae98971e30ad98ebcfb6b2434f0997a27a6cff86adbb48a5cacc0eb

To check an APK, in sqterm (or anywhere apkpack runs):

    apkpack verify sqterm-<...>.apk

It prints `verified (v2, ECDSA P-256), certificate SHA-256 <hex>`; the hex
must be the one above. The apk workflow checks every release this way, and
refuses to publish one signed with any other key.

## Where the key is

- The `APKPACK_KEY` secret of this repository (Settings, Secrets and
  variables, Actions): the PEM file whole. The apk workflow writes it to
  `~/.android/apkpack.pem` before it builds. Without it, CI signs with a key
  made for that run (pull requests, and forks, which never see the secret)
  and does not publish.
- A copy kept by the maintainer, off the phone. Lose every copy and the
  next release needs a new key, and every phone uninstalls sqterm once.

Builds on the phone sign with `~/.android/apkpack.pem` too: with the squared
key there, they update a release in place and a release updates them.
