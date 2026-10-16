// miwiwicap: record one process tree's audio (WASAPI process loopback) or the whole
// default output (WASAPI loopback) to a 48 kHz / 16-bit / stereo WAV file.
//
//   miwiwicap --name Spotify.exe --seconds 15 --out spotify.wav
//   miwiwicap --pid 1234 --seconds 15 --out app.wav
//   miwiwicap --system --seconds 15 --out all.wav
//
// Prints per-second RMS / peak levels (dBFS) to stdout when done.

#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

static const DWORD kRate = 48000;
static const WORD kChannels = 2;
static const WORD kBits = 16;

class ActivateHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
    LONG ref_ = 1;
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_FAIL;
    IAudioClient* client = nullptr;

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject))
            *ppv = static_cast<IAgileObject*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref_); }
    STDMETHOD_(ULONG, Release)() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT activateHr = E_FAIL;
        IUnknown* punk = nullptr;
        result = op->GetActivateResult(&activateHr, &punk);
        if (SUCCEEDED(result)) result = activateHr;
        if (SUCCEEDED(result)) result = punk->QueryInterface(IID_PPV_ARGS(&client));
        if (punk) punk->Release();
        SetEvent(done);
        return S_OK;
    }
};

// Root process of the given exe name: a match whose parent is not also a match.
static DWORD FindRootPid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    std::vector<PROCESSENTRY32W> matches;
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, name) == 0) matches.push_back(pe);
    CloseHandle(snap);
    for (auto& m : matches) {
        bool parentMatches = false;
        for (auto& o : matches)
            if (o.th32ProcessID == m.th32ParentProcessID) parentMatches = true;
        if (!parentMatches) return m.th32ProcessID;
    }
    return 0;
}

static void Fail(const char* what, HRESULT hr) {
    fprintf(stderr, "error: %s (hr=0x%08lX)\n", what, (unsigned long)hr);
    exit(1);
}

static void WriteWavHeader(FILE* f, uint32_t dataBytes) {
    uint32_t byteRate = kRate * kChannels * kBits / 8;
    uint16_t blockAlign = kChannels * kBits / 8;
    uint32_t riffSize = 36 + dataBytes, fmtSize = 16;
    uint16_t pcm = 1, ch = kChannels, bits = kBits;
    uint32_t rate = kRate;
    fwrite("RIFF", 1, 4, f); fwrite(&riffSize, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtSize, 4, 1, f); fwrite(&pcm, 2, 1, f);
    fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
}

int wmain(int argc, wchar_t** argv) {
    DWORD pid = 0;
    bool systemMode = false;
    double seconds = 10;
    std::wstring out = L"out.wav";
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--pid" && i + 1 < argc) pid = wcstoul(argv[++i], nullptr, 10);
        else if (a == L"--name" && i + 1 < argc) {
            const wchar_t* n = argv[++i];
            pid = FindRootPid(n);
            if (!pid) { fwprintf(stderr, L"error: no process named %s\n", n); return 1; }
        }
        else if (a == L"--system") systemMode = true;
        else if (a == L"--seconds" && i + 1 < argc) seconds = _wtof(argv[++i]);
        else if (a == L"--out" && i + 1 < argc) out = argv[++i];
        else { fwprintf(stderr, L"unknown argument: %s\n", a.c_str()); return 1; }
    }
    if (!systemMode && !pid) { fprintf(stderr, "usage: miwiwicap (--pid N | --name X.exe | --system) [--seconds S] [--out F.wav]\n"); return 1; }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) Fail("CoInitializeEx", hr);

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = kChannels;
    wfx.nSamplesPerSec = kRate;
    wfx.wBitsPerSample = kBits;
    wfx.nBlockAlign = kChannels * kBits / 8;
    wfx.nAvgBytesPerSec = kRate * wfx.nBlockAlign;

    IAudioClient* client = nullptr;
    HANDLE packetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (systemMode) {
        IMMDeviceEnumerator* en = nullptr;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (FAILED(hr)) Fail("MMDeviceEnumerator", hr);
        IMMDevice* dev = nullptr;
        hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (FAILED(hr)) Fail("GetDefaultAudioEndpoint", hr);
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
        if (FAILED(hr)) Fail("IMMDevice::Activate", hr);
        printf("capturing: default output device (everything)\n");
    } else {
        AUDIOCLIENT_ACTIVATION_PARAMS params = {};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = pid;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT pv = {};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(params);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
        auto* handler = new ActivateHandler();
        IActivateAudioInterfaceAsyncOperation* op = nullptr;
        hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler, &op);
        if (FAILED(hr)) Fail("ActivateAudioInterfaceAsync", hr);
        WaitForSingleObject(handler->done, INFINITE);
        if (FAILED(handler->result)) Fail("process loopback activation", handler->result);
        client = handler->client;
        op->Release();
        handler->Release();
        printf("capturing: process tree of pid %lu\n", pid);
    }

    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (!systemMode) flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &wfx, nullptr);
    if (FAILED(hr)) Fail("IAudioClient::Initialize", hr);
    if (!systemMode) {
        hr = client->SetEventHandle(packetEvent);
        if (FAILED(hr)) Fail("SetEventHandle", hr);
    }
    IAudioCaptureClient* capture = nullptr;
    hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) Fail("GetService(IAudioCaptureClient)", hr);

    FILE* f = _wfopen(out.c_str(), L"wb");
    if (!f) { fwprintf(stderr, L"error: cannot open %s\n", out.c_str()); return 1; }
    WriteWavHeader(f, 0);

    const uint64_t totalFrames = (uint64_t)(seconds * kRate);
    uint64_t written = 0;
    std::vector<int16_t> silence(kRate / 10 * kChannels, 0);
    std::vector<int16_t> pcm;  // whole capture, for the level report

    auto put = [&](const int16_t* data, uint64_t frames) {
        frames = std::min(frames, totalFrames - written);
        fwrite(data, sizeof(int16_t) * kChannels, (size_t)frames, f);
        pcm.insert(pcm.end(), data, data + frames * kChannels);
        written += frames;
    };

    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    hr = client->Start();
    if (FAILED(hr)) Fail("IAudioClient::Start", hr);
    QueryPerformanceCounter(&t0);

    while (written < totalFrames) {
        if (systemMode) Sleep(10); else WaitForSingleObject(packetEvent, 50);
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data; UINT32 frames; DWORD bflags;
            hr = capture->GetBuffer(&data, &frames, &bflags, nullptr, nullptr);
            if (FAILED(hr)) Fail("GetBuffer", hr);
            if (bflags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::vector<int16_t> z((size_t)frames * kChannels, 0);
                put(z.data(), frames);
            } else {
                put(reinterpret_cast<int16_t*>(data), frames);
            }
            capture->ReleaseBuffer(frames);
            if (written >= totalFrames) break;
        }
        // Loopback delivers nothing while the source is silent; keep the timeline
        // aligned to wall-clock time by padding with silence.
        QueryPerformanceCounter(&now);
        uint64_t expected = (uint64_t)((double)(now.QuadPart - t0.QuadPart) / freq.QuadPart * kRate);
        uint64_t slack = kRate / 5;
        while (written + slack < expected && written < totalFrames)
            put(silence.data(), std::min<uint64_t>(silence.size() / kChannels, expected - slack - written));
    }
    client->Stop();

    uint32_t dataBytes = (uint32_t)(written * kChannels * sizeof(int16_t));
    fseek(f, 0, SEEK_SET);
    WriteWavHeader(f, dataBytes);
    fclose(f);
    wprintf(L"wrote %s (%.1f s)\n", out.c_str(), (double)written / kRate);

    printf("sec   rms dBFS   peak dBFS\n");
    for (uint64_t s = 0; s * kRate < written; s++) {
        uint64_t a = s * kRate * kChannels, b = std::min<uint64_t>((s + 1) * kRate * kChannels, pcm.size());
        double sum = 0; int peak = 0;
        for (uint64_t i = a; i < b; i++) { int v = pcm[i]; sum += (double)v * v; peak = std::max(peak, abs(v)); }
        double rms = sqrt(sum / std::max<uint64_t>(1, b - a));
        auto db = [](double x) { return x > 0 ? 20 * log10(x / 32768.0) : -120.0; };
        printf("%3llu   %8.1f   %9.1f\n", (unsigned long long)s, db(rms), db((double)peak));
    }
    return 0;
}
