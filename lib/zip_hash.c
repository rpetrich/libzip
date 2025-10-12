/*
  zip_hash.c -- hash table string -> uint64
  Copyright (C) 2015-2022 Dieter Baron and Thomas Klausner

  This file is part of libzip, a library to manipulate ZIP archives.
  The authors can be contacted at <info@libzip.org>

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
  1. Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in
     the documentation and/or other materials provided with the
     distribution.
  3. The names of the authors may not be used to endorse or promote
     products derived from this software without specific prior
     written permission.

  THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS
  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "zipint.h"
#include <stdlib.h>
#include <string.h>

/* parameter for the string hash function */
#define HASH_MULTIPLIER 33
#define HASH_START 5381

/* hash table's fill ratio is kept between these by doubling/halfing its size as necessary */
#define HASH_MAX_FILL .75
#define HASH_MIN_FILL .01

/* but hash table size is kept between these */
#define HASH_MIN_SIZE 256
#define HASH_MAX_SIZE 0x80000000ul

struct zip_hash_entry {
    const zip_uint8_t *name;
#ifndef LIBZIP_MINIMAL
    zip_int32_t orig_index;
#endif
    zip_int32_t current_index;
    zip_uint32_t hash_value;
};
typedef struct zip_hash_entry zip_hash_entry_t;

struct zip_hash {
    zip_uint32_t table_size;
    zip_uint64_t nentries;
    zip_hash_entry_t *table;
};


/* compute hash of string, full 32 bit value */
static zip_uint32_t
hash_string(const zip_uint8_t *name) {
    zip_uint64_t value = HASH_START;

    if (name == NULL) {
        return 0;
    }

    while (*name != 0) {
        if (*name == '/' && name[1] == '\0') {
            break;
        }
        value = (zip_uint64_t)(((value * HASH_MULTIPLIER) + (zip_uint8_t)*name) % 0x100000000ul);
        name++;
    }

    return (zip_uint32_t)value;
}


/* resize hash table; new_size must be a power of 2, can be larger or smaller than current size */
static bool
hash_resize(zip_hash_t *hash, zip_uint32_t new_size, zip_error_t *error) {
    zip_hash_entry_t *new_table;

    if (new_size == hash->table_size) {
        return true;
    }

    if ((new_table = (zip_hash_entry_t *)calloc(new_size, sizeof(zip_hash_entry_t))) == NULL) {
        zip_error_set(error, ZIP_ER_MEMORY, 0);
        return false;
    }

    if (hash->nentries > 0) {
        zip_uint32_t i;

        for (i = 0; i < hash->table_size; i++) {
            zip_hash_entry_t entry = hash->table[i];
            if (entry.name != NULL) {
                zip_uint32_t new_index = entry.hash_value & (new_size - 1);
                while (new_table[new_index].name != NULL) {
                    new_index = (new_index + 1) & (new_size - 1);
                }
                new_table[new_index] = entry;
            }
        }
    }

    free(hash->table);
    hash->table = new_table;
    hash->table_size = new_size;

    return true;
}


static zip_uint32_t
size_for_capacity(zip_uint64_t capacity) {
    double needed_size = capacity / HASH_MAX_FILL;
    zip_uint32_t v;

    if (needed_size > ZIP_UINT32_MAX) {
        v = ZIP_UINT32_MAX;
    }
    else {
        v = (zip_uint32_t)needed_size;
    }

    if (v > HASH_MAX_SIZE) {
        return HASH_MAX_SIZE;
    }

    /* From Bit Twiddling Hacks by Sean Eron Anderson <seander@cs.stanford.edu>
     (http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2). */

    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;

    return v;
}


zip_hash_t *
_zip_hash_new(zip_error_t *error) {
    zip_hash_t *hash;

    if ((hash = (zip_hash_t *)malloc(sizeof(zip_hash_t))) == NULL) {
        zip_error_set(error, ZIP_ER_MEMORY, 0);
        return NULL;
    }

    hash->table_size = 0;
    hash->nentries = 0;
    hash->table = NULL;

    return hash;
}


void
_zip_hash_free(zip_hash_t *hash) {
    if (hash == NULL) {
        return;
    }

    if (hash->table != NULL) {
        free(hash->table);
    }
    free(hash);
}


/* insert into hash, return error on existence or memory issues */
bool
_zip_hash_add(zip_hash_t *hash, const zip_uint8_t *name, zip_uint64_t index, zip_flags_t flags, zip_error_t *error) {
    zip_uint32_t hash_value, table_index, table_mask;
    zip_hash_entry_t *entry;

    if (hash == NULL || name == NULL || index > ZIP_INT64_MAX) {
        zip_error_set(error, ZIP_ER_INVAL, 0);
        return false;
    }

    if (hash->table_size == 0) {
        if (!hash_resize(hash, HASH_MIN_SIZE, error)) {
            return false;
        }
    }

    hash_value = hash_string(name);

    table_mask = hash->table_size - 1;
    table_index = hash_value & table_mask;
    entry = &hash->table[table_index];

    while (entry->name != NULL) {
        if (entry->hash_value == hash_value && _zip_name_cmp((const char *)name, (const char *)entry->name) == 0) {
#ifdef LIBZIP_MINIMAL
            if (entry->current_index != -1) {
#else
            if (((flags & ZIP_FL_UNCHANGED) && entry->orig_index != -1) || entry->current_index != -1) {
#endif
                zip_error_set(error, ZIP_ER_EXISTS, 0);
                return false;
            }
            else {
                break;
            }
        }
        table_index = (table_index + 1) & table_mask;
        entry = &hash->table[table_index];
    }

    if (entry->name != NULL) {
        entry->current_index = (zip_int64_t)index;
    } else {
        entry->name = name;
        entry->hash_value = hash_value;
#ifndef LIBZIP_MINIMAL
        if (flags & ZIP_FL_UNCHANGED) {
            entry->orig_index = (zip_int64_t)index;
        } else {
            entry->orig_index = -1;
        }
#endif
        entry->current_index = (zip_int64_t)index;

        hash->nentries++;
        if (hash->nentries > hash->table_size * HASH_MAX_FILL && hash->table_size < HASH_MAX_SIZE) {
            if (!hash_resize(hash, hash->table_size * 2, error)) {
                return false;
            }
        }
    }

    return true;
}


/* remove entry from hash, error if not found */
bool
_zip_hash_delete(zip_hash_t *hash, const zip_uint8_t *name, zip_error_t *error) {
    zip_uint32_t hash_value, index, mask;
    zip_hash_entry_t *entry;

    if (hash == NULL || name == NULL) {
        zip_error_set(error, ZIP_ER_INVAL, 0);
        return false;
    }

    if (hash->nentries > 0) {
        hash_value = hash_string(name);
        mask = hash->table_size - 1;
        index = hash_value & mask;
        entry = &hash->table[index];
        while (entry->name != NULL) {
            if (entry->hash_value == hash_value && _zip_name_cmp((const char *)name, (const char *)entry->name) == 0) {
#ifndef LIBZIP_MINIMAL
                if (entry -> orig_index == -1) {
                    // TODO: support deleting
                    zip_error_set(ctx->error, ZIP_ER_INVAL, 0);
                    return false;
                }
#endif
                entry->current_index = -1;
                return true;
            }
            index = (index + 1) & mask;
            entry = &hash->table[index];
        }
    }

    zip_error_set(error, ZIP_ER_NOENT, 0);
    return false;
}


/* find value for entry in hash, -1 if not found */
zip_int64_t
_zip_hash_lookup(zip_hash_t *hash, const zip_uint8_t *name, zip_flags_t flags, zip_error_t *error) {
    zip_uint32_t hash_value, index, mask;
    zip_hash_entry_t *entry;

    if (hash == NULL || name == NULL) {
        zip_error_set(error, ZIP_ER_INVAL, 0);
        return -1;
    }

    if (hash->nentries > 0) {
        hash_value = hash_string(name);
        mask = (hash->table_size - 1);
        index = hash_value & mask;
        entry = &hash->table[index];
        while (entry->name != NULL) {
            if (entry->hash_value == hash_value && _zip_name_cmp((const char *)name, (const char *)entry->name) == 0) {
#ifdef LIBZIP_MINIMAL
                if (entry->current_index != -1) {
                    return entry->current_index;
                }
#else
                if (flags & ZIP_FL_UNCHANGED) {
                    if (entry->orig_index != -1) {
                        return entry->orig_index;
                    }
                }
                else {
                    if (entry->current_index != -1) {
                        return entry->current_index;
                    }
                }
#endif
                break;
            }
            index = (index + 1) & mask;
            entry = &hash->table[index];
        }
    }

    zip_error_set(error, ZIP_ER_NOENT, 0);
    return -1;
}


bool
_zip_hash_reserve_capacity(zip_hash_t *hash, zip_uint64_t capacity, zip_error_t *error) {
    zip_uint32_t new_size;

    if (capacity == 0) {
        return true;
    }

    new_size = size_for_capacity(capacity);

    if (new_size <= hash->table_size) {
        return true;
    }

    if (!hash_resize(hash, new_size, error)) {
        return false;
    }

    return true;
}


#ifndef LIBZIP_MINIMAL
bool
_zip_hash_revert(zip_hash_t *hash, zip_error_t *error) {
    zip_uint32_t i;
    zip_hash_entry_t *entry, *previous;

    for (i = 0; i < hash->table_size; i++) {
        previous = NULL;
        entry = hash->table[i];
        while (entry) {
            if (entry->orig_index == -1) {
                zip_hash_entry_t *p;
                if (previous) {
                    previous->next = entry->next;
                }
                else {
                    hash->table[i] = entry->next;
                }
                p = entry;
                entry = entry->next;
                /* previous does not change */
                free(p);
                hash->nentries--;
            }
            else {
                entry->current_index = entry->orig_index;
                previous = entry;
                entry = entry->next;
            }
        }
    }

    if (hash->nentries < hash->table_size * HASH_MIN_FILL && hash->table_size > HASH_MIN_SIZE) {
        zip_uint32_t new_size = hash->table_size / 2;
        while (hash->nentries < new_size * HASH_MIN_FILL && new_size > HASH_MIN_SIZE) {
            new_size /= 2;
        }
        if (!hash_resize(hash, new_size, error)) {
            return false;
        }
    }

    return true;
}
#endif
