# miwiwicap

**Mi**crosoft **Wi**ndows **wi**ndow **cap**ture: record the audio of a single app on Windows, without Stereo Mix, virtual cables or OBS.

It uses the WASAPI process loopback API (the same mechanism behind OBS's "Application Audio Capture"), so you can record Spotify on its own while YouTube plays in your browser.

## Usage

```
miwiwicap --name Spotify.exe --seconds 30 --out spotify.wav
miwiwicap --pid 1234 --seconds 30 --out app.wav
miwiwicap --system --seconds 30 --out everything.wav
```

| Option | Meaning |
|---|---|
| `--name X.exe` | Capture the process tree rooted at the first process with this exe name |
| `--pid N` | Capture the process tree of this process ID |
| `--system` | Capture everything sent to the default output device |
| `--seconds S` | Recording length (default 10) |
| `--out F.wav` | Output file (default `out.wav`) |

Output is always 48 kHz, 16-bit, stereo PCM WAV. Silence while the app plays nothing is padded, so the file length matches wall-clock time. After recording it prints per-second RMS and peak levels in dBFS.

Two recordings at once (e.g. Spotify and Edge into separate files) work fine; just run two instances.

## Notes

- Capture is per **process tree**, not per window: all tabs of a browser end up in the same recording.
- Audio is captured after the app's own volume and its slider in the Windows volume mixer.
- Requires Windows 10 version 2004 (build 19041) or later.

## Building

Run `build.bat` from any shell. It needs Visual Studio 2022 (or Build Tools) with the C++ workload, and produces `miwiwicap.exe`.

## License

[MIT](LICENSE)
