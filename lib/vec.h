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

#ifndef VEC_H
#define VEC_H


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct vector {
    void *buffer;    /* Data bytes. */
    size_t len;         /* Number of elements. */
    size_t esize;       /* Size of each element in bytes. */
    size_t capacity;    /* Element capacity. */
};

#define VECTOR_EMPTY_INITIALIZER(TYPE) \
    (struct vector) {                  \
        .buffer = NULL,                \
        .len = 0,                      \
        .esize = sizeof(TYPE),         \
        .capacity = 0                  \
    }

#define VECTOR_CAPACITY_INITIALIZER(TYPE, CAP)    \
    (struct vector) {                             \
        .buffer = xmalloc(sizeof (TYPE) * (CAP)), \
        .esize = sizeof(TYPE),                    \
        .len = 0,                                 \
        .capacity = (CAP),                        \
    }

#define VECTOR_PTR_DIFF(VEC, NODE, TYPE) \
    ITER_VAR(NODE) - (TYPE *) (VEC)->buffer < (VEC)->len

/* Note that storing the returned pointer will result in UB, as the vector
 * might change the memory location during insert. */
#define VECTOR_FOR_EACH_PTR(VEC, NODE)                              \
    for (INIT_MULTIVAR(NODE, 0, (VEC)->buffer, OVS_TYPEOF(*NODE));      \
         (VECTOR_PTR_DIFF(VEC, NODE, OVS_TYPEOF(*NODE)) ?               \
            (((NODE) = ITER_VAR(NODE)), 1) :                            \
            0);                                                         \
         UPDATE_MULTIVAR(NODE, ITER_VAR(NODE) + 1))

#define VECTOR_FOR_EACH(VEC, NODE)                                      \
    for (INIT_MULTIVAR(NODE, 0, (VEC)->buffer, OVS_TYPEOF(NODE));       \
         (VECTOR_PTR_DIFF(VEC, NODE, OVS_TYPEOF(NODE)) ?                \
            (((NODE) = *ITER_VAR(NODE)), 1) :                           \
            0);                                                         \
         UPDATE_MULTIVAR(NODE, ITER_VAR(NODE) + 1))

#define VECTOR_FOR_EACH_POP(VEC, NODE)                                  \
    while (!vector_is_empty((VEC)) ?                                    \
           (vector_pop((VEC), (&NODE)), 1) :                            \
           0)

#define vector_to_array(VEC) \
    (VEC)->buffer

#define vector_get(VEC, INDEX, TYPE) \
    (*(TYPE *) vector_get_ptr((VEC), (INDEX)))

/* Inerts element at index, the content at the index is shifted right.
 * Returns 'false' if the index is out of bounds. Note that the element
 * is pointer to the type being stored, e.g. in case of char *
 * char ** should be passed. */
bool vector_insert(struct vector *vec, size_t index, void *element);
/* Removes element at index, the argument "element" is populated with the
 * data. The content after index is shifted left. Returns 'false' if the index
 * is out of bounds. */
bool vector_remove(struct vector *vec, size_t index, void *element);
/* Gets pointer to the item at index. */
const void *vector_get_ptr(const struct vector *vec, size_t index);
/* Reallocates the vector to fit exactly the length if that's not the case. */
void vector_shrink_to_fit(struct vector *vec);
/* Clones the vector vec into new one, the content is memcopied. */
struct vector vector_clone(struct vector *vec);

/* Pushes element into the back of the vector. */
static inline void
vector_push(struct vector *vec, void *element)
{
    vector_insert(vec, vec->len, element);
}

/* Pops element from the back, the argument "element" is populated
 * with the data. */
static inline void
vector_pop(struct vector *vec, void *element)
{
    vector_remove(vec, vec->len - 1, element);
}

/* Clears the vector without deallocating the buffer. */
static inline void
vector_clear(struct vector *vec)
{
    vec->len = 0;
}

/* Destroys the vector content. It doesn't free individual elements, that's up
 * to the caller. */
static inline void
vector_destroy(struct vector *vec)
{
    free(vec->buffer);
    vec->len = 0;
    vec->capacity = 0;
    vec->buffer = NULL;
}

/* Returns the length in number of elements. */
static inline size_t
vector_len(const struct vector *vec)
{
    return vec->len;
}

/* Returns the capacity in number of elements. */
static inline size_t
vector_capacity(const struct vector *vec)
{
    return vec->capacity;
}

/* Return true if vector is empty. */
static inline bool
vector_is_empty(const struct vector * vec)
{
    return vec->len == 0;
}

/* Quick sort of all elements in the vector. */
static inline void
vector_qsort(struct vector *vec, int (*cmp)(const void *a, const void *b))
{
    if (vec->len) {
        qsort(vec->buffer, vec->len, vec->esize, cmp);
    }
}

#endif /* lib/vec.h */
