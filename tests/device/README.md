# Android device acceptance fixture

`AndroidMemoryFixture.cpp` is a disposable arm64 target for live AMem checks. It
publishes stable marker addresses and guard values without touching a user app.
Build it with an Android NDK compiler, push it under `/data/local/tmp`, and run
it as a normal shell process. The AMem server may run as root.

Example with NDK r27c:

```powershell
& "$env:ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android26-clang++.cmd" `
  -std=c++17 -O0 -g -fPIE -pie `
  tests/device/AndroidMemoryFixture.cpp -o build/amem_device_fixture

adb push build/amem_device_fixture /data/local/tmp/amem_fixture
adb shell chmod 755 /data/local/tmp/amem_fixture
adb shell "nohup /data/local/tmp/amem_fixture >/data/local/tmp/amem_fixture.log 2>&1 </dev/null &"
```

The startup line contains `marker_address`, `scan_address`, their original
values, and two guards. Restrict writes, scans, and breakpoints to this process.
Restore both markers, send `SIGUSR1`, and verify the snapshot plus guards before
ending a test. A live acceptance record must also identify the exact Android
server binary/version because AMem's full server protocol is not compatible
with the separately maintained compact MiniMem protocol.
