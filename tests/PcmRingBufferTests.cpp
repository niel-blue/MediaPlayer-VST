#include "PcmRingBuffer.h"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

void expectNear(float actual, float expected)
{
    assert(std::fabs(actual - expected) < 0.000001f);
}

void testEmptyAndBasicTransfer()
{
    MediaPlayer::PcmRingBuffer queue(4);
    assert(queue.capacityFrames() == 4);
    assert(queue.availableFrames() == 0);
    assert(queue.freeFrames() == 4);

    const float left[] = {1.0f, 2.0f, 3.0f};
    const float right[] = {11.0f, 12.0f, 13.0f};
    assert(queue.writeStereo(left, right, 3) == 3);
    assert(queue.availableFrames() == 3);
    assert(queue.freeFrames() == 1);

    float readLeft[2] {};
    float readRight[2] {};
    assert(queue.readStereo(readLeft, readRight, 2) == 2);
    expectNear(readLeft[0], 1.0f);
    expectNear(readLeft[1], 2.0f);
    expectNear(readRight[0], 11.0f);
    expectNear(readRight[1], 12.0f);
    assert(queue.availableFrames() == 1);
}

void testWrapAroundAndCapacity()
{
    MediaPlayer::PcmRingBuffer queue(4);
    const float firstLeft[] = {0.0f, 1.0f, 2.0f};
    const float firstRight[] = {10.0f, 11.0f, 12.0f};
    const float secondLeft[] = {3.0f, 4.0f, 5.0f};
    const float secondRight[] = {13.0f, 14.0f, 15.0f};

    assert(queue.writeStereo(firstLeft, firstRight, 3) == 3);
    float discardedLeft[2] {};
    float discardedRight[2] {};
    assert(queue.readStereo(discardedLeft, discardedRight, 2) == 2);
    assert(queue.writeStereo(secondLeft, secondRight, 3) == 3);

    float outputLeft[4] {};
    float outputRight[4] {};
    assert(queue.readStereo(outputLeft, outputRight, 4) == 4);
    for (int index = 0; index < 4; ++index) {
        expectNear(outputLeft[index], static_cast<float>(index + 2));
        expectNear(outputRight[index], static_cast<float>(index + 12));
    }

    const float overflowLeft[] = {6.0f, 7.0f, 8.0f, 9.0f, 10.0f};
    const float overflowRight[] = {16.0f, 17.0f, 18.0f, 19.0f, 20.0f};
    assert(queue.writeStereo(overflowLeft, overflowRight, 5) == 4);
    assert(queue.availableFrames() == 4);
}

void testClear()
{
    MediaPlayer::PcmRingBuffer queue(8);
    const float sample[] = {1.0f};
    assert(queue.writeStereo(sample, sample, 1) == 1);
    queue.clear();
    assert(queue.availableFrames() == 0);
    assert(queue.freeFrames() == 8);
}

} // namespace

int main()
{
    testEmptyAndBasicTransfer();
    testWrapAroundAndCapacity();
    testClear();
    std::cout << "PcmRingBufferTests: all tests passed\n";
    return 0;
}
