#include "Sample.h"

namespace SF64 {
bool gAudioSampleFreeData = false;

Sample::~Sample() {
    if (!gAudioSampleFreeData) {
        return;
    }
    delete[] mOwnedData;
    mOwnedData = nullptr;
    delete mOwnedLoop;
    mOwnedLoop = nullptr;
    if (mOwnedBook != nullptr) {
        delete[] mOwnedBook->book;
        delete mOwnedBook;
        mOwnedBook = nullptr;
    }
}

SampleData* Sample::GetPointer() {
    return &mSample;
}

size_t Sample::GetPointerSize() {
    return sizeof(mSample);
}
}