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

Run right after a successful build, without asking:

```bash
APP=out/Release/mokaGram.app
test -d "$APP" || { echo "no $APP"; exit 1; }
osascript -e 'tell application "mokaGram" to quit' 2>/dev/null; sleep 1
rm -rf /Applications/mokaGram.app
ditto "$APP" /Applications/mokaGram.app
codesign --force --deep -s - /Applications/mokaGram.app
xattr -dr com.apple.quarantine /Applications/mokaGram.app 2>/dev/null
```

Then check the result and report it:

```bash
/usr/libexec/PlistBuddy -c "Print CFBundleName" /Applications/mokaGram.app/Contents/Info.plist
ls -lT /Applications/mokaGram.app/Contents/MacOS/mokaGram
```

The binary timestamp must be from this build. If it is older, the build did
not relink: say so instead of reporting success.

## 3. dmg (only if asked)

```bash
cd out/Release && rm -rf dmgroot && mkdir dmgroot
cp -R mokaGram.app dmgroot/ && ln -s /Applications dmgroot/Applications
hdiutil create -volname mokaGram -srcfolder dmgroot -ov -format UDZO ~/Desktop/mokaGram-arm64.dmg
rm -rf dmgroot
```

Package `mokaGram.app` only, never `Telegram.app`.
