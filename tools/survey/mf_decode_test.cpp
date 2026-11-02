// mf_decode_test: decode movies with Windows Media Foundation (IMFSourceReader, the same H.264 MFT / AAC MFT the port would use)
// and report frames, audio samples, resolution and decode speed.
// usage: mf_decode_test.exe file.mp4 [file2.mp4 ...]
// build: pwsh tools\survey\build_mf_test.ps1
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <cstdio>
#include <chrono>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static int run(const wchar_t* path) {
    IMFSourceReader* rd = nullptr;
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 2);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   // lets the reader convert to the requested output type
    HRESULT hr = MFCreateSourceReaderFromURL(path, attr, &rd);
    rel(attr);
    if (FAILED(hr)) { printf("open failed 0x%08lx\n", hr); return 1; }

    IMFMediaType* vt = nullptr;
    MFCreateMediaType(&vt);
    vt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, vt);
    rel(vt);
    if (FAILED(hr)) { printf("set video type failed 0x%08lx\n", hr); rel(rd); return 1; }
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    bool has_audio = false;
    IMFMediaType* at = nullptr;
    MFCreateMediaType(&at);
    at->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    at->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, at))) {
        has_audio = true;
        rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    }
    rel(at);

    IMFMediaType* cur = nullptr;
    UINT32 w = 0, h = 0, fn = 0, fd = 0;
    if (SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(cur, MF_MT_FRAME_RATE, &fn, &fd);
    }
    rel(cur);
    UINT32 ach = 0, asr = 0, abits = 0;
    if (has_audio && SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) {
        ach = MFGetAttributeUINT32(cur, MF_MT_AUDIO_NUM_CHANNELS, 0);
        asr = MFGetAttributeUINT32(cur, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
        abits = MFGetAttributeUINT32(cur, MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
    }
    rel(cur);

    long long vframes = 0, abytes = 0, last_ts = 0;
    auto t0 = std::chrono::steady_clock::now();
    auto drain = [&](DWORD which, bool video) -> bool {
        for (;;) {
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            IMFSample* s = nullptr;
            HRESULT h2 = rd->ReadSample(which, 0, &stream, &flags, &ts, &s);
            if (FAILED(h2)) { printf("read failed 0x%08lx\n", h2); return false; }
            if (s) {
                if (video) { vframes++; last_ts = ts; }
                else { DWORD len = 0; IMFMediaBuffer* b = nullptr; if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) { b->GetCurrentLength(&len); abytes += len; rel(b); } }
                rel(s);
            }
            if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) return !(flags & MF_SOURCE_READERF_ERROR);
        }
    };
    if (!drain((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, true)) return 1;
    double dt_v = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (has_audio && !drain((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, false)) return 1;
    double dt = dt_v;
    printf("%ux%u %.2f fps  video frames %lld (last ts %.3f s)  decode %.0f fps", w, h, fd ? (double)fn / fd : 0.0, vframes, last_ts / 1e7,
           vframes / dt);
    if (has_audio) printf("  audio %uch %uHz %ubit, %lld bytes PCM (%.2f s)", ach, asr, abits, abytes, ach && asr && abits ? abytes / (double)(ach * asr * abits / 8) : 0.0);
    else printf("  no audio stream");
    printf("\n");
    rel(rd);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        wprintf(L"%.40ls  ", wcsrchr(argv[i], L'\\') ? wcsrchr(argv[i], L'\\') + 1 : argv[i]);
        bad += run(argv[i]);
    }
    MFShutdown();
    printf("%d of %d files failed\n", bad, argc - 1);
    return bad ? 1 : 0;
}
