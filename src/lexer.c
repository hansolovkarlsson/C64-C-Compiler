/*
 * lexer.c - turns raw source text into a flat array of Tokens.
 *
 * This is a classic hand-written lexer: one pass over the source
 * string, character by character, deciding what kind of token starts
 * at the current position by looking at just the first character (and
 * sometimes one or two more, for things like distinguishing `<` from
 * `<=` from `<<` from `<<=`). There's no separate "lexer state
 * machine" table - the control flow in lex() below *is* the state
 * machine, which is normal for a lexer this size and much easier to
 * follow than a generated one.
 *
 * cc64 tokenizes the *entire* file up front into g_toks[], rather than
 * producing one token at a time on demand. That's what makes the
 * two-pass parsing strategy in parser.c possible: pass_a() can do a
 * quick scan over all the tokens to record every function signature,
 * then pass_b() can rewind (g_pos = 0) and parse the whole thing again
 * for real, this time with full knowledge of every function that
 * exists - without re-reading or re-scanning the source file itself.
 */

#include "cc64.h"

/* ===================================================================
 * The token array
 * =================================================================== */

Token *g_toks = NULL;
int g_ntoks = 0;
static int g_tokcap = 0; /* only tok_push needs to know the allocated capacity */

/* Classic growable-array append: double the capacity whenever it fills
 * up, so appending n tokens overall does O(log n) reallocations rather
 * than O(n) of them. */
static void tok_push(Token t) {
    if (g_ntoks >= g_tokcap) {
        g_tokcap = g_tokcap ? g_tokcap * 2 : 4096;
        g_toks = xrealloc(g_toks, g_tokcap * sizeof(Token));
    }
    g_toks[g_ntoks++] = t;
}

static int is_id_start(int c) { return isalpha(c) || c == '_'; }
static int is_id_char(int c)  { return isalnum(c) || c == '_'; }

/* ===================================================================
 * #include support
 * ===================================================================
 * See cc64.h's "HOW #include WORKS" for the design in one paragraph.
 * Everything below is plain path-string manipulation, kept dependency-
 * free on purpose: no libgen.h dirname() (not part of C99), no
 * realpath() (POSIX, and would need a Windows equivalent) - just
 * enough '/' splitting to find files relative to each other, which is
 * all a small, single-platform-convention tool like this needs. Paths
 * are always compared and joined as given, not canonicalized, so
 * `#include "foo.h"` and `#include "./foo.h"` would (rarely, and
 * harmlessly beyond a little wasted work) be treated as different
 * files if a program mixed both spellings.
 * =================================================================== */

#define MAX_INCLUDE_DIRS 32
static char *g_include_dirs[MAX_INCLUDE_DIRS];
static int g_nincludedirs = 0;

void add_include_dir(const char *dir) {
    if (g_nincludedirs >= MAX_INCLUDE_DIRS) fatal(0, "too many -I include directories");
    g_include_dirs[g_nincludedirs++] = xstrdup(dir);
}

#define MAX_INCLUDED_FILES 256
static char *g_included[MAX_INCLUDED_FILES];
static int g_nincluded = 0;

static int already_included(const char *resolved_path) {
    for (int i = 0; i < g_nincluded; i++)
        if (strcmp(g_included[i], resolved_path) == 0) return 1;
    return 0;
}
static void mark_included(const char *resolved_path) {
    if (g_nincluded >= MAX_INCLUDED_FILES) fatal(0, "too many #include'd files (limit %d)", MAX_INCLUDED_FILES);
    g_included[g_nincluded++] = xstrdup(resolved_path);
}

/* The directory containing `path` - "foo/bar/baz.h" -> "foo/bar",
 * "baz.h" (no slash) -> ".". Used so `#include "x.h"` inside some
 * lib/a.h looks for x.h next to a.h, not next to whatever file is
 * being compiled at the top level. */
static char *dirname_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrdup(".");
    size_t len = (size_t)(slash - path);
    char *d = xmalloc(len + 1);
    memcpy(d, path, len);
    d[len] = '\0';
    return d;
}

static char *join_path(const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    char *r = xmalloc(dl + 1 + nl + 1);
    memcpy(r, dir, dl);
    r[dl] = '/';
    memcpy(r + dl + 1, name, nl + 1);
    return r;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int lex_string(const char *src, const char *base_dir);

/* Resolves `fname` to an actual file and lexes it in place, unless
 * it's already been included anywhere in this compile (in which case
 * this is a silent no-op - see the include-once note above). */
static void do_include(const char *fname, int angled, const char *base_dir, int line) {
    char *resolved = NULL;
    if (!angled) {
        char *candidate = join_path(base_dir, fname);
        if (file_exists(candidate)) resolved = candidate;
        else free(candidate);
    }
    if (!resolved) {
        for (int i = 0; i < g_nincludedirs; i++) {
            char *candidate = join_path(g_include_dirs[i], fname);
            if (file_exists(candidate)) { resolved = candidate; break; }
            free(candidate);
        }
    }
    if (!resolved)
        fatal(line, "cannot find include file '%s' (looked%s in the -I search path)",
                     fname, angled ? "" : " next to the including file and");
    if (already_included(resolved)) { free(resolved); return; }
    mark_included(resolved);
    char *inc_src = read_file(resolved);
    char *inc_dir = dirname_of(resolved);
    lex_string(inc_src, inc_dir);
    free(inc_src);
    free(inc_dir);
    free(resolved);
}

/* A linear scan through this table is plenty fast for ~10 keywords;
 * no need for a hash table or a generated trie at this scale. Every
 * identifier the lexer reads gets checked against this list to decide
 * whether it's really a keyword (T_IF, T_WHILE, ...) or a plain
 * T_IDENT that the parser will look up in a symbol table later. */
static struct { const char *kw; TokKind k; } keywords[] = {
    {"int", T_INT}, {"char", T_CHAR}, {"void", T_VOID},
    {"if", T_IF}, {"else", T_ELSE}, {"while", T_WHILE}, {"for", T_FOR},
    {"return", T_RETURN}, {"break", T_BREAK}, {"continue", T_CONTINUE},
    {NULL, T_EOF}
};

/* Shared by both char literals ('x') and string literals ("..."), since
 * both need to turn a backslash escape into the byte it represents.
 * Called with s[*i] == '\\'; advances *i past the whole escape and
 * returns the resulting byte value. */
static int read_escape(const char *s, size_t *i, size_t n, int line) {
    (*i)++;
    if (*i >= n) fatal(line, "unterminated escape sequence");
    char c = s[*i]; (*i)++;
    switch (c) {
        case 'n': return 13;  /* C64 CHROUT newline is carriage return, not $0A */
        case 't': return 9;
        case '0': return 0;
        case '\\': return '\\';
        case '\'': return '\'';
        case '"': return '"';
        default: fatal(line, "unsupported escape sequence '\\%c'", c);
    }
    return 0; /* unreachable; fatal() above never returns */
}

/* The main lexer loop. Reads `src` (a NUL-terminated buffer) from
 * start to end, appending one Token to g_toks per iteration of the
 * outer while loop (except for whitespace and comments, which are
 * consumed but produce no token). Tracks source line numbers as it
 * goes, purely so later error messages can say "line 42" instead of
 * "somewhere in your file". Returns the line count reached, so the
 * outermost caller (lex_file()) can use it for the final EOF token.
 *
 * `base_dir` is the directory `src` came from, used to resolve any
 * `#include "..."` (quoted) directives found in it - see do_include()
 * above and lex_file() below for the recursive-splicing mechanism
 * this is one piece of. */
static int lex_string(const char *src, const char *base_dir) {
    size_t n = strlen(src);
    size_t i = 0;
    int line = 1;
    while (i < n) {
        char c = src[i];
        if (c == '\n') { line++; i++; continue; }
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '/' && i + 1 < n && src[i+1] == '/') {
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i+1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i+1] == '/')) {
                if (src[i] == '\n') line++;
                i++;
            }
            if (i + 1 >= n) fatal(line, "unterminated block comment");
            i += 2;
            continue;
        }

        /* Preprocessor directive. cc64 doesn't use '#' for anything
         * else, so any '#' encountered here unambiguously starts one -
         * unlike real C, this doesn't require it to be the first
         * non-whitespace character on its line, which is a small,
         * harmless simplification. */
        if (c == '#') {
            i++;
            while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
            size_t ds = i;
            while (i < n && is_id_char((unsigned char)src[i])) i++;
            size_t dlen = i - ds;
            if (dlen == 0) fatal(line, "expected a preprocessor directive after '#'");
            char directive[32];
            if (dlen >= sizeof(directive)) fatal(line, "unknown preprocessor directive");
            memcpy(directive, src + ds, dlen); directive[dlen] = '\0';
            if (strcmp(directive, "include") != 0)
                fatal(line, "cc64 only supports #include, not #%s", directive);
            while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
            if (i >= n) fatal(line, "expected \"filename\" or <filename> after #include");
            int angled;
            char openc = src[i], closec;
            if (openc == '"') closec = '"';
            else if (openc == '<') closec = '>';
            else { fatal(line, "expected \"filename\" or <filename> after #include"); closec = 0; }
            angled = (openc == '<');
            i++;
            size_t fs = i;
            while (i < n && src[i] != closec && src[i] != '\n') i++;
            if (i >= n || src[i] != closec) fatal(line, "unterminated #include filename");
            size_t flen = i - fs;
            char *fname = xmalloc(flen + 1);
            memcpy(fname, src + fs, flen); fname[flen] = '\0';
            i++; /* consume the closing quote/angle-bracket */
            /* Deliberately NOT skipping to end-of-line here: a
             * trailing comment after the filename can span multiple
             * lines, and this loop's own comment handling at the top
             * already knows how to skip both comment styles correctly -
             * a second, naive "skip to newline" here would cut a
             * multi-line block comment in half and feed its second
             * half to the tokenizer as if it were real code. Just
             * stop; the outer loop picks up right after the filename
             * and handles whatever comes next exactly like anywhere
             * else in the file. */
            do_include(fname, angled, base_dir, line);
            free(fname);
            continue;
        }

        Token t; memset(&t, 0, sizeof(t)); t.line = line;

        /* Identifiers and keywords: a keyword is just an identifier
         * that happens to match something in the keywords[] table. */
        if (is_id_start((unsigned char)c)) {
            size_t j = i;
            while (j < n && is_id_char((unsigned char)src[j])) j++;
            size_t len = j - i;
            char *buf = xmalloc(len + 1);
            memcpy(buf, src + i, len); buf[len] = '\0';
            TokKind kk = T_IDENT;
            for (int k = 0; keywords[k].kw; k++)
                if (strcmp(keywords[k].kw, buf) == 0) { kk = keywords[k].k; break; }
            t.kind = kk;
            t.text = buf;
            tok_push(t);
            i = j;
            continue;
        }

        /* Number literals: decimal, or 0x/0X hex. cc64 doesn't need
         * octal or floating point, so those are simply not recognized
         * here - a leading 0 followed by digits is just decimal. */
        if (isdigit((unsigned char)c)) {
            size_t j = i;
            long v;
            if (c == '0' && j + 1 < n && (src[j+1] == 'x' || src[j+1] == 'X')) {
                j += 2;
                size_t start = j;
                while (j < n && isxdigit((unsigned char)src[j])) j++;
                if (j == start) fatal(line, "bad hex literal");
                v = strtol(src + start, NULL, 16);
            } else {
                while (j < n && isdigit((unsigned char)src[j])) j++;
                v = strtol(src + i, NULL, 10);
            }
            t.kind = T_NUM; t.ival = v;
            tok_push(t);
            i = j;
            continue;
        }

        /* Character literal: 'x' or 'x' with an escape. Stored as its
         * numeric byte value in t.ival, same as a T_NUM. */
        if (c == '\'') {
            i++;
            int v;
            if (i < n && src[i] == '\\') v = read_escape(src, &i, n, line);
            else if (i < n) { v = (unsigned char)src[i]; i++; }
            else { fatal(line, "unterminated char literal"); v = 0; }
            if (i >= n || src[i] != '\'') fatal(line, "unterminated char literal");
            i++;
            t.kind = T_CHARLIT; t.ival = v;
            tok_push(t);
            continue;
        }

        /* String literal: "...". Escapes are resolved here at lex
         * time, so by the time the parser sees a T_STRLIT its text is
         * already the exact bytes the string should contain - the
         * parser and codegen never need to know about backslashes. */
        if (c == '"') {
            i++;
            char buf[4096]; size_t bl = 0;
            while (i < n && src[i] != '"') {
                int v;
                if (src[i] == '\\') v = read_escape(src, &i, n, line);
                else { v = (unsigned char)src[i]; i++; }
                if (bl + 1 >= sizeof(buf)) fatal(line, "string literal too long");
                buf[bl++] = (char)v;
            }
            if (i >= n) fatal(line, "unterminated string literal");
            i++;
            buf[bl] = '\0';
            t.kind = T_STRLIT; t.text = xstrdup(buf);
            tok_push(t);
            continue;
        }

        /* Multi-character operators. These macros are a compact way
         * to write "if the next 2 (or 3) characters match exactly
         * this, consume them and produce this token kind" - tried
         * longest-match-first (e.g. THREE before TWO, and within TWO,
         * before falling through to the single-character switch
         * below) so `<<=` isn't accidentally lexed as `<<` followed
         * by `=`. Note the comma operator inside the macro: it relies
         * on C evaluating `a && b && c && (side-effects, 1)` left to
         * right and short-circuiting, so the side effects (setting
         * t.kind and advancing i) only happen if the whole match
         * succeeds. */
#define TWO(a,b,k) (c==(a) && i+1<n && src[i+1]==(b) && (t.kind=(k), i+=2, 1))
#define THREE(a,b,cc,k) (c==(a) && i+2<n && src[i+1]==(b) && src[i+2]==(cc) && (t.kind=(k), i+=3, 1))
        if (THREE('<','<','=',T_SHLEQ)) { tok_push(t); continue; }
        if (THREE('>','>','=',T_SHREQ)) { tok_push(t); continue; }
        if (TWO('=','=',T_EQ)) { tok_push(t); continue; }
        if (TWO('!','=',T_NE)) { tok_push(t); continue; }
        if (TWO('<','=',T_LE)) { tok_push(t); continue; }
        if (TWO('>','=',T_GE)) { tok_push(t); continue; }
        if (TWO('&','&',T_ANDAND)) { tok_push(t); continue; }
        if (TWO('|','|',T_OROR)) { tok_push(t); continue; }
        if (TWO('<','<',T_SHL)) { tok_push(t); continue; }
        if (TWO('>','>',T_SHR)) { tok_push(t); continue; }
        if (TWO('+','=',T_PLUSEQ)) { tok_push(t); continue; }
        if (TWO('-','=',T_MINUSEQ)) { tok_push(t); continue; }
        if (TWO('*','=',T_STAREQ)) { tok_push(t); continue; }
        if (TWO('/','=',T_SLASHEQ)) { tok_push(t); continue; }
        if (TWO('%','=',T_PERCENTEQ)) { tok_push(t); continue; }
        if (TWO('&','=',T_AMPEQ)) { tok_push(t); continue; }
        if (TWO('|','=',T_PIPEEQ)) { tok_push(t); continue; }
        if (TWO('^','=',T_CARETEQ)) { tok_push(t); continue; }
        if (TWO('+','+',T_INC)) { tok_push(t); continue; }
        if (TWO('-','-',T_DEC)) { tok_push(t); continue; }
#undef TWO
#undef THREE

        /* Everything else is a single-character token. */
        switch (c) {
            case '(': t.kind = T_LPAREN; break;
            case ')': t.kind = T_RPAREN; break;
            case '{': t.kind = T_LBRACE; break;
            case '}': t.kind = T_RBRACE; break;
            case '[': t.kind = T_LBRACKET; break;
            case ']': t.kind = T_RBRACKET; break;
            case ';': t.kind = T_SEMI; break;
            case ',': t.kind = T_COMMA; break;
            case '=': t.kind = T_ASSIGN; break;
            case '+': t.kind = T_PLUS; break;
            case '-': t.kind = T_MINUS; break;
            case '*': t.kind = T_STAR; break;
            case '/': t.kind = T_SLASH; break;
            case '%': t.kind = T_PERCENT; break;
            case '<': t.kind = T_LT; break;
            case '>': t.kind = T_GT; break;
            case '!': t.kind = T_NOT; break;
            case '&': t.kind = T_AMP; break;
            case '|': t.kind = T_PIPE; break;
            case '^': t.kind = T_CARET; break;
            case '~': t.kind = T_TILDE; break;
            default: fatal(line, "unexpected character '%c'", c); break;
        }
        i++;
        tok_push(t);
    }
    return line;
}

/* The public entry point. Reads `path`, lexes it (following any
 * #include directives it contains, recursively - see do_include()
 * above), and finally appends the T_EOF sentinel that lets the parser
 * always safely "peek one token ahead" without a separate bounds
 * check - the worst that happens is it peeks at T_EOF, which every
 * parse_* function already knows how to reject with a sensible
 * error. This is the only place EOF is appended: lex_string() itself
 * never does, so included files' tokens splice in cleanly without an
 * EOF marker landing in the middle of the stream. */
void lex_file(const char *path) {
    mark_included(path); /* harmless even if nothing ever tries to re-include the top-level file */
    char *src = read_file(path);
    char *dir = dirname_of(path);
    int line = lex_string(src, dir);
    free(src);
    free(dir);
    Token e; memset(&e, 0, sizeof(e)); e.kind = T_EOF; e.line = line;
    tok_push(e);
}
