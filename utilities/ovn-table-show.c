/* Licensed under the Apache License, Version 2.0 (the "License");
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

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db-ctl-base.h"
#include "hmapx.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/json.h"
#include "ovsdb-data.h"
#include "ovsdb-idl.h"
#include "ovsdb-idl-provider.h"
#include "sset.h"
#include "table.h"
#include "util.h"

#include "ovn-table-show.h"

/* Record identifiers configured by the client for ctl_get_row(). */
static const struct ctl_table_class *ctl_tables;

/* Formatting options are owned by the client's command loop. */
static const struct table_style *output_style;

/* Returns a table by its exact schema name, without CLI abbreviation. */
static const struct ovsdb_idl_table_class *
show_table_by_name(const struct ovsdb_idl_class *class, const char *name)
{
    for (size_t i = 0; i < class->n_tables; i++) {
        if (!strcmp(class->tables[i].name, name)) {
            return &class->tables[i];
        }
    }
    return NULL;
}

/* Matches the generic ctl commands' case-insensitive, hyphen-insensitive
 * prefixes, preferring exact matches over normalized matches and prefixes. */
static unsigned int
show_table_match(const char *name, const char *input)
{
    if (!strcmp(name, input)) {
        return UINT_MAX;
    }
    unsigned int score = 0;
    while (*input) {
        unsigned char a = *name;
        unsigned char b = *input;
        if ((a == '-' ? '_' : tolower(a))
            != (b == '-' ? '_' : tolower(b))) {
            return 0;
        }
        name++;
        input++;
        score++;
    }
    return *name ? score : UINT_MAX - 1;
}

/* Resolves a CLI table name and reports ambiguity through the ctl context. */
static const struct ovsdb_idl_table_class *
show_get_table(struct ctl_context *ctx)
{
    const struct ovsdb_idl_class *class = ovsdb_idl_get_class(ctx->idl);
    const struct ovsdb_idl_table_class *best = NULL;
    unsigned int best_score = 0;
    struct sset matches = SSET_INITIALIZER(&matches);

    for (size_t i = 0; i < class->n_tables; i++) {
        const struct ovsdb_idl_table_class *table = &class->tables[i];
        unsigned int score = show_table_match(table->name, ctx->argv[1]);
        if (score && score >= best_score) {
            if (score > best_score) {
                sset_clear(&matches);
            }
            best_score = score;
            best = table;
            sset_add(&matches, table->name);
        }
    }
    if (!best) {
        ctl_error(ctx, "unknown table \"%s\"", ctx->argv[1]);
    } else if (sset_count(&matches) > 1) {
        char *names = sset_join(&matches, ", ", "");
        ctl_error(ctx, "\"%s\" matches multiple table names: %s",
                  ctx->argv[1], names);
        free(names);
        best = NULL;
    }
    sset_destroy(&matches);
    return best;
}

/* Returns only a scalar or optional string column named "name". */
static const struct ovsdb_idl_column *
show_name_column(const struct ovsdb_idl_table_class *table)
{
    for (size_t i = 0; i < table->n_columns; i++) {
        const struct ovsdb_idl_column *column = &table->columns[i];
        if (!strcmp(column->name, "name")
            && column->type.key.type == OVSDB_TYPE_STRING
            && column->type.value.type == OVSDB_TYPE_VOID
            && column->type.n_max == 1) {
            return column;
        }
    }
    return NULL;
}

/* Only schema-declared UUID references are traversed. */
static const struct ovsdb_idl_table_class *
show_ref_table(const struct ovsdb_idl_class *class,
               const struct ovsdb_base_type *base)
{
    return ovsdb_base_type_is_ref(base)
           ? show_table_by_name(class, base->uuid.refTableName) : NULL;
}

/* Monitors all root columns, or the complete forward schema closure for
 * --all.  Schema cycles are visited once, independently of row cycles. */
static void
pre_show_table(struct ctl_context *ctx)
{
    if (output_style->format != TF_LIST
        || output_style->cell_format != CF_STRING
        || !output_style->headings) {
        ctl_error(ctx, "show-table requires list format, string data, "
                  "and headings");
        return;
    }

    const struct ovsdb_idl_table_class *root = show_get_table(ctx);
    if (!root) {
        return;
    }
    const struct ovsdb_idl_class *class = ovsdb_idl_get_class(ctx->idl);
    const struct ctl_table_class *ctl = &ctl_tables[root - class->tables];
    for (size_t i = 0; i < ARRAY_SIZE(ctl->row_ids); i++) {
        const struct ctl_row_id *id = &ctl->row_ids[i];
        if (id->name_column) {
            ovsdb_idl_add_column(ctx->idl, id->name_column);
        }
        if (id->uuid_column) {
            ovsdb_idl_add_column(ctx->idl, id->uuid_column);
        }
    }

    bool all = shash_find(&ctx->options, "--all") != NULL;
    const struct ovsdb_idl_table_class **pending =
        xmalloc(class->n_tables * sizeof *pending);
    struct sset visited = SSET_INITIALIZER(&visited);
    size_t n_pending = 1;
    pending[0] = root;
    sset_add(&visited, root->name);
    for (size_t n = 0; n < n_pending; n++) {
        const struct ovsdb_idl_table_class *table = pending[n];
        ovsdb_idl_add_table(ctx->idl, table);
        for (size_t i = 0; i < table->n_columns; i++) {
            const struct ovsdb_idl_column *column = &table->columns[i];
            ovsdb_idl_add_column(ctx->idl, column);
            const struct ovsdb_base_type *bases[] = {
                &column->type.key, &column->type.value,
            };
            for (size_t j = 0; j < ARRAY_SIZE(bases); j++) {
                const struct ovsdb_idl_table_class *ref =
                    show_ref_table(class, bases[j]);
                if (!ref) {
                    continue;
                }
                if (all) {
                    if (sset_add(&visited, ref->name)) {
                        pending[n_pending++] = ref;
                    }
                } else {
                    ovsdb_idl_add_table(ctx->idl, ref);
                    const struct ovsdb_idl_column *name =
                        show_name_column(ref);
                    if (name) {
                        ovsdb_idl_add_column(ctx->idl, name);
                    }
                }
            }
        }
    }
    free(pending);
    sset_destroy(&visited);
}

/* Formats an atom without changing the borrowed datum or its UUID type. */
static void
show_atom(struct ctl_context *ctx, const union ovsdb_atom *atom,
          const struct ovsdb_base_type *base, bool all)
{
    const struct ovsdb_idl_table_class *ref =
        show_ref_table(ovsdb_idl_get_class(ctx->idl), base);
    if (!all && ref) {
        const struct ovsdb_idl_column *name = show_name_column(ref);
        const struct ovsdb_idl_row *row =
            ovsdb_idl_get_row_for_uuid(ctx->idl, ref, &atom->uuid);
        if (row && name) {
            const struct ovsdb_datum *datum = ovsdb_idl_read(row, name);
            if (datum->n && json_string(datum->keys[0].s)[0]) {
                ovsdb_atom_to_string(&datum->keys[0], OVSDB_TYPE_STRING,
                                     &ctx->output);
                return;
            }
        }
    }
    ovsdb_atom_to_string(atom, base->type, &ctx->output);
}

/* Preserves set delimiters and map associations when rendering references. */
static void
show_datum(struct ctl_context *ctx, const struct ovsdb_datum *datum,
           const struct ovsdb_type *type, bool all)
{
    if (all || (!ovsdb_base_type_is_ref(&type->key)
                && !ovsdb_base_type_is_ref(&type->value))) {
        ovsdb_datum_to_string(datum, type, &ctx->output);
        return;
    }
    bool map = ovsdb_type_is_map(type);
    bool delimit = type->n_max > 1 || !datum->n;
    if (delimit) {
        ds_put_char(&ctx->output, map ? '{' : '[');
    }
    for (size_t i = 0; i < datum->n; i++) {
        if (i) {
            ds_put_cstr(&ctx->output, ", ");
        }
        show_atom(ctx, &datum->keys[i], &type->key, all);
        if (map) {
            ds_put_char(&ctx->output, '=');
            show_atom(ctx, &datum->values[i], &type->value, all);
        }
    }
    if (delimit) {
        ds_put_char(&ctx->output, map ? '}' : ']');
    }
}

/* A heap-allocated traversal stack avoids C stack limits on deep graphs. */
struct show_frame {
    struct show_frame *parent;             /* Previous active row. */
    const struct ovsdb_idl_row *row;       /* Row being displayed. */
    const struct ovsdb_idl_table_class *table; /* Schema table. */
    size_t column;                        /* Next column to display. */
    size_t atom;                          /* Next reference atom to visit. */
    size_t indent;                        /* Number of indentation spaces. */
    bool peer;                            /* A child has been displayed. */
};

/* Starts a row with its UUID, matching the list command's field alignment. */
static struct show_frame *
show_push(struct ctl_context *ctx, const struct ovsdb_idl_row *row,
          const struct ovsdb_idl_table_class *table, struct show_frame *parent,
          struct hmapx *active)
{
    struct show_frame *frame = xzalloc(sizeof *frame);
    frame->parent = parent;
    frame->row = row;
    frame->table = table;
    frame->indent = parent ? parent->indent + 4 : 0;
    hmapx_add(active, CONST_CAST(struct ovsdb_idl_row *, row));
    ds_put_char_multiple(&ctx->output, ' ', frame->indent);
    ds_put_format(&ctx->output, "%-20s: "UUID_FMT"\n", "_uuid",
                  UUID_ARGS(&row->uuid));
    return frame;
}

/* Prints each reference occurrence, stopping expansion only for rows on the
 * active path.  Shared children can appear under multiple parents. */
static void
cmd_show_table(struct ctl_context *ctx)
{
    const struct ovsdb_idl_table_class *table = show_get_table(ctx);
    if (!table) {
        return;
    }
    const struct ovsdb_idl_row *row;
    ctx->error = ctl_get_row(ctx, table, ctx->argv[2], true, &row);
    if (ctx->error) {
        return;
    }

    bool all = shash_find(&ctx->options, "--all") != NULL;
    const struct ovsdb_idl_class *class = ovsdb_idl_get_class(ctx->idl);
    struct hmapx active = HMAPX_INITIALIZER(&active);
    struct show_frame *frame = show_push(ctx, row, table, NULL, &active);
    while (frame) {
        if (all && frame->column) {
            const struct ovsdb_idl_column *column =
                &frame->table->columns[frame->column - 1];
            const struct ovsdb_datum *datum =
                ovsdb_idl_read(frame->row, column);
            const struct ovsdb_idl_row *child = NULL;
            const struct ovsdb_idl_table_class *ref = NULL;
            while (frame->atom / 2 < datum->n) {
                size_t i = frame->atom / 2;
                bool value = frame->atom++ % 2;
                ref = show_ref_table(class, value ? &column->type.value
                                                 : &column->type.key);
                if (ref) {
                    const union ovsdb_atom *atom = value ? &datum->values[i]
                                                        : &datum->keys[i];
                    child = ovsdb_idl_get_row_for_uuid(ctx->idl, ref,
                                                       &atom->uuid);
                    if (child && !hmapx_contains(&active, child)) {
                        break;
                    }
                    child = NULL;
                }
            }
            if (child) {
                ds_put_char(&ctx->output, '\n');
                if (frame->peer) {
                    ds_put_char_multiple(&ctx->output, ' ', frame->indent + 4);
                    ds_put_cstr(&ctx->output, "----\n\n");
                }
                frame->peer = true;
                frame = show_push(ctx, child, ref, frame, &active);
                continue;
            }
        }
        if (frame->column < frame->table->n_columns) {
            const struct ovsdb_idl_column *column =
                &frame->table->columns[frame->column++];
            ds_put_char_multiple(&ctx->output, ' ', frame->indent);
            ds_put_format(&ctx->output, "%-20s: ", column->name);
            show_datum(ctx, ovsdb_idl_read(frame->row, column),
                       &column->type, all);
            ds_put_char(&ctx->output, '\n');
            frame->atom = 0;
            frame->peer = false;
        } else {
            struct show_frame *parent = frame->parent;
            hmapx_find_and_delete_assert(&active, frame->row);
            free(frame);
            frame = parent;
        }
    }
    hmapx_destroy(&active);
}

/* Registers the same read-only command in each OVN ctl client. */
void
ovn_table_show_register(const struct ctl_table_class *tables,
                        const struct table_style *style)
{
    static const struct ctl_command_syntax commands[] = {
        {"show-table", 2, 2, "TABLE RECORD", pre_show_table, cmd_show_table,
         NULL, "--all", RO},
        {NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, RO},
    };
    ctl_tables = tables;
    output_style = style;
    ctl_register_commands(commands);
}

/* Documents the shared command without altering the OVS generic help. */
void
ovn_table_show_usage(void)
{
    puts("\nDatabase inspection commands:\n"
         "  [--all] show-table TABLE RECORD  show a row and its references\n");
}
