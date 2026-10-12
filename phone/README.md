# HaloAI phone app

Flutter, Android first. Turns device audio into text on the phone with Parakeet v3 (sherpa-onnx) and talks to the backend. See `docs/BRIEFING.md` section 3.2 for phases.

Current state: speech to text test bench.

- **Run clips:** transcribes the 5 test clips in `assets/clips/` twice and prints `P0RESULT` lines to logcat.
- **Hold to talk (test mic), TEST ONLY:** records from the phone's own mic while held, transcribes on release. Talk after the vibration (the mic needs about 0.2 s to start). Presses under 0.5 s are discarded, like a device hold. Each recording is saved for reuse as a test clip. In the real product audio comes from the device over BLE (P1); `lib/mic_test_recorder.dart` is not part of that path.

Parakeet runs in a long-lived background isolate (`lib/stt_worker.dart`): the model loads once at app start and stays in memory. P1 feeds it decoded BLE audio the same way.

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
