#pragma once

#include "irb_files.h"

bool irb_learn_store(
    Storage* storage,
    IrbProject* project,
    uint32_t slot,
    const InfraredSignal* signal,
    char* error);
