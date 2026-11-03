---
name: mokagram-build
description: Build mokaGram locally on macOS (Release only) and install it into /Applications. Use when the user asks to build, rebuild, compile, install or package mokaGram, or after any change that has to be tried in the real app.
---

# mokaGram: Release build and install

Rules, from `AGENTS.md`:

- Release only. Never build Debug: it produces a stale `Telegram.app` with the
  stock Telegram icon. The product is `out/Release/mokaGram.app`.
- Local builds only, no CI.
- Never run an Xcode clean against `out/`.
- Do not start a build the user did not ask for.

## 1. Build

From the repository root:

```bash
cmake --build out --config Release --parallel
```

If `out/` has no Release configuration, reconfigure first (needs `QT`, see
`AGENTS.md` "Build Configuration"):

```bash
cd Telegram && ./configure.sh -D CMAKE_CONFIGURATION_TYPES=Release \
  -D CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
  -D DESKTOP_APP_DISABLE_AUTOUPDATE=OFF -D DESKTOP_APP_DISABLE_CRASH_REPORTS=ON
```

Stop if the build fails; report the error and install nothing.

## 2. Install into /Applications

Run right after a successful build, without asking. For this checkout, the user
has explicitly requested that the agent close and relaunch the installed app.
Do not ask the user to quit mokaGram manually on every installation.

1. Copy `out/Release/mokaGram.app` with `ditto` into a temporary directory under
   `/Applications/.mokagram-install-*`. Ad-hoc sign that copy with
   `codesign --force --deep -s -`, clear quarantine with
   `xattr -dr com.apple.quarantine`, and verify with
   `codesign --verify --deep --strict` before replacing the installed bundle.
2. Quit mokaGram. If AppleScript quit fails, verify the PID and exact executable
   under `/Applications/mokaGram.app/Contents/MacOS/`, send SIGTERM only to that
   process and wait for it to exit. Never terminate another client or checkout.
   Stop and report an installation failure if the process cannot exit.
3. Move the installed bundle into the staging directory, then move the prepared
   bundle to `/Applications/mokaGram.app`. Restore the previous bundle if the
   replacement fails. Move the previous bundle to the user's Trash only after
   replacement succeeds, and remove the empty staging directory.

Then check the result and report it:

```bash
/usr/libexec/PlistBuddy -c "Print CFBundleName" /Applications/mokaGram.app/Contents/Info.plist
ls -lT /Applications/mokaGram.app/Contents/MacOS/mokaGram
```

The binary timestamp must be from this build. If it is older, the build did
not relink: say so instead of reporting success. After verification, run
`open /Applications/mokaGram.app` to relaunch it.

## 3. dmg (only if asked)

```bash
cd out/Release && rm -rf dmgroot && mkdir dmgroot
cp -R mokaGram.app dmgroot/ && ln -s /Applications dmgroot/Applications
hdiutil create -volname mokaGram -srcfolder dmgroot -ov -format UDZO ~/Desktop/mokaGram-arm64.dmg
rm -rf dmgroot
```

Package `mokaGram.app` only, never `Telegram.app`.

## 4. Publish when requested

When the user asks to push or open a PR, follow
[the publication skill](../mokagram-publish/SKILL.md). Publication requires both a
pushed source branch and an open PR targeting `ecronxy/mokagram: mokagram`, using
the authenticated user's fork as the head. Do not stop after pushing to `origin`,
and do not create the final PR in a fork.
