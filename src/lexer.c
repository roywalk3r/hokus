/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#include "lexer.h"
#include "value.h"

void initLexer(Lexer *lexer, const char *source) {
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
}

static bool isAtEnd(Lexer *l) { return *l->current == '\0'; }
static char advance(Lexer *l) { l->current++; return l->current[-1]; }
static char peekCh(Lexer *l) { return *l->current; }
static char peekNext(Lexer *l) { return isAtEnd(l) ? '\0' : l->current[1]; }

static bool match(Lexer *l, char expected) {
    if (isAtEnd(l) || *l->current != expected) return false;
    l->current++;
    return true;
}

static Token makeToken(Lexer *l, TokenType type) {
    Token t;
    t.type = type;
    t.start = l->start;
    t.length = (int)(l->current - l->start);
    t.line = l->line;
    return t;
}

static Token errorToken(Lexer *l, const char *message) {
    Token t;
    t.type = TOKEN_ERROR;
    t.start = message;
    t.length = 0;
    while (message[t.length]) t.length++;
    t.line = l->line;
    return t;
}

static void skipWhitespace(Lexer *l) {
    for (;;) {
        char c = peekCh(l);
        switch (c) {
            case ' ': case '\r': case '\t': advance(l); break;
            case '\n': l->line++; advance(l); break;
            case '/':
                if (peekNext(l) == '/') {
                    while (peekCh(l) != '\n' && !isAtEnd(l)) advance(l);
                } else {
                    return;
                }
                break;
            default: return;
        }
    }
}

static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

static TokenType checkKeyword(Lexer *l, int start, int length,
                              const char *rest, TokenType type) {
    if (l->current - l->start == start + length) {
        for (int i = 0; i < length; i++) {
            if (l->start[start + i] != rest[i]) return TOKEN_IDENTIFIER;
        }
        return type;
    }
    return TOKEN_IDENTIFIER;
}

static TokenType identifierType(Lexer *l) {
    switch (l->start[0]) {
        case 'a': return checkKeyword(l, 1, 2, "nd", TOKEN_AND);
        case 'e': return checkKeyword(l, 1, 3, "lse", TOKEN_ELSE);
        case 'f':
            if (l->current - l->start > 1) {
                switch (l->start[1]) {
                    case 'a': return checkKeyword(l, 2, 3, "lse", TOKEN_FALSE);
                    case 'o': return checkKeyword(l, 2, 1, "r", TOKEN_FOR);
                    case 'u': return checkKeyword(l, 2, 1, "n", TOKEN_FUN);
                }
            }
            break;
        case 'i':
            if (l->current - l->start > 1 && l->start[1] == 'm') {
                return checkKeyword(l, 2, 4, "port", TOKEN_IMPORT);
            }
            return checkKeyword(l, 1, 1, "f", TOKEN_IF);
        case 'n': return checkKeyword(l, 1, 2, "il", TOKEN_NIL);
        case 'o': return checkKeyword(l, 1, 1, "r", TOKEN_OR);
        case 'c': return checkKeyword(l, 1, 4, "lass", TOKEN_CLASS);
        case 'p': return checkKeyword(l, 1, 4, "rint", TOKEN_PRINT);
        case 'r': return checkKeyword(l, 1, 5, "eturn", TOKEN_RETURN);
        case 's':
            if (l->current - l->start > 1) {
                switch (l->start[1]) {
                    case 'u': return checkKeyword(l, 2, 3, "per", TOKEN_SUPER);
                    case 't': return checkKeyword(l, 2, 4, "atic", TOKEN_STATIC);
                }
            }
            break;
        case 't':
            if (l->current - l->start > 1) {
                switch (l->start[1]) {
                    case 'h': return checkKeyword(l, 2, 2, "is", TOKEN_THIS);
                    case 'r': return checkKeyword(l, 2, 2, "ue", TOKEN_TRUE);
                }
            }
            break;
        case 'v': return checkKeyword(l, 1, 2, "ar", TOKEN_VAR);
        case 'w': return checkKeyword(l, 1, 4, "hile", TOKEN_WHILE);
    }
    return TOKEN_IDENTIFIER;
}

static Token number(Lexer *l) {
    while (isDigit(peekCh(l))) advance(l);
    if (peekCh(l) == '.' && isDigit(peekNext(l))) {
        advance(l);
        while (isDigit(peekCh(l))) advance(l);
    }
    return makeToken(l, TOKEN_NUMBER);
}

static Token string(Lexer *l) {
    while (peekCh(l) != '"' && !isAtEnd(l)) {
        if (peekCh(l) == '\n') l->line++;
        if (peekCh(l) == '\\' && peekNext(l) != '\0') {
            advance(l); /* backslash — swallow the escaped char with it */
            advance(l);
            continue;
        }
        advance(l);
    }
    if (isAtEnd(l)) return errorToken(l, "Unterminated string.");
    advance(l);
    return makeToken(l, TOKEN_STRING);
}

Token nextToken(Lexer *l) {
    skipWhitespace(l);
    l->start = l->current;
    if (isAtEnd(l)) return makeToken(l, TOKEN_EOF);

    char c = advance(l);
    if (isAlpha(c)) {
        while (isAlpha(peekCh(l)) || isDigit(peekCh(l))) advance(l);
        return makeToken(l, identifierType(l));
    }
    if (isDigit(c)) return number(l);

    switch (c) {
        case '(': return makeToken(l, TOKEN_LEFT_PAREN);
        case ')': return makeToken(l, TOKEN_RIGHT_PAREN);
        case '{': return makeToken(l, TOKEN_LEFT_BRACE);
        case '}': return makeToken(l, TOKEN_RIGHT_BRACE);
        case ';': return makeToken(l, TOKEN_SEMICOLON);
        case ',': return makeToken(l, TOKEN_COMMA);
        case '.': return makeToken(l, TOKEN_DOT);
        case '-': return makeToken(l, TOKEN_MINUS);
        case '+': return makeToken(l, TOKEN_PLUS);
        case '/': return makeToken(l, TOKEN_SLASH);
        case '*': return makeToken(l, TOKEN_STAR);
        case '!': return makeToken(l, match(l, '=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
        case '=': return makeToken(l, match(l, '=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
        case '<': return makeToken(l, match(l, '=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
        case '>': return makeToken(l, match(l, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
        case '"': return string(l);
    }
    return errorToken(l, "Unexpected character.");
}

/* ---- token-stream dump (jlox-style, drives --scan vs the scanning corpus) ---- */

static const char *TOKEN_NAMES[] = {
    "LEFT_PAREN", "RIGHT_PAREN", "LEFT_BRACE", "RIGHT_BRACE",
    "COMMA", "DOT", "MINUS", "PLUS", "SEMICOLON", "SLASH", "STAR",
    "BANG", "BANG_EQUAL", "EQUAL", "EQUAL_EQUAL",
    "GREATER", "GREATER_EQUAL", "LESS", "LESS_EQUAL",
    "IDENTIFIER", "STRING", "NUMBER",
    "AND", "CLASS", "ELSE", "FALSE", "FOR", "FUN",
    "IF", "NIL", "OR", "PRINT", "RETURN",
    "SUPER", "THIS", "TRUE", "VAR", "WHILE",
    "STATIC", "IMPORT",
    "ERROR", "EOF"
};

static void emitSpan(const char *s, int length) {
    char c[2] = { 0, 0 };
    for (int i = 0; i < length; i++) { c[0] = s[i]; hkEmit(c); }
}

void lexDump(const char *source) {
    Lexer lexer;
    initLexer(&lexer, source);
    for (;;) {
        Token t = nextToken(&lexer);
        hkEmit(TOKEN_NAMES[t.type]);
        hkEmit(" ");
        emitSpan(t.start, t.length);
        hkEmit(" ");
        if (t.type == TOKEN_NUMBER) {
            char buf[40];
            numberToString(parseDouble(t.start, t.length), buf, sizeof(buf));
            hkEmit(buf);
            /* jlox prints doubles Java-style: integral values carry ".0" */
            bool hasDot = false;
            for (int i = 0; buf[i]; i++) if (buf[i] == '.') hasDot = true;
            if (!hasDot) hkEmit(".0");
        } else if (t.type == TOKEN_STRING) {
            emitSpan(t.start + 1, t.length - 2); /* strip the quotes */
        } else {
            hkEmit("null");
        }
        hkEmit("\n");
        if (t.type == TOKEN_EOF) break;
    }
}
