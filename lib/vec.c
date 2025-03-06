/* Copyright (c) 2025, Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "vec.h"
#include "util.h"

#define BYTE_SIZE(VEC, N) ((VEC)->esize * (N))

static void vector_resize(struct vector *vec, size_t new_capacity);

bool
vector_insert(struct vector *vec, size_t index, void *element)
{
    if (index > vec->len) {
        return false;
    }

    if (vec->len == vec->capacity) {
        vector_resize(vec, vec->capacity ? vec->capacity * 2 : 1);
    }

    uint8_t *new_dest = (uint8_t *) vec->buffer + BYTE_SIZE(vec, index);
    size_t shift_len = vec->len - index;
    memmove(new_dest + vec->esize, new_dest, BYTE_SIZE(vec, shift_len));
    memcpy(new_dest, element, vec->esize);
    vec->len++;

    return true;
}

bool
vector_remove(struct vector *vec, size_t index, void *element)
{
    if (index >= vec->len || vec->len == 0) {
        return false;
    }

    uint8_t *new_dest = (uint8_t *) vec->buffer + BYTE_SIZE(vec, index);
    size_t shift_len = vec->len - index - 1;
    memcpy(element, new_dest, vec->esize);
    memmove(new_dest, new_dest + vec->esize, BYTE_SIZE(vec, shift_len));
    vec->len--;

    return true;
}

const void *
vector_get_ptr(const struct vector *vec, size_t index)
{
    if (index >= vec->len) {
        return NULL;
    }

    return (uint8_t *) vec->buffer + BYTE_SIZE(vec, index);
}

void
vector_shrink_to_fit(struct vector *vec)
{
    if (vec->len == vec->capacity) {
        return;
    }

    vector_resize(vec, vec->len);
}

struct vector
vector_clone(struct vector *vec)
{
    struct vector clone = (struct vector) {
        .buffer = xmalloc(BYTE_SIZE(vec, vec->capacity)),
        .esize = vec->esize,
        .len = vec->len,
        .capacity = vec->capacity,
    };
    memcpy(clone.buffer, vec->buffer, BYTE_SIZE(vec, vec->len));

    return clone;
}

static void
vector_resize(struct vector *vec, size_t new_capacity)
{
    if (!new_capacity) {
        vector_destroy(vec);
        return;
    }

    vec->buffer = xrealloc(vec->buffer, BYTE_SIZE(vec, new_capacity));
    vec->capacity = new_capacity;
}
