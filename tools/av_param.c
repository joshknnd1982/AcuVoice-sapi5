/* av_param.c -- does avcore apply the prosody tags itself, or only parse them?
 *
 * The same sentence is synthesized at every extreme of every control tag. If avcore
 * applies a tag, the audio changes; if it only parses it out of the text and queues the
 * value for the caller, every rendering is byte-identical and the dequeue functions
 * hand back the requested numbers instead.
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
typedef int(__stdcall *fn_deq)(void *ssb, void *out);
typedef int(__stdcall *fn_noargs)(void);

static HMODULE g_dll;
static fn_initialize_SSB p_init;
static fn_close_SSB p_close;
static fn_set_tag_flag p_tag;
static fn_isSeg_Available p_avail;
static fn_synth_to_buffer p_synth;
static fn_deq p_pitch_deq, p_speed_deq, p_volume_deq, p_pause_deq, p_Rpit_deq, p_Rspd_deq;
static fn_noargs p_sps;
static char g_outdir[MAX_PATH];

static void *sym(const char *n)
{
    void *p = (void *)GetProcAddress(g_dll, n);
    if (!p) printf("  !! missing %s\n", n);
    return p;
}

static short ulaw2linear(unsigned char u)
{
    static const int lut[8] = { 0, 132, 396, 924, 1980, 4092, 8316, 16764 };
    int sign, e, m, s;
    u = (unsigned char)~u;
    sign = u & 0x80; e = (u >> 4) & 7; m = u & 15;
    s = lut[e] + (m << (e + 3));
    return (short)(sign ? -s : s);
}

static void write_wav(const char *name, const unsigned char *ulaw, size_t n, int sr)
{
    char path[MAX_PATH];
    FILE *f;
    unsigned long riff, fmtsz = 16, datasz = (unsigned long)(n * 2);
    unsigned short tag = 1, ch = 1, bps = 16, align = 2;
    unsigned long sps = (unsigned long)sr, avg = (unsigned long)sr * 2;
    size_t i;
    if (!name) return;
    sprintf(path, "%s\\%s", g_outdir, name);
    f = fopen(path, "wb");
    if (!f) return;
    riff = 36 + datasz;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtsz, 4, 1, f);
    fwrite(&tag, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sps, 4, 1, f);
    fwrite(&avg, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&datasz, 4, 1, f);
    for (i = 0; i < n; i++) { short s = ulaw2linear(ulaw[i]); fwrite(&s, 2, 1, f); }
    fclose(f);
}

#define MAXAUD (4 * 1024 * 1024)
static unsigned char g_ref[MAXAUD];
static unsigned long g_reflen = 0;

static void run(const char *label, const char *file, const char *text, int report_queues)
{
    unsigned char *ssb = (unsigned char *)calloc(1, 8192);
    unsigned char *acc = (unsigned char *)malloc(MAXAUD);
    unsigned char *tmp = (unsigned char *)malloc(1024 * 1024);
    unsigned long total = 0;
    double sum = 0.0;
    unsigned long i;
    int identical;

    if (p_init(ssb, text, 0) != 0) { printf("  %-22s INIT FAILED\n", label); goto done; }
    p_tag(ssb, 1);
    while (p_avail(ssb)) {
        unsigned long len = 0;
        if (p_synth(ssb, tmp, &len) != 0) break;
        if (total + len > MAXAUD) break;
        memcpy(acc + total, tmp, len);
        total += len;
        if (report_queues) {
            int q[4];
            int r;
            memset(q, 0, sizeof(q)); r = p_pitch_deq(ssb, q);
            printf("      pitch_dequeue  rc=%d -> %d %d\n", r, q[0], q[1]);
            memset(q, 0, sizeof(q)); r = p_speed_deq(ssb, q);
            printf("      speed_dequeue  rc=%d -> %d %d\n", r, q[0], q[1]);
            memset(q, 0, sizeof(q)); r = p_volume_deq(ssb, q);
            printf("      volume_dequeue rc=%d -> %d %d\n", r, q[0], q[1]);
            memset(q, 0, sizeof(q)); r = p_pause_deq(ssb, q);
            printf("      pause_dequeue  rc=%d -> %d %d\n", r, q[0], q[1]);
            memset(q, 0, sizeof(q)); r = p_Rpit_deq(ssb, q);
            printf("      Rpit_dequeue   rc=%d -> %d %d\n", r, q[0], q[1]);
            memset(q, 0, sizeof(q)); r = p_Rspd_deq(ssb, q);
            printf("      Rspd_dequeue   rc=%d -> %d %d\n", r, q[0], q[1]);
        }
    }
    p_close(ssb);

    for (i = 0; i < total; i++) {
        double s = ulaw2linear(acc[i]) / 32768.0;
        sum += s * s;
    }
    identical = (g_reflen == total && total && memcmp(g_ref, acc, total) == 0);
    printf("  %-22s %7lu bytes  %5.2f s  rms %.4f  %s\n", label, total, total / 8000.0,
           total ? sqrt(sum / total) : 0.0,
           g_reflen == 0 ? "(reference)" : (identical ? "IDENTICAL to reference" : "differs"));
    if (g_reflen == 0) { memcpy(g_ref, acc, total); g_reflen = total; }
    write_wav(file, acc, total, p_sps());
done:
    free(ssb); free(acc); free(tmp);
}

int main(int argc, char **argv)
{
    const char *dll = (argc > 1) ? argv[1] : "engine\\Lib\\avcore.dll";
    char buf[512];
    static const char *SENT = "The quick brown fox jumps over the lazy dog.";
    int v;

    setvbuf(stdout, NULL, _IONBF, 0);
    strcpy(g_outdir, (argc > 2) ? argv[2] : ".");

    g_dll = LoadLibraryA(dll);
    if (!g_dll) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    p_init = (fn_initialize_SSB)sym("_initialize_SSB@12");
    p_close = (fn_close_SSB)sym("_close_SSB@4");
    p_tag = (fn_set_tag_flag)sym("_set_tag_flag@8");
    p_avail = (fn_isSeg_Available)sym("_isSeg_Available@4");
    p_synth = (fn_synth_to_buffer)sym("_synth_to_buffer@12");
    p_pitch_deq = (fn_deq)sym("_pitch_dequeue@8");
    p_speed_deq = (fn_deq)sym("_speed_dequeue@8");
    p_volume_deq = (fn_deq)sym("_volume_dequeue@8");
    p_pause_deq = (fn_deq)sym("_pause_dequeue@8");
    p_Rpit_deq = (fn_deq)sym("_Rpit_dequeue@8");
    p_Rspd_deq = (fn_deq)sym("_Rspd_dequeue@8");
    p_sps = (fn_noargs)sym("_get_snd_sps@0");

    printf("### reference (no tags)\n");
    run("plain", "p_plain.wav", SENT, 0);

    printf("\n### \\spd= (85..350, default 175)\n");
    for (v = 0; v < 5; v++) {
        static const int vals[] = { 85, 120, 175, 250, 350 };
        sprintf(buf, "\\spd=%d\\%s", vals[v], SENT);
        { char lbl[64], fil[64];
          sprintf(lbl, "spd=%d", vals[v]); sprintf(fil, "p_spd%03d.wav", vals[v]);
          run(lbl, fil, buf, 0); }
    }

    printf("\n### \\rspd= (50..200, default 100)\n");
    for (v = 0; v < 5; v++) {
        static const int vals[] = { 50, 75, 100, 150, 200 };
        sprintf(buf, "\\rspd=%d\\%s", vals[v], SENT);
        { char lbl[64], fil[64];
          sprintf(lbl, "rspd=%d", vals[v]); sprintf(fil, "p_rspd%03d.wav", vals[v]);
          run(lbl, fil, buf, 0); }
    }

    printf("\n### \\pit= (45..91, default 63)\n");
    for (v = 0; v < 5; v++) {
        static const int vals[] = { 45, 54, 63, 77, 91 };
        sprintf(buf, "\\pit=%d\\%s", vals[v], SENT);
        { char lbl[64], fil[64];
          sprintf(lbl, "pit=%d", vals[v]); sprintf(fil, "p_pit%03d.wav", vals[v]);
          run(lbl, fil, buf, 0); }
    }

    printf("\n### \\Rpit= (70..145, default 100)\n");
    for (v = 0; v < 5; v++) {
        static const int vals[] = { 70, 85, 100, 120, 145 };
        sprintf(buf, "\\Rpit=%d\\%s", vals[v], SENT);
        { char lbl[64], fil[64];
          sprintf(lbl, "Rpit=%d", vals[v]); sprintf(fil, "p_rpit%03d.wav", vals[v]);
          run(lbl, fil, buf, 0); }
    }

    printf("\n### \\vol= (0..65535, default 32767)\n");
    for (v = 0; v < 5; v++) {
        static const int vals[] = { 0, 16000, 32767, 50000, 65535 };
        sprintf(buf, "\\vol=%d\\%s", vals[v], SENT);
        { char lbl[64], fil[64];
          sprintf(lbl, "vol=%d", vals[v]); sprintf(fil, "p_vol%05d.wav", vals[v]);
          run(lbl, fil, buf, 0); }
    }

    printf("\n### \\ctx= contexts\n");
    {
        static const char *ctxs[] = { "normal", "business", "calm", "excited", "monotone",
                                      "emphatic", "address", "html", "stock", "unknown" };
        for (v = 0; v < 10; v++) {
            char lbl[64], fil[64];
            sprintf(buf, "\\ctx=%s\\%s", ctxs[v], SENT);
            sprintf(lbl, "ctx=%s", ctxs[v]); sprintf(fil, "p_ctx_%s.wav", ctxs[v]);
            run(lbl, fil, buf, 0);
        }
    }

    printf("\n### \\emp\\ and \\rst\\\n");
    run("emp", "p_emp.wav", "The quick brown \\emp\\fox jumps over the lazy dog.", 0);
    run("rst", "p_rst.wav", "\\spd=350\\\\rst\\The quick brown fox jumps over the lazy dog.", 0);

    printf("\n### \\pau=\n");
    run("pau=1000", "p_pau.wav", "The quick brown fox \\pau=1000\\ jumps over the lazy dog.", 0);

    printf("\n### queue contents while a tagged utterance streams\n");
    g_reflen = 0;
    run("tagged, queues", NULL,
        "\\spd=250\\\\pit=80\\\\vol=60000\\\\Rpit=130\\\\rspd=170\\\\pau=500\\Queued values.", 1);

    FreeLibrary(g_dll);
    return 0;
}
