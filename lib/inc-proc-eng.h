/*
 * Copyright (c) 2018 eBay Inc.
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

#ifndef INC_PROC_ENG_H
#define INC_PROC_ENG_H 1

/* The Incremental Processing Engine is a framework for incrementally
 * processing changes from different inputs. A example use case is
 * ovn-controller.  To compute desired states (e.g. openflow rules) based on
 * many inputs (e.g.  south-bound DB tables, local OVSDB interfaces, etc.), it
 * is straightforward to recompute everything when there is any change in any
 * inputs, but it is inefficient when the size of the input data becomes large.
 * Instead, tracking the changes and update the desired states based on what's
 * changed is more efficient and scalable. However, it is not straightforward
 * to implement the change-based processing when there are a big number of
 * inputs. In addition, what makes it more complicated is that intermediate
 * results needs to be computed, which needs to be reused in different part of
 * the processing and finally generates the final desired states. It is proved
 * to be difficult and error-prone to implement this kind of complex processing
 * by ad-hoc implementation.
 *
 * This framework is to provide a generic way to solve the above problem.
 * It does not understand the processing logic, but provides a unified way
 * to describe the inputs and dependencies clearly, with interfaces for
 * users to implement the processing logic for how to handle each input
 * changes.
 *
 * The engine is composed of engine_nodes. Each engine node maintains its own
 * data, which is persistent across main loop iterations. Each node has zero to
 * ENGINE_MAX_INPUT inputs, which creates a DAG (directed acyclic graph),
 * representing the dependencies between the nodes. Nodes without inputs
 * maintains the pure inputs, nodes without offsprings maintain the final
 * output, and nodes in the middle are the ones maintaining intermediate
 * results.
 *
 * For each input of each engine_node, there is a user-defined change_handler
 * to process changes of that input, and update the data (output) of the
 * engine_node accordingly. A change_handler usually needs to consider other
 * inputs of the same node during this process.
 *
 * The user can simply call the run() method of the leaf output engine node to
 * trigger the processing of the whole engine, and the processing will happen
 * in the order according to the dependencies defined and handle the changes
 * incrementally. When there are multiple leaf nodes, a dummy output node can
 * be created with the leaf nodes as inputs, as a triggering point of the
 * engine.
 *
 * While the more fine-grained dependencies and change-handlers are
 * implemented, the more efficient the processing will be, it is not
 * realistic to implement all change-processing for all inputs (and
 * intermediate results). The engine doesn't require change-handler to be
 * implemented for every input of every node. Users can choose to implement
 * the most important change-handlers (for the changes happens most
 * frequently) for overall performance. When there is no change_handler
 * defined for a certain input on a certain engine_node, the run() method
 * of the engine_node will be called to fall-back to a full recompute
 * against all its inputs.
 *
 * The Incremental Processing Engine intends to help the correctness and
 * maintainability, but to achieve the goals, it requires the user to follow
 * some guidelines.  Otherwise it is easy to be abused and results in bugs or
 * unmanageable code complexity.
 *
 * - Focus on data when designing the node dependency graph.
 *
 *   An engine node exists for the data it maintains, and the data is the pure
 *   outcome of the inputs of the node. It is analogous to materialized views
 *   of database tables, although the data structure is not limited to
 *   relations and can be any form. The operations on the data implemented by
 *   change-handlers and run() method are analogous to the SQL operations such
 *   as selection, projection and join.
 *
 *   There may be exceptions that some nodes may look data-less, e.g. a node
 *   that is responsible for syncing data from a NB table to a SB table (in
 *   ovn-northd). The node would have the NB table as input and a
 *   change-handler simply converts the input changes and updates directly to
 *   SB IDL. The node itself doesn't need to maintain any data. However,
 *   essentially, this is still a data-centric design, and the data of the
 *   node is maintained in the SB IDL itself. (A more clear separation may be
 *   maintaining a copy of the desired SB data in the memory, and then updating
 *   the SB IDL outside of the Incremental Processing Engine. However, this
 *   merely increases CPU and memory footprint without obvious benefit.)
 *
 * - Avoid global variables and always get input from the engine node's input.
 *
 *   The main purpose of the Incremental Processing Engine is to make the
 *   dependencies explicit and avoid missing handling any hidden dependencies,
 *   but there is no way in the framework to prevent the usage of global
 *   variables. It is the users responsibility to avoid global variables. If
 *   there is any data/states that is participated in the generation of the
 *   output, the data should come from the node's input (accessible through
 *   engine APIs) only.
 *
 *   The engine framework doesn't have a mechanism to prevent the use of global
 *   variables in handlers/run(), so it is the developer's responsibility to
 *   keep this in mind and avoid such use. If there are some very specific
 *   reasons a global variable is required (such as to greatly simply code
 *   complexity while the change of the global data is handled properly -
 *   e.g. ensured to trigger a full recompute), it should be clearly documented
 *   to call out and raise attention for future changes that could possibly
 *   break the correctness.
 *
 *   Note that the engine context may be easily abused to contain global data.
 *   The purpose of the engine context is to include critical information that
 *   is required through out the engine processing by handlers, and mostly it
 *   is just the IDL txn handle that is required for some handlers to write
 *   data to OVSDB and nothing else. It must be careful not adding any real
 *   data dependency to the engine context.
 *
 * - All input changes need to be handled.
 *
 *   If a node depends on an input, it means the input data participated in the
 *   generation of the node's data. If the input changes, it means potential
 *   change to the node's data. If it is not clear how to handle the change, or
 *   if the change is very rare and doesn't worth a complex change handler
 *   implementation, simply return false from the change-handler so that a
 *   recompute is triggered to ensure the correctness. Not handling an input
 *   change suggests that either the input shouldn't be in the dependency graph
 *   of the current node, or there is a potential bug.
 *
 *   However, we do see special cases that a no-op handler (meaning doing
 *   nothing) can be used to skip an input change handling. In such cases, it
 *   is usually that the node depends on several inputs and the input changes
 *   are correlated, and handling some of the input changes is sufficient to
 *   cover the changes of some other input. Take an example:
 *
 *   Node A depends on input B and C. If we know that C always changes with B,
 *   and handling B's change would have covered C's change, then it is ok to
 *   ignore C's input change.
 *
 *   However, in practice this should be very rare. It should be extremely
 *   cautious to use a no-op handler with careful documentation for the reason.
 */

#define ENGINE_MAX_INPUT 256
#define ENGINE_MAX_OVSDB_INDEX 256

#include <stdbool.h>
#include <stdint.h>

#include "compiler.h"

struct engine_context {
    struct ovsdb_idl_txn *ovs_idl_txn;
    struct ovsdb_idl_txn *ovnsb_idl_txn;
    struct ovsdb_idl_txn *ovnnb_idl_txn;

    void *client_ctx;
};

/* Arguments to be passed to the engine at engine_init(). */
struct engine_arg {
    struct ovsdb_idl *sb_idl;
    struct ovsdb_idl *nb_idl;
    struct ovsdb_idl *ovs_idl;
};

struct engine_node;

enum engine_node_state {
    EN_STALE,     /* Data in the node is not up to date with the DB. */
    EN_UPDATED,   /* Data in the node is valid but was updated during the
                   * last run.
                   */
    EN_UNCHANGED, /* Data in the node is valid and didn't change during the
                   * last run.
                   */
    EN_CANCELED,  /* During the last run, processing was canceled for
                   * this node.
                   */
    EN_STATE_MAX,
};

enum engine_input_handler_result {
    EN_UNHANDLED = -1,
    EN_HANDLED_UPDATED = EN_UPDATED,
    EN_HANDLED_UNCHANGED = EN_UNCHANGED,
};

struct engine_node_input {
    /* The input node. */
    struct engine_node *node;

    /* Change handler for changes of the input node. The changes may need to be
     * evaluated against all the other inputs. Returns:
     *  - EN_UNHANDLED: the change cannot be handled (indicating full
     *    recompute needed).
     *  - EN_HANDLED_UPDATED: the change can be handled, and the node's
     *    data was updated as a result.
     *  - EN_HANDLED_UNCHANGED: the change can be handled, and the node's
     *    data was left unchanged.
     * A change handler can also call engine_get_context() but it must make
     * sure the txn pointers returned by it are non-NULL. In case the change
     * handler needs to use the txn pointers returned by engine_get_context(),
     * and the pointers are NULL, the change handler MUST return EN_UNHANDLED.
     */
    enum engine_input_handler_result (*change_handler)
        (struct engine_node *node, void *data);
};

struct engine_stats {
    uint64_t recompute;
    uint64_t compute;
    uint64_t cancel;
};

struct engine_node {
    /* A unique name for each node. */
    char *name;

    /* Number of inputs of this node. */
    size_t n_inputs;

    /* Inputs of this node. */
    struct engine_node_input inputs[ENGINE_MAX_INPUT];

    /* A pointer to node internal data. The data is safely accessible to
     * users through the engine_get_data() API. For special cases, when the
     * data is known to be valid (e.g., at init time), users can also call
     * engine_get_internal_data().
     */
    void *data;

    /* State of the node after the last engine run. */
    enum engine_node_state state;

    /* Method to allocate and initialize node data. It may be NULL.
     * The user supplied argument 'arg' is passed from the call to
     * engine_init().
     */
    void *(*init)(struct engine_node *node, struct engine_arg *arg);

    /* Method to clean up data. It may be NULL. */
    void (*cleanup)(void *data);

    /* Fully processes all inputs of this node and regenerates the data
     * of this node. The pointer to the node's data is passed as argument.
     * 'run' handlers can also call engine_get_context() and the
     * implementation guarantees that the txn pointers returned
     * engine_get_context() are not NULL and valid.
     */
    enum engine_node_state (*run)(struct engine_node *node, void *data);

    /* Method to validate if the 'internal_data' is valid. This allows users
     * to customize when 'data' can be used (e.g., even if the node
     * hasn't been refreshed in the last iteration, if 'data'
     * doesn't store pointers to DB records it's still safe to use).
     */
    bool (*is_valid)(struct engine_node *);

    /* Method to clear up tracked data maintained by the engine node in the
     * engine 'data'. It may be NULL. */
    void (*clear_tracked_data)(void *tracked_data);

    /* Method used to dump info about node input compute failues. It may be
     * NULL.
     */
    void (*get_compute_failure_info)(struct engine_node *);

    /* Engine stats. */
    struct engine_stats stats;
};

/* Initialize the data for the engine nodes. It calls each node's
 * init() method if not NULL passing the user supplied 'arg'.
 * It should be called before the main loop. */
void engine_init(struct engine_node *node, struct engine_arg *arg);

/* Initialize the engine nodes for a new run. It should be called in the
 * main processing loop before every potential engine_run().
 */
void engine_init_run(void);

/* Execute the processing, which should be called in the main loop.
 * Updates the engine node's states accordingly. If 'recompute_allowed' is
 * false and a recompute is required by the current engine run then the engine
 * cancels its processing.
 */
void engine_run(bool recompute_allowed);

/* Clean up the data for the engine nodes. It calls each node's
 * cleanup() method if not NULL. It should be called before the program
 * terminates. */
void engine_cleanup(void);

/* Check if engine needs to run but didn't. */
bool engine_need_run(void);

/* Get the input node with <name> for <node> */
struct engine_node * engine_get_input(const char *input_name,
                                      struct engine_node *);

/* Get the data from the input node with <name> for <node> */
void *engine_get_input_data(const char *input_name, struct engine_node *);

/* Add an input (dependency) for <node>, with corresponding change_handler,
 * which can be NULL. If the change_handler is NULL, the engine will not
 * be able to process the change incrementally, and will fall back to call
 * the run method to recompute. */
void engine_add_input(struct engine_node *node, struct engine_node *input,
                      enum engine_input_handler_result (*change_handler)
                          (struct engine_node *, void *));
void engine_add_input_with_compute_debug(
        struct engine_node *node, struct engine_node *input,
        enum engine_input_handler_result (*change_handler)
            (struct engine_node *, void *),
        void (*get_compute_failure_info)(struct engine_node *));

/* Force the engine to recompute everything. It is used
 * in circumstances when we are not sure there is change or not, or
 * when there is change but the engine couldn't be executed in that
 * iteration, and the change can't be tracked across iterations. */
void engine_set_force_recompute(void);

/* Same as "engine_set_force_recompute()", but the poll_loop is woken up
 * immediately and the next engine run is not delayed. */
void engine_set_force_recompute_immediate(void);

/* Clear the force flag for the next run so the engine does the
 * usual processing without forced full recompute. */
void engine_clear_force_recompute(void);

/* Returns whether next engine_run() is forced to rempute. */
bool engine_get_force_recompute(void);

/* Return the current engine_context. The values in the context can be NULL
 * if the engine is run with allow_recompute == false in the current
 * iteration.
 * Therefore, it is the responsibility of the caller to check the context
 * values when called from change handlers.
 */
const struct engine_context *engine_get_context(void);

void engine_set_context(const struct engine_context *);

/* Return true if during the last iteration the node's data was updated. */
bool engine_node_changed(struct engine_node *node);

/* Return true if the engine has run in the last iteration. */
bool engine_has_run(void);

/* Return true if the engine has any update in any node, i.e. any input
 * has changed; false if nothing has changed. */
bool engine_has_updated(void);

/* Returns true if during the last engine run we had to cancel processing. */
bool engine_canceled(void);

/* Return a pointer to node data accessible for users outside the processing
 * engine. If the node data is not valid (e.g., last engine_run() failed or
 * didn't happen), the node's is_valid() method is used to determine if the
 * data can be safely accessed. If it's not the case, the function returns
 * NULL.
 * The content of the data should be changed only by the change_handlers
 * and run() function of the current node. Users should ensure that the
 * data is read-only in change-handlers of the nodes that depends on this
 * node.
 */
void *engine_get_data(struct engine_node *node);

/* Return a pointer to node data *without* performing any coherance checks on
 * the state of the node. This may be used only in specific cases when data
 * is guaranteed to be valid, e.g., immediately after initialization and
 * before the first engine_run().
 */
void *engine_get_internal_data(struct engine_node *node);

/* Trigger a full recompute. */
void engine_trigger_recompute(void);

struct ed_ovsdb_index {
    const char *name;
    struct ovsdb_idl_index *index;
};

struct ed_type_ovsdb_table {
    const void *table;
    size_t n_indexes;
    struct ed_ovsdb_index indexes[ENGINE_MAX_OVSDB_INDEX];
};

#define EN_OVSDB_GET(NODE) \
    (((struct ed_type_ovsdb_table *)(NODE)->data)->table)

struct ovsdb_idl_index * engine_ovsdb_node_get_index(struct engine_node *,
                                                     const char *name);

/* Any engine node can use this function for no-op handlers. */
static inline enum engine_input_handler_result
engine_noop_handler(struct engine_node *node OVS_UNUSED, void *data OVS_UNUSED)
{
    return EN_HANDLED_UNCHANGED;
}

/* Adds an OVSDB IDL index to the node. This should be called only after
 * engine_init() as the index is stored in the node data.
 */
void engine_ovsdb_node_add_index(struct engine_node *, const char *name,
                                 struct ovsdb_idl_index *);

#define ENGINE_NODE_DEF_START(NAME, NAME_STR) \
    struct engine_node en_##NAME = { \
        .name = NAME_STR, \
        .data = NULL, \
        .state = EN_STALE, \
        .init = en_##NAME##_init, \
        .run = en_##NAME##_run, \
        .cleanup = en_##NAME##_cleanup,

#define ENGINE_NODE_DEF_END };

#define ENGINE_NODE1(NAME) \
    ENGINE_NODE_DEF_START(NAME, #NAME) \
    ENGINE_NODE_DEF_END

#define CLEAR_TRACKED_DATA(NAME) \
    .clear_tracked_data = en_##NAME##_clear_tracked_data

#define IS_VALID(NAME) \
    .is_valid = en_##NAME##_is_valid

#define COMPUTE_FAIL_INFO(NAME) \
        .get_compute_failure_info = en_##NAME##_compute_failure_info,

#define ENGINE_NODE2(NAME, ARG1) \
    ENGINE_NODE_DEF_START(NAME, #NAME) \
    ARG1(NAME), \
    ENGINE_NODE_DEF_END

#define ENGINE_NODE3(NAME, ARG1, ARG2) \
    ENGINE_NODE_DEF_START(NAME, #NAME) \
    ARG1(NAME), \
    ARG2(NAME), \
    ENGINE_NODE_DEF_END

#define ENGINE_NODE(...) VFUNC(ENGINE_NODE, __VA_ARGS__)

/* Macro to define member functions of an engine node which represents
 * a table of OVSDB */
#define ENGINE_FUNC_OVSDB(DB_NAME, TBL_NAME) \
static enum engine_node_state \
en_##DB_NAME##_##TBL_NAME##_run(struct engine_node *node, \
                                void *data OVS_UNUSED) \
{ \
    const struct DB_NAME##rec_##TBL_NAME##_table *table = \
        EN_OVSDB_GET(node); \
    if (DB_NAME##rec_##TBL_NAME##_table_track_get_first(table)) { \
        return EN_UPDATED; \
    } \
    return EN_UNCHANGED; \
} \
static void *en_##DB_NAME##_##TBL_NAME##_init( \
    struct engine_node *node OVS_UNUSED, \
    struct engine_arg *arg) \
{ \
    struct ovsdb_idl *idl = arg->DB_NAME##_idl; \
    struct ed_type_ovsdb_table *data = xzalloc(sizeof *data); \
    data->table = DB_NAME##rec_##TBL_NAME##_table_get(idl); \
    return data; \
} \
static void en_##DB_NAME##_##TBL_NAME##_cleanup(void *data OVS_UNUSED) \
{ \
} \
static void \
en_##DB_NAME##_##TBL_NAME##_compute_failure_info(struct engine_node *node)  \
{                                                                           \
    if (!VLOG_IS_DBG_ENABLED()) {                                           \
        return;                                                             \
    }                                                                       \
    const struct ovsdb_idl *table = EN_OVSDB_GET(node);                     \
    const struct ovsdb_idl_row *row;                                        \
    struct ds s = DS_EMPTY_INITIALIZER;                                     \
    ds_put_format(&s, "Node \"%s\" compute failure info:\n", node->name);   \
    for ((row) = ovsdb_idl_track_get_first(table,                           \
                                           &DB_NAME##rec_table_##TBL_NAME); \
         (row); (row) = ovsdb_idl_track_get_next(row)) {                    \
        if (ovsdb_idl_row_get_seqno((row), OVSDB_IDL_CHANGE_INSERT) > 0) {  \
            ds_put_format(&s, "%s (New) "UUID_FMT"\n",                      \
                          #DB_NAME"_"#TBL_NAME, UUID_ARGS(&row->uuid));     \
        } else if (ovsdb_idl_row_get_seqno((row),                           \
                                           OVSDB_IDL_CHANGE_DELETE) > 0) {  \
            ds_put_format(&s, "%s (Deleted) "UUID_FMT"\n",                  \
                          #DB_NAME"_"#TBL_NAME, UUID_ARGS(&row->uuid));     \
        } else {                                                            \
            ds_put_format(&s, "%s (Updated) "UUID_FMT" columns:",           \
                          #DB_NAME"_"#TBL_NAME, UUID_ARGS(&row->uuid));     \
            for (size_t i = 0; i < row->table->class_->n_columns; i++) {    \
                struct ovsdb_idl_column *col =                              \
                    &DB_NAME##rec_##TBL_NAME##_columns[i];                  \
                if (ovsdb_idl_track_is_updated(row, col)) {                 \
                    ds_put_format(&s, " %s,", col->name);                   \
                }                                                           \
            }                                                               \
            ds_chomp(&s, ',');                                              \
        }                                                                   \
    }                                                                       \
    VLOG_DBG("%s", ds_cstr(&s));                                            \
    ds_destroy(&s);                                                         \
}

/* Macro to define member functions of an engine node which represents
 * a table of OVN SB DB */
#define ENGINE_FUNC_SB(TBL_NAME) \
    ENGINE_FUNC_OVSDB(sb, TBL_NAME)

/* Macro to define member functions of an engine node which represents
 * a table of OVN NB DB */
#define ENGINE_FUNC_NB(TBL_NAME) \
    ENGINE_FUNC_OVSDB(nb, TBL_NAME)

/* Macro to define member functions of an engine node which represents
 * a table of open_vswitch DB */
#define ENGINE_FUNC_OVS(TBL_NAME) \
    ENGINE_FUNC_OVSDB(ovs, TBL_NAME)

/* Macro to define an engine node which represents a table of OVSDB */
#define ENGINE_NODE_OVSDB(DB_NAME, DB_NAME_STR, TBL_NAME, TBL_NAME_STR) \
    ENGINE_NODE_DEF_START(DB_NAME##_##TBL_NAME, DB_NAME_STR"_"TBL_NAME_STR) \
    COMPUTE_FAIL_INFO(DB_NAME##_##TBL_NAME) \
    ENGINE_NODE_DEF_END

/* Macro to define an engine node which represents a table of OVN SB DB */
#define ENGINE_NODE_SB(TBL_NAME) \
    ENGINE_NODE_OVSDB(sb, "SB", TBL_NAME, #TBL_NAME);

/* Macro to define an engine node which represents a table of OVN NB DB */
#define ENGINE_NODE_NB(TBL_NAME) \
    ENGINE_NODE_OVSDB(nb, "NB", TBL_NAME, #TBL_NAME);

/* Macro to define an engine node which represents a table of open_vswitch
 * DB */
#define ENGINE_NODE_OVS(TBL_NAME) \
    ENGINE_NODE_OVSDB(ovs, "OVS", TBL_NAME, #TBL_NAME);

#endif /* lib/inc-proc-eng.h */
