# HaloAI phone app

Flutter, Android first. Turns device audio into text on the phone with Parakeet v3 (sherpa-onnx) and talks to the backend. See `docs/BRIEFING.md` section 3.2 for phases.

Current state: speech to text test bench, plus the P1 BLE link to the device.

- **Run clips:** transcribes the 5 test clips in `assets/clips/` twice and prints `P0RESULT` lines to logcat.
- **Hold to talk (test mic), TEST ONLY:** records from the phone's own mic while held, transcribes on release. Talk after the vibration (the mic needs about 0.2 s to start). Presses under 0.5 s are discarded, like a device hold. Each recording is saved for reuse as a test clip. In the real product audio comes from the device over BLE (P1); `lib/mic_test_recorder.dart` is not part of that path.

- **Loopback (P1):** runs the 5 clips through the whole BLE audio path without a radio (IMA ADPCM encode, 176 byte chunks with every 9th lost on purpose, cumulative acks and `resend_from`, decode) and compares the transcript with the original WAV's. Prints `LOOPRESULT` lines.
- **Connect (P1):** scans for the HaloAI service, connects, asks for MTU 517 (fails loudly below 179), requests the 2M PHY, then receives audio and events. Each finished segment is transcribed and printed as a `BLERESULT` line with release to complete (BLE) and release to text times. **Test screen** writes a sample REVIEW screen. Needs Victor's firmware on the Waveshare ESP32-S3 board.

Parakeet runs in a long-lived background isolate (`lib/stt_worker.dart`): the model loads once at app start and stays in memory. P1 feeds it decoded BLE audio the same way.

## BLE (P1)

Code in `lib/ble/`. Protocol from BRIEFING section 2; UUIDs and JSON field names in `docs/ble_fields_draft.md` (DRAFT until Victor agrees). The BLE package (flutter_blue_plus) sits behind `DeviceTransport`, so everything else is tested with a fake transport.

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
