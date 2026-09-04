/* av_probe.c -- empirical probe of the AcuVoice avcore.dll C API.
 *
 * avcore.dll is a 32-bit dll that depends only on kernel32 and user32, so it can be
 * driven directly with no SAPI4, no COM and no registry. Everything checked here was
 * read out of the disassembly first; this confirms it against the real engine.
 *
 * Build: tools\msvc32.bat cl /nologo /W3 /MT av_probe.c
 * Usage: av_probe <avcore.dll> <outdir> [subtest]
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int(__stdcall *fn_get_av_version)(char *out);
typedef int(__stdcall *fn_initialize_SSB)(void *ssb, const char *text, int arg3);
typedef int(__stdcall *fn_close_SSB)(void *ssb);
typedef int(__stdcall *fn_set_tag_flag)(void *ssb, int on);
typedef int(__stdcall *fn_isSeg_Available)(void *ssb);
typedef int(__stdcall *fn_synth_to_buffer)(void *ssb, void *buf, unsigned long *len);
typedef int(__stdcall *fn_synth_to_bufferEX)(void *ssb, void *buf, unsigned long *len, int p4);
typedef int(__stdcall *fn_synth_to_wave)(void *ssb, const char *path);
typedef int(__stdcall *fn_txtstr_to_sndbuf)(const char *text, void **ppbuf,
                                            unsigned long *pcb, unsigned long flags);
typedef int(__stdcall *fn_txtstr_to_sndfil)(const char *text, const char *path,
                                            unsigned long flags);
typedef int(__stdcall *fn_free_sndbuf)(void **ppbuf);
typedef int(__stdcall *fn_noargs)(void);
typedef int(__stdcall *fn_get_pausation)(int which);
typedef int(__stdcall *fn_set_pausation)(int which, int msec);
typedef int(__stdcall *fn_get_buffer_size)(void *ssb, unsigned long *out);
typedef int(__stdcall *fn_get_bookmark)(void *ssb, void *out);
typedef int(__stdcall *fn_get_word_offset)(void *ssb, void *out);
typedef int(__stdcall *fn_get_syl_timestamp)(void *ssb, void *out);
typedef int(__stdcall *fn_voice_controls_fil)(void *a1, const char *text, const char *path,
                                              int mode, int speed, int pitch, int volume);
typedef int(__stdcall *fn_open_phon)(void *a, void *b, void *c, void *d);
typedef int(__stdcall *fn_close_phon)(void *a);

static HMODULE g_dll;

static fn_get_av_version p_get_av_version;
static fn_initialize_SSB p_initialize_SSB;
static fn_close_SSB p_close_SSB;
static fn_set_tag_flag p_set_tag_flag;
static fn_isSeg_Available p_isSeg_Available;
static fn_synth_to_buffer p_synth_to_buffer;
static fn_synth_to_bufferEX p_synth_to_bufferEX;
static fn_txtstr_to_sndbuf p_txtstr_to_sndbuf;
static fn_txtstr_to_sndfil p_txtstr_to_sndfil;
static fn_free_sndbuf p_free_sndbuf;
static fn_noargs p_get_snd_fmt, p_get_snd_bps, p_get_snd_sps, p_get_snd_other;
static fn_noargs p_get_channels_allowed, p_isChan_Available;
static fn_get_pausation p_get_pausation;
static fn_set_pausation p_set_pausation;
static fn_get_bookmark p_get_bookmark;
static fn_get_word_offset p_get_word_offset;
static fn_get_syl_timestamp p_get_syl_timestamp;
static fn_voice_controls_fil p_voice_controls_fil;

static char g_outdir[MAX_PATH];

static void *sym(const char *name)
{
    void *p = (void *)GetProcAddress(g_dll, name);
    if (!p) printf("  !! missing export %s\n", name);
    return p;
}

/* The SSB the engine's own entry points build on the stack is under 0x504 bytes. */
#define SSB_BYTES 8192

/* mu-law (G.711) to 16-bit linear, the standard table-free expansion. */
static short ulaw2linear(unsigned char u)
{
    static const int exp_lut[8] = { 0, 132, 396, 924, 1980, 4092, 8316, 16764 };
    int sign, exponent, mantissa, sample;
    u = (unsigned char)~u;
    sign = (u & 0x80);
    exponent = (u >> 4) & 0x07;
    mantissa = u & 0x0F;
    sample = exp_lut[exponent] + (mantissa << (exponent + 3));
    return (short)(sign ? -sample : sample);
}

static void write_wav_pcm16(const char *name, const unsigned char *ulaw, size_t n, int sr)
{
    char path[MAX_PATH];
    FILE *f;
    unsigned long riff, fmtsz = 16, datasz = (unsigned long)(n * 2);
    unsigned short tag = 1, ch = 1, bps = 16, align = 2;
    unsigned long sps = (unsigned long)sr, avg = (unsigned long)sr * 2;
    size_t i;

    sprintf(path, "%s\\%s", g_outdir, name);
    f = fopen(path, "wb");
    if (!f) { printf("  !! cannot write %s\n", path); return; }
    riff = 36 + datasz;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtsz, 4, 1, f);
    fwrite(&tag, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sps, 4, 1, f);
    fwrite(&avg, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&datasz, 4, 1, f);
    for (i = 0; i < n; i++) {
        short s = ulaw2linear(ulaw[i]);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    printf("  wrote %s  (%.2f s)\n", name, (double)n / sr);
}

/* Runs one utterance through the streaming path. Returns bytes, fills timing. */
static unsigned long synth(const char *text, int tags, unsigned char **out,
                           double *first_ms, double *total_ms, unsigned long *segments)
{
    unsigned char *ssb = (unsigned char *)calloc(1, SSB_BYTES);
    unsigned char *acc = (unsigned char *)malloc(8u * 1024 * 1024);
    unsigned char *tmp = (unsigned char *)malloc(1024 * 1024);
    unsigned long total = 0, seg = 0;
    LARGE_INTEGER freq, t0, t1;
    int rc;

    QueryPerformanceFrequency(&freq);
    if (first_ms) *first_ms = -1.0;

    QueryPerformanceCounter(&t0);
    rc = p_initialize_SSB(ssb, text, 0);
    if (rc != 0) {
        printf("  !! initialize_SSB rc=%d\n", rc);
        free(ssb); free(acc); free(tmp);
        if (out) *out = NULL;
        return 0;
    }
    p_set_tag_flag(ssb, tags ? 1 : 0);

    while (p_isSeg_Available(ssb)) {
        unsigned long len = 0;
        rc = p_synth_to_buffer(ssb, tmp, &len);
        if (first_ms && *first_ms < 0) {
            QueryPerformanceCounter(&t1);
            *first_ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        }
        if (rc != 0) { printf("  !! synth_to_buffer rc=%d at seg %lu\n", rc, seg); break; }
        if (total + len > 8u * 1024 * 1024) break;
        memcpy(acc + total, tmp, len);
        total += len;
        seg++;
        if (seg > 200000) break;
    }
    QueryPerformanceCounter(&t1);
    if (total_ms) *total_ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
    if (segments) *segments = seg;
    p_close_SSB(ssb);
    free(ssb); free(tmp);
    if (out) *out = acc; else free(acc);
    return total;
}

static void case_(const char *label, const char *file, const char *text, int tags)
{
    unsigned char *audio = NULL;
    double first = 0, total = 0;
    unsigned long segs = 0;
    unsigned long n = synth(text, tags, &audio, &first, &total, &segs);
    printf("-- %-28s %6lu bytes  %5.2f s  %3lu segs  first %.1f ms  synth %.1f ms\n",
           label, n, n / 8000.0, segs, first, total);
    if (n && file) write_wav_pcm16(file, audio, n, p_get_snd_sps());
    free(audio);
}

int main(int argc, char **argv)
{
    const char *dll_path = (argc > 1) ? argv[1] : "engine\\Lib\\avcore.dll";
    char ver[512];
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    strcpy(g_outdir, (argc > 2) ? argv[2] : ".");

    printf("== loading %s\n", dll_path);
    g_dll = LoadLibraryA(dll_path);
    if (!g_dll) { printf("  !! LoadLibrary failed, error %lu\n", GetLastError()); return 1; }

    p_get_av_version = (fn_get_av_version)sym("_get_av_version@4");
    p_initialize_SSB = (fn_initialize_SSB)sym("_initialize_SSB@12");
    p_close_SSB = (fn_close_SSB)sym("_close_SSB@4");
    p_set_tag_flag = (fn_set_tag_flag)sym("_set_tag_flag@8");
    p_isSeg_Available = (fn_isSeg_Available)sym("_isSeg_Available@4");
    p_synth_to_buffer = (fn_synth_to_buffer)sym("_synth_to_buffer@12");
    p_synth_to_bufferEX = (fn_synth_to_bufferEX)sym("_synth_to_bufferEX@16");
    p_txtstr_to_sndbuf = (fn_txtstr_to_sndbuf)sym("_txtstr_to_sndbuf@16");
    p_txtstr_to_sndfil = (fn_txtstr_to_sndfil)sym("_txtstr_to_sndfil@12");
    p_free_sndbuf = (fn_free_sndbuf)sym("_free_sndbuf@4");
    p_get_snd_fmt = (fn_noargs)sym("_get_snd_fmt@0");
    p_get_snd_bps = (fn_noargs)sym("_get_snd_bps@0");
    p_get_snd_sps = (fn_noargs)sym("_get_snd_sps@0");
    p_get_snd_other = (fn_noargs)sym("_get_snd_other@0");
    p_get_channels_allowed = (fn_noargs)sym("_get_channels_allowed@0");
    p_isChan_Available = (fn_noargs)sym("_isChan_Available@0");
    p_get_pausation = (fn_get_pausation)sym("_get_pausation@4");
    p_set_pausation = (fn_set_pausation)sym("_set_pausation@8");
    p_get_bookmark = (fn_get_bookmark)sym("_get_bookmark@8");
    p_get_word_offset = (fn_get_word_offset)sym("_get_word_offset@8");
    p_get_syl_timestamp = (fn_get_syl_timestamp)sym("_get_syl_timestamp@8");
    p_voice_controls_fil = (fn_voice_controls_fil)sym("_voice_controls_fil@28");

    memset(ver, 0, sizeof(ver));
    rc = p_get_av_version(ver);
    printf("== version rc=%d \"%s\"\n", rc, ver);
    printf("== format: fmt=%d (1=pcm 6=alaw 7=ulaw) bits=%d rate=%d other=%d\n",
           p_get_snd_fmt(), p_get_snd_bps(), p_get_snd_sps(), p_get_snd_other());
    printf("== channels: allowed=%d available=%d\n",
           p_get_channels_allowed(), p_isChan_Available());
    printf("== pausation: P1=%d P2=%d P3=%d P4=%d  (0=%d, 5=%d)\n",
           p_get_pausation(1), p_get_pausation(2), p_get_pausation(3), p_get_pausation(4),
           p_get_pausation(0), p_get_pausation(5));

    printf("\n### baseline and segmentation\n");
    case_("short", "01_short.wav", "Hello there.", 0);
    case_("three sentences", "02_sentences.wav",
          "The first sentence is here. The second one follows it. And a third, to finish.", 0);
    case_("long paragraph", "03_paragraph.wav",
          "AcuVoice was a concatenative text to speech engine built in San Jose California "
          "during the middle nineteen nineties. It was acquired by Fonix in nineteen ninety "
          "eight. Its sound bank stores real recorded speech, which is why it sounds far more "
          "human than the formant synthesizers of the same era.", 0);

    printf("\n### text normalization\n");
    case_("numbers", "04_numbers.wav",
          "The year 1998. Pi is 3.14159. Call 555-1234. It cost $42.50, up 7 percent.", 0);
    case_("punctuation", "05_punct.wav",
          "Wait -- what? Really! (Yes.) A; B: C, D... end.", 0);
    case_("abbreviations", "06_abbrev.wav",
          "Dr. Smith lives at 100 N. Main St. and works Mon. through Fri.", 0);
    case_("mixed case + symbols", "07_symbols.wav",
          "Email me at joe@example.com or visit http://www.acuvoice.com for 50% off.", 0);

    printf("\n### inline control tags (tag flag on)\n");
    case_("tags off, literal", "08_tags_off.wav", "\\spd=250\\ Tags are off here.", 0);
    case_("speed 85 slowest", "09_spd85.wav", "\\spd=85\\ This is the slowest speed.", 1);
    case_("speed 175 default", "10_spd175.wav", "\\spd=175\\ This is the default speed.", 1);
    case_("speed 350 fastest", "11_spd350.wav", "\\spd=350\\ This is the fastest speed.", 1);
    case_("pitch 45 lowest", "12_pit45.wav", "\\pit=45\\ This is the lowest pitch.", 1);
    case_("pitch 63 default", "13_pit63.wav", "\\pit=63\\ This is the default pitch.", 1);
    case_("pitch 91 highest", "14_pit91.wav", "\\pit=91\\ This is the highest pitch.", 1);
    case_("volume 0", "15_vol0.wav", "\\vol=0\\ This is the quietest volume.", 1);
    case_("volume 32767", "16_vol32767.wav", "\\vol=32767\\ This is the middle volume.", 1);
    case_("volume 65535", "17_vol65535.wav", "\\vol=65535\\ This is the loudest volume.", 1);
    case_("rspd 50 / 200", "18_rspd.wav",
          "\\rspd=50\\ Half relative speed. \\rspd=200\\ Double relative speed.", 1);
    case_("Rpit 70 / 145", "19_rpit.wav",
          "\\Rpit=70\\ Low relative pitch. \\Rpit=145\\ High relative pitch.", 1);
    case_("pause 2000 ms", "20_pause.wav", "Before the pause. \\pau=2000\\ After the pause.", 1);
    case_("emphasis", "21_emph.wav", "This word is \\emp\\ special, this one is not.", 1);
    case_("reset", "22_reset.wav", "\\spd=300\\ Fast. \\rst\\ Back to normal.", 1);
    case_("spell out (chr)", "23_chr.wav", "\\chr=on\\ Hello \\chr=off\\ world.", 1);
    case_("context excited", "24_ctx_excited.wav", "\\ctx=excited\\ Something happened today!", 1);
    case_("context monotone", "25_ctx_monotone.wav", "\\ctx=monotone\\ Something happened today!", 1);
    case_("context business", "26_ctx_business.wav", "\\ctx=business\\ Something happened today!", 1);
    case_("context calm", "27_ctx_calm.wav", "\\ctx=calm\\ Something happened today!", 1);
    case_("context emphatic", "28_ctx_emphatic.wav", "\\ctx=emphatic\\ Something happened today!", 1);
    case_("context address", "29_ctx_address.wav", "\\ctx=address\\ 100 N. Main St., Apt. 3B.", 1);
    case_("context stock", "30_ctx_stock.wav", "\\ctx=stock\\ IBM 121 3/8 up 1/2.", 1);
    case_("ad hoc pronunciation", "31_avprn.wav",
          "\\avprn=joshua=jo'(s~u~e'#]\\ The name joshua, respelled.", 1);
    case_("pro on/off", "32_pro.wav", "\\pro=1\\ Pronunciation flag on.", 1);
    case_("rms on/off", "33_rms.wav", "\\rms=1\\ RMS flag on.", 1);
    case_("rmw on/off", "34_rmw.wav", "\\rmw=1\\ RMW flag on.", 1);
    case_("vce roger", "35_vce.wav", "\\vce=Roger\\ Speaking as Roger.", 1);
    case_("eng tag", "36_eng.wav", "\\eng=AcuVoice\\ Engine tag.", 1);
    case_("bad tag value", "37_badtag.wav", "\\spd=9999\\ Out of range speed.", 1);

    printf("\n### bookmarks and word offsets\n");
    {
        unsigned char *ssb = (unsigned char *)calloc(1, SSB_BYTES);
        unsigned char *tmp = (unsigned char *)malloc(1024 * 1024);
        unsigned long seg = 0, off = 0;
        const char *text = "\\mrk=11\\ One two three \\mrk=22\\ four five six.";
        if (p_initialize_SSB(ssb, text, 0) == 0) {
            p_set_tag_flag(ssb, 1);
            while (p_isSeg_Available(ssb)) {
                unsigned long len = 0;
                int bm[8], wo[8], ts[8];
                int rb, rw, rt;
                if (p_synth_to_buffer(ssb, tmp, &len) != 0) break;
                memset(bm, 0, sizeof(bm)); memset(wo, 0, sizeof(wo)); memset(ts, 0, sizeof(ts));
                rb = p_get_bookmark(ssb, bm);
                rw = p_get_word_offset(ssb, wo);
                rt = p_get_syl_timestamp(ssb, ts);
                printf("  seg %2lu off %6lu len %5lu | bookmark rc=%d [%d %d] | word rc=%d [%d %d] | ts rc=%d [%d %d]\n",
                       seg, off, len, rb, bm[0], bm[1], rw, wo[0], wo[1], rt, ts[0], ts[1]);
                off += len;
                if (++seg > 40) break;
            }
            p_close_SSB(ssb);
        }
        free(ssb); free(tmp);
    }

    printf("\n### one-shot entry points\n");
    {
        void *buf = NULL;
        unsigned long cb = 0;
        rc = p_txtstr_to_sndbuf("A one shot buffer.", &buf, &cb, 2);
        printf("  txtstr_to_sndbuf rc=%d cb=%lu\n", rc, cb);
        if (rc == 0 && buf && cb) write_wav_pcm16("40_oneshot.wav", (unsigned char *)buf, cb, p_get_snd_sps());
        if (buf) { rc = p_free_sndbuf(&buf); printf("  free_sndbuf rc=%d\n", rc); }
    }
    {
        char path[MAX_PATH];
        sprintf(path, "%s\\41_txtstr_file.wav", g_outdir);
        rc = p_txtstr_to_sndfil("A one shot wave file.", path, 2);
        printf("  txtstr_to_sndfil rc=%d -> %s\n", rc, path);
    }
    {
        char path[MAX_PATH];
        sprintf(path, "%s\\42_controls_file.wav", g_outdir);
        rc = p_voice_controls_fil(NULL, "Voice controls, slow and low.", path, 1, 100, 50, 60000);
        printf("  voice_controls_fil(NULL,...) rc=%d\n", rc);
    }

    printf("\n### pausation\n");
    printf("  set_pausation(1, 1500) rc=%d -> P1=%d\n", p_set_pausation(1, 1500), p_get_pausation(1));
    case_("long sentence pause", "43_pause1.wav", "First sentence. Second sentence.", 0);
    printf("  set_pausation(1, 650) rc=%d -> P1=%d\n", p_set_pausation(1, 650), p_get_pausation(1));

    printf("\n### edge cases\n");
    case_("empty string", NULL, "", 0);
    case_("single space", NULL, " ", 0);
    case_("only punctuation", NULL, "...", 0);
    case_("very long word", NULL,
          "supercalifragilisticexpialidociousandthensomemorelettersjusttopushit", 0);
    case_("non-ascii", NULL, "Caf\xe9 na\xefve r\xe9sum\xe9.", 0);
    case_("newlines/tabs", NULL, "Line one.\r\nLine two.\tTabbed.", 0);

    FreeLibrary(g_dll);
    printf("\n== done\n");
    return 0;
}
