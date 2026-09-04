#pragma once

#include <stdint.h>

// avcore.dll is 32-bit, so a 64-bit SAPI host -- Windows 11 Narrator, 64-bit Balabolka,
// 64-bit NVDA -- cannot load it in process. The 64-bit build of the engine therefore
// talks to a 32-bit worker over this pipe.
//
// Text preparation and the prosody arithmetic both happen on the SAPI side, which is
// architecture independent, so the worker only ever receives finished engine bytes and
// the three numbers that drive the audio path. It hands back 16-bit PCM at
// OUTPUT_SAMPLE_RATE, already stretched, pitched and scaled, so the 64-bit side has
// nothing left to do but write it.
#define ACUVOICE_PIPE_NAME L"\\\\.\\pipe\\AcuVoiceTTS"

// Bumped whenever the wire format changes. A worker from a previous install can still be
// running when a new version starts speaking -- it outlives any one client and the pipe
// name does not change between versions -- so the client checks this on connect and
// replaces a worker that does not match. Without it an upgrade silently keeps talking to
// the old worker, which reads the command fields at the offsets it was built for.
#define ACUVOICE_PROTOCOL_VERSION 1u

enum PipeCommand : uint32_t {
    CMD_PING = 0,
    CMD_SPEAK = 2,
    CMD_STOP = 3,
    CMD_SHUTDOWN = 4,
    CMD_ENGINE_INFO = 8,
};

enum PipeResponse : uint32_t {
    RESP_OK = 0,
    RESP_ERROR = 1,
    RESP_AUDIO_DATA = 2,
    RESP_AUDIO_END = 3,
    RESP_SAMPLE_RATE = 4,
    RESP_PONG = 5,
    RESP_ENGINE_INFO = 6,
};

#pragma pack(push, 1)
struct PipeMessageHeader {
    uint32_t type;
    uint32_t size;
};

struct SpeakCommand {
    double   duration;      // output duration as a multiple of the engine's own
    double   pitch;         // output pitch as a multiple of the engine's own
    double   gain;          // linear amplitude
    uint32_t honour_tags;   // non-zero lets avcore's backslash tags reach the parser
    // The pause lengths are the one control the engine itself applies, and it applies
    // them from a process-wide setting. They travel with the utterance so that two
    // clients wanting different pacing cannot end up speaking with each other's.
    int32_t  pause[4];      // milliseconds; -1 leaves a slot at whatever it already is
    uint32_t text_length;   // bytes of prepared, Windows-1252 text following this struct
};

struct EngineInfo {
    uint32_t sample_rate;   // what the worker will send back
    uint32_t engine_rate;   // avcore's own rate, 8000
    uint32_t bits;          // avcore's own, 8
    uint32_t format;        // avcore's own, 7 == mu-law
    uint32_t channels_allowed;
    char     version[32];
};

struct AudioDataChunk {
    uint32_t size;
};
#pragma pack(pop)

#define ACUVOICE_BITS_PER_SAMPLE 16
#define ACUVOICE_CHANNELS 1
