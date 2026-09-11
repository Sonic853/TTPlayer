#ifndef TTPLAYER_REVERSE_TTPCOMM_API_H
#define TTPLAYER_REVERSE_TTPCOMM_API_H

/* Recovered public ABI of ttpcomm.dll 5.7.0 (x86).
 * The original DLL exports most functions by ordinal only.  Call
 * TtpComm_LoadApi() instead of linking those functions by a guessed name. */

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char  id3_byte_t;
typedef uint32_t       id3_length_t;
typedef uint32_t       id3_ucs4_t;
typedef unsigned char  id3_latin1_t;
typedef uint16_t       id3_utf16_t;
typedef signed char    id3_utf8_t;

struct id3_tag;
struct id3_frame;
union id3_field;

enum id3_field_type {
    ID3_FIELD_TYPE_TEXTENCODING,
    ID3_FIELD_TYPE_LATIN1,
    ID3_FIELD_TYPE_LATIN1FULL,
    ID3_FIELD_TYPE_LATIN1LIST,
    ID3_FIELD_TYPE_STRING,
    ID3_FIELD_TYPE_STRINGFULL,
    ID3_FIELD_TYPE_STRINGLIST,
    ID3_FIELD_TYPE_LANGUAGE,
    ID3_FIELD_TYPE_FRAMEID,
    ID3_FIELD_TYPE_DATE,
    ID3_FIELD_TYPE_INT8,
    ID3_FIELD_TYPE_INT16,
    ID3_FIELD_TYPE_INT24,
    ID3_FIELD_TYPE_INT32,
    ID3_FIELD_TYPE_INT32PLUS,
    ID3_FIELD_TYPE_BINARYDATA
};

/* zlib 1.1.4 stream layout used by ordinals 80-83.  It is 0x38 bytes on x86. */
typedef void *(__cdecl *TtpZAlloc)(void *opaque, unsigned items, unsigned size);
typedef void  (__cdecl *TtpZFree)(void *opaque, void *address);
typedef struct TtpZStream {
    unsigned char *next_in;
    unsigned avail_in;
    unsigned total_in;
    unsigned char *next_out;
    unsigned avail_out;
    unsigned total_out;
    char *msg;
    void *state;
    TtpZAlloc zalloc;
    TtpZFree zfree;
    void *opaque;
    int data_type;
    unsigned adler;
    unsigned reserved;
} TtpZStream;
#if defined(_M_IX86) || defined(__i386__)
typedef char TtpZStreamMustBe56Bytes[(sizeof(TtpZStream) == 56) ? 1 : -1];
#endif

/* Named, undecorated C exports. Disassembly confirms caller cleanup for the
 * seeded form (ordinal 3 reads [esp+4] and does not return with `ret 4`). */
typedef uint32_t (__cdecl *TtpCommGetVersionFn)(void);                      /* @1 */
typedef void     (__cdecl *TtpCommSrand48Fn)(uint32_t seed);                /* @3 */
typedef uint32_t (__cdecl *TtpCommLrand48Fn)(void);                         /* @4 */

typedef uint32_t (__stdcall *TtpDecoderSampleBitsFn)(void);                 /* @10: returns 64 */
typedef void *   (__cdecl   *TtpDecoderCreateFn)(uint32_t mode);            /* @11 */
typedef void     (__cdecl   *TtpFreeFn)(void *object);                      /* @12/@78 */
typedef int      (__cdecl   *TtpDecoderProcessFn)(void *decoder, void *io); /* @13 */
typedef void     (__cdecl   *TtpDecoderResetFn)(void *decoder);             /* @14 */

/* libid3tag 0.15.x ABI, recovered with high confidence. */
typedef struct id3_tag *(__stdcall *Id3TagNewFn)(void);                                  /* @50 */
typedef void            (__cdecl   *Id3TagDeleteFn)(struct id3_tag *);                    /* @51 */
typedef long            (__cdecl   *Id3TagQueryFn)(id3_byte_t const *, id3_length_t);     /* @52 */
typedef struct id3_tag *(__cdecl   *Id3TagParseFn)(id3_byte_t const *, id3_length_t);     /* @53 */
typedef id3_length_t    (__cdecl   *Id3TagRenderFn)(struct id3_tag const *, id3_byte_t *);/* @54 */
typedef int             (__cdecl   *Id3TagAttachFrameFn)(struct id3_tag *, struct id3_frame *); /* @55 */
typedef int             (__cdecl   *Id3TagDetachFrameFn)(struct id3_tag *, struct id3_frame *); /* @56 */
typedef void            (__cdecl   *Id3TagSetLengthFn)(struct id3_tag *, id3_length_t);   /* @57 */
typedef struct id3_frame *(__cdecl *Id3FrameNewFn)(char const *);                         /* @58 */
typedef void            (__cdecl   *Id3FrameDeleteFn)(struct id3_frame *);                /* @59 */
typedef union id3_field *(__cdecl  *Id3FrameFieldFn)(struct id3_frame const *, unsigned); /* @60 */
typedef id3_latin1_t *  (__cdecl   *Id3Ucs4Latin1DuplicateFn)(id3_ucs4_t const *);         /* @61 */
typedef id3_utf16_t *   (__cdecl   *Id3Ucs4Utf16DuplicateFn)(id3_ucs4_t const *);          /* @62 */
typedef id3_utf8_t *    (__cdecl   *Id3Ucs4Utf8DuplicateFn)(id3_ucs4_t const *);           /* @63 */
typedef id3_ucs4_t *    (__cdecl   *Id3Utf16Ucs4DuplicateFn)(id3_utf16_t const *);         /* @64 */
typedef id3_ucs4_t *    (__cdecl   *Id3Utf8Ucs4DuplicateFn)(id3_utf8_t const *);           /* @65 */
typedef id3_ucs4_t *    (__cdecl   *Id3Latin1Ucs4DuplicateFn)(id3_latin1_t const *);       /* @66 */
typedef enum id3_field_type (__cdecl *Id3FieldTypeFn)(union id3_field const *);            /* @67 */
typedef id3_latin1_t const *(__cdecl *Id3FieldGetLatin1Fn)(union id3_field const *);        /* @68 */
typedef id3_ucs4_t const *  (__cdecl *Id3FieldGetFullStringFn)(union id3_field const *);    /* @69 */
typedef id3_ucs4_t const *  (__cdecl *Id3FieldGetStringFn)(union id3_field const *);        /* @70 */
typedef id3_ucs4_t const *  (__cdecl *Id3FieldGetStringsFn)(union id3_field const *, unsigned); /* @71 */
typedef int (__cdecl *Id3FieldSetFullStringFn)(union id3_field *, id3_ucs4_t const *);      /* @72 */
typedef int (__cdecl *Id3FieldSetLatin1Fn)(union id3_field *, id3_latin1_t const *);        /* @73 */
typedef int (__cdecl *Id3FieldSetStringFn)(union id3_field *, id3_ucs4_t const *);          /* @74 */
typedef int (__cdecl *Id3FieldSetStringsFn)(union id3_field *, unsigned, id3_ucs4_t **);    /* @75 */
typedef int (__cdecl *Id3FieldSetLanguageFn)(union id3_field *, char const *);              /* @76 */
typedef int (__cdecl *Id3FieldSetBinaryDataFn)(union id3_field *, id3_byte_t const *, id3_length_t); /* @79 */

typedef int (__cdecl *TtpInflateInit2Fn)(TtpZStream *, int, char const *, int); /* @80 */
typedef int (__cdecl *TtpInflateFn)(TtpZStream *, int);                         /* @81 */
typedef int (__cdecl *TtpInflateEndFn)(TtpZStream *);                           /* @82 */
typedef int (__cdecl *TtpUncompressFn)(unsigned char *, unsigned *,
                                       unsigned char const *, unsigned);         /* @83 */

/* DSP / analysis factories and operations.  Object layouts remain opaque. */
typedef void *(__fastcall *TtpDspCreate2DFn)(uint32_t width, int height);         /* @90 */
typedef void  (__fastcall *TtpDspConfigure2DFn)(void *, uint32_t, int);           /* @91 */
typedef int   (__fastcall *TtpDspProcessFn)(void *, float, int, int);             /* @92 */
typedef void  (__fastcall *TtpDspDestroyFn)(void *);                             /* @93 */
typedef BOOL  (__cdecl    *TtpDspSupportsModeFn)(int);                           /* @100 */
typedef void *(__stdcall  *TtpDspFactory101Fn)(void);                            /* @101 */
typedef void *(__cdecl    *TtpDspFactory102Fn)(int);                             /* @102 */
typedef void *(__stdcall  *TtpDspFactoryFn)(void);                               /* @103-105 */
typedef int   (__cdecl    *TtpProbeAudioFn)(char const *, uint32_t, void *);       /* @106 */

typedef BOOL    (__stdcall *CoolSbInitAppFn)(void);                               /* @200 */
typedef BOOL    (__stdcall *CoolSbUninitAppFn)(void);                             /* @201 */
typedef BOOL    (__stdcall *InitializeCoolSBFn)(HWND);                            /* @202 */
typedef HRESULT (__stdcall *UninitializeCoolSBFn)(HWND);                          /* @203 */
typedef BOOL    (__stdcall *CoolSbSetStyleExFn)(HWND, int, int, int);             /* @204, TTPlayer extension */
typedef BOOL    (__stdcall *CoolSbSetSizeFn)(HWND, int, int, int);                /* @205 */
typedef BOOL    (__stdcall *CoolSbSetMinThumbSizeFn)(HWND, int, UINT);            /* @206 */

typedef int    (__stdcall *TtpGetMachineIdLengthFn)(void);                        /* @300 */
typedef size_t (__cdecl   *TtpGetDiskSerialFn)(char *, size_t);                   /* @301 */
typedef char * (__cdecl   *TtpMakeMachineTokenFn)(char *, int, unsigned char *);  /* @302 */
typedef void   (__cdecl   *TtpDigestRawFn)(int, uint32_t, uint32_t *);             /* @401 */
typedef char * (__cdecl   *TtpDigestHexFn)(char const *, uint32_t, char *, int);   /* @400 */

typedef struct TtpCommApi {
    HMODULE module;
    TtpCommGetVersionFn get_version;
    TtpCommSrand48Fn srand48;
    TtpCommLrand48Fn lrand48;
    TtpDecoderSampleBitsFn decoder_sample_bits;
    TtpDecoderCreateFn decoder_create;
    TtpFreeFn free_object;
    TtpDecoderProcessFn decoder_process;
    TtpDecoderResetFn decoder_reset;

    Id3TagNewFn id3_tag_new;
    Id3TagDeleteFn id3_tag_delete;
    Id3TagQueryFn id3_tag_query;
    Id3TagParseFn id3_tag_parse;
    Id3TagRenderFn id3_tag_render;
    Id3TagAttachFrameFn id3_tag_attachframe;
    Id3TagDetachFrameFn id3_tag_detachframe;
    Id3TagSetLengthFn id3_tag_setlength;
    Id3FrameNewFn id3_frame_new;
    Id3FrameDeleteFn id3_frame_delete;
    Id3FrameFieldFn id3_frame_field;
    Id3Ucs4Latin1DuplicateFn id3_ucs4_latin1duplicate;
    Id3Ucs4Utf16DuplicateFn id3_ucs4_utf16duplicate;
    Id3Ucs4Utf8DuplicateFn id3_ucs4_utf8duplicate;
    Id3Utf16Ucs4DuplicateFn id3_utf16_ucs4duplicate;
    Id3Utf8Ucs4DuplicateFn id3_utf8_ucs4duplicate;
    Id3Latin1Ucs4DuplicateFn id3_latin1_ucs4duplicate;
    Id3FieldTypeFn id3_field_type;
    Id3FieldGetLatin1Fn id3_field_getlatin1;
    Id3FieldGetFullStringFn id3_field_getfullstring;
    Id3FieldGetStringFn id3_field_getstring;
    Id3FieldGetStringsFn id3_field_getstrings;
    Id3FieldSetFullStringFn id3_field_setfullstring;
    Id3FieldSetLatin1Fn id3_field_setlatin1;
    Id3FieldSetStringFn id3_field_setstring;
    Id3FieldSetStringsFn id3_field_setstrings;
    Id3FieldSetLanguageFn id3_field_setlanguage;
    TtpFreeFn id3_free;
    Id3FieldSetBinaryDataFn id3_field_setbinarydata;

    TtpInflateInit2Fn inflateInit2_;
    TtpInflateFn inflate;
    TtpInflateEndFn inflateEnd;
    TtpUncompressFn uncompress;

    TtpDspCreate2DFn dsp_create_2d;
    TtpDspConfigure2DFn dsp_configure_2d;
    TtpDspProcessFn dsp_process;
    TtpDspDestroyFn dsp_destroy;
    TtpDspSupportsModeFn dsp_supports_mode;
    TtpDspFactory101Fn dsp_factory_101;
    TtpDspFactory102Fn dsp_factory_102;
    TtpDspFactoryFn dsp_factory_103;
    TtpDspFactoryFn dsp_factory_104;
    TtpDspFactoryFn dsp_factory_105;
    TtpProbeAudioFn probe_audio;

    CoolSbInitAppFn coolsb_init_app;
    CoolSbUninitAppFn coolsb_uninit_app;
    InitializeCoolSBFn initialize_coolsb;
    UninitializeCoolSBFn uninitialize_coolsb;
    CoolSbSetStyleExFn coolsb_set_style_ex;
    CoolSbSetSizeFn coolsb_set_size;
    CoolSbSetMinThumbSizeFn coolsb_set_min_thumb_size;

    TtpGetMachineIdLengthFn get_machine_id_length;
    TtpGetDiskSerialFn get_disk_serial;
    TtpMakeMachineTokenFn make_machine_token;
    TtpDigestHexFn digest_hex;
    TtpDigestRawFn digest_raw;
} TtpCommApi;

/* Load a compatible x86 DLL and resolve known ordinals, without calling or
   checking get_version. TRUE means the module was loaded, not that all
   features are present. Any optional function pointer may be NULL. */
BOOL TtpComm_LoadApi(TtpCommApi *api, wchar_t const *dll_path);
void TtpComm_UnloadApi(TtpCommApi *api);

#ifdef __cplusplus
}
#endif
#endif
