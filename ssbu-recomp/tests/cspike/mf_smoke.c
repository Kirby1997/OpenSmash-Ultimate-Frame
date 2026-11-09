/* C-spike: smoke test of plat_mf.c (Media Foundation source reader on FFmpeg) with a synthetic WebM (VP9 + Vorbis, made by
 * ffmpeg's testsrc2 / sine; no game data). Calls the reader the way nnmovie_mf.c does: SHCreateMemStream ->
 * MFCreateMFByteStreamOnStream -> MFCreateSourceReaderFromByteStream, native types, NV12 / PCM16 output types, then
 * ReadSample(stream) per stream to the end. Checks frame and sample counts, time stamps, a non-trivial picture; prints
 * the decode time per video frame.   mf_smoke <file.webm> <expected video frames> <expected audio seconds> */
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); fails++; } else printf("ok   "); printf(__VA_ARGS__); printf("\n"); } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s file.webm frames audio_seconds\n", argv[0]); return 2; }
    int want_frames = atoi(argv[2]); double want_secs = atof(argv[3]);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    BYTE *data = malloc((size_t)size);
    if (fread(data, 1, (size_t)size, f) != (size_t)size) return 2;
    fclose(f);
    CHECK(MFStartup(MF_VERSION, 0) == S_OK, "MFStartup");
    IStream *st = SHCreateMemStream(data, (UINT)size);
    IMFByteStream *bs = NULL; IMFSourceReader *rd = NULL;
    CHECK(st && SUCCEEDED(MFCreateMFByteStreamOnStream(st, &bs)), "byte stream on a memory stream (%ld bytes)", size);
    CHECK(SUCCEEDED(MFCreateSourceReaderFromByteStream(bs, NULL, &rd)), "source reader");
    if (!rd) return 1;
    PROPVARIANT var; memset(&var, 0, sizeof var);
    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(rd, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &var)) && var.vt == VT_UI8)
        printf("     duration %.3f s\n", var.uhVal.QuadPart / 1e7);
    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    int vid = -1, aud = -1, w = 0, h = 0; UINT32 rate = 0, ch = 0;
    for (DWORD i = 0; i < 8; i++) {
        IMFMediaType *t = NULL;
        if (FAILED(IMFSourceReader_GetNativeMediaType(rd, i, 0, &t))) break;
        GUID major; IMFMediaType_GetGUID(t, &MF_MT_MAJOR_TYPE, &major);
        if (IsEqualGUID(&major, &MFMediaType_Video) && vid < 0) {
            UINT64 fs = 0; IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &fs); w = (int)(fs >> 32); h = (int)(UINT32)fs; vid = (int)i;
        } else if (IsEqualGUID(&major, &MFMediaType_Audio) && aud < 0) {
            IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate); IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, &ch); aud = (int)i;
        }
        IMFMediaType_Release(t);
    }
    CHECK(vid >= 0 && w > 0 && h > 0, "video stream %d: %dx%d", vid, w, h);
    CHECK(aud >= 0 && rate > 0 && ch > 0, "audio stream %d: %u Hz, %u channels", aud, rate, ch);
    IMFMediaType *t; MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video); IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    CHECK(SUCCEEDED(IMFSourceReader_SetCurrentMediaType(rd, (DWORD)vid, NULL, t)), "video output NV12");
    IMFMediaType_Release(t); MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio); IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16); IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, ch);
    CHECK(SUCCEEDED(IMFSourceReader_SetCurrentMediaType(rd, (DWORD)aud, NULL, t)), "audio output PCM16");
    IMFMediaType_Release(t);
    IMFSourceReader_SetStreamSelection(rd, (DWORD)vid, TRUE);
    IMFSourceReader_SetStreamSelection(rd, (DWORD)aud, TRUE);
    /* interleaved as the game's two decoder threads would ask: one video frame, then audio up to the same time */
    int frames = 0, vdone = 0, adone = 0, mono = 1, picture_ok = 0; long long pcm_frames = 0; LONGLONG vt = -1, at = -1;
    double vsec = 0;
    while (!vdone || !adone) {
        DWORD idx, fl; LONGLONG ts; IMFSample *s = NULL;
        if (!vdone) {
            double t0 = now();
            HRESULT hr = IMFSourceReader_ReadSample(rd, (DWORD)vid, 0, &idx, &fl, &ts, &s);
            vsec += now() - t0;
            if (FAILED(hr) || (fl & MF_SOURCE_READERF_ERROR)) { printf("     video ReadSample error %08lx\n", (unsigned long)hr); fails++; vdone = 1; }
            else if (fl & MF_SOURCE_READERF_ENDOFSTREAM) vdone = 1;
            if (s) {
                IMFMediaBuffer *b; BYTE *p; DWORD mx, len;
                IMFSample_ConvertToContiguousBuffer(s, &b); IMFMediaBuffer_Lock(b, &p, &mx, &len);
                if (frames == 10) {             /* testsrc2: a picture with many luma levels; chroma not all 128 */
                    int lo = 255, hi = 0; for (int k = 0; k < w * h; k += 97) { if (p[k] < lo) lo = p[k]; if (p[k] > hi) hi = p[k]; }
                    picture_ok = len >= (DWORD)(w * h * 3 / 2) && hi - lo > 100;
                    printf("     frame 10: %lu bytes (NV12 %d), luma %d..%d\n", (unsigned long)len, w * h * 3 / 2, lo, hi);
                }
                IMFMediaBuffer_Unlock(b); IMFMediaBuffer_Release(b);
                if (ts <= vt) mono = 0;
                vt = ts; frames++; IMFSample_Release(s);
            }
        }
        while (!adone && (vdone || at < vt)) {
            HRESULT hr = IMFSourceReader_ReadSample(rd, (DWORD)aud, 0, &idx, &fl, &ts, &s);
            if (FAILED(hr) || (fl & MF_SOURCE_READERF_ERROR)) { printf("     audio ReadSample error %08lx\n", (unsigned long)hr); fails++; adone = 1; break; }
            if (fl & MF_SOURCE_READERF_ENDOFSTREAM) { adone = 1; break; }
            if (s) {
                IMFMediaBuffer *b; BYTE *p; DWORD mx, len;
                IMFSample_ConvertToContiguousBuffer(s, &b); IMFMediaBuffer_Lock(b, &p, &mx, &len);
                pcm_frames += len / (2 * ch);
                IMFMediaBuffer_Unlock(b); IMFMediaBuffer_Release(b);
                at = ts; IMFSample_Release(s);
            }
        }
    }
    CHECK(frames == want_frames, "video frames %d (want %d)", frames, want_frames);
    CHECK(mono, "video time stamps increase (last %.3f s)", vt / 1e7);
    CHECK(picture_ok, "decoded picture is not blank");
    double secs = rate ? (double)pcm_frames / rate : 0;
    CHECK(secs > want_secs - 0.1 && secs < want_secs + 0.1, "audio %.3f s of PCM16 (want %.1f)", secs, want_secs);
    printf("     video decode + NV12 convert: %.2f ms per frame (%dx%d, %d frames)\n", frames ? vsec * 1000 / frames : 0, w, h, frames);
    IMFSourceReader_Release(rd); IMFByteStream_Release(bs); IStream_Release(st);
    printf("%d failure(s)\n", fails);
    return fails != 0;
}
