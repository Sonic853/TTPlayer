#pragma once
#include <stdint.h>

/* Additive C ABI for rebuilt hosts. All buffers belong to the caller; opaque
   objects are created/destroyed by this DLL. No STL, z_stream or CRT ownership
   crosses the boundary. Counts and offsets are bytes unless named samples. */
#define TTPCOMM_RUNTIME_ABI 1u
#define TTPCOMM_RUNTIME_COMPRESSION 0x01u
#define TTPCOMM_RUNTIME_PCM 0x02u
#define TTPCOMM_RUNTIME_TAGS 0x04u
#define TTPCOMM_RUNTIME_REQUIRED 0x07u
#define TTPCOMM_OK 0
#define TTPCOMM_INVALID -1
#define TTPCOMM_CAPACITY -2
#define TTPCOMM_NOMEM -3
#define TTPCOMM_DATA -4
#define TTPCOMM_END 1
#define TTPCOMM_INFLATE_RAW 0u
#define TTPCOMM_INFLATE_ZLIB 1u
/* Trailing bytes are explicitly allowed only for the historical ID3 payload
   caller; strict ZIP consumers must pass 0. Stream end is always required. */
#define TTPCOMM_INFLATE_ALLOW_TRAILING 1u
#define TTPCOMM_PCM_HALF 0u
#define TTPCOMM_PCM_FULL 1u

#pragma pack(push, 8)
typedef struct TtpCommPcmFormat {
    uint32_t size, channels, sample_rate, bits, valid_bits, block_align;
    uint32_t floating_point, channel_mask;
} TtpCommPcmFormat;
typedef struct TtpCommInflateResult { uint32_t consumed, produced; } TtpCommInflateResult;
typedef struct TtpCommTagFrame {
    uint32_t offset, raw_size, payload_offset, payload_size, flags;
    char identifier[4]; /* v2.2 uses 3 bytes plus NUL; others use all 4. */
} TtpCommTagFrame;
typedef struct TtpCommPicture {
    uint32_t type, width, height, depth, data_offset, data_size;
    uint32_t mime_offset, mime_size, legacy_mime; /* legacy: 1=JPEG,2=PNG,3=BMP,4=GIF */
} TtpCommPicture;
typedef struct TtpCommDecoded {
    const void* data;
    uint32_t size;
    void* owner; /* NULL borrows the input; otherwise destroy with tag_decoded_destroy. */
} TtpCommDecoded;
typedef struct TtpCommRuntimeApi {
    uint32_t size, abi_version, capabilities, reserved;
    int (__cdecl *inflate_buffer)(uint32_t mode, uint32_t flags, const void* input,
        uint32_t input_size, void* output, uint32_t capacity, TtpCommInflateResult* result);
    uint32_t (__cdecl *crc32_buffer)(uint32_t seed, const void* input, uint32_t size);
    int (__cdecl *pcm_decode)(const TtpCommPcmFormat*, uint32_t domain, const void*, uint32_t bytes,
        double* output, uint32_t capacity_samples, uint32_t* produced_samples);
    int (__cdecl *pcm_encode_half)(const TtpCommPcmFormat*, const double*, uint32_t samples,
        void* output, uint32_t capacity, uint32_t* produced);
    int (__cdecl *pcm_gain_half)(double*, uint32_t samples, double gain);
    void* (__cdecl *quantizer_create)(const TtpCommPcmFormat*, int dither);
    void (__cdecl *quantizer_destroy)(void*);
    int (__cdecl *quantizer_encode)(void*, const double*, uint32_t samples,
        void* output, uint32_t capacity, uint32_t* produced);
    void (__cdecl *quantizer_reset)(void*);
    uint32_t (__cdecl *tag_terminator)(const void*, uint32_t size, uint32_t encoding);
    int (__cdecl *tag_unsynchronize)(const void*, uint32_t size, void* output,
        uint32_t capacity, uint32_t* produced);
    int (__cdecl *tag_frame_start)(const void*, uint32_t size, uint32_t major,
        uint32_t flags, uint32_t* offset);
    int (__cdecl *tag_next_frame)(const void*, uint32_t size, uint32_t major,
        uint32_t* offset, TtpCommTagFrame* frame);
    int (__cdecl *tag_picture)(const void*, uint32_t size, uint32_t major,
        TtpCommPicture* picture); /* major 0 = FLAC PICTURE, 2..4 = ID3 */
    int (__cdecl *tag_decode)(const void*, uint32_t size, uint32_t major, uint32_t flags,
        uint32_t tag_unsynchronized, uint64_t* budget, TtpCommDecoded*);
    void (__cdecl *tag_decoded_destroy)(void*);
} TtpCommRuntimeApi;
#pragma pack(pop)
typedef int (__cdecl *TtpCommQueryRuntimeFn)(uint32_t abi, uint32_t required, TtpCommRuntimeApi*);
