# HaloAI phone app

Flutter, Android first. Turns device audio into text on the phone with Parakeet v3 (sherpa-onnx) and talks to the backend. See `docs/BRIEFING.md` section 3.2 for phases.

Current state: test bench with speech to text, the P1 BLE link to the device, the P2 backend client (text only to the backend, never audio), and P3: it keeps working with the phone locked.

- **Run clips:** transcribes the 5 test clips in `assets/clips/` twice and prints `P0RESULT` lines to logcat.
- **Hold to talk (test mic), TEST ONLY:** records from the phone's own mic while held, transcribes on release. Talk after the vibration (the mic needs about 0.2 s to start). Presses under 0.5 s are discarded, like a device hold. Each recording is saved for reuse as a test clip. In the real product audio comes from the device over BLE (P1); `lib/mic_test_recorder.dart` is not part of that path.

- **Loopback (P1):** runs the 5 clips through the whole BLE audio path without a radio (IMA ADPCM encode, 176 byte chunks with every 9th lost on purpose, cumulative acks and `resend_from`, decode) and compares the transcript with the original WAV's. Prints `LOOPRESULT` lines.
- **Connect (P1):** scans for the HaloAI service, connects, asks for MTU 517 (fails loudly below 179), requests the 2M PHY, then receives audio and events. Each finished segment is transcribed and printed as a `BLERESULT` line with release to complete (BLE) and release to text times. **Test screen** writes a sample REVIEW screen (the device accepts it only while PROCESSING). Needs Victor's firmware, on the Waveshare ESP32-S3 badge or the CrowPanel development board.
- **Backend panel (P2):** type a command and tap **Send** (new request) or **Add** (addition), or use the mic hold (a hold while a proposal is on screen adds to it, like on the device). The panel shows the screen exactly as the device receives it, with **Approve**, **Continue**, **Cancel** and one button per candidate. When connected, device presses take the same paths. Prints `APIRESULT` lines with phone and backend timings.

Parakeet runs in a long-lived background isolate (`lib/stt_worker.dart`): the model loads once at app start and stays in memory. P1 feeds it decoded BLE audio the same way.

## BLE (P1)

Code in `lib/ble/`. Protocol from BRIEFING section 2; UUIDs and JSON field names in `docs/ble_fields_draft.md` (agreed with Victor 2026-10-13, now in BRIEFING section 2). The BLE package (flutter_blue_plus) sits behind `DeviceTransport`, so everything else is tested with a fake transport.

```sh
adb logcat -c
# open the app, tap Connect, hold the device button and talk
adb logcat -d | grep -E 'BLERESULT|BLELOG'
# or, without the device: tap Loopback
adb logcat -d | grep LOOPRESULT
```

Firmware check: `test/golden/adpcm_chunks.json` holds a test signal and the exact packets the phone expects; `tools/audio_reference/` is the Python reference that writes it.

```sh
python3 -m venv tools/audio_reference/.venv && tools/audio_reference/.venv/bin/pip install pytest
tools/audio_reference/.venv/bin/python -m pytest -q tools/audio_reference/tests
python3 tools/audio_reference/make_golden.py   # regenerate the golden vectors
```

## Backend (P2)

Code in `lib/backend/`: `backend_client.dart` (REST and Server-Sent Events; every retry reuses its `call_id`, so a retry never acts twice), `request_controller.dart` (results to Screen writes, device presses to backend calls, polling after approve, reconciling a saved execution key). The reference for every call and Screen write is `pipeline/scripts/phone_sim.py`.

The backend URL and API key never go in git. Copy the example and fill in a key from Orlando (scope `mock`, sample data, for development):

```sh
cp config/backend.example.json config/backend.json   # gitignored
flutter build apk --release --dart-define-from-file=config/backend.json
adb install -r build/app/outputs/flutter-apk/app-release.apk
adb logcat -c
# open the app, type "block 3 to 4 tomorrow for the gym", tap Send, then Approve
adb logcat -d | grep -E 'APIRESULT|APILOG'
```

Without the file the app still runs; the panel says the backend is off. Tests:

```sh
flutter test                                                        # offline, fake backend
flutter test test/backend/live_test.dart --dart-define-from-file=config/backend.json   # live server; approves, so sample data keys only
```

## Locked in a pocket (P3)

The speech model, the BLE session and the request controller live in `lib/halo_runtime.dart` for the whole process, not in the page, so closing or swiping away the screen does not stop them. On the first connect the app starts a foreground service (`android/app/src/main/kotlin/.../HaloService.kt`, type `connectedDevice`, notification "HaloAI, Connected to your badge"); Disconnect stops it. The Flutter engine is created by `MainApplication` and cached, so the activity only attaches to it. `lib/work_lock.dart` holds a partial wake lock only while a command is in flight (from `record_start` to REVIEW or an outcome; 120 s at most), so the phone can sleep while REVIEW waits for a press. The status line under Device shows the service, notifications, battery setting and the last wake lock.

No battery exemption or Samsung setting was needed on the Z Flip7 (2026-10-21). Every log line also goes to a file, because a locked run must be unplugged (USB charging keeps the phone out of Doze):

```sh
adb shell dumpsys activity services com.haloai.haloai_phone | grep isForeground   # service running
adb shell dumpsys deviceidle force-idle      # with the screen off: force deep Doze for a test
adb shell dumpsys deviceidle unforce && adb shell dumpsys battery reset           # undo
adb pull /sdcard/Android/data/com.haloai.haloai_phone/files/logs/halo_$(date +%Y%m%d).log
grep -E 'P3LOG|APIRESULT|BLERESULT' halo_*.log
```

## App features (P4, in progress)

The app has four tabs: **Home** (badge link, phone mic hold, typed commands, the current screen as the device gets it, and the full draft the device's 48 x 12 text budget cuts), **Memory** (view, add, edit and delete what the assistant remembers), **Settings** (badge orientation, wallpaper, Google account) and **Debug** (the P0 to P3 bench). Code in `lib/ui/`, `lib/backend/draft_view.dart` and `lib/wallpaper/`.

- **Orientation** is sent to the badge in Settings and with every request, and saved in the app's own storage.
- **Wallpaper**: pick a photo, and the app crops, resizes and dithers it to the panel's 1 bit framebuffer and previews exactly those bits. Sending uses a format proposed to Victor (`docs/ble_fields_draft.md`, Wallpaper section) and stays off until the badge's `hello` says `wallpaper: true`.
- **Google account**: Connect opens Google's sign in page in the browser and the app polls until it is linked. The sample data key cannot connect (422). For testing on your own Google account, Orlando issues a separate real scope key on its own test user; put it in `config/backend.google-test.json` (gitignored) as `HALO_GOOGLE_KEY`. Only the Google section uses it; requests and approvals keep the sample key.

```sh
flutter build apk --release --dart-define-from-file=config/backend.json --dart-define-from-file=config/backend.google-test.json
```

## Pull mic test recordings

```sh
adb pull /sdcard/Android/data/com.haloai.haloai_phone/files/recordings ./recordings
cat recordings/log.tsv   # file, audio s, decode ms, release to text ms, text
```

`recordings/` is gitignored. Add a good clip to `assets/clips/` and its spoken text to `test/clips_reference.txt` to make it part of the benchmark.

## Setup (macOS)

```sh
brew install --cask flutter android-commandlinetools
brew install openjdk@17 ffmpeg
export ANDROID_SDK_ROOT=/opt/homebrew/share/android-commandlinetools
export JAVA_HOME=/opt/homebrew/opt/openjdk@17 PATH=$JAVA_HOME/bin:$ANDROID_SDK_ROOT/platform-tools:$PATH
yes | sdkmanager --licenses
sdkmanager "platform-tools" "platforms;android-36" "build-tools;36.0.0"
flutter config --android-sdk $ANDROID_SDK_ROOT --jdk-dir $JAVA_HOME
```

Verify: `flutter doctor` shows "Android toolchain" with a check mark, and `adb devices` lists the phone.

## Model (not in git, not in the APK)

```sh
curl -LO https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8.tar.bz2
tar xjf sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8.tar.bz2
```

Install the app, open it and tap Run once (that makes the app create its model folder; it will say the model is missing). Then push the model and restart the app:

```sh
D=/sdcard/Android/data/com.haloai.haloai_phone/files/models/parakeet-v3
for f in encoder.int8.onnx decoder.int8.onnx joiner.int8.onnx tokens.txt; do
  adb push sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/$f $D/
done
adb shell am force-stop com.haloai.haloai_phone
```

Do not create that folder with `adb shell mkdir`: the app cannot see files in a folder the shell user owns.

## Run the P0 benchmark

```sh
flutter build apk --release
adb install -r build/app/outputs/flutter-apk/app-release.apk
adb shell cmd connectivity airplane-mode enable   # proves no network
adb logcat -c
# open the app, tap Run, wait about 5 s
adb logcat -d | grep P0RESULT
adb shell cmd connectivity airplane-mode disable
```

## Test clips

`assets/clips/*.wav` are 16 kHz mono 16-bit PCM, converted from phone recordings with
`ffmpeg -i in.m4a -ar 16000 -ac 1 -c:a pcm_s16le out.wav`. What was actually said is in `test/clips_reference.txt`.
