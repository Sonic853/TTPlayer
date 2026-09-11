#include "ttpcomm_api.h"
#include <string.h>

#define LOAD_ORD(field, type, ordinal) \
    api->field = (type)GetProcAddress(api->module, (LPCSTR)(uintptr_t)(ordinal))

BOOL TtpComm_LoadApi(TtpCommApi *api, wchar_t const *dll_path)
{
    if (!api || !dll_path) return FALSE;
    memset(api, 0, sizeof(*api));
    api->module = LoadLibraryW(dll_path);
    if (!api->module) return FALSE;

    LOAD_ORD(get_version, TtpCommGetVersionFn, 1);
    LOAD_ORD(srand48, TtpCommSrand48Fn, 3);
    LOAD_ORD(lrand48, TtpCommLrand48Fn, 4);
    LOAD_ORD(decoder_sample_bits, TtpDecoderSampleBitsFn, 10);
    LOAD_ORD(decoder_create, TtpDecoderCreateFn, 11);
    LOAD_ORD(free_object, TtpFreeFn, 12);
    LOAD_ORD(decoder_process, TtpDecoderProcessFn, 13);
    LOAD_ORD(decoder_reset, TtpDecoderResetFn, 14);

    LOAD_ORD(id3_tag_new, Id3TagNewFn, 50);
    LOAD_ORD(id3_tag_delete, Id3TagDeleteFn, 51);
    LOAD_ORD(id3_tag_query, Id3TagQueryFn, 52);
    LOAD_ORD(id3_tag_parse, Id3TagParseFn, 53);
    LOAD_ORD(id3_tag_render, Id3TagRenderFn, 54);
    LOAD_ORD(id3_tag_attachframe, Id3TagAttachFrameFn, 55);
    LOAD_ORD(id3_tag_detachframe, Id3TagDetachFrameFn, 56);
    LOAD_ORD(id3_tag_setlength, Id3TagSetLengthFn, 57);
    LOAD_ORD(id3_frame_new, Id3FrameNewFn, 58);
    LOAD_ORD(id3_frame_delete, Id3FrameDeleteFn, 59);
    LOAD_ORD(id3_frame_field, Id3FrameFieldFn, 60);
    LOAD_ORD(id3_ucs4_latin1duplicate, Id3Ucs4Latin1DuplicateFn, 61);
    LOAD_ORD(id3_ucs4_utf16duplicate, Id3Ucs4Utf16DuplicateFn, 62);
    LOAD_ORD(id3_ucs4_utf8duplicate, Id3Ucs4Utf8DuplicateFn, 63);
    LOAD_ORD(id3_utf16_ucs4duplicate, Id3Utf16Ucs4DuplicateFn, 64);
    LOAD_ORD(id3_utf8_ucs4duplicate, Id3Utf8Ucs4DuplicateFn, 65);
    LOAD_ORD(id3_latin1_ucs4duplicate, Id3Latin1Ucs4DuplicateFn, 66);
    LOAD_ORD(id3_field_type, Id3FieldTypeFn, 67);
    LOAD_ORD(id3_field_getlatin1, Id3FieldGetLatin1Fn, 68);
    LOAD_ORD(id3_field_getfullstring, Id3FieldGetFullStringFn, 69);
    LOAD_ORD(id3_field_getstring, Id3FieldGetStringFn, 70);
    LOAD_ORD(id3_field_getstrings, Id3FieldGetStringsFn, 71);
    LOAD_ORD(id3_field_setfullstring, Id3FieldSetFullStringFn, 72);
    LOAD_ORD(id3_field_setlatin1, Id3FieldSetLatin1Fn, 73);
    LOAD_ORD(id3_field_setstring, Id3FieldSetStringFn, 74);
    LOAD_ORD(id3_field_setstrings, Id3FieldSetStringsFn, 75);
    LOAD_ORD(id3_field_setlanguage, Id3FieldSetLanguageFn, 76);
    LOAD_ORD(id3_free, TtpFreeFn, 78);
    LOAD_ORD(id3_field_setbinarydata, Id3FieldSetBinaryDataFn, 79);

    LOAD_ORD(inflateInit2_, TtpInflateInit2Fn, 80);
    LOAD_ORD(inflate, TtpInflateFn, 81);
    LOAD_ORD(inflateEnd, TtpInflateEndFn, 82);
    LOAD_ORD(uncompress, TtpUncompressFn, 83);
    LOAD_ORD(dsp_create_2d, TtpDspCreate2DFn, 90);
    LOAD_ORD(dsp_configure_2d, TtpDspConfigure2DFn, 91);
    LOAD_ORD(dsp_process, TtpDspProcessFn, 92);
    LOAD_ORD(dsp_destroy, TtpDspDestroyFn, 93);
    LOAD_ORD(dsp_supports_mode, TtpDspSupportsModeFn, 100);
    LOAD_ORD(dsp_factory_101, TtpDspFactory101Fn, 101);
    LOAD_ORD(dsp_factory_102, TtpDspFactory102Fn, 102);
    LOAD_ORD(dsp_factory_103, TtpDspFactoryFn, 103);
    LOAD_ORD(dsp_factory_104, TtpDspFactoryFn, 104);
    LOAD_ORD(dsp_factory_105, TtpDspFactoryFn, 105);
    LOAD_ORD(probe_audio, TtpProbeAudioFn, 106);
    LOAD_ORD(coolsb_init_app, CoolSbInitAppFn, 200);
    LOAD_ORD(coolsb_uninit_app, CoolSbUninitAppFn, 201);
    LOAD_ORD(initialize_coolsb, InitializeCoolSBFn, 202);
    LOAD_ORD(uninitialize_coolsb, UninitializeCoolSBFn, 203);
    LOAD_ORD(coolsb_set_style_ex, CoolSbSetStyleExFn, 204);
    LOAD_ORD(coolsb_set_size, CoolSbSetSizeFn, 205);
    LOAD_ORD(coolsb_set_min_thumb_size, CoolSbSetMinThumbSizeFn, 206);
    LOAD_ORD(get_machine_id_length, TtpGetMachineIdLengthFn, 300);
    LOAD_ORD(get_disk_serial, TtpGetDiskSerialFn, 301);
    LOAD_ORD(make_machine_token, TtpMakeMachineTokenFn, 302);
    LOAD_ORD(digest_hex, TtpDigestHexFn, 400);
    LOAD_ORD(digest_raw, TtpDigestRawFn, 401);

    /* Version is an optional diagnostic export, never a load requirement.
       Other revisions may preserve these ordinals without reporting 5.7.0.
       A successful load does not guarantee every feature is supported:
       consumers must check the exports they need before calling them. */
    return TRUE;
}

void TtpComm_UnloadApi(TtpCommApi *api)
{
    HMODULE module;
    if (!api) return;
    module = api->module;
    memset(api, 0, sizeof(*api));
    if (module) FreeLibrary(module);
}
