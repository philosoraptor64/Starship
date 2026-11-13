#pragma once

#include <cstdint>
#include <Resource.h>
#include "Envelope.h"
#include "AdpcmLoop.h"
#include "AdpcmBook.h"

namespace SF64 {
struct SampleData {
    uint32_t codec : 4;
    uint32_t medium : 2;
    uint32_t unk : 1;
    uint32_t isRelocated : 1;
    uint32_t size : 24;
    uint8_t* sampleAddr;
    AdpcmLoopData* loop;
    AdpcmBookData* book;
    float tuning = 0.0f;
};

class Sample : public Ship::Resource<SampleData> {
  public:
    using Resource::Resource;

    Sample() : Resource(std::shared_ptr<Ship::ResourceInitData>()) {}

    ~Sample() override;
    SampleData* GetPointer();
    size_t GetPointerSize();

    SampleData mSample;
    // what the factory allocated for this sample (sampleAddr may be relocated by the audio code)
    uint8_t* mOwnedData = nullptr;
    AdpcmLoopData* mOwnedLoop = nullptr;
    AdpcmBookData* mOwnedBook = nullptr;
};

// Normally a sample's data is kept when its resource goes: the audio thread may still play from it.
// At a full shutdown (an i64 run ending) the audio thread is gone, so it's freed with its resource.
extern bool gAudioSampleFreeData;
}