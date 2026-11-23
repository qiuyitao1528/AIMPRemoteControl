# Android probe

What a throwaway app in `android/` found out about the assumptions of [android-app-plan.md](android-app-plan.md), on
2026-11-21. The probe is a single Activity with a form (address, login, password) and a `WebView` on the player's web
client. It is not the app: the app starts as a new project, and takes from here the findings, not the code.

## Bench

- **Toolchain:** Android Studio 2026.2.2 with its JDK 25, Gradle 9.9.2, AGP 9.5.2 with its built-in Kotlin, Kotlin
  2.4.21 for the Compose plugin, Compose BOM 2026.11.01, `androidx.activity` 1.13.0, `androidx.webkit` 1.18.0. `minSdk`
  26, `targetSdk` 37, `compileSdk` 37.2: Compose 1.13 refuses to build against 37.0, the platform Android Studio
  installs; Gradle downloads 37.2 by itself.
- **Device:** the emulator of Android Studio, Android 17 (API 37.0), Google Play image, 1080×2400, gesture navigation,
  WebView 145.0.7632.218.
- **Player:** the throwaway AIMP of the e2e tests (`web-client/e2e/runtime/aimp`), v6.00 build 3097 with the plugin
  2.0.0, reached from the emulator at `10.0.2.2:3333`; a login set through its profile; two playlists of generated WAV
  files.
- **Driving:** `adb`. The probe takes its orders from intent extras
  ([Probe.kt](../android/app/src/main/kotlin/io/github/tarcode_apps/aimpremotecontrol/Probe.kt)): a player to open, a
  script to run in the page, a message to push over the bridge; what it sees goes to logcat under the tag `Probe`.
  Touches were injected with `adb shell input`, which is not a finger: see the drag below.

Both builds were run: debug (13 MB) and release with R8 and resource shrinking (1.3 MB).

## Results

| #   | Check                                                     | Result                                                                                                                                                                                                                                                            |
| --- | --------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | Does the web view forget a password                       | **No.** Once the right password has let it in, the web view signs every request itself until the process dies, and asks the app nothing. `WebViewDatabase.clearHttpAuthUsernamePassword()` changes nothing. See [Remembered credentials](#remembered-credentials) |
| 2   | A session longer than the nonce lifetime (15 min)         | **Passes.** At 16 minutes requests of the page went through, with no challenge reaching the app. The app was asked twice in the whole session, both times as it began                                                                                             |
| 3   | An address where nothing listens                          | **Passes.** `onReceivedError` for the main frame, `net::ERR_CONNECTION_REFUSED`, within a second; a reload gives the same                                                                                                                                         |
| 4   | A player without a password; a protected one and no login | **Passes.** The first opens with no challenge. The second reaches `onReceivedHttpAuthRequest`, where the app can ask for the login                                                                                                                                |
| 5   | The back key through the client's screens                 | **Passes.** It closes, one at a time: the keyboard, the search, the selection, a menu, the drawer, a second screen (`/files/`). On the client's first screen `WebView.canGoBack()` is false, and the app takes over                                               |
| 6   | Rotation; four minutes in the background                  | **Passes.** With `configChanges` in the manifest the Activity is not recreated and the page is not reloaded. After the background the page was the same one and showed a change made on the player meanwhile                                                      |
| 7   | Dark theme; the keyboard                                  | **Passes.** `prefers-color-scheme` follows the system at once, without a reload, given a day/night theme on the Activity. The keyboard shrinks the visual viewport only (914 → 578 px) and leaves the layout viewport, as Chrome does                             |
| 8   | The client's gestures                                     | **Passes with a risk.** A tap plays a track, a swipe changes the playlist, a track is dragged by its handle. A drag that begins within 24 dp of the left edge went to the system's back gesture instead. See [The edge of the screen](#the-edge-of-the-screen)    |
| 9   | Safe-area insets under the system bars                    | **Passes.** Drawn edge to edge, the page gets `env(safe-area-inset-top)` and `-bottom` (24 px each here) and the client lays itself out by them, as it is. Kept inside the bars, it gets zeros                                                                    |
| 10  | The bridge, `WebViewCompat.addWebMessageListener`         | **Passes.** Messages go both ways; the reply proxy can be kept and used later to push, which is what forwarding events needs. `addDocumentStartJavaScript` works too: the page can tell the app before its own scripts run                                        |
| 11  | The client's file input                                   | **Passes.** `onShowFileChooser` comes with the mode "open multiple". `acceptTypes` holds file extensions (`.aiff, .ogg, .wav, .mp3, …`), not MIME types                                                                                                           |
| 12  | A download                                                | **Passes.** The `DownloadListener` gets the URL, `Content-Disposition` with the file name, the type and the length. The web view downloads nothing itself                                                                                                         |
| 13  | The release build                                         | **Passes.** With R8 and no rules of its own it opens the player and the bridge works                                                                                                                                                                              |

Found on the way:

- **The local network permission.** Targeting Android 17, the app needs the runtime permission `ACCESS_LOCAL_NETWORK`;
  the system asks for it in the words of the "Nearby devices" group. The web view shares the app's permission. Without
  it the page does not fail: the web view shows a blank page and reports nothing, for at least two and a half minutes.
  The app must ask before it opens a player, and cannot leave the refusal to an error screen.
- **A web view in Compose must be told to fill its parent.** `AndroidView` measures a view without layout parameters by
  its content. The page then looks right where it is laid out by the screen, but the client's lists get no height and
  playlists show empty. `layoutParams = LayoutParams(MATCH_PARENT, MATCH_PARENT)` in the factory cures it.
- **A wrong password** in a fresh process comes back to `onReceivedHttpAuthRequest` with
  `HttpAuthHandler.useHttpAuthUsernamePassword()` false, which is how the app tells a refusal from a first challenge.
- **The emulator is slow to start.** Its first boot keeps it busy for some ten minutes, and the first web view of a
  process takes 15–20 seconds there. Commands sent while the app starts are lost; this is the bench, not the app.

### Remembered credentials

The web view keeps the credentials of a realm in the memory of its network stack, apart from anything the app can clear
through `WebViewDatabase`. So, in one process:

- a password that has worked keeps working in the web view after the user has typed another one, a wrong one included;
- a player the user has removed stays open to the web view until the app is restarted.

A password changed on the player is not affected: the old one fails, and the challenge comes to the app.

For the app this means: check the credentials natively (`GET /api/v1/settings` through its own Digest client) before the
web view opens, and never take "the web client loaded" for "the password is right". A profile of its own for each player
(`androidx.webkit`'s `ProfileStore`) would give each a network stack of its own, and was not tried: it would also part
the players' `localStorage`.

### The edge of the screen

The handle a track is dragged by sits with its middle 24 dp from the left edge, which is where the zone of the system's
back gesture ends. A drag that began at 24 dp moved the window aside as a back gesture does and reordered nothing; one
that began at 36 dp, still on the handle, moved the track. The touches were injected and went straight down; a finger
may fare differently, and Chrome on a phone has the same zone. To verify on a device, with the web client in Chrome too.
If it holds, the remedies are `View.setSystemGestureExclusionRects` (the system allows 200 dp of an edge) or a handle
further from the edge.

## Media session

A second part of the probe: a Media3 `MediaSessionService` whose player is a `SimpleBasePlayer` over the player's HTTP
API, with a remote `DeviceInfo` of 25 volume steps, an event stream on OkHttp and a Digest interceptor of its own (about
70 lines). Opening a player in the probe connects a `MediaController`, which starts the service. On the emulator
(Android 17):

- **The session** is listed by the system as remote (`volumeType=REMOTE, max=25`), with the track's title and the
  playback state. Media3 puts the service in the foreground while the player plays and shows the media notification with
  previous, pause and next and a seek bar, with no code of the probe's.
- **The volume keys** go to the session while the player plays: each press came as `handleIncreaseDeviceVolume` or
  `handleDecreaseDeviceVolume`, was sent as `PATCH /api/v1/player` and changed the player's volume by 0.04.
- **Media buttons do not reach the session** by themselves. The system sends them to the app that played audio last;
  this one plays none, and `dumpsys media_session` shows no media button session at all. With a loop of silence played
  while the player plays, the session becomes the media button session, and pause, play and next arrive; it stays so
  after the silence stops on a pause. The volume keys still go to the remote session then. The plan says the app plays
  no audio: for the buttons of a headset it has to play silence, or do without them.
- **The stream tells of changes only.** The session showed nothing until the first change; the state has to be read with
  `GET /api/v1/player` once the stream is open, as the web client does.
- **Next and previous need neighbours.** A player of one item has neither, and Media3 drops the commands before they
  reach `handleSeek`. The probe gives the track an empty item on each side and reads the command off the index it is
  asked to seek to.
- **Digest on OkHttp:** one counter of requests for all threads, and a new signature on `stale=true`, were enough;
  `okhttp-digest` was not tried.
- **Cosmetic:** the system's media card names the output "This phone", and the notification has Media3's stock icon.

On a phone with Android 11, without the silence (2026-11-22):

- **The notification and the lock screen** show the title, the artist and the cover, and their buttons work.
- **The volume keys** set the player's volume with the app open, in the background and with the screen off, while the
  player plays; the system shows its slider for a remote device. A key held down sent up to 19 commands a second, all
  answered. On pause the keys go back to the phone's own volume, which is acceptable.
- **Swiped away while playing,** the app leaves the service running: the notification stays and the keys still work.
  Swiped away on pause, it takes the notification with it. Media3 pauses the player as it stops the service then; the
  player's pause being a toggle, only the check "is it playing" kept that from resuming the music.
- **The seek bar jumped back** to the old position after a seek, then came right. A command's request is answered before
  the player's event arrives, and Media3 reads the state as soon as the command's future completes: the state must
  already hold what the command asked for, as it did for the volume. The probe now sets the position at once (not tried
  on the phone again); play and pause have the same gap.
- **Not tried:** the buttons of a headset, with the silence or without.

## What this changes in the plan

- **Confirmed:** the web view answers Digest for the page, its `fetch` calls and the event stream through
  `onReceivedHttpAuthRequest`, with no change on the server or in the client; the bridge; the file chooser; native
  downloads fed by the `DownloadListener`; back navigation left to the client's history.
- **Answered:** the open question on the local network permission. `ACCESS_LOCAL_NETWORK` in the manifest, asked at run
  time from API 37, before the first connection; the UDP discovery of stage 3 needs it too.
- **To add:** credentials are verified natively before the web view opens (stage 4). The file chooser maps extensions to
  MIME types, or opens `*/*` and filters (stage 7). The web view is drawn edge to edge from the start, since the client
  already keeps to the safe areas (stage 8 has less to do).
- **To take over as it is:** `configChanges` on the Activity; `MATCH_PARENT` on the web view; a day/night theme.

## Not checked

- **The media session on a device:** see [Media session](#media-session). Lowering the volume for a call was not probed.
- **`okhttp-digest`:** the probe signs requests itself. Whether the library keeps to the server's `nc` rules is open,
  and matters only if the app prefers it to 70 lines of its own.
- **On a device only:** UDP discovery (the emulator's NAT drops broadcasts), Wi-Fi without internet, the rocker and
  calls on vendors' builds, the drag by the edge.
- **Older Android:** everything ran on API 37; `minSdk` is 26.
- **Android Studio:** the probe was built from the command line only (`gradlew` with `JAVA_HOME` at Studio's `jbr`).
