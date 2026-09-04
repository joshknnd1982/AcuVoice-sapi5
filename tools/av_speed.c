/* av_speed.c -- is Aspeed.dll's ChangeBufferSpeed16 usable as the rate control?
 *
 * AcuEng.dll (the original SAPI4 engine) drives Aspeed for rate, so its algorithm is
 * the authentic AcuVoice one. ChangeBufferSpeed16 is the whole-buffer entry point:
 *   ChangeBufferSpeed16(short *in, int inSamples, short **ppOut, int *pOutSamples,
 *                       const char *optional_file, float factor)
 * This checks that reading against the real dll, at several factors.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef int(__stdcall *fn_initialize_SSB)(void *ssb, const char *text, int arg3);
typedef int(__stdcall *fn_close_SSB)(void *ssb);
typedef int(__stdcall *fn_set_tag_flag)(void *ssb, int on);
typedef int(__stdcall *fn_isSeg_Available)(void *ssb);
typedef int(__stdcall *fn_synth_to_buffer)(void *ssb, void *buf, unsigned long *len);
typedef int(__stdcall *fn_ChangeBufferSpeed16)(short *in, int inLen, short **ppOut,
                                               int *pOutLen, const char *file, float factor);

static HMODULE g_av, g_as;
static char g_outdir[MAX_PATH];

static short ulaw2linear(unsigned char u)
{
    static const int lut[8] = { 0, 132, 396, 924, 1980, 4092, 8316, 16764 };
    int sign, e, m, s;
    u = (unsigned char)~u;
    sign = u & 0x80; e = (u >> 4) & 7; m = u & 15;
    s = lut[e] + (m << (e + 3));
    return (short)(sign ? -s : s);
}

static void write_wav(const char *name, const short *pcm, size_t n, int sr)
{
    char path[MAX_PATH];
    FILE *f;
    unsigned long riff, fmtsz = 16, datasz = (unsigned long)(n * 2);
    unsigned short tag = 1, ch = 1, bps = 16, align = 2;
    unsigned long sps = (unsigned long)sr, avg = (unsigned long)sr * 2;
    sprintf(path, "%s\\%s", g_outdir, name);
    f = fopen(path, "wb");
    if (!f) { printf("   !! cannot write %s\n", path); return; }
    riff = 36 + datasz;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtsz, 4, 1, f);
    fwrite(&tag, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sps, 4, 1, f);
    fwrite(&avg, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&datasz, 4, 1, f);
    fwrite(pcm, 2, n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *avdll = (argc > 1) ? argv[1] : "engine\\Lib\\avcore.dll";
    const char *asdll = (argc > 2) ? argv[2] : "engine\\Lib\\Aspeed.dll";
    static const char *SENT =
        "The quick brown fox jumps over the lazy dog, and then it does so again.";

    fn_initialize_SSB p_init;
    fn_close_SSB p_close;
    fn_set_tag_flag p_tag;
    fn_isSeg_Available p_avail;
    fn_synth_to_buffer p_synth;
    fn_ChangeBufferSpeed16 p_speed;

    unsigned char *ssb, *tmp, *raw;
    short *pcm, *out;
    unsigned long total = 0;
    int i, k;
    static const float FACTORS[] = { 0.4f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 2.5f };

    setvbuf(stdout, NULL, _IONBF, 0);
    strcpy(g_outdir, (argc > 3) ? argv[3] : ".");

    g_av = LoadLibraryA(avdll);
    if (!g_av) { printf("avcore load failed %lu\n", GetLastError()); return 1; }
    g_as = LoadLibraryA(asdll);
    if (!g_as) { printf("Aspeed load failed %lu\n", GetLastError()); return 1; }

    p_init = (fn_initialize_SSB)GetProcAddress(g_av, "_initialize_SSB@12");
    p_close = (fn_close_SSB)GetProcAddress(g_av, "_close_SSB@4");
    p_tag = (fn_set_tag_flag)GetProcAddress(g_av, "_set_tag_flag@8");
    p_avail = (fn_isSeg_Available)GetProcAddress(g_av, "_isSeg_Available@4");
    p_synth = (fn_synth_to_buffer)GetProcAddress(g_av, "_synth_to_buffer@12");
    p_speed = (fn_ChangeBufferSpeed16)GetProcAddress(g_as, "_ChangeBufferSpeed16@24");
    if (!p_speed) { printf("no _ChangeBufferSpeed16@24\n"); return 1; }

    ssb = (unsigned char *)calloc(1, 8192);
    tmp = (unsigned char *)malloc(1024 * 1024);
    raw = (unsigned char *)malloc(4 * 1024 * 1024);

    if (p_init(ssb, SENT, 0) != 0) { printf("init failed\n"); return 1; }
    p_tag(ssb, 1);
    while (p_avail(ssb)) {
        unsigned long len = 0;
        if (p_synth(ssb, tmp, &len) != 0) break;
        memcpy(raw + total, tmp, len);
        total += len;
    }
    p_close(ssb);
    printf("source: %lu ulaw bytes (%.2f s at 8 kHz)\n", total, total / 8000.0);

    pcm = (short *)malloc(total * 2 + 64);
    for (i = 0; i < (int)total; i++) pcm[i] = ulaw2linear(raw[i]);
    write_wav("s_source.wav", pcm, total, 8000);

    out = (short *)malloc(16 * 1024 * 1024);

    for (k = 0; k < (int)(sizeof(FACTORS) / sizeof(FACTORS[0])); k++) {
        short *inCopy = (short *)malloc(total * 2 + 8 * 1024 * 1024);
        short *pOut = out;
        int outLen = -1;
        int rc;
        char name[64];
        memcpy(inCopy, pcm, total * 2);
        memset(out, 0, 1024);
        rc = p_speed(inCopy, (int)total, &pOut, &outLen, NULL, FACTORS[k]);
        printf("  factor %.2f -> rc=%d outLen=%d  (in %lu samples)  ratio %.3f\n",
               FACTORS[k], rc, outLen, total,
               outLen > 0 ? (double)outLen / (double)total : 0.0);
        if (rc == 0 && outLen > 0 && outLen < 8 * 1024 * 1024) {
            sprintf(name, "s_factor_%03d.wav", (int)(FACTORS[k] * 100));
            write_wav(name, pOut, outLen, 8000);
        }
        free(inCopy);
    }

    return 0;
}
