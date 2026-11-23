#pragma once

// SXC (FlashWOW): the models' bones (CM2Model::AnimateMT) over more threads than the stock two. The client
// ran the scene's animate list on the main thread and one more ("Model2", CM2Cache's thread: every other
// model); with crowds the bones are the part that grows with each unit. These threads select the cache's
// prop context, as the stock thread does, and take the models one at a time (some cost far more).
#include <cstdint>
#include "Engine/Source/Base/Prop.h"

namespace M2AnimatePool {
    // fn(param, i) for every i in [0, count), on the pool's threads and the calling one; returns when all are
    // done. The pool starts on first use. With no threads (or a second caller meanwhile) all run here.
    void ForEach(uint32_t count, void (*fn)(void* param, uint32_t i), void* param, HPROPCONTEXT context);

    // The threads besides the caller (0 until started or when they couldn't be made).
    int Threads();
}
