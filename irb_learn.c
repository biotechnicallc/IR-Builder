#include "irb_learn.h"

#include <flipper_format/flipper_format.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IRB_LEARN_PREFIX IRB_IMPORT_DIR "/LR_"

static bool fail(char* error, const char* text) {
    snprintf(error, IRB_ERROR_SIZE, "%s", text);
    return false;
}

static bool is_learn_source(const char* path) {
    return path && !strncmp(path, IRB_LEARN_PREFIX, strlen(IRB_LEARN_PREFIX));
}

static bool next_learn_path(Storage* storage, char path[IRB_PATH_SIZE]) {
    for(unsigned i = 1; i <= 9999; ++i) {
        int written = snprintf(path, IRB_PATH_SIZE, "%s%04u.ir", IRB_LEARN_PREFIX, i);
        if(written <= 0 || written >= IRB_PATH_SIZE) return false;
        if(storage_common_stat(storage, path, NULL) == FSE_NOT_EXIST) return true;
    }
    return false;
}

static bool copy_file(Storage* storage, const char* source, const char* target) {
    File* input = storage_file_alloc(storage);
    File* output = storage_file_alloc(storage);
    bool output_created = false;

    bool ok = storage_file_open(input, source, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) {
        ok = storage_file_open(output, target, FSAM_WRITE, FSOM_CREATE_NEW);
        output_created = ok;
    }

    uint8_t buffer[512];
    while(ok) {
        size_t amount = storage_file_read(input, buffer, sizeof(buffer));
        if(!amount) break;
        if(storage_file_write(output, buffer, amount) != amount) ok = false;
    }

    storage_file_close(input);
    if(output_created && !storage_file_close(output)) ok = false;
    storage_file_free(input);
    storage_file_free(output);

    if(!ok && output_created) storage_common_remove(storage, target);
    return ok;
}

static bool create_empty_source(Storage* storage, const char* path) {
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    bool opened = flipper_format_file_open_new(ff, path);
    bool ok = opened && flipper_format_write_header_cstr(ff, "IR signals file", 1);
    if(opened && !flipper_format_file_close(ff)) ok = false;
    flipper_format_free(ff);
    if(!ok) storage_common_remove(storage, path);
    return ok;
}

static bool append_signal(
    Storage* storage,
    const char* path,
    const InfraredSignal* signal,
    const char* label) {
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    bool opened = flipper_format_file_open_append(ff, path);
    bool ok = opened &&
              infrared_signal_save(signal, ff, label) == InfraredErrorCodeNone;
    if(opened && !flipper_format_file_close(ff)) ok = false;
    flipper_format_free(ff);
    return ok;
}

bool irb_learn_store(
    Storage* storage,
    IrbProject* project,
    uint32_t slot,
    const InfraredSignal* signal,
    char* error) {
    error[0] = 0;

    if(!storage || !project || !signal || irb_slot_group_index(slot) < 0 ||
       !infrared_signal_is_valid(signal) || !irb_project_valid(project))
        return fail(error, "Invalid learned signal or destination button.");

    if(!irb_storage_prepare(storage))
        return fail(error, "Cannot prepare IR Builder storage.");

    IrbImportSource preserved = {0};
    for(unsigned i = 0; i < project->import_count; ++i) {
        if(is_learn_source(project->imports[i].path)) {
            preserved = project->imports[i];
            break;
        }
    }

    IrbProject* next = malloc(sizeof(*next));
    if(!next) return fail(error, "Not enough memory.");
    *next = *project;

    irb_project_remove(next, slot);

    int source_index = -1;
    for(unsigned i = 0; i < next->import_count; ++i) {
        if(is_learn_source(next->imports[i].path)) {
            source_index = (int)i;
            break;
        }
    }

    if(source_index < 0) {
        if(next->import_count >= IRB_MAX_IMPORTS) {
            free(next);
            return fail(
                error,
                "All 4 import-source slots are in use. Remove one imported .ir source first.");
        }
        source_index = (int)next->import_count++;
        if(preserved.path[0])
            next->imports[source_index] = preserved;
        else
            memset(&next->imports[source_index], 0, sizeof(next->imports[source_index]));
    }

    IrbImportSource* bank = &next->imports[source_index];
    const char* previous_path = bank->path[0] ? bank->path : preserved.path;

    char target[IRB_PATH_SIZE];
    if(!next_learn_path(storage, target)) {
        free(next);
        return fail(error, "Could not allocate learned-signal storage.");
    }

    bool created = false;
    if(previous_path[0]) {
        uint32_t hash = 0, size = 0;
        if(!irb_file_fingerprint(
               storage, previous_path, &hash, &size, IrbLoadVerify, NULL, NULL) ||
           hash != bank->hash || size != bank->size) {
            free(next);
            return fail(error, "Previous learned-signal source is missing or changed.");
        }
        created = copy_file(storage, previous_path, target);
    } else {
        created = create_empty_source(storage, target);
    }

    if(!created) {
        free(next);
        return fail(error, "Could not create learned-signal file.");
    }

    const char* label = irb_project_label(next, slot);
    if(!append_signal(storage, target, signal, label)) {
        storage_common_remove(storage, target);
        free(next);
        return fail(error, "Signal was received but could not be written.");
    }

    IrbCatalog* catalog = malloc(sizeof(*catalog));
    if(!catalog) {
        storage_common_remove(storage, target);
        free(next);
        return fail(error, "Not enough memory to verify learned signal.");
    }

    bool ok = irb_catalog_load(storage, catalog, target, error, NULL, NULL);
    uint32_t offset = 0;
    if(ok && catalog->count)
        offset = catalog->entries[catalog->count - 1].offset;
    else
        ok = false;

    if(ok) {
        snprintf(bank->path, sizeof(bank->path), "%s", target);
        bank->hash = catalog->hash;
        bank->size = catalog->size;
        ok = irb_project_set_imported(
                 next, slot, (uint32_t)source_index, offset) &&
             irb_project_valid(next);
    }

    free(catalog);

    if(ok) {
        *project = *next;
    } else {
        storage_common_remove(storage, target);
        if(!error[0])
            snprintf(error, IRB_ERROR_SIZE, "%s", "Learned signal verification failed.");
    }

    free(next);
    return ok;
}
