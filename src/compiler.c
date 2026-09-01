/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
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

typedef enum {
    TYPE_FUNCTION,
    TYPE_INITIALIZER,
    TYPE_METHOD,
    TYPE_SCRIPT
} FunctionType;

struct Compiler {
    Compiler *enclosing;
    ObjFunction *function;
    Chunk *chunk;
    FunctionType type;
    Local locals[256];
    int localCount;
    Upvalue upvalues[256];
    int scopeDepth;
    int numConstStart; /* byte offset of latest OP_CONSTANT push, -1 if none */
    int localGetStart; /* byte offset of latest OP_GET_LOCAL push, -1 if none */
    int setStmtStart; /* byte offset of latest SET_LOCAL/GLOBAL, -1 if none */
    int cmpJumpStart; /* byte offset of latest LESS/GREATER op, -1 if none */
    /* end-of-operand jump target from and/or: fusions that rewind the chunk
       tail must never cross it (patched jumps still point into the old
       layout — crossing means mid-instruction targets) */
    int andOrTarget;
};

typedef struct ClassCompiler {
    struct ClassCompiler *enclosing;
    bool hasSuperclass;
} ClassCompiler;

static ClassCompiler *currentClass = NULL;

HkReadModuleFn hkReadModule; /* host-provided (compiler.h); NULL = no fs */

/* requested-path stack for cycle detection; entries leak intentionally —
   path bytes are parse-lifetime data */
static const char *importStack[16];
static int importDepth = 0;

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
    Chunk *chunk = currentChunk();
    /* reuse an existing identical string/number constant instead of duplicating;
       bitwise compare: -0.0 and 0.0 are == but print differently */
    if (IS_STRING(value) || IS_NUMBER(value)) {
        for (int i = 0; i < chunk->constants.count; i++) {
            if (chunk->constants.values[i] == value) return i;
        }
    }
    int constant = addConstant(chunk, value);
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
    current->numConstStart = currentChunk()->count; /* fold-mark: instruction start */
    int k = makeConstant(p, value);
    if (k <= UINT8_MAX) emitBytes(p, OP_CONSTANT8, k); /* dense short form */
    else emitConstRef(p, OP_CONSTANT, k);
}

/* ---- compiler scope management ---- */

static void initCompiler(Compiler *compiler, Compiler *enclosing,
                         FunctionType type, const char *name, int nameLen) {
    compiler->enclosing = enclosing;
    compiler->type = type;
    compiler->function = newFunction();
    compiler->chunk = HK_REALLOC(NULL, 0, sizeof(Chunk));
    initChunk(compiler->chunk);
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->numConstStart = -1;
    compiler->localGetStart = -1;
    compiler->setStmtStart = -1;
    compiler->cmpJumpStart = -1;
    compiler->andOrTarget = -1;
    compiler->function->chunk = compiler->chunk;
    if (name != NULL) compiler->function->name = copyString(name, nameLen);
    current = compiler;
    /* slot 0: "this" for methods, the function itself for plain functions
       (own name enables local recursion), "" for the script */
    Local *local = &current->locals[current->localCount++];
    local->depth = 0;
    local->isCaptured = false;
    if (type != TYPE_FUNCTION && type != TYPE_SCRIPT) {
        local->name.start = "this";
        local->name.length = 4;
    } else if (name != NULL) {
        local->name.start = name;
        local->name.length = nameLen;
    } else {
        local->name.start = "";
        local->name.length = 0;
    }
}

static ObjFunction *endCompiler(Parser *p) {
    if (current->type == TYPE_INITIALIZER) {
        emitByte(p, OP_GET_LOCAL);
        emitByte(p, 0);
        emitByte(p, OP_RETURN);
    } else {
        emitReturn(p);
    }
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
            if (local->depth == -1) {
                return -2; /* read in its own initializer */
            }
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
    if (local == -2) {
        error(p, "Can't read local variable in its own initializer.");
        local = -1;
    }
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
            error(p, "Already a variable with this name in this scope.");
        }
    }
    addLocal(p, *name);
    /* -1: not yet initialized — reading it before defineVariable is an error */
    current->locals[current->localCount - 1].depth = -1;
}

static int parseVariable(Parser *p, const char *errorMessage) {
    consume(p, TOKEN_IDENTIFIER, errorMessage);
    declareVariable(p, &p->previous);
    if (current->scopeDepth > 0) return 0;
    return identifierConstant(p, &p->previous);
}

static void defineVariable(Parser *p, int global) {
    if (current->scopeDepth > 0) {
        current->locals[current->localCount - 1].depth = current->scopeDepth;
        return;
    }
    emitConstRef(p, OP_DEFINE_GLOBAL, global);
}

/* ---- parse functions ---- */

/* Constant folding. numConstStart (per-Compiler) records where the most
   recent OP_CONSTANT push began — always a true instruction boundary, unlike
   peeking raw bytes (operand bytes can alias the opcode). A fold fires when
   two marked pushes sit adjacent at the end of the chunk; chunk->lines runs
   in parallel with ->code, so lowering ->count rewinds both. Adjacency also
   guarantees no jump target can point inside the rewound bytes. */
/* Instruction length from the folded form's opcode; 0 = not a const push. */
static int constPushLen(uint8_t op) {
    return op == OP_CONSTANT ? 3 : op == OP_CONSTANT8 ? 2 : 0;
}

static double constNumberAt(Chunk *ch, int start) {
    uint16_t idx = ch->code[start] == OP_CONSTANT8
        ? ch->code[start + 1]
        : (uint16_t)((ch->code[start + 1] << 8) | ch->code[start + 2]);
    Value v = ch->constants.values[idx];
    return IS_NUMBER(v) ? AS_NUMBER(v) : 0.0 / 0.0; /* NaN = not a number */
}

static bool tryFoldBinary(TokenType op, int lhsStart, double *out) {
    Chunk *ch = currentChunk();
    int rhsStart = current->numConstStart;
    if (lhsStart < 0 || rhsStart < 0) return false;
    int ll = constPushLen(ch->code[lhsStart]);
    int rl = constPushLen(ch->code[rhsStart]);
    if (ll == 0 || rl == 0) return false;
    if (rhsStart != ch->count - rl || lhsStart != ch->count - rl - ll)
        return false;
    double a = constNumberAt(ch, lhsStart), b = constNumberAt(ch, rhsStart);
    if (a != a || b != b) return false;
    if (op == TOKEN_PERCENT && b == 0) return false; /* runtime error, not a fold */
    ch->count -= ll + rl;
    current->numConstStart = -1;
    *out = op == TOKEN_PLUS  ? a + b
         : op == TOKEN_MINUS ? a - b
         : op == TOKEN_STAR  ? a * b
         : op == TOKEN_SLASH ? a / b : hkModulo(a, b);
    return true;
}

static bool tryFoldNegate(double *out) {
    Chunk *ch = currentChunk();
    int start = current->numConstStart;
    if (start < 0) return false;
    int len = constPushLen(ch->code[start]);
    if (len == 0 || start != ch->count - len) return false;
    double a = constNumberAt(ch, start);
    if (a != a) return false;
    ch->count -= len;
    current->numConstStart = -1;
    *out = -a;
    return true;
}

/* Peephole: `local - numeric-literal` (both freshly pushed, adjacent at the
   chunk tail) fuses GET_LOCAL + CONSTANT8 + SUBTRACT into one 3-byte op.
   Marks are only hints — the pattern is verified byte-for-byte. Constant
   must be a number: a string rhs has different (concat-for-+) semantics and
   subtraction rejects it at runtime anyway. */
static bool tryFuseLocalConstOp(Parser *p, int lhsLocalMark, uint8_t fusedOp) {
    Chunk *ch = currentChunk();
    int rhsStart = current->numConstStart;
    if (lhsLocalMark < 0 || rhsStart < 0) return false;
    if (ch->code[lhsLocalMark] != OP_GET_LOCAL ||
        ch->code[rhsStart] != OP_CONSTANT8) return false;
    if (lhsLocalMark != ch->count - 4 || rhsStart != ch->count - 2)
        return false;
    int cidx = ch->code[rhsStart + 1];
    if (!IS_NUMBER(ch->constants.values[cidx])) return false;
    int slot = ch->code[lhsLocalMark + 1];
    ch->count -= 4; /* rewinds code+lines together; nothing can jump into the tail */
    current->numConstStart = -1;
    emitByte(p, fusedOp);
    emitByte(p, (uint8_t)slot);
    emitByte(p, (uint8_t)cidx);
    return true;
}

static void binary(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    TokenType operatorType = p->previous.type;
    ParseRule *rule = getRule(operatorType);
    int lhsConstMark = current->numConstStart; /* lhs parse just ended */
    int lhsLocalMark = current->localGetStart;
    parsePrecedence(p, (Precedence)(rule->precedence + 1));
    switch (operatorType) {
        case TOKEN_BANG_EQUAL:    emitBytes(p, OP_EQUAL, OP_NOT); break;
        case TOKEN_EQUAL_EQUAL:   emitByte(p, OP_EQUAL); break;
        case TOKEN_GREATER:
            current->cmpJumpStart = currentChunk()->count; /* fuse-mark */
            emitByte(p, OP_GREATER); break;
        case TOKEN_GREATER_EQUAL: emitBytes(p, OP_LESS, OP_NOT); break;
        case TOKEN_LESS:
            current->cmpJumpStart = currentChunk()->count; /* fuse-mark */
            emitByte(p, OP_LESS); break;
        case TOKEN_LESS_EQUAL:    emitBytes(p, OP_GREATER, OP_NOT); break;
        case TOKEN_MINUS: {
            double r;
            if (tryFoldBinary(operatorType, lhsConstMark, &r)) {
                emitConstant(p, NUMBER_VAL(r));
                break;
            }
            if (tryFuseLocalConstOp(p, lhsLocalMark, OP_SUB_LOCAL_CONST8)) break;
            emitByte(p, OP_SUBTRACT);
            break;
        }
        case TOKEN_PLUS: {
            double r;
            if (tryFoldBinary(operatorType, lhsConstMark, &r)) {
                emitConstant(p, NUMBER_VAL(r));
                break;
            }
            /* fused op errors like OP_ADD when the slot isn't a number:
               "two numbers or two strings" (rhs is a number, verified) */
            if (tryFuseLocalConstOp(p, lhsLocalMark, OP_ADD_LOCAL_CONST8)) break;
            emitByte(p, OP_ADD);
            break;
        }
        case TOKEN_STAR:
        case TOKEN_SLASH:
        case TOKEN_PERCENT: {
            double r;
            if (tryFoldBinary(operatorType, lhsConstMark, &r)) {
                emitConstant(p, NUMBER_VAL(r));
                break;
            }
            emitByte(p, operatorType == TOKEN_STAR    ? OP_MULTIPLY
                      : operatorType == TOKEN_STAR    ? OP_MULTIPLY
                      : operatorType == TOKEN_SLASH   ? OP_DIVIDE : OP_MODULO);
            break;
        }
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
    if (arg == -2) {
        error(p, "Can't read local variable in its own initializer.");
        arg = -1;
    }
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
        if (setOp == OP_SET_GLOBAL) {
            current->setStmtStart = currentChunk()->count; /* fuse-mark */
            emitConstRef(p, setOp, arg);
        } else {
            if (setOp == OP_SET_LOCAL)
                current->setStmtStart = currentChunk()->count; /* fuse-mark */
            emitBytes(p, setOp, (uint8_t)arg);
        }
    } else {
        if (getOp == OP_GET_GLOBAL) emitConstRef(p, getOp, arg);
        else {
            if (getOp == OP_GET_LOCAL)
                current->localGetStart = currentChunk()->count; /* fuse-mark */
            emitBytes(p, getOp, (uint8_t)arg);
        }
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
        case TOKEN_MINUS: {
            double x;
            if (tryFoldNegate(&x)) { emitConstant(p, NUMBER_VAL(x)); break; }
            emitByte(p, OP_NEGATE);
            break;
        }
        default: return;
    }
}

static void grouping(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    expression(p);
    consume(p, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void dot(Parser *p, Compiler *c, bool canAssign) {
    (void)c;
    consume(p, TOKEN_IDENTIFIER, "Expect property name after '.'.");
    int name = identifierConstant(p, &p->previous);
    if (canAssign && matchToken(p, TOKEN_EQUAL)) {
        expression(p);
        emitConstRef(p, OP_SET_PROPERTY, name);
    } else if (matchToken(p, TOKEN_LEFT_PAREN)) {
        uint8_t argCount = 0;
        if (!check(p, TOKEN_RIGHT_PAREN)) {
            do {
                expression(p);
                if (argCount == 255) error(p, "Can't have more than 255 arguments.");
                argCount++;
            } while (matchToken(p, TOKEN_COMMA));
        }
        consume(p, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
        emitConstRef(p, OP_INVOKE, name);
        emitByte(p, argCount);
    } else {
        emitConstRef(p, OP_GET_PROPERTY, name);
    }
}

static void this_(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    if (currentClass == NULL) {
        error(p, "Can't use 'this' outside of a class.");
        return;
    }
    /* resolve through namedVariable so nested closures capture via upvalues */
    Token thisTok = { .type = TOKEN_THIS, .start = "this",
                      .length = 4, .line = p->previous.line };
    namedVariable(p, thisTok, false);
}

static void super_(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    if (currentClass == NULL) {
        error(p, "Can't use 'super' outside of a class.");
        return;
    }
    if (!currentClass->hasSuperclass) {
        error(p, "Can't use 'super' in a class with no superclass.");
    }
    consume(p, TOKEN_DOT, "Expect '.' after 'super'.");
    consume(p, TOKEN_IDENTIFIER, "Expect superclass method name.");
    int name = identifierConstant(p, &p->previous);
    {
        Token thisTok = { .type = TOKEN_THIS, .start = "this",
                          .length = 4, .line = p->previous.line };
        Token superTok = { .type = TOKEN_SUPER, .start = "super",
                           .length = 5, .line = p->previous.line };
        if (matchToken(p, TOKEN_LEFT_PAREN)) {
            uint8_t argCount = 0;
            namedVariable(p, thisTok, false);
            if (!check(p, TOKEN_RIGHT_PAREN)) {
                do {
                    expression(p);
                    if (argCount == 255) error(p, "Can't have more than 255 arguments.");
                    argCount++;
                } while (matchToken(p, TOKEN_COMMA));
            }
            consume(p, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
            namedVariable(p, superTok, false);
            emitConstRef(p, OP_SUPER_INVOKE, name);
            emitByte(p, argCount);
        } else {
            namedVariable(p, thisTok, false);
            namedVariable(p, superTok, false);
            emitConstRef(p, OP_GET_SUPER, name);
        }
    }
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
    current->andOrTarget = currentChunk()->count;
}

static void orOp(Parser *p, Compiler *c, bool canAssign) {
    (void)c; (void)canAssign;
    int elseJump = emitJump(p, OP_JUMP_IF_FALSE);
    int endJump = emitJump(p, OP_JUMP);
    patchJump(p, elseJump);
    emitByte(p, OP_POP);
    parsePrecedence(p, PREC_OR);
    patchJump(p, endJump);
    current->andOrTarget = currentChunk()->count;
}

/* ---- rules table ---- */

static ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping, call,   PREC_CALL},
    [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE},
    [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_DOT]           = {NULL,     dot,    PREC_CALL},
    [TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
    [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
    [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
    [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
    [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
    [TOKEN_PERCENT]       = {NULL,     binary, PREC_FACTOR},
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
    [TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
    [TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
    [TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_FUN]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
    [TOKEN_NIL]           = {literal,  NULL,   PREC_NONE},
    [TOKEN_OR]            = {NULL,     orOp,   PREC_OR},
    [TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
    [TOKEN_SUPER]         = {super_,   NULL,   PREC_NONE},
    [TOKEN_THIS]          = {this_,    NULL,   PREC_NONE},
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

static void function(Parser *p, FunctionType type) {
    Compiler compiler;
    initCompiler(&compiler, current, type, p->previous.start, p->previous.length);
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
    /* Recursion resolves via the function's own slot-0 local, not this one. */
    function(p, TYPE_FUNCTION);
    defineVariable(p, global);
}

static void method(Parser *p) {
    /* static methods live on the class object: plain function, no `this` */
    bool isStatic = matchToken(p, TOKEN_STATIC);
    consume(p, TOKEN_IDENTIFIER, "Expect method name.");
    int constant = identifierConstant(p, &p->previous);
    FunctionType type = isStatic ? TYPE_FUNCTION : TYPE_METHOD;
    Token name = p->previous;
    if (!isStatic && name.length == 4 &&
        hk_strncmp(p->previous.start, "init", 4) == 0) {
        type = TYPE_INITIALIZER;
    }
    function(p, type);
    emitConstRef(p, isStatic ? OP_STATIC_METHOD : OP_METHOD, constant);
}

static void classDeclaration(Parser *p) {
    consume(p, TOKEN_IDENTIFIER, "Expect class name.");
    Token className = p->previous;
    int nameConstant = identifierConstant(p, &p->previous);
    declareVariable(p, &className);

    emitConstRef(p, OP_CLASS, nameConstant);
    defineVariable(p, nameConstant);

    ClassCompiler classCompiler;
    classCompiler.hasSuperclass = false;
    classCompiler.enclosing = currentClass;
    currentClass = &classCompiler;

    if (matchToken(p, TOKEN_LESS)) {
        consume(p, TOKEN_IDENTIFIER, "Expect superclass name.");
        namedVariable(p, p->previous, false);
        if (identifiersEqual(&className, &p->previous)) {
            error(p, "A class can't inherit from itself.");
        }
        beginScope();
        Token superTok = { .type = TOKEN_SUPER, .start = "super",
                           .length = 5, .line = p->previous.line };
        addLocal(p, superTok);
        defineVariable(p, 0);
        namedVariable(p, className, false);
        emitByte(p, OP_INHERIT);
        classCompiler.hasSuperclass = true;
    }

    namedVariable(p, className, false);
    consume(p, TOKEN_LEFT_BRACE, "Expect '{' before class body.");
    while (!check(p, TOKEN_RIGHT_BRACE) && !check(p, TOKEN_EOF)) {
        method(p);
    }
    consume(p, TOKEN_RIGHT_BRACE, "Expect '}' after class body.");
    emitByte(p, OP_POP);

    if (classCompiler.hasSuperclass) {
        endScope(p);
    }
    currentClass = classCompiler.enclosing;
}

static void importStatement(Parser *p) {
    if (current->scopeDepth > 0) {
        error(p, "import only allowed at top level.");
    }
    consume(p, TOKEN_STRING, "Expect module path string after 'import'.");
    Token pathTok = p->previous;
    if (pathTok.length - 2 > 250) {
        error(p, "Import path too long.");
        consume(p, TOKEN_SEMICOLON, "Expect ';' after import.");
        return;
    }
    char path[256];
    int plen = pathTok.length - 2; /* strip quotes */
    for (int i = 0; i < plen; i++) path[i] = pathTok.start[1 + i];
    path[plen] = '\0';
    for (int i = 0; i < importDepth; i++) {
        int j = 0;
        while (importStack[i][j] == path[j] && path[j] != '\0') j++;
        if (importStack[i][j] == path[j]) {
            error(p, "Circular import.");
            consume(p, TOKEN_SEMICOLON, "Expect ';' after import.");
            return;
        }
    }
    if (importDepth == 16) {
        error(p, "Import depth exceeds 16.");
        consume(p, TOKEN_SEMICOLON, "Expect ';' after import.");
        return;
    }
    const char *source = hkReadModule == NULL ? NULL : hkReadModule(path);
    if (source == NULL) {
        /* freestanding: hand-built message, no snprintf */
        char msg[300];
        int n = 0;
        const char *pre = "Cannot import module \"";
        while (*pre) msg[n++] = *pre++;
        for (int i = 0; i < plen && n < 290; i++) msg[n++] = path[i];
        msg[n++] = '"'; msg[n] = '\0';
        error(p, msg);
        consume(p, TOKEN_SEMICOLON, "Expect ';' after import.");
        return;
    }
    /* module parses inline: its declarations emit into the importing unit */
    char *persist = HK_REALLOC(NULL, 0, plen + 1);
    for (int i = 0; i <= plen; i++) persist[i] = path[i];
    importStack[importDepth++] = persist;
    Lexer savedLexer = p->lexer;
    Token savedCurrent = p->current, savedPrevious = p->previous;
    initLexer(&p->lexer, source);
    advanceParser(p);
    while (!matchToken(p, TOKEN_EOF)) {
        declaration(p);
    }
    importDepth--;
    p->lexer = savedLexer;
    p->current = savedCurrent;
    p->previous = savedPrevious;
    consume(p, TOKEN_SEMICOLON, "Expect ';' after import.");
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

/* Discard the expression value. `x = expr` in discard position ends
   SET_x; POP — fuse: the assigned value is discarded by definition.
   Tail bytes verified exactly. */
static void emitExprPop(Parser *p) {
    Chunk *ch = currentChunk();
    int n = ch->count;
    int mark = current->setStmtStart; /* instruction-start mark, not a guess */
    if (mark >= 0 && mark == n - 2 && ch->code[mark] == OP_SET_LOCAL) {
        /* binary() may have already fused the rhs: tail is then
           ADD_LOCAL_CONST8; SET_LOCAL — fold back to the one-op form */
        if (current->andOrTarget <= n - 5 && n >= 5 &&
            ch->code[n - 5] == OP_ADD_LOCAL_CONST8 &&
            ch->code[n - 4] == ch->code[n - 1]) { /* same slot */
            uint8_t slot = ch->code[n - 4], cidx = ch->code[n - 3];
            ch->count = n - 5;
            emitByte(p, OP_ADD_LOCAL_CONST8_SET);
            emitByte(p, slot);
            emitByte(p, cidx);
            return;
        }
        /* deeper fusion: `slot = slot + const` (`i = i + 1` increments) is
           GET_LOCAL; CONSTANT8; ADD; SET_LOCAL — all four marks verified */
        int lm = current->localGetStart, cm = current->numConstStart;
        if (current->andOrTarget <= n - 7 &&
            lm == n - 7 && cm == n - 5 &&
            ch->code[lm] == OP_GET_LOCAL && ch->code[cm] == OP_CONSTANT8 &&
            ch->code[n - 3] == OP_ADD &&
            ch->code[lm + 1] == ch->code[n - 1] && /* same slot */
            IS_NUMBER(ch->constants.values[ch->code[cm + 1]])) {
            uint8_t slot = ch->code[lm + 1], cidx = ch->code[cm + 1];
            ch->count = n - 7;
            emitByte(p, OP_ADD_LOCAL_CONST8_SET);
            emitByte(p, slot);
            emitByte(p, cidx);
            return;
        }
        uint8_t slot = ch->code[n - 1];
        ch->count = n - 2;
        emitBytes(p, OP_SET_LOCAL_POP, slot);
        return;
    }
    if (mark >= 0 && mark == n - 3 && ch->code[mark] == OP_SET_GLOBAL) {
        uint8_t hi = ch->code[n - 2], lo = ch->code[n - 1];
        ch->count = n - 3;
        emitByte(p, OP_SET_GLOBAL_POP);
        emitByte(p, hi);
        emitByte(p, lo);
        return;
    }
    emitByte(p, OP_POP);
}

static void expressionStatement(Parser *p) {
    expression(p);
    consume(p, TOKEN_SEMICOLON, "Expect ';' after expression.");
    emitExprPop(p);
}

/* Statement-condition jump. If the condition just compiled to a bare
   comparison at the chunk tail, fuse it with the branch: the comparison
   result is only ever consumed by this jump. (and/or keep plain
   OP_POP_JUMP_IF_FALSE — their value must survive.) */
static int emitCondJump(Parser *p) {
    Chunk *ch = currentChunk();
    int last = ch->count - 1;
    /* cmpJumpStart is an instruction-start mark — a bare code[count-1]
       check would also match operand bytes that alias LESS/GREATER */
    /* A and/or chain in the condition leaves jump targets at the chunk tail;
       rewinding past them would strand those targets mid-instruction. */
    if (current->cmpJumpStart == last && last >= 0 &&
        current->andOrTarget <= last &&
        (ch->code[last] == OP_LESS || ch->code[last] == OP_GREATER)) {
        /* deeper: `if (local < const)` / loop conditions are GET_LOCAL;
           CONSTANT8; LESS — fuse all three with the branch. Every mark
           re-verified by bytes and offsets. */
        int lm = current->localGetStart, cm = current->numConstStart;
        if (current->andOrTarget <= last - 4 &&
            ch->code[last] == OP_LESS && lm == last - 4 && cm == last - 2 &&
            ch->code[lm] == OP_GET_LOCAL && ch->code[cm] == OP_CONSTANT8 &&
            IS_NUMBER(ch->constants.values[ch->code[cm + 1]])) {
            uint8_t slot = ch->code[lm + 1], cidx = ch->code[cm + 1];
            ch->count -= 5; /* rewind the whole 5-byte pattern; jump operand appended after */
            int off = ch->count + 3; /* patchJump points at the operand */
            emitByte(p, OP_LESS_LOCAL_CONST8_JUMP);
            emitByte(p, slot);
            emitByte(p, cidx);
            emitByte(p, 0xff);
            emitByte(p, 0xff);
            return off;
        }
        uint8_t fused = ch->code[last] == OP_LESS ? OP_LESS_JUMP
                                                  : OP_GREATER_JUMP;
        ch->count = last; /* rewind 1-byte comparison; code+lines go together */
        return emitJump(p, fused);
    }
    return emitJump(p, OP_POP_JUMP_IF_FALSE);
}

static void ifStatement(Parser *p) {
    consume(p, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
    expression(p);
    consume(p, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
    int thenJump = emitCondJump(p);
    statement(p);
    int elseJump = emitJump(p, OP_JUMP);
    patchJump(p, thenJump);
    if (matchToken(p, TOKEN_ELSE)) statement(p);
    patchJump(p, elseJump);
}

static void whileStatement(Parser *p) {
    int loopStart = currentChunk()->count;
    consume(p, TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
    expression(p);
    consume(p, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
    int exitJump = emitCondJump(p);
    statement(p);
    emitLoop(p, loopStart);
    patchJump(p, exitJump);
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
        exitJump = emitCondJump(p);
    }
    if (!matchToken(p, TOKEN_RIGHT_PAREN)) {
        int bodyJump = emitJump(p, OP_JUMP);
        int incrementStart = currentChunk()->count;
        expression(p);
        emitExprPop(p);
        consume(p, TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");
        emitLoop(p, loopStart);
        loopStart = incrementStart;
        patchJump(p, bodyJump);
    }
    statement(p);
    emitLoop(p, loopStart);
    if (exitJump != -1) {
        patchJump(p, exitJump);
    }
    endScope(p);
}

static void printStatement(Parser *p) {
    expression(p);
    consume(p, TOKEN_SEMICOLON, "Expect ';' after value.");
    emitByte(p, OP_PRINT);
}

static void returnStatement(Parser *p) {
    if (current->type == TYPE_SCRIPT) {
        error(p, "Can't return from top-level code.");
    }
    if (matchToken(p, TOKEN_SEMICOLON)) {
        if (current->type == TYPE_INITIALIZER) {
            emitByte(p, OP_GET_LOCAL);
            emitByte(p, 0);
            emitByte(p, OP_RETURN);
        } else {
            emitReturn(p);
        }
    } else {
        if (current->type == TYPE_INITIALIZER) {
            error(p, "Can't return a value from an initializer.");
        }
        expression(p);
        consume(p, TOKEN_SEMICOLON, "Expect ';' after return value.");
        /* `return local;` is GET_LOCAL; RETURN at the tail — one op */
        Chunk *ch = currentChunk();
        int lm = current->localGetStart;
        if (lm >= 0 && lm == ch->count - 2 && ch->code[lm] == OP_GET_LOCAL) {
            uint8_t slot = ch->code[lm + 1];
            ch->count -= 2;
            emitBytes(p, OP_RETURN_LOCAL, slot);
            return;
        }
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
    if (matchToken(p, TOKEN_CLASS)) {
        classDeclaration(p);
    } else if (matchToken(p, TOKEN_IMPORT)) {
        importStatement(p);
    } else if (matchToken(p, TOKEN_FUN)) {
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
    currentClass = NULL;
    initCompiler(&compiler, NULL, TYPE_SCRIPT, NULL, 0);
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
