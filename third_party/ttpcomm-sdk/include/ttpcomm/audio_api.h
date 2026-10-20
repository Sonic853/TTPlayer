#pragma once
#include <ttpcomm/runtime_api.h>

/* Independent table queried through ttpcomm_query_runtime(version=2).
   ABI 1 and the original ordinal exports are unchanged. Interleaved double
   samples use TTPCOMM_PCM_HALF: +/-0.5 maps to full-scale external PCM.
   All counts named frames count one sample per channel; capacities for
   encoded data count bytes. Objects are single-threaded, DLL-owned. */
#define TTPCOMM_AUDIO_ABI 2u
#define TTPCOMM_AUDIO_ENCODER 0x01u
#define TTPCOMM_AUDIO_EFFECTS 0x02u
#define TTPCOMM_AUDIO_REQUIRED 0x03u
#define TTPCOMM_AUDIO_END_INPUT 0x01u
#define TTPCOMM_AUDIO_EQ 0x01u
#define TTPCOMM_AUDIO_SURROUND 0x02u

#pragma pack(push, 8)
typedef struct TtpCommAudioEncoderConfig {
    uint32_t size;
    TtpCommPcmFormat format;
    uint32_t dither; /* 0=off, 1=unshaped TPDF, integers only. */
    uint32_t seed;   /* 0 uses a fixed nonzero seed; reset repeats the sequence. */
} TtpCommAudioEncoderConfig;
typedef struct TtpCommAudioEncodeResult {
    uint32_t size, bytes;
    uint64_t clipped_samples, nonfinite_samples;
} TtpCommAudioEncodeResult;
typedef struct TtpCommAudioEffectsConfig {
    uint32_t size, sample_rate, channels, flags;
    double bands_db[10]; /* Boundaries: 32,64,125,250,500,1000,2000,4000,8000 Hz. */
    double preamp_db;    /* Each band and preamp must be finite, -60..+24 dB. */
    uint32_t surround_strength; /* 1..16; ignored unless SURROUND is enabled. */
    uint32_t transition_frames; /* Kernel/gain changes interpolate over this duration. */
} TtpCommAudioEffectsConfig;
typedef struct TtpCommAudioEffectsInfo {
    uint32_t size, fir_taps, fft_size, block_frames, group_delay_frames;
    uint32_t maximum_buffered_frames, surround_delay_frames, reserved;
} TtpCommAudioEffectsInfo;
typedef struct TtpCommAudioProcessResult {
    uint32_t size, consumed_frames, produced_frames, reserved;
} TtpCommAudioProcessResult;
typedef struct TtpCommAudioApi {
    uint32_t size, abi_version, capabilities, reserved;
    void* (__cdecl *encoder_create)(const TtpCommAudioEncoderConfig*);
    void (__cdecl *encoder_destroy)(void*);
    /* gains=NULL/count=0 means unity; otherwise count must equal channels.
       Gain is applied BEFORE rounding/dither. Finite inputs, including
       floating headroom, are preserved for float outputs; integers saturate.
       NaN/Inf become silence and are counted. Capacity/error changes no state.
       Output must not overlap input or gains. */
    int (__cdecl *encoder_encode)(void*, const double* input, uint32_t frames,
        const double* gains, uint32_t gain_count, void* output, uint32_t capacity_bytes,
        TtpCommAudioEncodeResult*);
    void (__cdecl *encoder_reset)(void*);
    void* (__cdecl *effects_create)(const TtpCommAudioEffectsConfig*);
    void (__cdecl *effects_destroy)(void*);
    int (__cdecl *effects_info)(void*, TtpCommAudioEffectsInfo*);
    /* rate/channels cannot change in place. New kernel is prepared before
       modifying live state. A failure leaves parameters/history unchanged. */
    int (__cdecl *effects_configure)(void*, const TtpCommAudioEffectsConfig*);
    /* END_INPUT takes effect only once all offered frames are consumed.
       Repeat with no input and END_INPUT to drain. Output capacity may force
       partial consumption; retry only the unconsumed input. END means all
       input has been returned. FIR group delay is removed internally, zero
       padding supplies the end, and total output equals total accepted input.
       No in-place or overlapping input/output buffers. No process allocations. */
    int (__cdecl *effects_process)(void*, const double* input, uint32_t input_frames,
        double* output, uint32_t output_capacity_frames, uint32_t flags,
        TtpCommAudioProcessResult*);
    /* Reset discards all tails/queues, retains latest parameters, and starts
       a new independent track. No preceding-track samples survive. */
    void (__cdecl *effects_reset)(void*);
} TtpCommAudioApi;
#pragma pack(pop)
