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

#include <stdint.h>
#include <string.h>

#include "lib/vec.h"
#include "tests/ovstest.h"
#include "lib/ovn-util.h"

static void
test_add(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    uint32_t elements[5] = {0, 1, 2, 3, 4};

    struct vector vec = VECTOR_EMPTY_INITIALIZER(uint32_t);
    ovs_assert(vector_capacity(&vec) == 0);
    ovs_assert(vector_len(&vec) == 0);

    vector_push(&vec, &elements[1]);
    ovs_assert(vector_capacity(&vec) == 1);
    ovs_assert(vector_len(&vec) == 1);
    ovs_assert(vector_get(&vec, 0, uint32_t) == elements[1]);

    vector_push(&vec, &elements[2]);
    ovs_assert(vector_capacity(&vec) == 2);
    ovs_assert(vector_len(&vec) == 2);
    ovs_assert(vector_get(&vec, 1, uint32_t) == elements[2]);

    vector_push(&vec, &elements[3]);
    ovs_assert(vector_capacity(&vec) == 4);
    ovs_assert(vector_len(&vec) == 3);
    ovs_assert(vector_get(&vec, 2, uint32_t) == elements[3]);

    vector_push(&vec, &elements[4]);
    ovs_assert(vector_capacity(&vec) == 4);
    ovs_assert(vector_len(&vec) == 4);
    ovs_assert(vector_get(&vec, 3, uint32_t) == elements[4]);

    ovs_assert(vector_insert(&vec, 0, &elements[0]));
    ovs_assert(vector_capacity(&vec) == 8);
    ovs_assert(vector_len(&vec) == 5);
    ovs_assert(vector_get(&vec, 0, uint32_t) == elements[0]);

    size_t i = 0;
    uint32_t *ptr;
    VECTOR_FOR_EACH_PTR (&vec, ptr) {
        ovs_assert(elements[i++] == *ptr);
    }
    ovs_assert(i == ARRAY_SIZE(elements));

    i = 0;
    uint32_t num;
    VECTOR_FOR_EACH (&vec, num) {
        ovs_assert(elements[i++] == num);
    }
    ovs_assert(i == ARRAY_SIZE(elements));

    vector_destroy(&vec);
}

static void
test_remove(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    uint32_t elements[3] = {0, 1, 2};
    struct vector vec = VECTOR_EMPTY_INITIALIZER(uint32_t);

    for (size_t i = 0; i < ARRAY_SIZE(elements); i++) {
        vector_push(&vec, &elements[i]);
    }
    ovs_assert(vector_capacity(&vec) == 4);
    ovs_assert(vector_len(&vec) == 3);

    uint32_t element;
    vector_pop(&vec, &element);
    ovs_assert(vector_len(&vec) == 2);
    ovs_assert(element == 2);

    ovs_assert(vector_remove(&vec, 0, &element));
    ovs_assert(vector_len(&vec) == 1);
    ovs_assert(element == 0);
    ovs_assert(vector_get(&vec, 0, uint32_t) == 1);

    size_t i = 0;
    uint32_t num;
    VECTOR_FOR_EACH (&vec, num) {
        ovs_assert(num == 1);
        i++;
    }
    ovs_assert(i == 1);

    ovs_assert(vector_remove(&vec, 0, &element));
    ovs_assert(vector_len(&vec) == 0);
    ovs_assert(element == 1);

    vector_destroy(&vec);
}

static void
test_out_of_bounds(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    uint32_t num = 1;
    struct vector vec = VECTOR_EMPTY_INITIALIZER(uint32_t);

    ovs_assert(!vector_insert(&vec, 1, &num));
    ovs_assert(vector_capacity(&vec) == 0);
    ovs_assert(vector_len(&vec) == 0);

    ovs_assert(!vector_remove(&vec, 0, &num));
    ovs_assert(vector_capacity(&vec) == 0);
    ovs_assert(vector_len(&vec) == 0);

    uint32_t *ptr;
    VECTOR_FOR_EACH_PTR (&vec, ptr) {
        OVS_NOT_REACHED();
    }

    VECTOR_FOR_EACH (&vec, num) {
        OVS_NOT_REACHED();
    }

    vector_destroy(&vec);
}

static void
test_shrink(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    uint32_t elements[3] = {0, 1, 2};
    struct vector vec = VECTOR_CAPACITY_INITIALIZER(uint32_t, 10);

    for (size_t i = 0; i < ARRAY_SIZE(elements); i++) {
        vector_push(&vec, &elements[i]);
    }

    ovs_assert(vector_capacity(&vec) == 10);
    ovs_assert(vector_len(&vec) == 3);

    vector_shrink_to_fit(&vec);
    ovs_assert(vector_capacity(&vec) == 3);
    ovs_assert(vector_len(&vec) == 3);

    uint32_t num;
    vector_pop(&vec, &num);
    ovs_assert(vector_capacity(&vec) == 3);
    ovs_assert(vector_len(&vec) == 2);

    vector_shrink_to_fit(&vec);
    ovs_assert(vector_capacity(&vec) == 2);
    ovs_assert(vector_len(&vec) == 2);

    vector_push(&vec, &num);
    ovs_assert(vector_capacity(&vec) == 4);
    ovs_assert(vector_len(&vec) == 3);

    vector_clear(&vec);
    ovs_assert(vector_capacity(&vec) == 4);
    ovs_assert(vector_len(&vec) == 0);

    vector_shrink_to_fit(&vec);
    ovs_assert(vector_capacity(&vec) == 0);
    ovs_assert(vector_len(&vec) == 0);

    vector_destroy(&vec);
}

static void
test_clone(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    uint32_t elements[3] = {0, 1, 2};
    struct vector vec = VECTOR_CAPACITY_INITIALIZER(uint32_t, 10);

    for (size_t i = 0; i < ARRAY_SIZE(elements); i++) {
        vector_push(&vec, &elements[i]);
    }

    struct vector clone = vector_clone(&vec);
    ovs_assert(vector_capacity(&vec) == 10);
    ovs_assert(vector_len(&vec) == 3);

    size_t i = 0;
    uint32_t num;
    VECTOR_FOR_EACH (&clone, num) {
        ovs_assert(elements[i++] == num);
    }
    ovs_assert(i == ARRAY_SIZE(elements));

    vector_shrink_to_fit(&clone);
    ovs_assert(vector_capacity(&clone) == 3);
    ovs_assert(vector_len(&clone) == 3);
    ovs_assert(vector_capacity(&vec) == 10);
    ovs_assert(vector_len(&vec) == 3);

    vector_destroy(&vec);
    vector_destroy(&clone);
}

static void
test_pointers(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    const char *elements[3] = {"a", "b", "c"};
    struct vector vec = VECTOR_EMPTY_INITIALIZER(const char *);

    for (size_t j = 0; j < ARRAY_SIZE(elements); j++) {
        vector_push(&vec, &elements[j]);
    }
    ovs_assert(vector_capacity(&vec) == 4);
    ovs_assert(vector_len(&vec) == 3);

    size_t i = 0;
    const char **ptr;
    VECTOR_FOR_EACH_PTR (&vec, ptr) {
        ovs_assert(!strcmp(elements[i++], *ptr));
    }
    ovs_assert(i == ARRAY_SIZE(elements));

    i = 0;
    const char *str;
    VECTOR_FOR_EACH (&vec, str) {
        ovs_assert(!strcmp(elements[i++], str));
    }
    ovs_assert(i == ARRAY_SIZE(elements));

    vector_destroy(&vec);

    vec = VECTOR_EMPTY_INITIALIZER(char *);
    for (size_t j = 0; j < ARRAY_SIZE(elements); j++) {
        char *dup = xstrdup(elements[j]);
        vector_push(&vec, &dup);
    }
    ovs_assert(vector_capacity(&vec) == 4);
    ovs_assert(vector_len(&vec) == 3);

    char *string;
    i = 0;
    VECTOR_FOR_EACH_POP (&vec, string) {
        free(string);
        i++;
    }
    ovs_assert(i == ARRAY_SIZE(elements));

    vector_destroy(&vec);
}

static void
test_vector_main(int argc OVS_UNUSED, char *argv[] OVS_UNUSED)
{
    ovn_set_program_name(argv[0]);
    static const struct ovs_cmdl_command commands[] = {
        {"add", NULL, 0, 0, test_add, OVS_RO},
        {"remove", NULL, 0, 0, test_remove, OVS_RO},
        {"out-of-bounds", NULL, 0, 0, test_out_of_bounds, OVS_RO},
        {"shrink", NULL, 0, 0, test_shrink, OVS_RO},
        {"clone", NULL, 0, 0, test_clone, OVS_RO},
        {"pointers", NULL, 0, 0, test_pointers, OVS_RO},
        {NULL, NULL, 0, 0, NULL, OVS_RO},
    };
    struct ovs_cmdl_context ctx;
    ctx.argc = argc - 1;
    ctx.argv = argv + 1;
    ovs_cmdl_run_command(&ctx, commands);
}

OVSTEST_REGISTER("test-vector", test_vector_main);
