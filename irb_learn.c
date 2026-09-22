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

static int find_learn_source(const IrbProject* project) {
    for(unsigned i = 0; i < project->import_count; ++i)
        if(is_learn_source(project->imports[i].path)) return (int)i;
    return -1;
}

static bool append_to_learn_bank(
    Storage* storage,
    IrbProject* project,
    const IrbImportSource* preserved,
    const InfraredSignal* signal,
    const char* label,
    uint32_t* source_out,
    uint32_t* offset_out,
    char* error) {
    int source_index = find_learn_source(project);

    if(source_index < 0) {
        if(project->import_count >= IRB_MAX_IMPORTS)
            return fail(
                error,
                "All 4 import-source slots are in use. Remove one imported .ir source first.");

        source_index = (int)project->import_count++;
        if(preserved && preserved->path[0])
            project->imports[source_index] = *preserved;
        else
            memset(&project->imports[source_index], 0, sizeof(project->imports[source_index]));
    }

    IrbImportSource* bank = &project->imports[source_index];
    const char* previous_path = bank->path;
    if((!previous_path || !previous_path[0]) && preserved) previous_path = preserved->path;

    char target[IRB_PATH_SIZE];
    if(!next_learn_path(storage, target))
        return fail(error, "Could not allocate learned-signal storage.");

    bool created = false;
    if(previous_path && previous_path[0]) {
        uint32_t hash = 0, size = 0;
        uint32_t expected_hash = bank->hash ? bank->hash : preserved->hash;
        uint32_t expected_size = bank->size ? bank->size : preserved->size;

        if(!irb_file_fingerprint(
               storage,
               previous_path,
               &hash,
               &size,
               IrbLoadVerify,
               NULL,
               NULL) ||
           hash != expected_hash || size != expected_size)
            return fail(error, "Previous learned-signal source is missing or changed.");

        created = copy_file(storage, previous_path, target);
    } else {
        created = create_empty_source(storage, target);
    }

    if(!created) return fail(error, "Could not create learned-signal file.");

    if(!append_signal(storage, target, signal, label)) {
        storage_common_remove(storage, target);
        return fail(error, "Signal was received but could not be written.");
    }

    IrbCatalog* catalog = malloc(sizeof(*catalog));
    if(!catalog) {
        storage_common_remove(storage, target);
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
        *source_out = (uint32_t)source_index;
        *offset_out = offset;
    } else {
        storage_common_remove(storage, target);
        if(!error[0])
            snprintf(error, IRB_ERROR_SIZE, "%s", "Learned signal verification failed.");
    }

    free(catalog);
    return ok;
}

bool irb_learn_store(
    Storage* storage,
    IrbProject* project,
    uint32_t slot,
    const InfraredSignal* signal,
    char* error) {
    error[0] = 0;

    bool fixed = irb_slot_group_index(slot) >= 0;
    bool extra =
        slot >= IRB_SLOTS &&
        slot < IRB_SLOTS + project->extra_count &&
        !irb_slot_is_nav(slot);

    if(!storage ||
       !project ||
       !signal ||
       (!fixed && !extra) ||
       !infrared_signal_is_valid(signal) ||
       !irb_project_valid(project))
        return fail(error, "Invalid learned signal or destination button.");

    if(!irb_storage_prepare(storage))
        return fail(error, "Cannot prepare IR Builder storage.");

    char label[IRB_NAME_SIZE];
    snprintf(label, sizeof(label), "%s", irb_project_label(project, slot));

    IrbImportSource preserved = {0};
    int old_bank = find_learn_source(project);
    if(old_bank >= 0) preserved = project->imports[old_bank];

    IrbProject* next = malloc(sizeof(*next));
    if(!next) return fail(error, "Not enough memory.");
    *next = *project;

    if(fixed) irb_project_remove(next, slot);

    uint32_t source = 0;
    uint32_t offset = 0;

    bool ok = append_to_learn_bank(
        storage,
        next,
        &preserved,
        signal,
        label,
        &source,
        &offset,
        error);

    if(ok) {
        if(fixed) {
            ok = irb_project_set_imported(next, slot, source, offset);
        } else {
            uint32_t index = slot - IRB_SLOTS;
            next->extras[index].source = source;
            next->extras[index].offset = offset;
        }
    }

    ok = ok && irb_project_valid(next);

    if(ok)
        *project = *next;
    else if(!error[0])
        snprintf(error, IRB_ERROR_SIZE, "%s", "Learned signal could not be assigned.");

    free(next);
    return ok;
}

bool irb_learn_add_extra(
    Storage* storage,
    IrbProject* project,
    const char* label,
    const InfraredSignal* signal,
    uint32_t* new_slot,
    char* error) {
    error[0] = 0;

    if(!storage ||
       !project ||
       !label ||
       !signal ||
       !new_slot ||
       !infrared_signal_is_valid(signal) ||
       !irb_project_valid(project))
        return fail(error, "Invalid custom button request.");

    if(project->extra_count >= IRB_MAX_EXTRAS)
        return fail(error, "This remote already has 48 custom buttons.");

    if(!irb_name_valid(label, false))
        return fail(error, "Use 1-31 printable characters for the button name.");

    if(!irb_project_label_available(project, label, UINT32_MAX))
        return fail(error, "Button name already exists.");

    if(!irb_storage_prepare(storage))
        return fail(error, "Cannot prepare IR Builder storage.");

    IrbProject* next = malloc(sizeof(*next));
    if(!next) return fail(error, "Not enough memory.");
    *next = *project;

    uint32_t source = 0;
    uint32_t offset = 0;

    bool ok = append_to_learn_bank(
        storage,
        next,
        NULL,
        signal,
        label,
        &source,
        &offset,
        error);

    if(ok) {
        uint32_t index = next->extra_count;
        IrbExtraButton* extra = &next->extras[index];
        memset(extra, 0, sizeof(*extra));
        snprintf(extra->label, sizeof(extra->label), "%s", label);
        extra->source = source;
        extra->offset = offset;
        ++next->extra_count;

        *new_slot = IRB_SLOTS + index;
        ok = irb_project_valid(next);
    }

    if(ok)
        *project = *next;
    else if(!error[0])
        snprintf(error, IRB_ERROR_SIZE, "%s", "Custom button could not be added.");

    free(next);
    return ok;
}
