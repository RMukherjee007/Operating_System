#include "minic.h"
#include "vfs.h"
#include "heap.h"
#include "uart.h"
#include "sched.h"
#include "timer.h"
#include "string.h"

#define MC_MAX_TOKENS  4096
#define MC_MAX_FUNCS   64
#define MC_MAX_VARS    256
#define MC_LIT_STRS    64
#define MC_DYN_STRS    32
#define MC_MAX_STRS    (MC_LIT_STRS + MC_DYN_STRS)
#define MC_STR_LEN     128
#define MC_MAX_ARR     128
#define MC_ARR_POOL    1024
#define MC_OBJ_POOL    512
#define MC_MAX_CLASSES 8
#define MC_MAX_FIELDS  12
#define MC_MAX_METHODS 12
#define MC_MAX_DEPTH   48
#define MC_MAX_ALLOCS  16
#define MC_MAX_ARGV    16

enum tok_kind {
    TOK_EOF = 0,
    TOK_INT_LIT,
    TOK_STR_LIT,
    TOK_IDENT,
    TOK_KW_INT,
    TOK_KW_CHAR,
    TOK_KW_VOID,
    TOK_KW_BOOL,
    TOK_KW_STRING,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_DO,
    TOK_KW_FOR,
    TOK_KW_SWITCH,
    TOK_KW_CASE,
    TOK_KW_DEFAULT,
    TOK_KW_RETURN,
    TOK_KW_BREAK,
    TOK_KW_CONTINUE,
    TOK_KW_STRUCT,
    TOK_KW_CLASS,
    TOK_KW_PUBLIC,
    TOK_KW_PRIVATE,
    TOK_KW_PROTECTED,
    TOK_KW_USING,
    TOK_KW_NAMESPACE,
    TOK_KW_NEW,
    TOK_KW_DELETE,
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACK, TOK_RBRACK,
    TOK_SEMI, TOK_COMMA, TOK_COLON, TOK_COLON_COLON, TOK_DOT, TOK_ARROW, TOK_QUESTION,
    TOK_ASSIGN,
    TOK_PLUS_EQ, TOK_MINUS_EQ, TOK_STAR_EQ, TOK_SLASH_EQ, TOK_PERCENT_EQ,
    TOK_PLUS_PLUS, TOK_MINUS_MINUS,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_EQ, TOK_NEQ, TOK_LT, TOK_LE, TOK_GT, TOK_GE,
    TOK_AND_AND, TOK_OR_OR,
    TOK_AMP, TOK_PIPE, TOK_CARET, TOK_SHL, TOK_SHR,
    TOK_BANG, TOK_TILDE
};

struct mc_token {
    uint16_t kind;
    char text[30];
    int64_t ival;
};

struct mc_func {
    char name[32];
    char param_names[8][32];
    bool param_is_str[8];
    bool param_is_char[8];
    int param_count;
    int body_tok;
    int class_idx; /* -1 for free function */
};

struct mc_class_field {
    char name[32];
    bool is_str;
    bool is_char;
};

struct mc_class {
    char name[32];
    struct mc_class_field fields[MC_MAX_FIELDS];
    int field_count;
    int method_funcs[MC_MAX_METHODS];
    int method_count;
};

struct mc_var {
    char name[32];
    int64_t val;
    bool is_array;
    bool is_str;
    bool is_char;
    bool is_obj;
    bool is_argv;
    int arr_size;
    int arr_base;
    int obj_base;
    int class_idx;
    int scope_depth;
};

struct mc_state {
    struct mc_token toks[MC_MAX_TOKENS];
    int tok_count;
    int pos;

    struct mc_func funcs[MC_MAX_FUNCS];
    int func_count;

    struct mc_class classes[MC_MAX_CLASSES];
    int class_count;

    struct mc_var vars[MC_MAX_VARS];
    int var_count;
    int global_var_end;
    int scope_depth;
    int local_var_floor;

    int64_t arr_pool[MC_ARR_POOL];
    int arr_used;

    int64_t obj_pool[MC_OBJ_POOL];
    int obj_used;

    char str_pool[MC_MAX_STRS][MC_STR_LEN];
    int lit_str_count;
    int dyn_str_cursor;

    const char *argv_ptrs[MC_MAX_ARGV];
    int argv_count;

    void *allocs[MC_MAX_ALLOCS];
    int alloc_count;

    int cur_obj_base;
    int cur_class_idx;

    bool last_is_str;
    bool last_is_char;
    bool returning;
    int64_t ret_val;
    bool breaking;
    bool continuing;
    bool has_error;
    char err_msg[96];
    char tmp_str[MC_STR_LEN];
    int call_depth;
};

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) {
    return (c >= '0' && c <= '9');
}

static void mc_error(struct mc_state *mc, const char *msg) {
    if (!mc->has_error) {
        mc->has_error = true;
        strncpy(mc->err_msg, msg, sizeof(mc->err_msg) - 1);
        mc->err_msg[sizeof(mc->err_msg) - 1] = '\0';
    }
}

static char *mc_alloc_dyn_str(struct mc_state *mc, const char *src) {
    int slot = MC_LIT_STRS + (mc->dyn_str_cursor % MC_DYN_STRS);
    mc->dyn_str_cursor++;
    char *dst = mc->str_pool[slot];
    if (src) {
        strncpy(dst, src, MC_STR_LEN - 1);
        dst[MC_STR_LEN - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
    return dst;
}

static bool mc_is_qualifier_kw(const char *s) {
    return strcmp(s, "const") == 0 || strcmp(s, "static") == 0 ||
           strcmp(s, "signed") == 0 || strcmp(s, "unsigned") == 0 ||
           strcmp(s, "inline") == 0 || strcmp(s, "volatile") == 0;
}

static bool mc_is_int_type_kw(const char *s) {
    return strcmp(s, "int") == 0 || strcmp(s, "long") == 0 ||
           strcmp(s, "short") == 0 || strcmp(s, "float") == 0 ||
           strcmp(s, "double") == 0 || strcmp(s, "auto") == 0 ||
           strcmp(s, "size_t") == 0 || strcmp(s, "ssize_t") == 0 ||
           strcmp(s, "int64_t") == 0 || strcmp(s, "uint64_t") == 0 ||
           strcmp(s, "int32_t") == 0 || strcmp(s, "uint32_t") == 0 ||
           strcmp(s, "int16_t") == 0 || strcmp(s, "uint16_t") == 0 ||
           strcmp(s, "int8_t") == 0 || strcmp(s, "uint8_t") == 0;
}

static bool mc_tokenize(struct mc_state *mc, const char *src) {
    const char *p = src;
    mc->tok_count = 0;
    mc->lit_str_count = 0;

    while (*p && mc->tok_count < MC_MAX_TOKENS - 1) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
            continue;
        }
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }

        struct mc_token tok;
        memset(&tok, 0, sizeof(tok));

        if (*p == '"') {
            p++;
            char str_tmp[MC_STR_LEN];
            size_t len = 0;
            while (*p && *p != '"') {
                char ch;
                if (*p == '\\' && p[1]) {
                    p++;
                    if (*p == 'n') ch = '\n';
                    else if (*p == 't') ch = '\t';
                    else if (*p == 'r') ch = '\r';
                    else if (*p == '0') ch = '\0';
                    else ch = *p;
                } else {
                    ch = *p;
                }
                if (len + 1 < MC_STR_LEN) str_tmp[len++] = ch;
                p++;
            }
            if (*p == '"') p++;
            str_tmp[len] = '\0';

            if (mc->lit_str_count >= MC_LIT_STRS) {
                mc_error(mc, "Too many string literals");
                return false;
            }
            int idx = mc->lit_str_count++;
            strncpy(mc->str_pool[idx], str_tmp, MC_STR_LEN - 1);
            mc->str_pool[idx][MC_STR_LEN - 1] = '\0';
            tok.kind = TOK_STR_LIT;
            tok.ival = idx;
            mc->toks[mc->tok_count++] = tok;
            continue;
        }

        if (*p == '\'') {
            p++;
            int64_t ch = 0;
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n') ch = '\n';
                else if (*p == 't') ch = '\t';
                else if (*p == 'r') ch = '\r';
                else if (*p == '0') ch = '\0';
                else ch = (uint8_t)*p;
                p++;
            } else if (*p) {
                ch = (uint8_t)*p++;
            }
            if (*p == '\'') p++;
            tok.kind = TOK_INT_LIT;
            tok.ival = ch;
            mc->toks[mc->tok_count++] = tok;
            continue;
        }

        if (is_digit(*p)) {
            int64_t v = 0;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                while (*p) {
                    if (*p >= '0' && *p <= '9') v = v * 16 + (*p - '0');
                    else if (*p >= 'a' && *p <= 'f') v = v * 16 + (*p - 'a' + 10);
                    else if (*p >= 'A' && *p <= 'F') v = v * 16 + (*p - 'A' + 10);
                    else break;
                    p++;
                }
            } else {
                while (is_digit(*p)) {
                    v = v * 10 + (*p - '0');
                    p++;
                }
            }
            /* Skip integer suffixes such as U, UL, ULL, LL */
            while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') p++;
            tok.kind = TOK_INT_LIT;
            tok.ival = v;
            mc->toks[mc->tok_count++] = tok;
            continue;
        }

        if (is_alpha(*p)) {
            size_t len = 0;
            while (is_alpha(*p) || is_digit(*p)) {
                if (len + 1 < sizeof(tok.text)) tok.text[len++] = *p;
                p++;
            }
            tok.text[len] = '\0';

            if (mc_is_qualifier_kw(tok.text)) {
                /* Collapse standalone qualifiers; if 'unsigned' is alone, next check handles below */
                if ((strcmp(tok.text, "unsigned") == 0 || strcmp(tok.text, "signed") == 0) &&
                    (*p == ' ' || *p == '\t')) {
                    /* Peek ahead: if next word isn't an int type, treat as int */
                    const char *q = p;
                    while (*q == ' ' || *q == '\t') q++;
                    char nxt[32];
                    size_t nl = 0;
                    while ((is_alpha(*q) || is_digit(*q)) && nl + 1 < sizeof(nxt)) nxt[nl++] = *q++;
                    nxt[nl] = '\0';
                    if (mc_is_int_type_kw(nxt) || strcmp(nxt, "char") == 0) {
                        continue;
                    }
                    tok.kind = TOK_KW_INT;
                } else {
                    continue;
                }
            } else if (strcmp(tok.text, "true") == 0) {
                tok.kind = TOK_INT_LIT;
                tok.ival = 1;
            } else if (strcmp(tok.text, "false") == 0 ||
                       strcmp(tok.text, "NULL") == 0 ||
                       strcmp(tok.text, "nullptr") == 0) {
                tok.kind = TOK_INT_LIT;
                tok.ival = 0;
            } else if (mc_is_int_type_kw(tok.text)) {
                /* Collapse 'long long' / 'long int' into single TOK_KW_INT */
                if (mc->tok_count > 0 && mc->toks[mc->tok_count - 1].kind == TOK_KW_INT) {
                    continue;
                }
                tok.kind = TOK_KW_INT;
            } else {
                static const struct { const char *kw; uint16_t kind; } kw_map[] = {
                    { "char", TOK_KW_CHAR },       { "void", TOK_KW_VOID },
                    { "bool", TOK_KW_BOOL },       { "string", TOK_KW_STRING },
                    { "if", TOK_KW_IF },           { "else", TOK_KW_ELSE },
                    { "while", TOK_KW_WHILE },     { "do", TOK_KW_DO },
                    { "for", TOK_KW_FOR },         { "switch", TOK_KW_SWITCH },
                    { "case", TOK_KW_CASE },       { "default", TOK_KW_DEFAULT },
                    { "return", TOK_KW_RETURN },   { "break", TOK_KW_BREAK },
                    { "continue", TOK_KW_CONTINUE }, { "struct", TOK_KW_STRUCT },
                    { "class", TOK_KW_CLASS },     { "public", TOK_KW_PUBLIC },
                    { "private", TOK_KW_PRIVATE }, { "protected", TOK_KW_PROTECTED },
                    { "using", TOK_KW_USING },     { "namespace", TOK_KW_NAMESPACE },
                    { "new", TOK_KW_NEW },         { "delete", TOK_KW_DELETE },
                };
                tok.kind = TOK_IDENT;
                for (size_t i = 0; i < sizeof(kw_map) / sizeof(kw_map[0]); i++) {
                    if (strcmp(tok.text, kw_map[i].kw) == 0) {
                        tok.kind = kw_map[i].kind;
                        break;
                    }
                }
            }

            /* Collapse 'std :: <ident/string>' into just '<ident/string>' */
            if (mc->tok_count >= 2 &&
                mc->toks[mc->tok_count - 2].kind == TOK_IDENT &&
                strcmp(mc->toks[mc->tok_count - 2].text, "std") == 0 &&
                mc->toks[mc->tok_count - 1].kind == TOK_COLON_COLON) {
                mc->tok_count -= 2;
            }
            mc->toks[mc->tok_count++] = tok;
            continue;
        }

        /* Multi-character operators */
        static const struct { char c0, c1; uint16_t kind; } op2_map[] = {
            { ':', ':', TOK_COLON_COLON }, { '-', '>', TOK_ARROW },
            { '=', '=', TOK_EQ },          { '!', '=', TOK_NEQ },
            { '<', '=', TOK_LE },          { '>', '=', TOK_GE },
            { '<', '<', TOK_SHL },         { '>', '>', TOK_SHR },
            { '&', '&', TOK_AND_AND },     { '|', '|', TOK_OR_OR },
            { '+', '+', TOK_PLUS_PLUS },   { '-', '-', TOK_MINUS_MINUS },
            { '+', '=', TOK_PLUS_EQ },     { '-', '=', TOK_MINUS_EQ },
            { '*', '=', TOK_STAR_EQ },     { '/', '=', TOK_SLASH_EQ },
            { '%', '=', TOK_PERCENT_EQ },
        };
        bool matched_op2 = false;
        for (size_t i = 0; i < sizeof(op2_map) / sizeof(op2_map[0]); i++) {
            if (p[0] == op2_map[i].c0 && p[1] == op2_map[i].c1) {
                tok.kind = op2_map[i].kind;
                p += 2;
                mc->toks[mc->tok_count++] = tok;
                matched_op2 = true;
                break;
            }
        }
        if (matched_op2) continue;

        switch (*p++) {
            case '(': tok.kind = TOK_LPAREN; break;
            case ')': tok.kind = TOK_RPAREN; break;
            case '{': tok.kind = TOK_LBRACE; break;
            case '}': tok.kind = TOK_RBRACE; break;
            case '[': tok.kind = TOK_LBRACK; break;
            case ']': tok.kind = TOK_RBRACK; break;
            case ';': tok.kind = TOK_SEMI; break;
            case ',': tok.kind = TOK_COMMA; break;
            case ':': tok.kind = TOK_COLON; break;
            case '.': tok.kind = TOK_DOT; break;
            case '?': tok.kind = TOK_QUESTION; break;
            case '=': tok.kind = TOK_ASSIGN; break;
            case '+': tok.kind = TOK_PLUS; break;
            case '-': tok.kind = TOK_MINUS; break;
            case '*': tok.kind = TOK_STAR; break;
            case '/': tok.kind = TOK_SLASH; break;
            case '%': tok.kind = TOK_PERCENT; break;
            case '<': tok.kind = TOK_LT; break;
            case '>': tok.kind = TOK_GT; break;
            case '&': tok.kind = TOK_AMP; break;
            case '|': tok.kind = TOK_PIPE; break;
            case '^': tok.kind = TOK_CARET; break;
            case '!': tok.kind = TOK_BANG; break;
            case '~': tok.kind = TOK_TILDE; break;
            default:
                mc_error(mc, "Unexpected character in source");
                return false;
        }
        mc->toks[mc->tok_count++] = tok;
    }

    mc->toks[mc->tok_count].kind = TOK_EOF;
    return !mc->has_error;
}

static struct mc_token *peek(struct mc_state *mc) {
    return &mc->toks[mc->pos];
}

static struct mc_token *peek_at(struct mc_state *mc, int offset) {
    int idx = mc->pos + offset;
    if (idx < 0) idx = 0;
    if (idx >= mc->tok_count) return &mc->toks[mc->tok_count];
    return &mc->toks[idx];
}

static bool match(struct mc_state *mc, enum tok_kind k) {
    if (peek(mc)->kind == k) {
        mc->pos++;
        return true;
    }
    return false;
}

static bool expect(struct mc_state *mc, enum tok_kind k, const char *err) {
    if (match(mc, k)) return true;
    mc_error(mc, err);
    return false;
}

static int find_class(struct mc_state *mc, const char *name) {
    for (int i = 0; i < mc->class_count; i++) {
        if (strcmp(mc->classes[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_class_field(struct mc_class *cls, const char *name) {
    for (int i = 0; i < cls->field_count; i++) {
        if (strcmp(cls->fields[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_class_method(struct mc_state *mc, int class_idx, const char *name) {
    if (class_idx < 0 || class_idx >= mc->class_count) return -1;
    struct mc_class *cls = &mc->classes[class_idx];
    for (int i = 0; i < cls->method_count; i++) {
        int fidx = cls->method_funcs[i];
        if (strcmp(mc->funcs[fidx].name, name) == 0) return fidx;
    }
    return -1;
}

static bool is_builtin_type_kw(enum tok_kind k) {
    return k == TOK_KW_INT || k == TOK_KW_CHAR || k == TOK_KW_VOID ||
           k == TOK_KW_BOOL || k == TOK_KW_STRING;
}

static bool is_type_start(struct mc_state *mc) {
    enum tok_kind k = peek(mc)->kind;
    if (is_builtin_type_kw(k)) return true;
    if ((k == TOK_KW_STRUCT || k == TOK_KW_CLASS) &&
        peek_at(mc, 1)->kind == TOK_IDENT &&
        peek_at(mc, 2)->kind != TOK_LBRACE) {
        return find_class(mc, peek_at(mc, 1)->text) >= 0;
    }
    if (k == TOK_IDENT && find_class(mc, peek(mc)->text) >= 0) {
        /* Distinguish 'Point p;' (decl) vs expression */
        if (peek_at(mc, 1)->kind == TOK_IDENT || peek_at(mc, 1)->kind == TOK_STAR) {
            return true;
        }
    }
    return false;
}

static bool consume_type_spec(struct mc_state *mc, int *out_class_idx,
                              bool *out_is_str, bool *out_is_char) {
    if (out_class_idx) *out_class_idx = -1;
    if (out_is_str) *out_is_str = false;
    if (out_is_char) *out_is_char = false;

    enum tok_kind k = peek(mc)->kind;
    if (k == TOK_KW_STRUCT || k == TOK_KW_CLASS) {
        mc->pos++;
        if (peek(mc)->kind != TOK_IDENT) {
            mc_error(mc, "Expected struct/class type name");
            return false;
        }
        int cidx = find_class(mc, peek(mc)->text);
        if (cidx < 0) {
            mc_error(mc, "Unknown struct/class type");
            return false;
        }
        if (out_class_idx) *out_class_idx = cidx;
        mc->pos++;
    } else if (k == TOK_IDENT && find_class(mc, peek(mc)->text) >= 0) {
        int cidx = find_class(mc, peek(mc)->text);
        if (out_class_idx) *out_class_idx = cidx;
        mc->pos++;
    } else if (is_builtin_type_kw(k)) {
        if (k == TOK_KW_STRING && out_is_str) *out_is_str = true;
        if (k == TOK_KW_CHAR && out_is_char) *out_is_char = true;
        mc->pos++;
    } else {
        return false;
    }

    int ptr_depth = 0;
    while (match(mc, TOK_STAR) || match(mc, TOK_AMP)) {
        ptr_depth++;
    }
    if (k == TOK_KW_CHAR && ptr_depth == 1 && out_is_str) {
        *out_is_str = true;
        if (out_is_char) *out_is_char = false;
    }
    return true;
}

static struct mc_var *find_var(struct mc_state *mc, const char *name) {
    for (int i = mc->var_count - 1; i >= mc->local_var_floor; i--) {
        if (strcmp(mc->vars[i].name, name) == 0) return &mc->vars[i];
    }
    for (int i = mc->global_var_end - 1; i >= 0; i--) {
        if (strcmp(mc->vars[i].name, name) == 0) return &mc->vars[i];
    }
    return NULL;
}

static struct mc_var *declare_var(struct mc_state *mc, const char *name, int64_t val) {
    if (mc->var_count >= MC_MAX_VARS) {
        mc_error(mc, "Too many variables");
        return NULL;
    }
    struct mc_var *v = &mc->vars[mc->var_count++];
    memset(v, 0, sizeof(*v));
    strncpy(v->name, name, sizeof(v->name) - 1);
    v->name[sizeof(v->name) - 1] = '\0';
    v->val = val;
    v->class_idx = -1;
    v->scope_depth = mc->scope_depth;
    return v;
}

static void pop_scope(struct mc_state *mc, int target_depth) {
    while (mc->var_count > mc->local_var_floor &&
           mc->vars[mc->var_count - 1].scope_depth >= target_depth) {
        mc->var_count--;
    }
}

static struct mc_func *find_func(struct mc_state *mc, const char *name) {
    for (int i = 0; i < mc->func_count; i++) {
        if (mc->funcs[i].class_idx < 0 && strcmp(mc->funcs[i].name, name) == 0) {
            return &mc->funcs[i];
        }
    }
    return NULL;
}

static int64_t eval_unary(struct mc_state *mc);
static int64_t eval_expr(struct mc_state *mc);
static void exec_stmt(struct mc_state *mc);

static void skip_balanced(struct mc_state *mc, enum tok_kind open_k, enum tok_kind close_k) {
    if (!match(mc, open_k)) return;
    int depth = 1;
    while (peek(mc)->kind != TOK_EOF && depth > 0) {
        if (peek(mc)->kind == open_k) depth++;
        else if (peek(mc)->kind == close_k) depth--;
        mc->pos++;
    }
}

/* A skipped logical operand ends before the next lower-precedence operator. */
static void skip_expr_branch(struct mc_state *mc, enum tok_kind op) {
    int pdepth = 0;
    int tdepth = 0;
    while (peek(mc)->kind != TOK_EOF) {
        enum tok_kind k = peek(mc)->kind;
        if (k == TOK_LPAREN || k == TOK_LBRACK || k == TOK_LBRACE) {
            pdepth++;
        } else if (k == TOK_RPAREN || k == TOK_RBRACK || k == TOK_RBRACE) {
            if (pdepth == 0) break;
            pdepth--;
        } else if (pdepth == 0) {
            if ((op == TOK_AND_AND && k == TOK_AND_AND) ||
                ((op == TOK_AND_AND || op == TOK_OR_OR) &&
                 (k == TOK_OR_OR || k == TOK_QUESTION))) break;
            if (k == TOK_QUESTION) {
                tdepth++;
            } else if (k == TOK_COLON) {
                if (tdepth == 0) break;
                tdepth--;
            } else if (k == TOK_SEMI || k == TOK_COMMA) {
                break;
            }
        }
        mc->pos++;
    }
}

static void skip_stmt(struct mc_state *mc) {
    if (peek(mc)->kind == TOK_LBRACE) {
        skip_balanced(mc, TOK_LBRACE, TOK_RBRACE);
        return;
    }
    if (match(mc, TOK_KW_IF)) {
        skip_balanced(mc, TOK_LPAREN, TOK_RPAREN);
        skip_stmt(mc);
        if (match(mc, TOK_KW_ELSE)) skip_stmt(mc);
        return;
    }
    if (match(mc, TOK_KW_WHILE) || match(mc, TOK_KW_FOR)) {
        skip_balanced(mc, TOK_LPAREN, TOK_RPAREN);
        skip_stmt(mc);
        return;
    }
    if (match(mc, TOK_KW_DO)) {
        skip_stmt(mc);
        match(mc, TOK_KW_WHILE);
        skip_balanced(mc, TOK_LPAREN, TOK_RPAREN);
        match(mc, TOK_SEMI);
        return;
    }
    while (peek(mc)->kind != TOK_EOF && peek(mc)->kind != TOK_SEMI && peek(mc)->kind != TOK_RBRACE) {
        mc->pos++;
    }
    match(mc, TOK_SEMI);
}

static bool is_valid_mc_ptr(struct mc_state *mc, const int64_t *p) {
    if (!p) return false;
    if (p >= &mc->vars[0].val && p <= &mc->vars[MC_MAX_VARS - 1].val) {
        uintptr_t off = (uintptr_t)p - (uintptr_t)&mc->vars[0].val;
        return (off % sizeof(struct mc_var)) == 0;
    }
    if (p >= &mc->arr_pool[0] && p < &mc->arr_pool[MC_ARR_POOL]) return true;
    if (p >= &mc->obj_pool[0] && p < &mc->obj_pool[MC_OBJ_POOL]) return true;
    return false;
}

static void mc_read_token(char *out, size_t cap) {
    size_t pos = 0;
    for (;;) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') {
            uart_putc('\n');
            continue;
        }
        if (c == ' ' || c == '\t') {
            uart_putc(c);
            continue;
        }
        uart_putc(c);
        if (pos + 1 < cap) out[pos++] = c;
        break;
    }
    for (;;) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') {
            uart_putc('\n');
            break;
        } else if (c == ' ' || c == '\t') {
            uart_putc(c);
            break;
        } else if (c == '\b' || (uint8_t)c == 0x7F) {
            if (pos > 0) {
                pos--;
                uart_puts("\b \b");
            }
        } else if ((uint8_t)c >= 0x20 && pos + 1 < cap) {
            uart_putc(c);
            out[pos++] = c;
        }
    }
    out[pos] = '\0';
}

static int64_t mc_parse_int64(const char *s) {
    if (!s || !*s) return 0;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') s++;
    int64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

static const char *mc_char_arr_to_str(struct mc_state *mc, struct mc_var *v) {
    char tmp[MC_STR_LEN];
    int len = 0;
    for (int i = 0; i < v->arr_size && len + 1 < MC_STR_LEN; i++) {
        char ch = (char)mc->arr_pool[v->arr_base + i];
        if (ch == '\0') break;
        tmp[len++] = ch;
    }
    tmp[len] = '\0';
    return mc_alloc_dyn_str(mc, tmp);
}

static bool resolve_obj_var(struct mc_state *mc, struct mc_var *v, bool is_arrow,
                            int *out_cidx, int *out_obase) {
    int cidx = v->class_idx;
    int obase = v->obj_base;
    bool is_obj = v->is_obj;
    if (is_arrow && !is_obj && v->val >= 0 && v->val < mc->var_count) {
        struct mc_var *target = &mc->vars[v->val];
        cidx = target->class_idx;
        obase = target->obj_base;
        is_obj = target->is_obj;
    }
    *out_cidx = cidx;
    *out_obase = obase;
    return is_obj && cidx >= 0;
}

static int64_t *eval_lvalue(struct mc_state *mc, bool *out_is_str, bool *out_is_char,
                            struct mc_var **out_var) {
    if (out_is_str) *out_is_str = false;
    if (out_is_char) *out_is_char = false;
    if (out_var) *out_var = NULL;

    if (match(mc, TOK_STAR)) {
        int64_t addr = eval_unary(mc);
        int64_t *p = (int64_t *)(uintptr_t)addr;
        if (!is_valid_mc_ptr(mc, p)) {
            mc_error(mc, "Invalid pointer dereference");
            return NULL;
        }
        return p;
    }

    if (peek(mc)->kind != TOK_IDENT) return NULL;
    char name[32];
    strncpy(name, peek(mc)->text, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    mc->pos++;

    /* Explicit 'this->field' inside a class method */
    if (strcmp(name, "this") == 0 && (match(mc, TOK_ARROW) || match(mc, TOK_DOT))) {
        if (mc->cur_class_idx < 0 || mc->cur_obj_base < 0 || peek(mc)->kind != TOK_IDENT) {
            mc_error(mc, "Invalid 'this' member access");
            return NULL;
        }
        int fidx = find_class_field(&mc->classes[mc->cur_class_idx], peek(mc)->text);
        if (fidx < 0) {
            mc_error(mc, "Unknown class member");
            return NULL;
        }
        mc->pos++;
        struct mc_class_field *cf = &mc->classes[mc->cur_class_idx].fields[fidx];
        if (out_is_str) *out_is_str = cf->is_str;
        if (out_is_char) *out_is_char = cf->is_char;
        return &mc->obj_pool[mc->cur_obj_base + fidx];
    }

    struct mc_var *v = find_var(mc, name);
    if (!v) {
        /* Implicit 'this' field lookup inside a method */
        if (mc->cur_class_idx >= 0 && mc->cur_obj_base >= 0) {
            int fidx = find_class_field(&mc->classes[mc->cur_class_idx], name);
            if (fidx >= 0) {
                struct mc_class_field *cf = &mc->classes[mc->cur_class_idx].fields[fidx];
                if (out_is_str) *out_is_str = cf->is_str;
                if (out_is_char) *out_is_char = cf->is_char;
                return &mc->obj_pool[mc->cur_obj_base + fidx];
            }
        }
        return NULL;
    }

    if (out_var) *out_var = v;

    /* Array indexing */
    if (match(mc, TOK_LBRACK)) {
        int64_t idx = eval_expr(mc);
        if (!expect(mc, TOK_RBRACK, "Expected ']'")) return NULL;
        if (!v->is_array || idx < 0 || idx >= v->arr_size) {
            mc_error(mc, "Array index out of bounds");
            return NULL;
        }
        if (out_is_char) *out_is_char = v->is_char;
        return &mc->arr_pool[v->arr_base + idx];
    }

    /* Object field access: obj.field or ptr->field */
    if (peek(mc)->kind == TOK_DOT || peek(mc)->kind == TOK_ARROW) {
        bool is_arrow = (peek(mc)->kind == TOK_ARROW);
        if (peek_at(mc, 1)->kind == TOK_IDENT && peek_at(mc, 2)->kind != TOK_LPAREN) {
            mc->pos++;
            const char *fname = peek(mc)->text;
            mc->pos++;
            int cidx = -1, obase = 0;
            if (!resolve_obj_var(mc, v, is_arrow, &cidx, &obase)) {
                mc_error(mc, "Member access on non-object");
                return NULL;
            }
            int fidx = find_class_field(&mc->classes[cidx], fname);
            if (fidx < 0) {
                mc_error(mc, "Unknown struct/class field");
                return NULL;
            }
            struct mc_class_field *cf = &mc->classes[cidx].fields[fidx];
            if (out_is_str) *out_is_str = cf->is_str;
            if (out_is_char) *out_is_char = cf->is_char;
            return &mc->obj_pool[obase + fidx];
        }
    }

    if (out_is_str) *out_is_str = v->is_str;
    if (out_is_char) *out_is_char = v->is_char;
    return &v->val;
}

static int64_t invoke_mc_func(struct mc_state *mc, struct mc_func *fn,
                              int argc, int64_t *args, const bool *arg_is_str,
                              int obj_base, int class_idx) {
    if (mc->call_depth >= MC_MAX_DEPTH) {
        mc_error(mc, "Max recursion depth reached");
        return 0;
    }

    int saved_pos = mc->pos;
    int saved_scope = mc->scope_depth;
    int saved_vars = mc->var_count;
    int saved_floor = mc->local_var_floor;
    int saved_obj_base = mc->cur_obj_base;
    int saved_class_idx = mc->cur_class_idx;
    bool saved_ret = mc->returning;
    int64_t saved_rval = mc->ret_val;

    mc->call_depth++;
    mc->local_var_floor = mc->var_count;
    mc->scope_depth++;
    mc->returning = false;
    mc->ret_val = 0;

    if (class_idx >= 0) {
        mc->cur_obj_base = obj_base;
        mc->cur_class_idx = class_idx;
    }

    for (int i = 0; i < fn->param_count; i++) {
        struct mc_var *pv = declare_var(mc, fn->param_names[i], (i < argc) ? args[i] : 0);
        if (pv) {
            pv->is_str = fn->param_is_str[i] || (arg_is_str && i < argc && arg_is_str[i]);
            pv->is_char = fn->param_is_char[i];
            if (strcmp(fn->param_names[i], "argv") == 0 && i == 1 && mc->argv_count > 0) {
                pv->is_argv = true;
            }
        }
    }

    mc->pos = fn->body_tok;
    exec_stmt(mc);

    int64_t result = mc->ret_val;

    mc->var_count = saved_vars;
    mc->local_var_floor = saved_floor;
    mc->scope_depth = saved_scope;
    mc->cur_obj_base = saved_obj_base;
    mc->cur_class_idx = saved_class_idx;
    mc->returning = saved_ret;
    mc->ret_val = saved_rval;
    mc->pos = saved_pos;
    mc->call_depth--;
    mc->last_is_str = false;
    mc->last_is_char = false;
    return result;
}

static int64_t read_current_el_val(void) {
    uint64_t el = 0;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (int64_t)((el >> 2) & 0x3UL);
}

static int64_t call_func(struct mc_state *mc, const char *name,
                         int argc, int64_t *args, const char **str_args,
                         const bool *arg_is_str, struct mc_var **arg_vars) {
    mc->last_is_str = false;
    mc->last_is_char = false;

    if (strcmp(name, "printf") == 0) {
        const char *fmt = (argc > 0 && str_args[0]) ? str_args[0] : "";
        int ai = 1;
        while (*fmt) {
            if (*fmt != '%') {
                uart_putc(*fmt++);
                continue;
            }
            fmt++;
            if (*fmt == '%') { uart_putc('%'); fmt++; continue; }
            bool left_align = false;
            int width = 0;
            while (*fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ') {
                if (*fmt == '-') left_align = true;
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
            if (*fmt == '.') {
                fmt++;
                while (*fmt >= '0' && *fmt <= '9') fmt++;
            }
            while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h') fmt++;

            int64_t v = (ai < argc) ? args[ai] : 0;
            const char *sv = (ai < argc) ? str_args[ai] : NULL;
            ai++;

            char num_buf[32];
            switch (*fmt) {
                case 'd':
                case 'i':
                    snprintf(num_buf, sizeof(num_buf), "%ld", v);
                    goto print_padded;
                case 'u':
                    snprintf(num_buf, sizeof(num_buf), "%lu", (uint64_t)v);
                    goto print_padded;
                case 'x':
                    snprintf(num_buf, sizeof(num_buf), "%lx", (uint64_t)v);
                    goto print_padded;
                case 'X':
                    snprintf(num_buf, sizeof(num_buf), "%lX", (uint64_t)v);
                    goto print_padded;
                case 'p':
                    snprintf(num_buf, sizeof(num_buf), "0x%lx", (uint64_t)v);
                    goto print_padded;
                case 'c':
                    uart_putc((char)v);
                    break;
                case 's': {
                    const char *out_s = sv ? sv : "(null)";
                    int slen = (int)strlen(out_s);
                    if (!left_align) for (int p = slen; p < width; p++) uart_putc(' ');
                    kprintf("%s", out_s);
                    if (left_align) for (int p = slen; p < width; p++) uart_putc(' ');
                    break;
                }
                default:
                    uart_putc('%');
                    if (*fmt) uart_putc(*fmt);
                    break;
                print_padded: {
                    int nlen = (int)strlen(num_buf);
                    if (!left_align) for (int p = nlen; p < width; p++) uart_putc(' ');
                    kprintf("%s", num_buf);
                    if (left_align) for (int p = nlen; p < width; p++) uart_putc(' ');
                    break;
                }
            }
            if (*fmt) fmt++;
        }
        return 0;
    }

    if (strcmp(name, "scanf") == 0) {
        const char *fmt = (argc > 0 && str_args[0]) ? str_args[0] : "";
        int ai = 1;
        int matched = 0;
        while (*fmt && ai < argc) {
            if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') { fmt++; continue; }
            if (*fmt != '%') { fmt++; continue; }
            fmt++;
            while (*fmt == 'l' || *fmt == 'h') fmt++;
            char tok[MC_STR_LEN];
            if (*fmt == 'c') {
                char ch = uart_getc();
                uart_putc(ch);
                int64_t *dst = (int64_t *)(uintptr_t)args[ai++];
                if (is_valid_mc_ptr(mc, dst)) { *dst = (uint8_t)ch; matched++; }
            } else if (*fmt == 's') {
                mc_read_token(tok, sizeof(tok));
                struct mc_var *v = (arg_vars && ai < argc) ? arg_vars[ai] : NULL;
                if (v && v->is_array) {
                    int n = (int)strlen(tok);
                    if (n >= v->arr_size) n = v->arr_size - 1;
                    for (int k = 0; k < n; k++) mc->arr_pool[v->arr_base + k] = (uint8_t)tok[k];
                    mc->arr_pool[v->arr_base + n] = '\0';
                    matched++;
                } else {
                    int64_t *dst = (int64_t *)(uintptr_t)args[ai];
                    if (is_valid_mc_ptr(mc, dst)) {
                        *dst = (int64_t)(uintptr_t)mc_alloc_dyn_str(mc, tok);
                        matched++;
                    }
                }
                ai++;
            } else if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u') {
                mc_read_token(tok, sizeof(tok));
                int64_t *dst = (int64_t *)(uintptr_t)args[ai++];
                if (is_valid_mc_ptr(mc, dst)) {
                    *dst = mc_parse_int64(tok);
                    matched++;
                }
            }
            if (*fmt) fmt++;
        }
        return matched;
    }

    if (strcmp(name, "puts") == 0) {
        kprintf("%s\n", (argc > 0 && str_args[0]) ? str_args[0] : "");
        return 0;
    }
    if (strcmp(name, "putchar") == 0) {
        uart_putc(argc > 0 ? (char)args[0] : '\0');
        return 0;
    }
    if (strcmp(name, "getchar") == 0) {
        mc->last_is_char = true;
        return (int64_t)(uint8_t)uart_getc();
    }
    if (strcmp(name, "sleep") == 0) {
        if (argc > 0 && args[0] > 0) task_sleep_ms((uint64_t)args[0]);
        return 0;
    }
    if (strcmp(name, "uptime") == 0 || strcmp(name, "sys_uptime") == 0) {
        return (int64_t)timer_get_ms();
    }
    if (strcmp(name, "getpid") == 0 || strcmp(name, "sys_getpid") == 0) {
        return (int64_t)sched_current()->pid;
    }
    if (strcmp(name, "sys_getel") == 0) {
        return read_current_el_val();
    }
    if (strcmp(name, "strlen") == 0) {
        return (argc > 0 && str_args[0]) ? (int64_t)strlen(str_args[0]) : 0;
    }
    if (strcmp(name, "strcmp") == 0) {
        const char *a = (argc > 0 && str_args[0]) ? str_args[0] : "";
        const char *b = (argc > 1 && str_args[1]) ? str_args[1] : "";
        return (int64_t)strcmp(a, b);
    }
    if (strcmp(name, "atoi") == 0) {
        return (argc > 0 && str_args[0]) ? (int64_t)atoi(str_args[0]) : 0;
    }
    if (strcmp(name, "abs") == 0) {
        int64_t v = (argc > 0) ? args[0] : 0;
        return (v < 0) ? -v : v;
    }
    if (strcmp(name, "max") == 0) {
        int64_t a = (argc > 0) ? args[0] : 0;
        int64_t b = (argc > 1) ? args[1] : 0;
        return (a > b) ? a : b;
    }
    if (strcmp(name, "min") == 0) {
        int64_t a = (argc > 0) ? args[0] : 0;
        int64_t b = (argc > 1) ? args[1] : 0;
        return (a < b) ? a : b;
    }
    if (strcmp(name, "pow") == 0) {
        int64_t base = (argc > 0) ? args[0] : 0;
        int64_t exp = (argc > 1) ? args[1] : 0;
        int64_t res = 1;
        while (exp-- > 0) res *= base;
        return res;
    }
    if (strcmp(name, "sqrt") == 0) {
        int64_t x = (argc > 0) ? args[0] : 0;
        if (x <= 0) return 0;
        int64_t r = x;
        for (int i = 0; i < 32; i++) {
            int64_t nr = (r + x / r) / 2;
            if (nr >= r) break;
            r = nr;
        }
        return r;
    }
    if (strcmp(name, "malloc") == 0) {
        size_t sz = (argc > 0 && args[0] > 0) ? (size_t)args[0] : 16U;
        void *p = kcalloc(1, sz);
        if (p && mc->alloc_count < MC_MAX_ALLOCS) {
            mc->allocs[mc->alloc_count++] = p;
        }
        return (int64_t)(uintptr_t)p;
    }
    if (strcmp(name, "free") == 0) {
        void *p = (argc > 0) ? (void *)(uintptr_t)args[0] : NULL;
        for (int i = 0; i < mc->alloc_count; i++) {
            if (mc->allocs[i] == p) {
                kfree(p);
                mc->allocs[i] = mc->allocs[--mc->alloc_count];
                break;
            }
        }
        return 0;
    }
    if (strcmp(name, "exit") == 0) {
        mc->ret_val = (argc > 0) ? args[0] : 0;
        mc->returning = true;
        return mc->ret_val;
    }

    /* Check active class methods first when called without qualifier inside a method */
    if (mc->cur_class_idx >= 0) {
        int midx = find_class_method(mc, mc->cur_class_idx, name);
        if (midx >= 0) {
            return invoke_mc_func(mc, &mc->funcs[midx], argc, args, arg_is_str,
                                  mc->cur_obj_base, mc->cur_class_idx);
        }
    }

    struct mc_func *fn = find_func(mc, name);
    if (!fn) {
        mc_error(mc, "Undefined function call");
        return 0;
    }
    return invoke_mc_func(mc, fn, argc, args, arg_is_str, mc->cur_obj_base, mc->cur_class_idx);
}

static int parse_call_args(struct mc_state *mc, int64_t *args, const char **str_args,
                           bool *arg_is_str, struct mc_var **arg_vars, int max_args) {
    int argc = 0;
    if (match(mc, TOK_RPAREN)) return 0;

    do {
        if (argc < max_args) {
            struct mc_var *v = NULL;
            if (peek(mc)->kind == TOK_IDENT) {
                v = find_var(mc, peek(mc)->text);
            }
            arg_vars[argc] = v;
            mc->last_is_str = false;
            mc->last_is_char = false;
            int64_t val = eval_expr(mc);
            args[argc] = val;
            arg_is_str[argc] = mc->last_is_str;
            str_args[argc] = mc->last_is_str ? (const char *)(uintptr_t)val : NULL;
            argc++;
        } else {
            eval_expr(mc);
        }
    } while (match(mc, TOK_COMMA));

    expect(mc, TOK_RPAREN, "Expected ')' after arguments");
    return argc;
}

static int64_t eval_primary(struct mc_state *mc) {
    if (mc->has_error) return 0;
    mc->last_is_str = false;
    mc->last_is_char = false;

    struct mc_token *t = peek(mc);
    if (t->kind == TOK_INT_LIT) {
        mc->pos++;
        return t->ival;
    }
    if (t->kind == TOK_STR_LIT) {
        mc->pos++;
        mc->last_is_str = true;
        return (int64_t)(uintptr_t)mc->str_pool[t->ival];
    }
    if (match(mc, TOK_LPAREN)) {
        int64_t v = eval_expr(mc);
        expect(mc, TOK_RPAREN, "Expected ')'");
        return v;
    }
    if (match(mc, TOK_KW_NEW)) {
        if (peek(mc)->kind != TOK_IDENT) {
            mc_error(mc, "Expected class name after 'new'");
            return 0;
        }
        int cidx = find_class(mc, peek(mc)->text);
        if (cidx < 0) {
            mc_error(mc, "Unknown class after 'new'");
            return 0;
        }
        mc->pos++;
        struct mc_class *cls = &mc->classes[cidx];
        if (mc->obj_used + cls->field_count > MC_OBJ_POOL) {
            mc_error(mc, "Object pool full");
            return 0;
        }
        int obase = mc->obj_used;
        for (int f = 0; f < cls->field_count; f++) mc->obj_pool[mc->obj_used++] = 0;

        /* Create hidden heap-like var descriptor so 'ptr->method' works uniformly */
        int var_idx = mc->var_count;
        struct mc_var *ov = declare_var(mc, "__new_obj", 0);
        if (!ov) return 0;
        ov->is_obj = true;
        ov->class_idx = cidx;
        ov->obj_base = obase;

        int64_t args[12];
        const char *str_args[12];
        bool arg_is_str[12];
        struct mc_var *arg_vars[12];
        int argc = 0;
        if (match(mc, TOK_LPAREN)) {
            argc = parse_call_args(mc, args, str_args, arg_is_str, arg_vars, 12);
        }
        int ctor = find_class_method(mc, cidx, cls->name);
        if (ctor >= 0) {
            invoke_mc_func(mc, &mc->funcs[ctor], argc, args, arg_is_str, obase, cidx);
        }
        return (int64_t)var_idx;
    }

    if (t->kind == TOK_IDENT) {
        char name[32];
        strncpy(name, t->text, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        mc->pos++;

        /* Free function or implicit method call */
        if (match(mc, TOK_LPAREN)) {
            int64_t args[12];
            const char *str_args[12];
            bool arg_is_str[12];
            struct mc_var *arg_vars[12];
            int argc = parse_call_args(mc, args, str_args, arg_is_str, arg_vars, 12);
            return call_func(mc, name, argc, args, str_args, arg_is_str, arg_vars);
        }

        /* Special 'this->member' or 'this->method()' inside class method */
        if (strcmp(name, "this") == 0 && (match(mc, TOK_ARROW) || match(mc, TOK_DOT))) {
            if (mc->cur_class_idx < 0 || mc->cur_obj_base < 0 || peek(mc)->kind != TOK_IDENT) {
                mc_error(mc, "Invalid 'this' access");
                return 0;
            }
            char mname[32];
            strncpy(mname, peek(mc)->text, sizeof(mname) - 1);
            mname[sizeof(mname) - 1] = '\0';
            mc->pos++;
            if (match(mc, TOK_LPAREN)) {
                int64_t args[12];
                const char *str_args[12];
                bool arg_is_str[12];
                struct mc_var *arg_vars[12];
                int argc = parse_call_args(mc, args, str_args, arg_is_str, arg_vars, 12);
                int midx = find_class_method(mc, mc->cur_class_idx, mname);
                if (midx < 0) { mc_error(mc, "Undefined method"); return 0; }
                return invoke_mc_func(mc, &mc->funcs[midx], argc, args, arg_is_str,
                                      mc->cur_obj_base, mc->cur_class_idx);
            }
            int fidx = find_class_field(&mc->classes[mc->cur_class_idx], mname);
            if (fidx < 0) { mc_error(mc, "Unknown member"); return 0; }
            struct mc_class_field *cf = &mc->classes[mc->cur_class_idx].fields[fidx];
            mc->last_is_str = cf->is_str;
            mc->last_is_char = cf->is_char;
            return mc->obj_pool[mc->cur_obj_base + fidx];
        }

        struct mc_var *v = find_var(mc, name);
        if (!v) {
            /* Implicit 'this' field lookup inside a method */
            if (mc->cur_class_idx >= 0 && mc->cur_obj_base >= 0) {
                int fidx = find_class_field(&mc->classes[mc->cur_class_idx], name);
                if (fidx >= 0) {
                    int64_t *slot = &mc->obj_pool[mc->cur_obj_base + fidx];
                    struct mc_class_field *cf = &mc->classes[mc->cur_class_idx].fields[fidx];
                    if (match(mc, TOK_PLUS_PLUS)) return (*slot)++;
                    if (match(mc, TOK_MINUS_MINUS)) return (*slot)--;
                    mc->last_is_str = cf->is_str;
                    mc->last_is_char = cf->is_char;
                    return *slot;
                }
            }
            mc_error(mc, "Undefined variable");
            return 0;
        }

        /* Array element access */
        if (match(mc, TOK_LBRACK)) {
            int64_t idx = eval_expr(mc);
            expect(mc, TOK_RBRACK, "Expected ']'");
            if (v->is_argv) {
                if (idx < 0 || idx >= mc->argv_count) {
                    mc_error(mc, "argv index out of bounds");
                    return 0;
                }
                mc->last_is_str = true;
                return (int64_t)(uintptr_t)mc->argv_ptrs[idx];
            }
            if (!v->is_array || idx < 0 || idx >= v->arr_size) {
                mc_error(mc, "Array index out of bounds");
                return 0;
            }
            if (match(mc, TOK_PLUS_PLUS)) return mc->arr_pool[v->arr_base + idx]++;
            if (match(mc, TOK_MINUS_MINUS)) return mc->arr_pool[v->arr_base + idx]--;
            mc->last_is_char = v->is_char;
            return mc->arr_pool[v->arr_base + idx];
        }

        /* Member access: obj.member, obj.method(), s.length(), ptr->method() */
        if (peek(mc)->kind == TOK_DOT || peek(mc)->kind == TOK_ARROW) {
            bool is_arrow = (peek(mc)->kind == TOK_ARROW);
            mc->pos++;
            if (peek(mc)->kind != TOK_IDENT) {
                mc_error(mc, "Expected member name");
                return 0;
            }
            char mname[32];
            strncpy(mname, peek(mc)->text, sizeof(mname) - 1);
            mname[sizeof(mname) - 1] = '\0';
            mc->pos++;

            if (match(mc, TOK_LPAREN)) {
                int64_t args[12];
                const char *str_args[12];
                bool arg_is_str[12];
                struct mc_var *arg_vars[12];
                int argc = parse_call_args(mc, args, str_args, arg_is_str, arg_vars, 12);

                if (v->is_str && (strcmp(mname, "length") == 0 || strcmp(mname, "size") == 0)) {
                    const char *s = (const char *)(uintptr_t)v->val;
                    return s ? (int64_t)strlen(s) : 0;
                }
                if (v->is_str && strcmp(mname, "empty") == 0) {
                    const char *s = (const char *)(uintptr_t)v->val;
                    return (!s || s[0] == '\0') ? 1 : 0;
                }
                if (v->is_str && strcmp(mname, "c_str") == 0) {
                    mc->last_is_str = true;
                    return v->val;
                }

                int cidx = -1, obase = 0;
                if (!resolve_obj_var(mc, v, is_arrow, &cidx, &obase)) {
                    mc_error(mc, "Method call on non-object");
                    return 0;
                }
                int midx = find_class_method(mc, cidx, mname);
                if (midx < 0) {
                    mc_error(mc, "Undefined class method");
                    return 0;
                }
                return invoke_mc_func(mc, &mc->funcs[midx], argc, args, arg_is_str, obase, cidx);
            }

            int cidx = -1, obase = 0;
            if (!resolve_obj_var(mc, v, is_arrow, &cidx, &obase)) {
                mc_error(mc, "Field access on non-object");
                return 0;
            }
            int fidx = find_class_field(&mc->classes[cidx], mname);
            if (fidx < 0) {
                mc_error(mc, "Unknown field");
                return 0;
            }
            int64_t *slot = &mc->obj_pool[obase + fidx];
            if (match(mc, TOK_PLUS_PLUS)) return (*slot)++;
            if (match(mc, TOK_MINUS_MINUS)) return (*slot)--;
            struct mc_class_field *cf = &mc->classes[cidx].fields[fidx];
            mc->last_is_str = cf->is_str;
            mc->last_is_char = cf->is_char;
            return *slot;
        }

        /* Char array decaying to C-string */
        if (v->is_array && v->is_char) {
            mc->last_is_str = true;
            return (int64_t)(uintptr_t)mc_char_arr_to_str(mc, v);
        }

        if (match(mc, TOK_PLUS_PLUS)) return v->val++;
        if (match(mc, TOK_MINUS_MINUS)) return v->val--;

        mc->last_is_str = v->is_str;
        mc->last_is_char = v->is_char;
        return v->val;
    }

    mc_error(mc, "Unexpected token in expression");
    return 0;
}

static bool lookahead_is_cast(struct mc_state *mc) {
    if (peek(mc)->kind != TOK_LPAREN) return false;
    enum tok_kind k1 = peek_at(mc, 1)->kind;
    if (!is_builtin_type_kw(k1)) return false;
    int off = 2;
    while (peek_at(mc, off)->kind == TOK_STAR) off++;
    return peek_at(mc, off)->kind == TOK_RPAREN;
}

static int64_t eval_unary(struct mc_state *mc) {
    if (lookahead_is_cast(mc)) {
        mc->pos++; /* '(' */
        consume_type_spec(mc, NULL, NULL, NULL);
        expect(mc, TOK_RPAREN, "Expected ')' after cast");
        return eval_unary(mc);
    }
    if (match(mc, TOK_MINUS)) {
        int64_t v = -eval_unary(mc);
        mc->last_is_str = false;
        mc->last_is_char = false;
        return v;
    }
    if (match(mc, TOK_PLUS)) return eval_unary(mc);
    if (match(mc, TOK_BANG)) {
        int64_t v = !eval_unary(mc);
        mc->last_is_str = false;
        mc->last_is_char = false;
        return v;
    }
    if (match(mc, TOK_TILDE)) {
        int64_t v = ~eval_unary(mc);
        mc->last_is_str = false;
        mc->last_is_char = false;
        return v;
    }
    if (match(mc, TOK_AMP)) {
        int64_t *slot = eval_lvalue(mc, NULL, NULL, NULL);
        if (!slot) {
            mc_error(mc, "Invalid '&' address-of target");
            return 0;
        }
        mc->last_is_str = false;
        mc->last_is_char = false;
        return (int64_t)(uintptr_t)slot;
    }
    if (match(mc, TOK_STAR)) {
        int64_t addr = eval_unary(mc);
        int64_t *p = (int64_t *)(uintptr_t)addr;
        if (!is_valid_mc_ptr(mc, p)) {
            mc_error(mc, "Invalid pointer dereference");
            return 0;
        }
        mc->last_is_str = false;
        mc->last_is_char = false;
        return *p;
    }
    if (match(mc, TOK_PLUS_PLUS)) {
        int64_t *slot = eval_lvalue(mc, NULL, NULL, NULL);
        if (!slot) { mc_error(mc, "Expected lvalue after ++"); return 0; }
        return ++(*slot);
    }
    if (match(mc, TOK_MINUS_MINUS)) {
        int64_t *slot = eval_lvalue(mc, NULL, NULL, NULL);
        if (!slot) { mc_error(mc, "Expected lvalue after --"); return 0; }
        return --(*slot);
    }
    return eval_primary(mc);
}

static int64_t eval_mul(struct mc_state *mc) {
    int64_t v = eval_unary(mc);
    bool v_str = mc->last_is_str;
    bool v_char = mc->last_is_char;
    while (!mc->has_error) {
        if (match(mc, TOK_STAR)) {
            v *= eval_unary(mc);
            v_str = false; v_char = false;
        } else if (match(mc, TOK_SLASH)) {
            int64_t d = eval_unary(mc);
            if (d == 0) { mc_error(mc, "Division by zero"); return 0; }
            v /= d;
            v_str = false; v_char = false;
        } else if (match(mc, TOK_PERCENT)) {
            int64_t d = eval_unary(mc);
            if (d == 0) { mc_error(mc, "Modulo by zero"); return 0; }
            v %= d;
            v_str = false; v_char = false;
        } else {
            break;
        }
    }
    mc->last_is_str = v_str;
    mc->last_is_char = v_char;
    return v;
}

static int64_t eval_add(struct mc_state *mc) {
    int64_t v = eval_mul(mc);
    bool v_str = mc->last_is_str;
    bool v_char = mc->last_is_char;

    while (!mc->has_error) {
        if (match(mc, TOK_PLUS)) {
            int64_t rhs = eval_mul(mc);
            bool r_str = mc->last_is_str;
            if (v_str || r_str) {
                const char *ls = v_str ? (const char *)(uintptr_t)v : "";
                const char *rs = r_str ? (const char *)(uintptr_t)rhs : "";
                if (v_str && r_str) snprintf(mc->tmp_str, sizeof(mc->tmp_str), "%s%s", ls, rs);
                else if (v_str) snprintf(mc->tmp_str, sizeof(mc->tmp_str), "%s%ld", ls, rhs);
                else snprintf(mc->tmp_str, sizeof(mc->tmp_str), "%ld%s", v, rs);
                v = (int64_t)(uintptr_t)mc_alloc_dyn_str(mc, mc->tmp_str);
                v_str = true;
                v_char = false;
            } else {
                v += rhs;
                v_char = false;
            }
        } else if (match(mc, TOK_MINUS)) {
            v -= eval_mul(mc);
            v_str = false;
            v_char = false;
        } else {
            break;
        }
    }
    mc->last_is_str = v_str;
    mc->last_is_char = v_char;
    return v;
}

static int64_t eval_shift(struct mc_state *mc) {
    int64_t v = eval_add(mc);
    while (!mc->has_error) {
        if (match(mc, TOK_SHL))      v <<= eval_add(mc);
        else if (match(mc, TOK_SHR)) v >>= eval_add(mc);
        else break;
        mc->last_is_str = false;
        mc->last_is_char = false;
    }
    return v;
}

static int64_t eval_rel(struct mc_state *mc) {
    int64_t v = eval_shift(mc);
    while (!mc->has_error) {
        if (match(mc, TOK_LT))      v = (v < eval_shift(mc));
        else if (match(mc, TOK_LE)) v = (v <= eval_shift(mc));
        else if (match(mc, TOK_GT)) v = (v > eval_shift(mc));
        else if (match(mc, TOK_GE)) v = (v >= eval_shift(mc));
        else break;
        mc->last_is_str = false;
        mc->last_is_char = false;
    }
    return v;
}

static int64_t eval_eq(struct mc_state *mc) {
    int64_t v = eval_rel(mc);
    bool v_str = mc->last_is_str;
    while (!mc->has_error) {
        bool is_eq = match(mc, TOK_EQ);
        if (!is_eq && !match(mc, TOK_NEQ)) break;
        int64_t r = eval_rel(mc);
        if (v_str && mc->last_is_str) {
            const char *a = (const char *)(uintptr_t)v;
            const char *b = (const char *)(uintptr_t)r;
            v = (a && b) ? (strcmp(a, b) == 0) : (a == b);
        } else {
            v = (v == r);
        }
        if (!is_eq) v = !v;
        v_str = false;
        mc->last_is_str = false;
        mc->last_is_char = false;
    }
    return v;
}

static int64_t eval_bitand(struct mc_state *mc) {
    int64_t v = eval_eq(mc);
    while (match(mc, TOK_AMP)) { v &= eval_eq(mc); mc->last_is_str = mc->last_is_char = false; }
    return v;
}

static int64_t eval_bitxor(struct mc_state *mc) {
    int64_t v = eval_bitand(mc);
    while (match(mc, TOK_CARET)) { v ^= eval_bitand(mc); mc->last_is_str = mc->last_is_char = false; }
    return v;
}

static int64_t eval_bitor(struct mc_state *mc) {
    int64_t v = eval_bitxor(mc);
    while (match(mc, TOK_PIPE)) { v |= eval_bitxor(mc); mc->last_is_str = mc->last_is_char = false; }
    return v;
}

static int64_t eval_logand(struct mc_state *mc) {
    int64_t v = eval_bitor(mc);
    while (match(mc, TOK_AND_AND)) {
        if (!v) {
            skip_expr_branch(mc, TOK_AND_AND);
            v = 0;
        } else {
            v = (v && eval_bitor(mc));
        }
        mc->last_is_str = false;
        mc->last_is_char = false;
    }
    return v;
}

static int64_t eval_logor(struct mc_state *mc) {
    int64_t v = eval_logand(mc);
    while (match(mc, TOK_OR_OR)) {
        if (v) {
            skip_expr_branch(mc, TOK_OR_OR);
            v = 1;
        } else {
            v = (v || eval_logand(mc));
        }
        mc->last_is_str = false;
        mc->last_is_char = false;
    }
    return v;
}

static int64_t eval_ternary(struct mc_state *mc) {
    int64_t cond = eval_logor(mc);
    if (!match(mc, TOK_QUESTION)) {
        return cond;
    }

    int64_t res = 0;
    bool res_str = false, res_char = false;
    if (cond) {
        res = eval_expr(mc);
        res_str = mc->last_is_str;
        res_char = mc->last_is_char;
        expect(mc, TOK_COLON, "Expected ':' in ternary expression");
        skip_expr_branch(mc, TOK_COLON);
    } else {
        skip_expr_branch(mc, TOK_QUESTION);
        expect(mc, TOK_COLON, "Expected ':' in ternary expression");
        res = eval_expr(mc);
        res_str = mc->last_is_str;
        res_char = mc->last_is_char;
    }
    mc->last_is_str = res_str;
    mc->last_is_char = res_char;
    return res;
}

static bool is_assign_op(enum tok_kind k) {
    return k == TOK_ASSIGN || k == TOK_PLUS_EQ || k == TOK_MINUS_EQ ||
           k == TOK_STAR_EQ || k == TOK_SLASH_EQ || k == TOK_PERCENT_EQ;
}

static int64_t eval_expr(struct mc_state *mc) {
    if (mc->has_error) return 0;

    enum tok_kind k0 = peek(mc)->kind;
    if (k0 == TOK_STAR ||
        (k0 == TOK_IDENT && peek_at(mc, 1)->kind != TOK_LPAREN)) {
        int saved = mc->pos;
        bool lv_str = false, lv_char = false;
        struct mc_var *lv_var = NULL;
        int64_t *slot = eval_lvalue(mc, &lv_str, &lv_char, &lv_var);
        if (slot && is_assign_op(peek(mc)->kind)) {
            enum tok_kind op = peek(mc)->kind;
            mc->pos++;
            int64_t rhs = eval_expr(mc);
            bool r_str = mc->last_is_str;

            if (op == TOK_ASSIGN) {
                if (lv_str && r_str) {
                    *slot = (int64_t)(uintptr_t)mc_alloc_dyn_str(mc, (const char *)(uintptr_t)rhs);
                } else {
                    *slot = rhs;
                }
            } else if (op == TOK_PLUS_EQ) {
                if (lv_str) {
                    const char *ls = (const char *)(uintptr_t)(*slot);
                    const char *rs = r_str ? (const char *)(uintptr_t)rhs : "";
                    if (r_str) snprintf(mc->tmp_str, sizeof(mc->tmp_str), "%s%s", ls ? ls : "", rs ? rs : "");
                    else snprintf(mc->tmp_str, sizeof(mc->tmp_str), "%s%ld", ls ? ls : "", rhs);
                    *slot = (int64_t)(uintptr_t)mc_alloc_dyn_str(mc, mc->tmp_str);
                } else {
                    *slot += rhs;
                }
            } else if (op == TOK_MINUS_EQ) {
                *slot -= rhs;
            } else if (op == TOK_STAR_EQ) {
                *slot *= rhs;
            } else if (op == TOK_SLASH_EQ) {
                if (rhs == 0) { mc_error(mc, "Division by zero"); return 0; }
                *slot /= rhs;
            } else if (op == TOK_PERCENT_EQ) {
                if (rhs == 0) { mc_error(mc, "Modulo by zero"); return 0; }
                *slot %= rhs;
            }
            mc->last_is_str = lv_str;
            mc->last_is_char = lv_char;
            return *slot;
        }
        mc->has_error = false;
        mc->pos = saved;
    }

    return eval_ternary(mc);
}

static void exec_var_decl(struct mc_state *mc, bool consume_semi) {
    int class_idx = -1;
    bool type_is_str = false;
    bool type_is_char = false;

    if (!consume_type_spec(mc, &class_idx, &type_is_str, &type_is_char)) {
        mc_error(mc, "Expected type specifier");
        return;
    }

    do {
        while (match(mc, TOK_STAR) || match(mc, TOK_AMP)) {}
        if (peek(mc)->kind != TOK_IDENT) {
            mc_error(mc, "Expected variable name");
            return;
        }
        char name[32];
        strncpy(name, peek(mc)->text, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        mc->pos++;

        if (class_idx >= 0) {
            struct mc_class *cls = &mc->classes[class_idx];
            if (mc->obj_used + cls->field_count > MC_OBJ_POOL) {
                mc_error(mc, "Object pool full");
                return;
            }
            struct mc_var *v = declare_var(mc, name, 0);
            if (!v) return;
            v->is_obj = true;
            v->class_idx = class_idx;
            v->obj_base = mc->obj_used;
            for (int f = 0; f < cls->field_count; f++) {
                mc->obj_pool[mc->obj_used++] = 0;
            }

            if (match(mc, TOK_LPAREN)) {
                int64_t args[12];
                const char *str_args[12];
                bool arg_is_str[12];
                struct mc_var *arg_vars[12];
                int argc = parse_call_args(mc, args, str_args, arg_is_str, arg_vars, 12);
                int ctor = find_class_method(mc, class_idx, cls->name);
                if (ctor >= 0) {
                    invoke_mc_func(mc, &mc->funcs[ctor], argc, args, arg_is_str, v->obj_base, class_idx);
                }
            } else if (match(mc, TOK_ASSIGN)) {
                if (match(mc, TOK_LBRACE)) {
                    int f = 0;
                    if (!match(mc, TOK_RBRACE)) {
                        do {
                            int64_t fv = eval_expr(mc);
                            if (f < cls->field_count) mc->obj_pool[v->obj_base + f] = fv;
                            f++;
                        } while (match(mc, TOK_COMMA));
                        expect(mc, TOK_RBRACE, "Expected '}'");
                    }
                } else {
                    int64_t rhs = eval_expr(mc);
                    (void)rhs;
                }
            }
        } else if (match(mc, TOK_LBRACK)) {
            int64_t sz = -1;
            if (!match(mc, TOK_RBRACK)) {
                sz = eval_expr(mc);
                expect(mc, TOK_RBRACK, "Expected ']'");
                if (sz <= 0 || sz > MC_MAX_ARR) {
                    mc_error(mc, "Invalid array size");
                    return;
                }
            }
            struct mc_var *v = declare_var(mc, name, 0);
            if (!v) return;
            v->is_array = true;
            v->is_char = type_is_char;
            v->arr_base = mc->arr_used;

            if (match(mc, TOK_ASSIGN)) {
                if (peek(mc)->kind == TOK_STR_LIT && type_is_char) {
                    const char *lit = mc->str_pool[peek(mc)->ival];
                    mc->pos++;
                    int slen = (int)strlen(lit) + 1;
                    if (sz < 0) sz = slen;
                    if (sz <= 0 || sz > MC_MAX_ARR || mc->arr_used + sz > MC_ARR_POOL) {
                        mc_error(mc, "Array pool full");
                        return;
                    }
                    v->arr_size = (int)sz;
                    mc->arr_used += (int)sz;
                    for (int i = 0; i < v->arr_size; i++) {
                        mc->arr_pool[v->arr_base + i] = (i < slen) ? (uint8_t)lit[i] : 0;
                    }
                } else {
                    expect(mc, TOK_LBRACE, "Expected '{' in array initializer");
                    int count = 0;
                    if (!match(mc, TOK_RBRACE)) {
                        do {
                            int64_t ev = eval_expr(mc);
                            if (count < MC_MAX_ARR && v->arr_base + count < MC_ARR_POOL) {
                                mc->arr_pool[v->arr_base + count] = ev;
                            }
                            if (count < MC_MAX_ARR) count++;
                        } while (match(mc, TOK_COMMA));
                        expect(mc, TOK_RBRACE, "Expected '}'");
                    }
                    if (sz < 0) sz = (count > 0) ? count : 1;
                    if (sz > MC_MAX_ARR || v->arr_base + sz > MC_ARR_POOL) {
                        mc_error(mc, "Array pool full");
                        return;
                    }
                    v->arr_size = (int)sz;
                    mc->arr_used += (int)sz;
                    for (int i = count; i < v->arr_size; i++) {
                        mc->arr_pool[v->arr_base + i] = 0;
                    }
                }
            } else {
                if (sz <= 0 || mc->arr_used + sz > MC_ARR_POOL) {
                    mc_error(mc, "Array size required or pool full");
                    return;
                }
                v->arr_size = (int)sz;
                mc->arr_used += (int)sz;
                for (int i = 0; i < v->arr_size; i++) mc->arr_pool[v->arr_base + i] = 0;
            }
        } else {
            int64_t init_val = type_is_str ? (int64_t)(uintptr_t)mc_alloc_dyn_str(mc, "") : 0;
            bool init_is_str = type_is_str;
            if (match(mc, TOK_ASSIGN)) {
                mc->last_is_str = false;
                init_val = eval_expr(mc);
                if (type_is_str && mc->last_is_str) {
                    init_val = (int64_t)(uintptr_t)mc_alloc_dyn_str(mc, (const char *)(uintptr_t)init_val);
                    init_is_str = true;
                }
            } else if (type_is_str && match(mc, TOK_LPAREN)) {
                if (peek(mc)->kind != TOK_RPAREN) {
                    init_val = eval_expr(mc);
                    if (mc->last_is_str) {
                        init_val = (int64_t)(uintptr_t)mc_alloc_dyn_str(mc, (const char *)(uintptr_t)init_val);
                    }
                }
                expect(mc, TOK_RPAREN, "Expected ')'");
                init_is_str = true;
            }
            struct mc_var *v = declare_var(mc, name, init_val);
            if (v) {
                v->is_str = init_is_str;
                v->is_char = type_is_char;
            }
        }
    } while (match(mc, TOK_COMMA));

    if (consume_semi) expect(mc, TOK_SEMI, "Expected ';'");
}

static void exec_cout_stmt(struct mc_state *mc) {
    mc->pos++; /* consume 'cout' or 'cerr' */
    while (match(mc, TOK_SHL)) {
        if (peek(mc)->kind == TOK_IDENT && strcmp(peek(mc)->text, "endl") == 0) {
            mc->pos++;
            uart_putc('\n');
        } else {
            mc->last_is_str = false;
            mc->last_is_char = false;
            int64_t v = eval_add(mc);
            if (mc->has_error) return;
            if (mc->last_is_str) {
                kprintf("%s", v ? (const char *)(uintptr_t)v : "");
            } else if (mc->last_is_char) {
                uart_putc((char)v);
            } else {
                kprintf("%ld", v);
            }
        }
    }
    expect(mc, TOK_SEMI, "Expected ';' after cout statement");
}

static void exec_cin_stmt(struct mc_state *mc) {
    mc->pos++; /* consume 'cin' */
    while (match(mc, TOK_SHR)) {
        bool lv_str = false, lv_char = false;
        struct mc_var *lv_var = NULL;
        int64_t *slot = eval_lvalue(mc, &lv_str, &lv_char, &lv_var);
        if (!slot) {
            mc_error(mc, "Expected variable after 'cin >>'");
            return;
        }
        char tok[MC_STR_LEN];
        mc_read_token(tok, sizeof(tok));
        if (lv_str) {
            *slot = (int64_t)(uintptr_t)mc_alloc_dyn_str(mc, tok);
        } else if (lv_char) {
            *slot = (uint8_t)tok[0];
        } else {
            *slot = mc_parse_int64(tok);
        }
    }
    expect(mc, TOK_SEMI, "Expected ';' after cin statement");
}

static void exec_stmt(struct mc_state *mc) {
    if (mc->has_error || mc->returning || mc->breaking || mc->continuing) return;

    if (match(mc, TOK_LBRACE)) {
        mc->scope_depth++;
        int my_depth = mc->scope_depth;
        while (!mc->has_error && !mc->returning && !mc->breaking && !mc->continuing &&
               peek(mc)->kind != TOK_RBRACE && peek(mc)->kind != TOK_EOF) {
            exec_stmt(mc);
        }
        if (mc->returning || mc->breaking || mc->continuing) {
            int brace_depth = 1;
            while (peek(mc)->kind != TOK_EOF && brace_depth > 0) {
                if (peek(mc)->kind == TOK_LBRACE) brace_depth++;
                else if (peek(mc)->kind == TOK_RBRACE) brace_depth--;
                mc->pos++;
            }
        } else {
            expect(mc, TOK_RBRACE, "Expected '}'");
        }
        pop_scope(mc, my_depth);
        mc->scope_depth--;
        return;
    }

    if (match(mc, TOK_KW_USING)) {
        while (peek(mc)->kind != TOK_EOF && peek(mc)->kind != TOK_SEMI) mc->pos++;
        expect(mc, TOK_SEMI, "Expected ';'");
        return;
    }

    if (match(mc, TOK_KW_DELETE)) {
        match(mc, TOK_LBRACK);
        match(mc, TOK_RBRACK);
        (void)eval_expr(mc);
        expect(mc, TOK_SEMI, "Expected ';'");
        return;
    }

    if (peek(mc)->kind == TOK_IDENT &&
        (strcmp(peek(mc)->text, "cout") == 0 || strcmp(peek(mc)->text, "cerr") == 0) &&
        peek_at(mc, 1)->kind == TOK_SHL) {
        exec_cout_stmt(mc);
        return;
    }

    if (peek(mc)->kind == TOK_IDENT &&
        strcmp(peek(mc)->text, "cin") == 0 &&
        peek_at(mc, 1)->kind == TOK_SHR) {
        exec_cin_stmt(mc);
        return;
    }

    if (is_type_start(mc)) {
        exec_var_decl(mc, true);
        return;
    }

    if (match(mc, TOK_KW_RETURN)) {
        int64_t rv = 0;
        if (peek(mc)->kind != TOK_SEMI) rv = eval_expr(mc);
        expect(mc, TOK_SEMI, "Expected ';' after return");
        mc->ret_val = rv;
        mc->returning = true;
        return;
    }

    if (match(mc, TOK_KW_BREAK)) {
        expect(mc, TOK_SEMI, "Expected ';'");
        mc->breaking = true;
        return;
    }

    if (match(mc, TOK_KW_CONTINUE)) {
        expect(mc, TOK_SEMI, "Expected ';'");
        mc->continuing = true;
        return;
    }

    if (match(mc, TOK_KW_IF)) {
        expect(mc, TOK_LPAREN, "Expected '(' after if");
        int64_t cond = eval_expr(mc);
        expect(mc, TOK_RPAREN, "Expected ')' after if cond");
        if (cond) {
            exec_stmt(mc);
            if (!mc->has_error && match(mc, TOK_KW_ELSE)) skip_stmt(mc);
        } else {
            skip_stmt(mc);
            if (!mc->has_error && match(mc, TOK_KW_ELSE)) exec_stmt(mc);
        }
        return;
    }

    if (match(mc, TOK_KW_WHILE)) {
        expect(mc, TOK_LPAREN, "Expected '(' after while");
        int cond_pos = mc->pos;
        int iter = 0;
        for (;;) {
            mc->pos = cond_pos;
            int64_t cond = eval_expr(mc);
            expect(mc, TOK_RPAREN, "Expected ')'");
            if (!cond || mc->has_error) {
                skip_stmt(mc);
                break;
            }
            int body_pos = mc->pos;
            exec_stmt(mc);
            if (mc->returning || mc->has_error) break;
            if (mc->breaking) {
                mc->breaking = false;
                mc->pos = body_pos;
                skip_stmt(mc);
                break;
            }
            mc->continuing = false;
            if (++iter > 100000) {
                mc_error(mc, "While loop iteration limit exceeded");
                break;
            }
        }
        return;
    }

    if (match(mc, TOK_KW_DO)) {
        int body_pos = mc->pos;
        int iter = 0;
        for (;;) {
            mc->pos = body_pos;
            exec_stmt(mc);
            if (mc->returning || mc->has_error) break;
            if (mc->breaking) {
                mc->breaking = false;
                mc->pos = body_pos;
                skip_stmt(mc);
                match(mc, TOK_KW_WHILE);
                skip_balanced(mc, TOK_LPAREN, TOK_RPAREN);
                expect(mc, TOK_SEMI, "Expected ';' after do-while");
                break;
            }
            mc->continuing = false;
            expect(mc, TOK_KW_WHILE, "Expected 'while' after 'do'");
            expect(mc, TOK_LPAREN, "Expected '(' after while");
            int64_t cond = eval_expr(mc);
            expect(mc, TOK_RPAREN, "Expected ')'");
            expect(mc, TOK_SEMI, "Expected ';'");
            if (!cond || mc->has_error) break;
            if (++iter > 100000) {
                mc_error(mc, "Do-while loop iteration limit exceeded");
                break;
            }
        }
        return;
    }

    if (match(mc, TOK_KW_FOR)) {
        expect(mc, TOK_LPAREN, "Expected '(' after for");
        mc->scope_depth++;
        int my_depth = mc->scope_depth;

        if (is_type_start(mc)) {
            exec_var_decl(mc, true);
        } else if (!match(mc, TOK_SEMI)) {
            eval_expr(mc);
            expect(mc, TOK_SEMI, "Expected ';' in for init");
        }

        int cond_pos = mc->pos;
        while (peek(mc)->kind != TOK_EOF && peek(mc)->kind != TOK_SEMI) mc->pos++;
        expect(mc, TOK_SEMI, "Expected ';' after for cond");
        int step_pos = mc->pos;
        int paren_depth = 1;
        while (peek(mc)->kind != TOK_EOF && paren_depth > 0) {
            if (peek(mc)->kind == TOK_LPAREN) paren_depth++;
            else if (peek(mc)->kind == TOK_RPAREN) paren_depth--;
            if (paren_depth > 0) mc->pos++;
        }
        expect(mc, TOK_RPAREN, "Expected ')' after for step");
        int body_pos = mc->pos;

        int iter = 0;
        for (;;) {
            mc->pos = cond_pos;
            int64_t cond = 1;
            if (peek(mc)->kind != TOK_SEMI) cond = eval_expr(mc);
            if (!cond || mc->has_error) {
                mc->pos = body_pos;
                skip_stmt(mc);
                break;
            }
            mc->pos = body_pos;
            exec_stmt(mc);
            if (mc->returning || mc->has_error) break;
            if (mc->breaking) {
                mc->breaking = false;
                mc->pos = body_pos;
                skip_stmt(mc);
                break;
            }
            mc->continuing = false;

            mc->pos = step_pos;
            if (peek(mc)->kind != TOK_RPAREN) eval_expr(mc);

            if (++iter > 100000) {
                mc_error(mc, "For loop iteration limit exceeded");
                break;
            }
        }

        pop_scope(mc, my_depth);
        mc->scope_depth--;
        return;
    }

    if (match(mc, TOK_SEMI)) return;

    eval_expr(mc);
    expect(mc, TOK_SEMI, "Expected ';'");
}

static bool parse_param_list(struct mc_state *mc, struct mc_func *fn) {
    if (match(mc, TOK_RPAREN)) return true;

    do {
        bool p_str = false, p_char = false;
        int p_cls = -1;
        if (is_type_start(mc)) {
            consume_type_spec(mc, &p_cls, &p_str, &p_char);
        }
        while (match(mc, TOK_STAR) || match(mc, TOK_AMP)) {}
        if (peek(mc)->kind == TOK_IDENT) {
            if (fn->param_count < 8) {
                int idx = fn->param_count++;
                strncpy(fn->param_names[idx], peek(mc)->text, 31);
                fn->param_names[idx][31] = '\0';
                fn->param_is_str[idx] = p_str;
                fn->param_is_char[idx] = p_char;
            }
            mc->pos++;
            if (match(mc, TOK_LBRACK)) {
                while (peek(mc)->kind != TOK_EOF && peek(mc)->kind != TOK_RBRACK) mc->pos++;
                match(mc, TOK_RBRACK);
            }
        }
    } while (match(mc, TOK_COMMA));

    return expect(mc, TOK_RPAREN, "Expected ')' in function parameters");
}

static bool parse_class_method(struct mc_state *mc, struct mc_class *cls,
                               int cidx, const char *mname) {
    if (mc->func_count >= MC_MAX_FUNCS || cls->method_count >= MC_MAX_METHODS) {
        mc_error(mc, "Too many methods");
        return false;
    }
    int fidx = mc->func_count++;
    struct mc_func *fn = &mc->funcs[fidx];
    memset(fn, 0, sizeof(*fn));
    fn->class_idx = cidx;
    strncpy(fn->name, mname, sizeof(fn->name) - 1);
    mc->pos += 2; /* name + '(' */
    parse_param_list(mc, fn);
    fn->body_tok = mc->pos;
    skip_balanced(mc, TOK_LBRACE, TOK_RBRACE);
    cls->method_funcs[cls->method_count++] = fidx;
    return true;
}

static bool parse_class_def(struct mc_state *mc) {
    mc->pos++; /* 'struct' or 'class' */
    if (peek(mc)->kind != TOK_IDENT) {
        mc_error(mc, "Expected struct/class name");
        return false;
    }
    if (mc->class_count >= MC_MAX_CLASSES) {
        mc_error(mc, "Too many struct/class definitions");
        return false;
    }
    int cidx = mc->class_count++;
    struct mc_class *cls = &mc->classes[cidx];
    memset(cls, 0, sizeof(*cls));
    strncpy(cls->name, peek(mc)->text, sizeof(cls->name) - 1);
    mc->pos++; /* name */

    expect(mc, TOK_LBRACE, "Expected '{' in struct/class body");

    while (peek(mc)->kind != TOK_EOF && peek(mc)->kind != TOK_RBRACE && !mc->has_error) {
        if (match(mc, TOK_KW_PUBLIC) || match(mc, TOK_KW_PRIVATE) || match(mc, TOK_KW_PROTECTED)) {
            match(mc, TOK_COLON);
            continue;
        }
        /* Constructor: ClassName(...) { ... } */
        if (peek(mc)->kind == TOK_IDENT && strcmp(peek(mc)->text, cls->name) == 0 &&
            peek_at(mc, 1)->kind == TOK_LPAREN) {
            if (!parse_class_method(mc, cls, cidx, cls->name)) return false;
            continue;
        }

        bool f_str = false, f_char = false;
        int f_cls = -1;
        if (!consume_type_spec(mc, &f_cls, &f_str, &f_char)) {
            mc_error(mc, "Expected member declaration inside struct/class");
            return false;
        }

        if (peek(mc)->kind == TOK_IDENT && peek_at(mc, 1)->kind == TOK_LPAREN) {
            if (!parse_class_method(mc, cls, cidx, peek(mc)->text)) return false;
        } else {
            /* Member fields: int a, b; */
            do {
                while (match(mc, TOK_STAR)) {}
                if (peek(mc)->kind != TOK_IDENT) {
                    mc_error(mc, "Expected field name");
                    return false;
                }
                if (cls->field_count >= MC_MAX_FIELDS) {
                    mc_error(mc, "Too many fields in struct/class");
                    return false;
                }
                struct mc_class_field *cf = &cls->fields[cls->field_count++];
                strncpy(cf->name, peek(mc)->text, sizeof(cf->name) - 1);
                cf->is_str = f_str;
                cf->is_char = f_char;
                mc->pos++;
                if (match(mc, TOK_LBRACK)) {
                    while (peek(mc)->kind != TOK_EOF && peek(mc)->kind != TOK_RBRACK) mc->pos++;
                    match(mc, TOK_RBRACK);
                }
                if (match(mc, TOK_ASSIGN)) {
                    skip_expr_branch(mc, TOK_ASSIGN);
                }
            } while (match(mc, TOK_COMMA));
            expect(mc, TOK_SEMI, "Expected ';' after field declaration");
        }
    }

    expect(mc, TOK_RBRACE, "Expected '}' at end of struct/class");
    match(mc, TOK_SEMI);
    return !mc->has_error;
}

static bool parse_top_level(struct mc_state *mc) {
    mc->pos = 0;
    mc->cur_obj_base = -1;
    mc->cur_class_idx = -1;

    while (peek(mc)->kind != TOK_EOF && !mc->has_error) {
        if (match(mc, TOK_KW_USING)) {
            while (peek(mc)->kind != TOK_EOF && peek(mc)->kind != TOK_SEMI) mc->pos++;
            expect(mc, TOK_SEMI, "Expected ';'");
            continue;
        }
        if ((peek(mc)->kind == TOK_KW_STRUCT || peek(mc)->kind == TOK_KW_CLASS) &&
            peek_at(mc, 1)->kind == TOK_IDENT &&
            peek_at(mc, 2)->kind == TOK_LBRACE) {
            if (!parse_class_def(mc)) return false;
            continue;
        }

        if (!is_type_start(mc)) {
            mc_error(mc, "Expected type or declaration at top level");
            return false;
        }

        int saved = mc->pos;
        consume_type_spec(mc, NULL, NULL, NULL);
        if (peek(mc)->kind == TOK_IDENT && peek_at(mc, 1)->kind == TOK_LPAREN) {
            if (mc->func_count >= MC_MAX_FUNCS) {
                mc_error(mc, "Too many functions");
                return false;
            }
            struct mc_func *fn = &mc->funcs[mc->func_count++];
            memset(fn, 0, sizeof(*fn));
            fn->class_idx = -1;
            strncpy(fn->name, peek(mc)->text, sizeof(fn->name) - 1);
            mc->pos += 2; /* ident + '(' */
            parse_param_list(mc, fn);
            if (peek(mc)->kind == TOK_LBRACE) {
                fn->body_tok = mc->pos;
                skip_balanced(mc, TOK_LBRACE, TOK_RBRACE);
            } else {
                mc->func_count--; /* forward declaration */
                expect(mc, TOK_SEMI, "Expected ';' or '{'");
            }
        } else {
            mc->pos = saved;
            exec_var_decl(mc, true);
        }
    }

    mc->global_var_end = mc->var_count;
    mc->local_var_floor = mc->var_count;
    return !mc->has_error;
}

static void mc_cleanup(struct mc_state *mc) {
    if (!mc) return;
    for (int i = 0; i < mc->alloc_count; i++) {
        if (mc->allocs[i]) kfree(mc->allocs[i]);
    }
    kfree(mc);
}

int minic_eval_source(const char *source, const char *filename, int argc, char **argv) {
    struct mc_state *mc = (struct mc_state *)kcalloc(1, sizeof(struct mc_state));
    if (!mc) return -ENOMEM;

    if (!mc_tokenize(mc, source)) {
        kprintf(ANSI_RED "[Compile Error] %s: %s\n" ANSI_RESET, filename, mc->err_msg);
        mc_cleanup(mc);
        return -EINVAL;
    }

    if (!parse_top_level(mc)) {
        kprintf(ANSI_RED "[Parse Error] %s: %s\n" ANSI_RESET, filename, mc->err_msg);
        mc_cleanup(mc);
        return -EINVAL;
    }

    if (!find_func(mc, "main")) {
        kprintf(ANSI_RED "[Link Error] %s: Undefined reference to 'main'\n" ANSI_RESET, filename);
        mc_cleanup(mc);
        return -EINVAL;
    }

    mc->argv_count = (argc > MC_MAX_ARGV) ? MC_MAX_ARGV : argc;
    for (int i = 0; i < mc->argv_count; i++) {
        mc->argv_ptrs[i] = argv[i];
    }

    struct mc_func *main_fn = find_func(mc, "main");
    int64_t main_args[2] = { (int64_t)argc, (int64_t)(uintptr_t)mc->argv_ptrs };
    bool main_is_str[2] = { false, false };
    int64_t ret = invoke_mc_func(mc, main_fn, 2, main_args, main_is_str, -1, -1);

    if (mc->has_error) {
        kprintf(ANSI_RED "[Runtime Error] %s: %s\n" ANSI_RESET, filename, mc->err_msg);
        mc_cleanup(mc);
        return -EINVAL;
    }

    kprintf(ANSI_GREEN "[Mini-C] %s exited with status %ld\n" ANSI_RESET, filename, ret);
    mc_cleanup(mc);
    return (int)ret;
}

int minic_run_file(const char *path, int argc, char **argv) {
    struct vfs_node *node = vfs_resolve(path);
    if (!node) return -ENOENT;
    if (node->type != VFS_FILE) return -EISDIR;

    char *buf = (char *)kmalloc(node->size + 1);
    if (!buf) return -ENOMEM;
    if (node->size > 0 && node->data) {
        memcpy(buf, node->data, node->size);
    }
    buf[node->size] = '\0';

    int rc = minic_eval_source(buf, path, argc, argv);
    kfree(buf);
    return rc;
}

static bool str_ends_with(const char *s, const char *sfx) {
    size_t sl = strlen(s);
    size_t xl = strlen(sfx);
    if (sl < xl) return false;
    return strcmp(s + sl - xl, sfx) == 0;
}

int minic_compile_command(int argc, char **argv, bool is_cpp) {
    const char *tool = (argc > 0 && argv[0]) ? argv[0] : (is_cpp ? "g++" : "gcc");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            kprintf("%s (FulmenOS Built-in AArch64 %s Compiler) 13.2.0\n",
                    tool, is_cpp ? "C++20" : "C17");
            kprintf("Target: aarch64-fulmenos | Supports C structs, pointers, and C++ classes/iostream\n");
            return 0;
        }
    }

    const char *src_path = NULL;
    const char *out_path = NULL;
    bool has_o_flag = false;
    bool compile_only = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-o") == 0) {
            has_o_flag = true;
            if (i + 1 >= argc) {
                kprintf("%s: error: missing filename after '-o'\n", tool);
                return -EINVAL;
            }
            out_path = argv[++i];
        } else if (strncmp(a, "-o", 2) == 0 && a[2] != '\0') {
            has_o_flag = true;
            out_path = a + 2;
        } else if (strcmp(a, "-c") == 0) {
            compile_only = true;
        } else if (a[0] == '-') {
            /* Skip common GCC/G++ flags (-Wall, -Wextra, -O2, -g, -std=..., -lm, -I..., etc.) */
            continue;
        } else if (!src_path) {
            src_path = a;
        }
    }

    if (!src_path) {
        kprintf("%s: fatal error: no input files\n", tool);
        kprintf("Usage: %s <source.c|source.cpp> [-o <output>] [-Wall] [-O2]\n", tool);
        return -EINVAL;
    }

    struct vfs_node *src_node = vfs_resolve(src_path);
    if (!src_node) {
        kprintf("%s: error: %s: No such file or directory\n", tool, src_path);
        return -ENOENT;
    }
    if (src_node->type != VFS_FILE) {
        kprintf("%s: error: %s: Is a directory\n", tool, src_path);
        return -EISDIR;
    }

    bool detected_cpp = is_cpp || str_ends_with(src_path, ".cpp") ||
                        str_ends_with(src_path, ".cc") || str_ends_with(src_path, ".cxx");
    (void)detected_cpp;

    char *src_buf = (char *)kmalloc(src_node->size + 1U);
    if (!src_buf) return -ENOMEM;
    if (src_node->size > 0 && src_node->data) {
        memcpy(src_buf, src_node->data, src_node->size);
    }
    src_buf[src_node->size] = '\0';

    /* Validate syntax, parse top-level declarations, and check main() */
    struct mc_state *mc = (struct mc_state *)kcalloc(1, sizeof(struct mc_state));
    if (!mc) {
        kfree(src_buf);
        return -ENOMEM;
    }

    if (!mc_tokenize(mc, src_buf)) {
        kprintf(ANSI_RED "%s: error in %s: %s\n" ANSI_RESET, tool, src_path, mc->err_msg);
        mc_cleanup(mc);
        kfree(src_buf);
        return -EINVAL;
    }
    if (!parse_top_level(mc)) {
        kprintf(ANSI_RED "%s: parse error in %s: %s\n" ANSI_RESET, tool, src_path, mc->err_msg);
        mc_cleanup(mc);
        kfree(src_buf);
        return -EINVAL;
    }
    if (!compile_only && !find_func(mc, "main")) {
        kprintf(ANSI_RED "%s: link error in %s: undefined reference to 'main'\n" ANSI_RESET, tool, src_path);
        mc_cleanup(mc);
        kfree(src_buf);
        return -EINVAL;
    }
    mc_cleanup(mc);

    /* 'cc file.c' without -o keeps immediate execution without writing a.out unless gcc/g++ is used */
    if (strcmp(tool, "cc") == 0 && !has_o_flag && !compile_only) {
        int rc = minic_eval_source(src_buf, src_path, 1, (char **)&src_path);
        kfree(src_buf);
        return rc;
    }

    /* Write FulmenOS executable binary (#!fulmen-c\n + source) */
    const char *target = out_path ? out_path : "a.out";
    size_t hdr_len = 11U; /* strlen("#!fulmen-c\n") */
    char *bin_buf = (char *)kmalloc(hdr_len + src_node->size + 1U);
    if (!bin_buf) {
        kfree(src_buf);
        return -ENOMEM;
    }
    memcpy(bin_buf, "#!fulmen-c\n", hdr_len);
    memcpy(bin_buf + hdr_len, src_buf, src_node->size);
    bin_buf[hdr_len + src_node->size] = '\0';

    int wrc = vfs_write(target, bin_buf, hdr_len + src_node->size, false);
    kfree(bin_buf);
    if (wrc != 0) {
        kprintf("%s: error: cannot write output '%s' (err=%d)\n", tool, target, wrc);
        kfree(src_buf);
        return wrc;
    }

    struct vfs_node *out_node = vfs_resolve(target);
    if (out_node) out_node->mode = 0755U;

    kprintf(ANSI_GREEN "[%s]" ANSI_RESET " Compiled %s -> %s (%lu bytes)\n",
            tool, src_path, target, (uint64_t)(hdr_len + src_node->size));

    if (!has_o_flag && !compile_only) {
        char *run_argv[1] = { (char *)target };
        int rc = minic_eval_source(src_buf, target, 1, run_argv);
        kfree(src_buf);
        return rc;
    }

    kfree(src_buf);
    return 0;
}
