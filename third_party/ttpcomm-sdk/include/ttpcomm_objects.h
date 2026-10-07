#pragma once
#include <stdint.h>

// MSVC x86 vtable contracts. Slot 0 is the scalar deleting destructor;
// the compiler supplies its flags argument. These types expose no private data.
namespace ttpcomm {
struct ReplayGain {
    virtual ~ReplayGain()=default;
    virtual bool initialize(unsigned sampleRate)=0;                         // 1
    virtual bool process(const double* input,unsigned channels,unsigned frames)=0; // 2
    virtual double trackGain()=0;                                         // 3
    virtual double peak()=0;                                              // 4
    virtual double albumGain()=0;                                         // 5
};
struct Equalizer {
    virtual ~Equalizer()=default;
    virtual bool initialize(int sampleRate,int channels)=0;                // 1
    virtual void setParameters(const int* tenBandsThenPreamp)=0;            // 2
    virtual int latency()=0;                                              // 3: original returns 0
    virtual bool submit(const double* input,int interleavedSamples)=0;     // 4
    virtual bool receive(double* output,int* capacityAndProducedSamples)=0;// 5
    virtual void reset()=0;                                               // 6
};
struct Surround {
    virtual ~Surround()=default;
    virtual void initialize(int sampleRate,int channels,int strength)=0;   // 1
    virtual void process(double* samples,int interleavedSamples)=0;        // 2
    virtual void reset()=0;                                               // 3
};
struct Spectrum {
    virtual ~Spectrum()=default;
    virtual void process(int16_t* workspace1536Samples,int channels)=0;    // 1
};
struct Resampler {
    virtual ~Resampler()=default;
    virtual bool initialize(int inputRate,int outputRate,int channels,bool quality)=0; // 1
    virtual int process(const double* input,int inputSamples,double* output,int outputCapacity)=0; // 2
    virtual void reset()=0;                                               // 3
};
static_assert(sizeof(void*)==4,"The original ttpcomm ABI requires x86");
}
