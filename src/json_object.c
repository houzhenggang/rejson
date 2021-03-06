/*
* Copyright (C) 2016 Redis Labs
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU Affero General Public License as
* published by the Free Software Foundation, either version 3 of the
* License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Affero General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "json_object.h"

/* === Parser === */
/* A custom context for the JSON lexer. */
typedef struct {
    jsonsl_error_t err;  // lexer error
    size_t errpos;       // error position
    Node **nodes;        // stack of created nodes
    int nlen;            // size of node stack
} JsonObjectContext;

#define _pushNode(ctx, n) ctx->nodes[ctx->nlen++] = n
#define _popNode(ctx) ctx->nodes[--ctx->nlen]

/* Decalre it. */
static int _AllowedEscapes[];
static int _IsAllowedWhitespace(unsigned c);

inline static int errorCallback(jsonsl_t jsn, jsonsl_error_t err, struct jsonsl_state_st *state, char *errat) {
    JsonObjectContext *joctx = (JsonObjectContext *)jsn->data;

    joctx->err = err;
    joctx->errpos = state->pos_cur;
    jsonsl_stop(jsn);
    return 0;
}

inline static void pushCallback(jsonsl_t jsn, jsonsl_action_t action, struct jsonsl_state_st *state,
                  const jsonsl_char_t *at) {
    JsonObjectContext *joctx = (JsonObjectContext *)jsn->data;

    // only objects (dictionaries) and lists (arrays) create a container on push
    switch (state->type) {
        case JSONSL_T_OBJECT:
            _pushNode(joctx, NewDictNode(1));
            break;
        case JSONSL_T_LIST:
            _pushNode(joctx, NewArrayNode(1));
            break;
        default:
            break;
    }
}

inline static void popCallback(jsonsl_t jsn, jsonsl_action_t action, struct jsonsl_state_st *state,
                 const jsonsl_char_t *at) {
    JsonObjectContext *joctx = (JsonObjectContext *)jsn->data;
    const char *pos = jsn->base + state->pos_begin;  // element starting position
    size_t len = state->pos_cur - state->pos_begin;  // element length

    // popping string and key values means addingg them to the node stack
    if (JSONSL_T_STRING == state->type || JSONSL_T_HKEY == state->type) {
        char *buffer = NULL;  // a temporary buffer for unescaped strings

        // ignore the quote marks
        pos++;
        len--;

        // deal with escapes
        if (state->nescapes) {
            jsonsl_error_t err;
            size_t newlen;

            buffer = calloc(len, sizeof(char));
            newlen = jsonsl_util_unescape(pos, buffer, len, _AllowedEscapes, &err);
            if (!newlen) {
                free(buffer);
                errorCallback(jsn, err, state, NULL);
                return;
            }

            pos = buffer;
            len = newlen;
        }

        // push it
        Node *n;
        if (JSONSL_T_STRING == state->type) n = NewStringNode(pos, len);
        else n = NewKeyValNode(pos, len, NULL);  // NULL is a placeholder for now
        _pushNode(joctx, n);

        if (buffer) free(buffer);
    }

    // popped special values are also added to the node stack
    if (JSONSL_T_SPECIAL == state->type) {
        if (state->special_flags & JSONSL_SPECIALf_NUMERIC) {
            if (state->special_flags & (JSONSL_SPECIALf_FLOAT | JSONSL_SPECIALf_EXPONENT)) {
                // convert to double
                double value;
                char *eptr;

                errno = 0;
                value = strtod (pos, &eptr);
                // in lieu of "ERR value is not a double or out of range"
                if ((errno == ERANGE && (value == HUGE_VAL || value == -HUGE_VAL)) || 
                    (errno != 0 && value == 0) ||
                    isnan(value) ||
                    (eptr != pos + len)) {
                        errorCallback(jsn, JSONSL_ERROR_INVALID_NUMBER, state, NULL);
                        return;
                }
                _pushNode(joctx, NewDoubleNode(value));
            } else {
                // convert long long (int64_t)
                long long value;
                char *eptr;

                errno = 0;
                value = strtoll(pos, &eptr, 10);
                // in lieu of "ERR value is not an integer or out of range"
                if ((errno == ERANGE && (value == LLONG_MAX || value == LLONG_MIN)) ||
                    (errno != 0 && value == 0) ||
                    (eptr != pos + len)) {
                        errorCallback(jsn, JSONSL_ERROR_INVALID_NUMBER, state, NULL);
                        return;
                }

                _pushNode(joctx, NewIntNode((int64_t)value));
            }
        } else if (state->special_flags & JSONSL_SPECIALf_BOOLEAN) {
            _pushNode(joctx, NewBoolNode(state->special_flags & JSONSL_SPECIALf_TRUE));
        } else if (state->special_flags & JSONSL_SPECIALf_NULL) {
            _pushNode(joctx, NULL);
        }
    }

    // anything that pops needs to be set in its parent, except the root element and keys
    if (joctx->nlen > 1 && state->type != JSONSL_T_HKEY) {
        NodeType p = joctx->nodes[joctx->nlen - 2]->type;
        switch (p) {
            case N_DICT:
                Node_DictSetKeyVal(joctx->nodes[joctx->nlen - 1], _popNode(joctx));
                break;
            case N_ARRAY:
                Node_ArrayAppend(joctx->nodes[joctx->nlen - 1], _popNode(joctx));
                break;
            case N_KEYVAL:
                joctx->nodes[joctx->nlen - 2]->value.kvval.val = _popNode(joctx);
                Node_DictSetKeyVal(joctx->nodes[joctx->nlen - 1], _popNode(joctx));
                break;
            default:
                break;
        }
    }
}

int CreateNodeFromJSON(const char *buf, size_t len, Node **node, char **err) {
    int levels = JSONSL_MAX_LEVELS;  // TODO: heur levels from len since we're not really streaming?

    size_t _off = 0, _len = len;
    char *_buf = (char *)buf;
    int is_scalar = 0;

    // munch any leading whitespaces
    while (_IsAllowedWhitespace(_buf[_off]) && _off < _len) _off++;

    /* Embed scalars in a list (also avoids JSONSL_ERROR_STRING_OUTSIDE_CONTAINER).
     * Copying is necc. evil to avoid messing w/ non-standard string implementations (e.g. sds), but
     * forgivable because most scalars are supposed to be short-ish.
    */
    if ((is_scalar = ('{' != _buf[_off]) && ('[' != _buf[_off]) && _off < _len)) {
        _len = _len - _off + 2;
        _buf = malloc(_len * sizeof(char));
        _buf[0] = '[';
        _buf[_len - 1] = ']';
        memcpy(&_buf[1], &buf[_off], len - _off);
    }

    /* The lexer. */
    jsonsl_t jsn = jsonsl_new(levels);
    jsn->error_callback = errorCallback;
    jsn->action_callback_POP = popCallback;
    jsn->action_callback_PUSH = pushCallback;
    jsonsl_enable_all_callbacks(jsn);

    /* Set up our custom context. */
    JsonObjectContext *joctx = calloc(1, sizeof(JsonObjectContext));
    joctx->nodes = calloc(levels, sizeof(Node *));
    jsn->data = joctx;

    /* Feed the lexer. */
    jsonsl_feed(jsn, _buf, _len);

    /* Check for lexer errors. */
    sds serr = sdsempty();
    if (JSONSL_ERROR_SUCCESS != joctx->err) {
        serr = sdscatprintf(serr, "ERR JSON lexer error %s at position %zd",
                            jsonsl_strerror(joctx->err), joctx->errpos + 1);
        goto error;
    }

    /* Verify that parsing had ended at level 0. */
    if (jsn->level) {
        serr = sdscatprintf(serr, "ERR JSON value incomplete - %u containers unterminated",
                            jsn->level);
        goto error;
    }

    /* Verify that an element. */
    if (!jsn->stack[0].nelem) {
        serr = sdscatprintf(serr, "ERR JSON value not found");
        goto error;
    }

    /* Finalize. */
    if (is_scalar) {
        // extract the scalar and discard the wrapper array
        Node_ArrayItem(joctx->nodes[0], 0, node);
        Node_ArraySet(joctx->nodes[0], 0, NULL);
        Node_Free(_popNode(joctx));
        free(_buf);
    } else {
        *node = _popNode(joctx);
    }

    sdsfree(serr);
    free(joctx->nodes);
    free(joctx);
    jsonsl_destroy(jsn);

    return JSONOBJECT_OK;

error:
    // set error string, if one has been passed
    if (err) {
        *err = strdup(serr);
    }

    // free any nodes that are in the stack
    while (joctx->nlen) Node_Free(_popNode(joctx));

    sdsfree(serr);
    free(joctx->nodes);
    free(joctx);
    jsonsl_destroy(jsn);

    return JSONOBJECT_ERROR;
}

/* === JSON serializer === */

typedef struct {
    sds buf;         // serialization buffer
    int depth;       // current tree depth
    int indent;      // indentation string length
    sds indentstr;   // indentaion string
    sds newlinestr;  // newline string
    sds spacestr;    // space string
    sds delimstr;    // delimiter string
} _JSONBuilderContext;

#define _JSONSerialize_Indent(b) \
    if (b->indent)               \
        for (int i = 0; i < b->depth; i++) b->buf = sdscatsds(b->buf, b->indentstr);

inline static void _JSONSerialize_StringValue(Node *n, void *ctx) {
    _JSONBuilderContext *b = (_JSONBuilderContext *)ctx;
    size_t len = n->value.strval.len;
    const char *p = n->value.strval.data;

    b->buf = sdsMakeRoomFor(b->buf, len + 2);  // we'll need at least as much room as the original
    b->buf = sdscatlen(b->buf, "\"", 1);
    while (len--) {
        switch (*p) {
            case '"':   // quotation mark
            case '\\':  // reverse solidus
                b->buf = sdscatprintf(b->buf, "\\%c", *p);
                break;
            case '/':  // the standard is clear wrt solidus so we're zealous
                b->buf = sdscatlen(b->buf, "\\/", 2);
                break;
            case '\b':  // backspace
                b->buf = sdscatlen(b->buf, "\\b", 2);
                break;
            case '\f':  // formfeed
                b->buf = sdscatlen(b->buf, "\\f", 2);
                break;
            case '\n':  // newline
                b->buf = sdscatlen(b->buf, "\\n", 2);
                break;
            case '\r':  // carriage return
                b->buf = sdscatlen(b->buf, "\\r", 2);
                break;
            case '\t':  // horizontal tab
                b->buf = sdscatlen(b->buf, "\\t", 2);
                break;
            default:
                if ((unsigned char)*p > 31 && isprint(*p))
                    b->buf = sdscatprintf(b->buf, "%c", *p);
                else
                    b->buf = sdscatprintf(b->buf, "\\u%04x", (unsigned char)*p);
                break;
        }
        p++;
    }

    b->buf = sdscatlen(b->buf, "\"", 1);
}

inline static void _JSONSerialize_BeginValue(Node *n, void *ctx) {
    _JSONBuilderContext *b = (_JSONBuilderContext *)ctx;

    if (!n) {  // NULL nodes are literal nulls
        b->buf = sdscatlen(b->buf, "null", 4);
    } else {
        switch (n->type) {
            case N_BOOLEAN:
                if (n->value.boolval) {
                    b->buf = sdscatlen(b->buf, "true", 4);
                } else {
                    b->buf = sdscatlen(b->buf, "false", 5);
                }
                break;
            case N_INTEGER:
                b->buf = sdscatfmt(b->buf, "%I", n->value.intval);
                break;
            case N_NUMBER:
                if (fabs(floor(n->value.numval) - n->value.numval) <= DBL_EPSILON &&
                    fabs(n->value.numval) < 1.0e60)
                    b->buf = sdscatprintf(b->buf, "%.0f", n->value.numval);
                else if (fabs(n->value.numval) < 1.0e-6 || fabs(n->value.numval) > 1.0e9)
                    b->buf = sdscatprintf(b->buf, "%e", n->value.numval);
                else
                    b->buf = sdscatprintf(b->buf, "%.17g", n->value.numval);
                break;
            case N_STRING:
                _JSONSerialize_StringValue(n, b);
                break;
            case N_KEYVAL:
                b->buf = sdscatfmt(b->buf, "\"%s\":%s", n->value.kvval.key, b->spacestr);
                break;
            case N_DICT:
                b->buf = sdscatlen(b->buf, "{", 1);
                b->depth++;
                if (n->value.dictval.len) {
                    b->buf = sdscatsds(b->buf, b->newlinestr);
                    _JSONSerialize_Indent(b);
                }
                break;
            case N_ARRAY:
                b->buf = sdscatlen(b->buf, "[", 1);
                b->depth++;
                if (n->value.arrval.len) {
                    b->buf = sdscatsds(b->buf, b->newlinestr);
                    _JSONSerialize_Indent(b);
                }
                break;
            case N_NULL:  // keeps the compiler from complaining
                break;
        }  // switch(n->type)
    }
}

inline static void _JSONSerialize_EndValue(Node *n, void *ctx) {
    _JSONBuilderContext *b = (_JSONBuilderContext *)ctx;
    if (n) {
        switch (n->type) {
            case N_DICT:
                if (n->value.dictval.len) {
                    b->buf = sdscatsds(b->buf, b->newlinestr);
                }
                b->depth--;
                _JSONSerialize_Indent(b);
                b->buf = sdscatlen(b->buf, "}", 1);
                break;
            case N_ARRAY:
                if (n->value.arrval.len) {
                    b->buf = sdscatsds(b->buf, b->newlinestr);
                }
                b->depth--;
                _JSONSerialize_Indent(b);
                b->buf = sdscatlen(b->buf, "]", 1);
                break;
            default:  // keeps the compiler from complaining
                break;
        }
    }
}

inline static void _JSONSerialize_ContainerDelimiter(void *ctx) {
    _JSONBuilderContext *b = (_JSONBuilderContext *)ctx;
    b->buf = sdscat(b->buf, b->delimstr);
    _JSONSerialize_Indent(b);
}

void SerializeNodeToJSON(const Node *node, const JSONSerializeOpt *opt, sds *json) {
    int levels = JSONSL_MAX_LEVELS;

    // set up the builder
    _JSONBuilderContext *b = calloc(1, sizeof(_JSONBuilderContext));
    b->indentstr = opt->indentstr ? sdsnew(opt->indentstr) : sdsempty();
    b->newlinestr = opt->newlinestr ? sdsnew(opt->newlinestr) : sdsempty();
    b->spacestr = opt->spacestr ? sdsnew(opt->spacestr) : sdsempty();
    b->indent = sdslen(b->indentstr);
    b->delimstr = sdsnewlen(",", 1);
    b->delimstr = sdscat(b->delimstr, b->newlinestr);

    NodeSerializerOpt nso = {.fBegin = _JSONSerialize_BeginValue,
                             .xBegin = 0xffff,
                             .fEnd = _JSONSerialize_EndValue,
                             .xEnd = (N_DICT | N_ARRAY),
                             .fDelim = _JSONSerialize_ContainerDelimiter,
                             .xDelim = (N_DICT | N_ARRAY)};

    // the real work
    b->buf = *json;
    Node_Serializer(node, &nso, b);
    *json = b->buf;

    sdsfree(b->indentstr);
    sdsfree(b->newlinestr);
    sdsfree(b->spacestr);
    sdsfree(b->delimstr);
    free(b);
}

// clang-format off
// from jsonsl.c

/**
 * This table contains entries for the allowed whitespace as per RFC 4627
 */
static int _AllowedWhitespace[0x100] = {
    /* 0x00 */ 0,0,0,0,0,0,0,0,0,                                               /* 0x08 */
    /* 0x09 */ 1 /* <TAB> */,                                                   /* 0x09 */
    /* 0x0a */ 1 /* <LF> */,                                                    /* 0x0a */
    /* 0x0b */ 0,0,                                                             /* 0x0c */
    /* 0x0d */ 1 /* <CR> */,                                                    /* 0x0d */
    /* 0x0e */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                             /* 0x1f */
    /* 0x20 */ 1 /* <SP> */,                                                    /* 0x20 */
    /* 0x21 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x40 */
    /* 0x41 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x60 */
    /* 0x61 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x80 */
    /* 0x81 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xa0 */
    /* 0xa1 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xc0 */
    /* 0xc1 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xe0 */
    /* 0xe1 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0      /* 0xfe */
};

// adapted for use with jsonsl_util_unescape_ex
/**
 * Allowable two-character 'common' escapes:
 */
static int _AllowedEscapes[0x80] = {
        /* 0x00 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x1f */
        /* 0x20 */ 0,0,                                                             /* 0x21 */
        /* 0x22 */ 1 /* <"> */,                                                     /* 0x22 */
        /* 0x23 */ 0,0,0,0,0,0,0,0,0,0,0,0,                                         /* 0x2e */
        /* 0x2f */ 1 /* </> */,                                                     /* 0x2f */
        /* 0x30 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x4f */
        /* 0x50 */ 0,0,0,0,0,0,0,0,0,0,0,0,                                         /* 0x5b */
        /* 0x5c */ 1 /* <\> */,                                                     /* 0x5c */
        /* 0x5d */ 0,0,0,0,0,                                                       /* 0x61 */
        /* 0x62 */ 1 /* <b> */,                                                     /* 0x62 */
        /* 0x63 */ 0,0,0,                                                           /* 0x65 */
        /* 0x66 */ 1 /* <f> */,                                                     /* 0x66 */
        /* 0x67 */ 0,0,0,0,0,0,0,                                                   /* 0x6d */
        /* 0x6e */ 1 /* <n> */,                                                     /* 0x6e */
        /* 0x6f */ 0,0,0,                                                           /* 0x71 */
        /* 0x72 */ 1 /* <r> */,                                                     /* 0x72 */
        /* 0x73 */ 0,                                                               /* 0x73 */
        /* 0x74 */ 1 /* <t> */,                                                     /* 0x74 */
        /* 0x75 */ 1 /* <u> */,                                                     /* 0x75 */
        /* 0x76 */ 0,0,0,0,0,                                                       /* 0x80 */
};

static int _IsAllowedWhitespace(unsigned c) { return c == ' ' || _AllowedWhitespace[c & 0xff]; }

// clang-format on
