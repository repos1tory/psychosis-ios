# Psychosis

Source: [repos1tory/psychosis-ios](https://github.com/repos1tory/psychosis-ios).
The current package is `com.alsinas.psychosis` 0.7.1 for rootless
`iphoneos-arm64` devices.

Psychosis is a rootless iOS automation tweak and on-device CLI. The tweak runs in SpringBoard and binds its line protocol to `127.0.0.1:6001`; host automation reaches it by SSH and runs `psychosis` on the device. The server is not exposed on the LAN.

The short command is **`psi`** — **P**sychosis **S**ystem **I**nterface, also evoking the Greek letter ψ. Both names run the same executable, with identical arguments and exit codes.

## Protocol

Commands are one newline-terminated request per TCP connection. Coordinates are logical screen points. Portrait defaults are 768 × 1024; pass dimensions explicitly when the UI is in another layout.

| Command | Behavior |
| --- | --- |
| `ping` | Check whether the server answers. |
| `tap X Y [W H]` | Tap at a point. Dimensions default to 768 × 1024. |
| `swipe X1 Y1 X2 Y2 [steps [W H]]` | Swipe between points. Steps default to 20 and are limited to 1–120. |
| `home` / `volup` | Send the corresponding hardware-style event. |
| `launch BUNDLE_ID` | Ask SpringBoard to launch an app by bundle ID. |
| `ax [depth]` | Return accessibility JSON for the frontmost app, or SpringBoard when no app is foreground, with depth 1–12 (default 12). AX requests are marshaled to SpringBoard's main thread, with a 4-second tree budget, per-element AX timeouts, and a 6-second response guard. |
| `type TEXT` | Text insertion is currently unavailable on this device; the command returns `text-unavailable`. |

Tap commands draw a high-contrast, double-ring blue marker at the touch point. It fades over 0.45 seconds and is removed after about half a second. Swipe commands mark the start and end points. The overlay is non-interactive and hidden from accessibility so it does not alter the tree or intercept input.

Tap and swipe coordinates use UIKit's current screen coordinate space. Before dispatch, the tweak converts each point to the screen's fixed coordinate space for HID, so landscape AX frames and visible tap markers align with the physical touch.

The first `ax` request primes the on-device accessibility workspace and registers an `AXUIClient` on SpringBoard's main thread. It enables the application accessibility runtime and the VoiceOver usage-confirmed flag when the private setters exist, but does not enable VoiceOver or change VoiceOver touch interaction. The JSON response includes an `axBootstrap` status object with the before/after state, client registration, and server readiness. The server worker coordinates automation, while cross-process AX reads and text insertion run synchronously on SpringBoard's main thread; iOS 17 aborts SpringBoard when AX callbacks are entered from a background thread.

Tap and swipe points must be inside the selected dimensions. Dimensions must be positive and at most 32768. Invalid requests return `err <code>` and do not dispatch input. AX failures return a small JSON error object. A successful `launch` acknowledgement means SpringBoard accepted the launch request; use `ax` to verify which app became visible.

## Host tests

The command parser is a pure-C module shared by the tweak and CLI, so it can be tested on Linux or macOS without iOS private frameworks:

```sh
tests/run-unit.sh
```

The suite checks command forms, defaults, strict numeric parsing, coordinate bounds, swipe step limits, bundle IDs, AX depth, bounded text input, and overlong lines.

## Autonomous AI QA prompt

`docs/AI_AUTOMATION_TEST_PROMPT.md` contains a reusable prompt for an agent to
enumerate and test every parser, CLI, SpringBoard, AX, HID, and Lens workflow.
It includes a safety gate for chat sending, settings changes, microphone/camera
access, clipboard mutation, and other destructive actions. The prompt requires
an explicit Lens bundle ID and disposable fixture before Lens-specific or
authorized chat-flow tests are run.

## Build and install on the iPad

Copy the project to the device or edit its existing checkout, then run:

```sh
cd /path/to/Projects/psychosis
./build.sh
sudo dpkg -i com.alsinas.psychosis_0.7.1_iphoneos-arm64.deb
# Only when a SpringBoard reload is authorized:
# sudo sbreload
```

`build.sh` builds the SpringBoard dylib and `psychosis`, packages the SpringBoard filter plist, and stages the CLI at `/var/jb/usr/bin/psychosis` with `psi` as a symlink. Installing the package replaces the retired CLI alias. A separately authorized SpringBoard reload is needed for the new dylib to load; building or installing alone does not reload it. Keep the prior package for rollback. Historical package identifiers remain only in Debian migration metadata and archived build artifacts.

After building, run `sh tests/run-branding.sh` to check both command names and package branding without dispatching device input.

## On-device smoke suite

After installing and reloading SpringBoard, run from the host:

```sh
node tests/device_issues_test.mjs
```

The device regression launches safe read-only web content and checks coordinate
space, AX-frame tapping, scrolling, and settled navigation. Configure its
`PSY_*` environment variables for a fixture URL and labels. It does not submit
forms or send chat messages. Its report contains step status and observed
bundle IDs, never raw AX trees or element values.

## Error codes

The parser may return `invalid-arguments`, `invalid-number`, `invalid-steps`, `invalid-depth`, `invalid-bundle-id`, `out-of-bounds`, `line-too-long`, or `unknown-command`. Runtime errors identify unavailable or failed HID/launch operations. A transport-level connection failure means the tweak did not start its loopback server or is not reachable on port 6001.
