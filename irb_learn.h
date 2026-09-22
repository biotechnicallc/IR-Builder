#pragma once

#include "irb_files.h"

bool irb_learn_store(
    Storage* storage,
    IrbProject* project,
    uint32_t slot,
    const InfraredSignal* signal,
    char* error);

bool irb_learn_add_extra(
    Storage* storage,
    IrbProject* project,
    const char* label,
    const InfraredSignal* signal,
    uint32_t* new_slot,
    char* error);
