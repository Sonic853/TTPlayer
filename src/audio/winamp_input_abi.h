#pragma once

#include <cstddef>
#include <windows.h>

namespace ttplayer::audio::winamp_input {

// x86 In_Module 0x100 and Out_Module 0x10. Recovered from 004E6CBD,
// 004E6FB6 and the static output table at 0053DA08. All callbacks are cdecl.
struct OutputModule {
    int version;
    char* description;
    int id;
    HWND window;
    HINSTANCE instance;
    void (__cdecl* Config)(HWND);
    void (__cdecl* About)(HWND);
    void (__cdecl* Init)();
    void (__cdecl* Quit)();
    int (__cdecl* Open)(int, int, int, int, int);
    void (__cdecl* Close)();
    int (__cdecl* Write)(char*, int);
    int (__cdecl* CanWrite)();
    int (__cdecl* IsPlaying)();
    int (__cdecl* Pause)(int);
    void (__cdecl* SetVolume)(int);
    void (__cdecl* SetPan)(int);
    void (__cdecl* Flush)(int);
    int (__cdecl* GetOutputTime)();
    int (__cdecl* GetWrittenTime)();
};

struct InputModule {
    int version;
    char* description;
    HWND window;
    HINSTANCE instance;
    char* extensions;
    int seekable;
    int uses_output;
    void (__cdecl* Config)(HWND);
    void (__cdecl* About)(HWND);
    void (__cdecl* Init)();
    void (__cdecl* Quit)();
    void (__cdecl* GetFileInfo)(char*, char*, int*);
    int (__cdecl* InfoBox)(char*, HWND);
    int (__cdecl* IsOurFile)(char*);
    int (__cdecl* Play)(char*);
    void (__cdecl* Pause)();
    void (__cdecl* UnPause)();
    int (__cdecl* IsPaused)();
    void (__cdecl* Stop)();
    int (__cdecl* GetLength)();
    int (__cdecl* GetOutputTime)();
    void (__cdecl* SetOutputTime)(int);
    void (__cdecl* SetVolume)(int);
    void (__cdecl* SetPan)(int);
    void (__cdecl* SAVSAInit)(int, int);
    void (__cdecl* SAVSADeInit)();
    void (__cdecl* SAAddPCMData)(void*, int, int, int);
    int (__cdecl* SAGetMode)();
    void (__cdecl* SAAdd)(void*, int, int);
    void (__cdecl* VSAAddPCMData)(void*, int, int, int);
    int (__cdecl* VSAGetMode)(int*, int*);
    void (__cdecl* VSAAdd)(void*, int);
    void (__cdecl* VSASetInfo)(int, int);
    int (__cdecl* DspIsActive)();
    int (__cdecl* DspDoSamples)(short*, int, int, int, int);
    void (__cdecl* EQSet)(int, char*, int);
    void (__cdecl* SetInfo)(int, int, int, int);
    OutputModule* output;
};

using GetInputModule = InputModule* (__cdecl*)();
static_assert(sizeof(void*) == 4);
static_assert(sizeof(InputModule) == 0x98);
static_assert(offsetof(InputModule, Play) == 0x38);
static_assert(offsetof(InputModule, Stop) == 0x48);
static_assert(offsetof(InputModule, output) == 0x94);
static_assert(sizeof(OutputModule) == 0x50);
static_assert(offsetof(OutputModule, Write) == 0x2c);
static_assert(offsetof(OutputModule, Flush) == 0x44);

} // namespace ttplayer::audio::winamp_input
