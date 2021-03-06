/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "debug_ptrace.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/ptrace.h>

#include "debug_map_info.h"
#include "dlmalloc.h"
#include "libc_logging.h"

static const uint32_t ELF_MAGIC = 0x464C457f; // "ELF\0177"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#ifndef PAGE_MASK
#define PAGE_MASK (~(PAGE_SIZE - 1))
#endif

/* Custom extra data we stuff into map_info_t structures as part
 * of our ptrace_context_t. */
typedef struct {
#ifdef __arm__
    uintptr_t exidx_start;
    size_t exidx_size;
#endif
    symbol_table_t* symbol_table;
} map_info_data_t;


/* Describes how to access memory from a process. */
typedef struct {
    const map_info_t* map_info_list;
} memory_t;

/*
 * Initializes a memory structure for accessing memory from this process.
 */
void init_memory(memory_t* memory, const map_info_t* map_info_list) {
    memory->map_info_list = map_info_list;
}

/*
 * Reads a word of memory safely.
 * If the memory is local, ensures that the address is readable before dereferencing it.
 * Returns false and a value of 0xffffffff if the word could not be read.
 */
bool try_get_word(const memory_t* memory, uintptr_t ptr, uint32_t* out_value) {
    __libc_format_log(TANGMAI_LOG_DEBUG, "libc", "try_get_word: reading word at 0x%08x", ptr);
    if (ptr & 3) {
        __libc_format_log(TANGMAI_LOG_DEBUG, "libc", "try_get_word: invalid pointer 0x%08x", ptr);
        *out_value = 0xffffffffL;
        return false;
    }
    if (!is_readable_map(memory->map_info_list, ptr)) {
        __libc_format_log(TANGMAI_LOG_DEBUG, "libc", "try_get_word: pointer 0x%08x not in a readable map", ptr);
        *out_value = 0xffffffffL;
        return false;
    }
    *out_value = *(uint32_t*)ptr;
    return true;
}

/*
 * Reads a word of memory safely
 * Returns false and a value of 0xffffffff if the word could not be read.
 */
static bool try_get_word_ptrace(const map_info_t* map_info_list, uintptr_t ptr, uint32_t* out_value) {
    memory_t memory;
    init_memory(&memory, map_info_list);
    return try_get_word(&memory, ptr, out_value);
}

static void load_ptrace_map_info_data(const map_info_t* map_info_list, map_info_t* mi) {
    if (mi->is_executable && mi->is_readable) {
        uint32_t elf_magic;
        if (try_get_word_ptrace(map_info_list, mi->start, &elf_magic) && elf_magic == ELF_MAGIC) {
            map_info_data_t* data = (map_info_data_t*)dlcalloc(1, sizeof(map_info_data_t));
            if (data) {
                mi->data = data;
                if (mi->name[0]) {
                    data->symbol_table = load_symbol_table(mi->name);
                }
            }
        }
    }
}

ptrace_context_t* load_ptrace_context(pid_t pid) {
    ptrace_context_t* context =
            (ptrace_context_t*)dlcalloc(1, sizeof(ptrace_context_t));
    if (context) {
        context->map_info_list = load_map_info_list(pid);
		map_info_t* mi;
        for (mi = context->map_info_list; mi; mi = mi->next) {
            load_ptrace_map_info_data(context->map_info_list, mi);
        }
    }
    return context;
}

static void free_ptrace_map_info_data(map_info_t* mi) {
    map_info_data_t* data = (map_info_data_t*)mi->data;
    if (data) {
        if (data->symbol_table) {
            free_symbol_table(data->symbol_table);
        }
        dlfree(data);
        mi->data = NULL;
    }
}

void free_ptrace_context(ptrace_context_t* context) {
	map_info_t* mi;
    for (mi = context->map_info_list; mi; mi = mi->next) {
        free_ptrace_map_info_data(mi);
    }
    free_map_info_list(context->map_info_list);
}

void find_symbol_ptrace(const ptrace_context_t* context,
        uintptr_t addr, const map_info_t** out_map_info, const symbol_t** out_symbol) {
    const map_info_t* mi = find_map_info(context->map_info_list, addr);
    const symbol_t* symbol = NULL;
    if (mi) {
        const map_info_data_t* data = (const map_info_data_t*)mi->data;
        if (data && data->symbol_table) {
            symbol = find_symbol(data->symbol_table, addr - mi->start);
        }
    }
    *out_map_info = mi;
    *out_symbol = symbol;
}
