#include "chunk.h"
#include "compiler.h"
#include "lexer.h"

typedef struct {
    Lexer lexer;
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
    void (*errorOut)(const char *);
} Parser;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_TERM,
    PREC_FACTOR,
    PREC_UNARY,
    PREC_CALL,
    PREC_PRIMARY
} Precedence;

typedef struct {
    Token name;
    int depth;
    bool isCaptured;
} Local;

typedef struct {
    uint8_t index;
    bool isLocal;
} Upvalue;

typedef struct Compiler Compiler;
struct Compiler {
    Compiler *enclosing;
    ObjFunction *function;
    Chunk *chunk;
    Local locals[256];
    int localCount;
    Upvalue upvalues[256];
    int scopeDepth;
};

typedef void (*ParseFn)(Parser *, Compiler *, bool);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

static Parser parser;
static Compiler *current = NULL; /* innermost compiler */

/* ---- errors ---- */

static void errorAt(Parser *p, Token *token, const char *message) {
    if (p->panicMode) return;
    p->panicMode = true;
    char line[128];
    int n = 0;
    const char *prefix = "[line ";
    while (*prefix) line[n++] = *prefix++;
    char num[16];
    numberToString(token->line, num, sizeof(num));
    for (int i = 0; num[i]; i++) line[n++] = num[i];
    const char *mid = "] Error";
    while (*mid) line[n++] = *mid++;
    if (token->type == TOKEN_EOF) {
        const char *m = " at end";
        while (*m) line[n++] = *m++;
    }
    line[n++] = ':'; line[n++] = ' ';
    while (*message && n < 120) line[n++] = *message++;
    line[n] = '\0';
    hkEmit(line);
    hkEmit("\n");
    p->hadError = true;
}

static void error(Parser *p, const char *message) { errorAt(p, &p->previous, message); }
static void errorAtCurrent(Parser *p, const char *message) { errorAt(p, &p->current, message); }

/* ---- parser helpers ---- */

static void advanceParser(Parser *p) {
    p->previous = p->current;
    for (;;) {
        p->current = nextToken(&p->lexer);
        if (p->current.type != TOKEN_ERROR) break;
        errorAtCurrent(p, p->current.start);
    }
}

static void consume(Parser *p, TokenType type, const char *message) {
    if (p->current.type == type) { advanceParser(p); return; }
    errorAtCurrent(p, message);
}

static bool check(Parser *p, TokenType type) { return p->current.type == type; }

static bool matchToken(Parser *p, TokenType type) {
    if (!check(p, type)) return false;
    advanceParser(p);
    return true;
}

/* ---- bytecode emit ---- */

static Chunk *currentChunk(void) { return current->chunk; }

static void emitByte(Parser *p, uint8_t byte) {
    writeChunk(currentChunk(), byte, p->previous.line);
}

static void emitBytes(Parser *p, uint8_t a, uint8_t b) {
    emitByte(p, a);
    emitByte(p, b);
}

static int emitJump(Parser *p, uint8_t instruction) {
    emitByte(p, instruction);
    emitByte(p, 0xff);
    emitByte(p, 0xff);
    return currentChunk()->count - 2;
}

static void patchJump(Parser *p, int offset) {
    int jump = currentChunk()->count - offset - 2;
    if (jump > UINT16_MAX) error(p, "Too much code to jump over.");
    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

static void emitLoop(Parser *p, int loopStart) {
    emitByte(p, OP_LOOP);
    int offset = currentChunk()->count - loopStart + 2;
    if (offset > UINT16_MAX) error(p, "Loop body too large.");
    emitByte(p, (offset >> 8) & 0xff);
    emitByte(p, offset & 0xff);
}

static void emitReturn(Parser *p) {
    emitByte(p, OP_NIL);
    emitByte(p, OP_RETURN);
}

static int makeConstant(Parser *p, Value value) {
    int constant = addConstant(currentChunk(), value);
    if (constant > UINT16_MAX) {
        error(p, "Too many constants in one chunk.");
        return 0;
    }
    return constant;
}

static void emitConstRef(Parser *p, uint8_t op, int constant) {
    emitByte(p, op);
    emitByte(p, (constant >> 8) & 0xff);
    emitByte(p, constant & 0xff);
}

static void emitConstant(Parser *p, Value value) {
    emitConstRef(p, OP_CONSTANT, makeConstant(p, value));
}

/* ---- compiler scope management ---- */

static void initCompiler(Compiler *compiler, Compiler *enclosing, const char *name, int nameLen) {
    compiler->enclosing = enclosing;
    compiler->function = newFunction();
    compiler->chunk = HK_REALLOC(NULL, 0, sizeof(Chunk));
    initChunk(compiler->chunk);
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->function->chunk = compiler->chunk;
    if (name != NULL) compiler->function->name = copyString(name, nameLen);
    current = compiler;
    /* slot 0: the function itself (name "" for script/anon, own name for fns) */
    Local *local = &current->locals[current->localCount++];
    local->depth = 0;
    local->isCaptured = false;
    if (name != NULL) {
        local->name.start = name;
        local->name.length = nameLen;
    } else {
        local->name.start = "";
        local->name.length = 0;
    }
}

static ObjFunction *endCompiler(Parser *p) {
    emitReturn(p);
    ObjFunction *function = current->function;
    current = current->enclosing;
    return function;
}

static void beginScope(void) { current->scopeDepth++; }

static void endScope(Parser *p) {
    current->scopeDepth--;
    while (current->localCount > 0 &&
           current->locals[current->localCount - 1].depth > current->scopeDepth) {
        if (current->locals[current->localCount - 1].isCaptured) {
            emitByte(p, OP_CLOSE_UPVALUE);
        } else {
            emitByte(p, OP_POP);
        }
        current->localCount--;
    }
}

/* ---- forward decls ---- */

static void expression(Parser *p);
static void statement(Parser *p);
static void declaration(Parser *p);
static ParseRule *getRule(TokenType type);
static void parsePrecedence(Parser *p, Precedence precedence);

/* identifiers */
static int identifierConstant(Parser *p, Token *name) {
    return makeConstant(p, OBJ_VAL(copyString(name->start, name->length)));
}

static bool identifiersEqual(Token *a, Token *b) {
    if (a->length != b->length) return false;
    return hk_strncmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler *compiler, Token *name) {
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local *local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name)) {
            return i;
        }
    }
    return -1;
}

static int addUpvalue(Parser *p, Compiler *compiler, uint8_t index, bool isLocal) {
    int upvalueCount = compiler->function->upvalueCount;
    for (int i = 0; i < upvalueCount; i++) {
        Upvalue *upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->isLocal == isLocal) return i;
    }
    if (upvalueCount == 256) {
        error(p, "Too many closure variables in function.");
        return 0;
    }
    compiler->upvalues[upvalueCount].isLocal = isLocal;
    compiler->upvalues[upvalueCount].index = index;
    return compiler->function->upvalueCount++;
}

static int resolveUpvalue(Parser *p, Compiler *compiler, Token *name) {
    if (compiler->enclosing == NULL) return -1;
    int local = resolveLocal(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].isCaptured = true;
        return addUpvalue(p, compiler, (uint8_t)local, true);
    }
    int upvalue = resolveUpvalue(p, compiler->enclosing, name);
    if (upvalue != -1) {
        return addUpvalue(p, compiler, (uint8_t)upvalue, false);
    }
    return -1;
}

static void addLocal(Parser *p, Token name) {
    if (current->localCount == 256) {
        error(p, "Too many local variables in function.");
        return;
    }
    Local *local = &current->locals[current->localCount++];
    local->name = name;
    local->depth = current->scopeDepth;
}

static void declareVariable(Parser *p, Token *name) {
    if (current->scopeDepth == 0) return;
    for (int i = current->localCount - 1; i >= 0; i--) {
        Local *local = &current->locals[i];
        if (local->depth < current->scopeDepth) break;
        if (identifiersEqual(name, &local->name)) {
            error(p, "Variable already declared in this scope.");
        }
    }
    addLocal(p, *name);
}

static int parseVariable(Parser *p, const char *errorMessage) {
    consume(p, TOKEN_IDENTIFIER, errorMessage);
    declareVariable(p, &p->previous);
    if (current->scopeDepth > 0) return 0;
    return identifierConstant(p, &p->previous);
}

static void defineVariable(Parser *p, int global) {
    if (current->scopeDepth > 0) return; /* local: already on stack */
    emitConstRef(p, OP_DEFINE_GLOBAL, global);
}

static void markInitialized(void) {
    /* locals are born initialized in this single-pass design */
}

/* ---- parse functions ---- */

static void binary(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    TokenType operatorType = p->previous.type;
    ParseRule *rule = getRule(operatorType);
    parsePrecedence(p, (Precedence)(rule->precedence + 1));
    switch (operatorType) {
        case TOKEN_BANG_EQUAL:    emitBytes(p, OP_EQUAL, OP_NOT); break;
        case TOKEN_EQUAL_EQUAL:   emitByte(p, OP_EQUAL); break;
        case TOKEN_GREATER:       emitByte(p, OP_GREATER); break;
        case TOKEN_GREATER_EQUAL: emitBytes(p, OP_LESS, OP_NOT); break;
        case TOKEN_LESS:          emitByte(p, OP_LESS); break;
        case TOKEN_LESS_EQUAL:    emitBytes(p, OP_GREATER, OP_NOT); break;
        case TOKEN_PLUS:          emitByte(p, OP_ADD); break;
        case TOKEN_MINUS:         emitByte(p, OP_SUBTRACT); break;
        case TOKEN_STAR:          emitByte(p, OP_MULTIPLY); break;
        case TOKEN_SLASH:         emitByte(p, OP_DIVIDE); break;
        default: return;
    }
}

static void call(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    uint8_t argCount = 0;
    if (!check(p, TOKEN_RIGHT_PAREN)) {
        do {
            expression(p);
            if (argCount == 255) error(p, "Can't have more than 255 arguments.");
            argCount++;
        } while (matchToken(p, TOKEN_COMMA));
    }
    consume(p, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
    emitBytes(p, OP_CALL, argCount);
}

static void literal(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    switch (p->previous.type) {
        case TOKEN_FALSE: emitByte(p, OP_FALSE); break;
        case TOKEN_NIL:   emitByte(p, OP_NIL); break;
        case TOKEN_TRUE:  emitByte(p, OP_TRUE); break;
        default: return;
    }
}

static double parseDouble(const char *s, int length) {
    double result = 0;
    int i = 0;
    while (i < length && s[i] >= '0' && s[i] <= '9') {
        result = result * 10 + (s[i] - '0');
        i++;
    }
    if (i < length && s[i] == '.') {
        i++;
        double frac = 0, scale = 1;
        while (i < length && s[i] >= '0' && s[i] <= '9') {
            frac = frac * 10 + (s[i] - '0');
            scale *= 10;
            i++;
        }
        result += frac / scale;
    }
    return result;
}

static void number(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    emitConstant(p, NUMBER_VAL(parseDouble(p->previous.start, p->previous.length)));
}

static void string(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    /* token span includes both quotes; unescape into a temp buffer */
    const char *raw = p->previous.start + 1;
    int rawLen = p->previous.length - 2;
    char *buf = HK_REALLOC(NULL, 0, rawLen + 1);
    int n = 0;
    bool bad = false;
    for (int i = 0; i < rawLen; i++) {
        if (raw[i] == '\\' && i + 1 < rawLen) {
            i++;
            switch (raw[i]) {
                case 'n':  buf[n++] = '\n'; break;
                case 't':  buf[n++] = '\t'; break;
                case 'r':  buf[n++] = '\r'; break;
                case '"':  buf[n++] = '"'; break;
                case '\\': buf[n++] = '\\'; break;
                case '0':  buf[n++] = '\0'; break;
                default:
                    error(p, "Unknown escape sequence.");
                    bad = true;
                    break;
            }
        } else {
            buf[n++] = raw[i];
        }
    }
    buf[n] = '\0';
    if (!bad) emitConstant(p, OBJ_VAL(takeString(buf, n)));
}

static void namedVariable(Parser *p, Token name, bool canAssign) {
    uint8_t getOp, setOp;
    int arg = resolveLocal(current, &name);
    if (arg != -1) {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
    } else if ((arg = resolveUpvalue(p, current, &name)) != -1) {
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
    } else {
        arg = identifierConstant(p, &name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }
    if (canAssign && matchToken(p, TOKEN_EQUAL)) {
        expression(p);
        if (setOp == OP_SET_GLOBAL) emitConstRef(p, setOp, arg);
        else emitBytes(p, setOp, (uint8_t)arg);
    } else {
        if (getOp == OP_GET_GLOBAL) emitConstRef(p, getOp, arg);
        else emitBytes(p, getOp, (uint8_t)arg);
    }
}

static void variable(Parser *p, Compiler *c, bool canAssign) {
    (void)c;
    namedVariable(p, p->previous, canAssign);
}

static void unary(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    TokenType operatorType = p->previous.type;
    parsePrecedence(p, PREC_UNARY);
    switch (operatorType) {
        case TOKEN_BANG:  emitByte(p, OP_NOT); break;
        case TOKEN_MINUS: emitByte(p, OP_NEGATE); break;
        default: return;
    }
}

static void grouping(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    expression(p);
    consume(p, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void parsePrecedence(Parser *p, Precedence precedence) {
    advanceParser(p);
    ParseFn prefixRule = getRule(p->previous.type)->prefix;
    if (prefixRule == NULL) {
        error(p, "Expect expression.");
        return;
    }
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(p, current, canAssign);
    while (precedence <= getRule(p->current.type)->precedence) {
        advanceParser(p);
        ParseFn infixRule = getRule(p->previous.type)->infix;
        infixRule(p, current, canAssign);
    }
    if (canAssign && matchToken(p, TOKEN_EQUAL)) {
        error(p, "Invalid assignment target.");
    }
}

static void expression(Parser *p) { parsePrecedence(p, PREC_ASSIGNMENT); }

/* ---- and / or ---- */

static void andOp(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    int endJump = emitJump(p, OP_JUMP_IF_FALSE);
    emitByte(p, OP_POP);
    parsePrecedence(p, PREC_AND);
    patchJump(p, endJump);
}

static void orOp(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    int elseJump = emitJump(p, OP_JUMP_IF_FALSE);
    int endJump = emitJump(p, OP_JUMP);
    patchJump(p, elseJump);
    emitByte(p, OP_POP);
    parsePrecedence(p, PREC_OR);
    patchJump(p, endJump);
}

/* ---- rules table ---- */

static ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping, call,   PREC_CALL},
    [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE},
    [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_DOT]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
    [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
    [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
    [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
    [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
    [TOKEN_BANG]          = {unary,    NULL,   PREC_NONE},
    [TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
    [TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
    [TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
    [TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
    [TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE},
    [TOKEN_AND]           = {NULL,     andOp,  PREC_AND},
    [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
    [TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
    [TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_FUN]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
    [TOKEN_NIL]           = {literal,  NULL,   PREC_NONE},
    [TOKEN_OR]            = {NULL,     orOp,   PREC_OR},
    [TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
    [TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
    [TOKEN_VAR]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
};

static ParseRule *getRule(TokenType type) { return &rules[type]; }

/* ---- statements & declarations ---- */

static void block(Parser *p) {
    while (!check(p, TOKEN_RIGHT_BRACE) && !check(p, TOKEN_EOF)) {
        declaration(p);
    }
    consume(p, TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void function(Parser *p) {
    Compiler compiler;
    initCompiler(&compiler, current, p->previous.start, p->previous.length);
    beginScope();
    consume(p, TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    if (!check(p, TOKEN_RIGHT_PAREN)) {
        do {
            current->function->arity++;
            if (current->function->arity > 255) {
                errorAtCurrent(p, "Can't have more than 255 parameters.");
            }
            int constant = parseVariable(p, "Expect parameter name.");
            defineVariable(p, constant);
        } while (matchToken(p, TOKEN_COMMA));
    }
    consume(p, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
    consume(p, TOKEN_LEFT_BRACE, "Expect '{' before function body.");
    block(p);
    ObjFunction *fn = endCompiler(p);
    emitConstRef(p, OP_CLOSURE, makeConstant(p, OBJ_VAL(fn)));
    for (int i = 0; i < fn->upvalueCount; i++) {
        emitByte(p, compiler.upvalues[i].isLocal ? 1 : 0);
        emitByte(p, compiler.upvalues[i].index);
    }
}

static void funDeclaration(Parser *p) {
    int global = parseVariable(p, "Expect function name.");
    markInitialized();
    function(p);
    defineVariable(p, global);
}

static void varDeclaration(Parser *p) {
    int global = parseVariable(p, "Expect variable name.");
    if (matchToken(p, TOKEN_EQUAL)) {
        expression(p);
    } else {
        emitByte(p, OP_NIL);
    }
    consume(p, TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
    defineVariable(p, global);
}

static void expressionStatement(Parser *p) {
    expression(p);
    consume(p, TOKEN_SEMICOLON, "Expect ';' after expression.");
    emitByte(p, OP_POP);
}

static void ifStatement(Parser *p) {
    consume(p, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
    expression(p);
    consume(p, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
    int thenJump = emitJump(p, OP_JUMP_IF_FALSE);
    emitByte(p, OP_POP);
    statement(p);
    int elseJump = emitJump(p, OP_JUMP);
    patchJump(p, thenJump);
    emitByte(p, OP_POP);
    if (matchToken(p, TOKEN_ELSE)) statement(p);
    patchJump(p, elseJump);
}

static void whileStatement(Parser *p) {
    int loopStart = currentChunk()->count;
    consume(p, TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
    expression(p);
    consume(p, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
    int exitJump = emitJump(p, OP_JUMP_IF_FALSE);
    emitByte(p, OP_POP);
    statement(p);
    emitLoop(p, loopStart);
    patchJump(p, exitJump);
    emitByte(p, OP_POP);
}

static void forStatement(Parser *p) {
    beginScope();
    consume(p, TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
    if (matchToken(p, TOKEN_SEMICOLON)) {
        /* no initializer */
    } else if (matchToken(p, TOKEN_VAR)) {
        varDeclaration(p);
    } else {
        expressionStatement(p);
    }
    int loopStart = currentChunk()->count;
    int exitJump = -1;
    if (!matchToken(p, TOKEN_SEMICOLON)) {
        expression(p);
        consume(p, TOKEN_SEMICOLON, "Expect ';' after loop condition.");
        exitJump = emitJump(p, OP_JUMP_IF_FALSE);
        emitByte(p, OP_POP);
    }
    if (!matchToken(p, TOKEN_RIGHT_PAREN)) {
        int bodyJump = emitJump(p, OP_JUMP);
        int incrementStart = currentChunk()->count;
        expression(p);
        emitByte(p, OP_POP);
        consume(p, TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");
        emitLoop(p, loopStart);
        loopStart = incrementStart;
        patchJump(p, bodyJump);
    }
    statement(p);
    emitLoop(p, loopStart);
    if (exitJump != -1) {
        patchJump(p, exitJump);
        emitByte(p, OP_POP);
    }
    endScope(p);
}

static void printStatement(Parser *p) {
    expression(p);
    consume(p, TOKEN_SEMICOLON, "Expect ';' after value.");
    emitByte(p, OP_PRINT);
}

static void returnStatement(Parser *p) {
    if (current->enclosing == NULL) {
        error(p, "Can't return from top-level code.");
    }
    if (matchToken(p, TOKEN_SEMICOLON)) {
        emitReturn(p);
    } else {
        expression(p);
        consume(p, TOKEN_SEMICOLON, "Expect ';' after return value.");
        emitByte(p, OP_RETURN);
    }
}

static void statement(Parser *p) {
    if (matchToken(p, TOKEN_PRINT)) {
        printStatement(p);
    } else if (matchToken(p, TOKEN_IF)) {
        ifStatement(p);
    } else if (matchToken(p, TOKEN_WHILE)) {
        whileStatement(p);
    } else if (matchToken(p, TOKEN_FOR)) {
        forStatement(p);
    } else if (matchToken(p, TOKEN_RETURN)) {
        returnStatement(p);
    } else if (matchToken(p, TOKEN_LEFT_BRACE)) {
        beginScope();
        block(p);
        endScope(p);
    } else {
        expressionStatement(p);
    }
}

static void synchronize(Parser *p) {
    p->panicMode = false;
    while (p->current.type != TOKEN_EOF) {
        if (p->previous.type == TOKEN_SEMICOLON) return;
        switch (p->current.type) {
            case TOKEN_FUN: case TOKEN_VAR: case TOKEN_FOR:
            case TOKEN_IF: case TOKEN_WHILE: case TOKEN_PRINT:
            case TOKEN_RETURN:
                return;
            default: break;
        }
        advanceParser(p);
    }
}

static void declaration(Parser *p) {
    if (matchToken(p, TOKEN_FUN)) {
        funDeclaration(p);
    } else if (matchToken(p, TOKEN_VAR)) {
        varDeclaration(p);
    } else {
        statement(p);
    }
    if (p->panicMode) synchronize(p);
}

ObjFunction *compile(const char *source, int sourceLen) {
    (void)sourceLen;
    Compiler compiler;
    initCompiler(&compiler, NULL, NULL, 0);
    initLexer(&parser.lexer, source);
    parser.hadError = false;
    parser.panicMode = false;

    advanceParser(&parser);
    while (!matchToken(&parser, TOKEN_EOF)) {
        declaration(&parser);
    }
    ObjFunction *function = endCompiler(&parser);
    return parser.hadError ? NULL : function;
}
