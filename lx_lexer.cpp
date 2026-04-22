/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  LX 3.0 — lx_lexer.cpp                                              ║
 * ║  Tokeniser implementation                                            ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */
#include "lx_lexer.h"

static const std::unordered_map<std::string, TK> KEYWORDS = {
    {"d_type",          TK::D_TYPE},
    {"reassign",        TK::REASSIGN},
    // types
    {"short",           TK::TY_SHORT},
    {"long",            TK::TY_LONG},
    {"int",             TK::TY_INT},
    {"float",           TK::TY_FLOAT},
    {"double",          TK::TY_DOUBLE},
    {"string",          TK::TY_STRING},
    {"char",            TK::TY_CHAR},
    {"bool",            TK::TY_BOOL},
    {"list",            TK::TY_LIST},
    {"map",             TK::TY_MAP},
    {"object",          TK::TY_OBJECT},
    // control
    {"maybe",           TK::MAYBE},
    {"perhaps_elif",    TK::PERHAPS_ELIF},
    {"perhaps_not",     TK::PERHAPS_NOT},
    {"end_maybe",       TK::END_MAYBE},
    {"loop_from",       TK::LOOP_FROM},
    {"loop_while",      TK::LOOP_WHILE},
    {"loop_each",       TK::LOOP_EACH},
    {"end_loop",        TK::END_LOOP},
    {"break_out",       TK::BREAK_OUT},
    {"skip_over",       TK::SKIP_OVER},
    // functions
    {"func_begin",      TK::FUNC_BEGIN},
    {"func_end",        TK::FUNC_END},
    {"give_back",       TK::GIVE_BACK},
    {"call",            TK::CALL},
    // OOP
    {"blueprint_begin", TK::BLUEPRINT_BEGIN},
    {"blueprint_end",   TK::BLUEPRINT_END},
    {"init_begin",      TK::INIT_BEGIN},
    {"init_end",        TK::INIT_END},
    {"method_begin",    TK::METHOD_BEGIN},
    {"method_end",      TK::METHOD_END},
    {"new_obj",         TK::NEW_OBJ},
    {"self",            TK::SELF},
    {"inherits",        TK::INHERITS},
    {"super_call",      TK::SUPER_CALL},
    // io
    {"with_text",       TK::WITH_TEXT},
    {"with_text_raw",   TK::WITH_TEXT_RAW},
    {"scream",          TK::SCREAM},
    // jump
    {"label_mark",      TK::LABEL_MARK},
    {"jump",            TK::JUMP},
    {"jump_if",         TK::JUMP_IF},
    // module
    {"with_get",        TK::WITH_GET},
    // error
    {"attempt",         TK::ATTEMPT},
    {"catch_err",       TK::CATCH_ERR},
    {"catch_type",      TK::CATCH_TYPE},
    {"end_attempt",     TK::END_ATTEMPT},
    {"throw_err",       TK::THROW_ERR},
    // match
    {"match_on",        TK::MATCH_ON},
    {"when_is",         TK::WHEN_IS},
    {"when_else",       TK::WHEN_ELSE},
    {"end_match",       TK::END_MATCH},
    // tasks
    {"task_begin",      TK::TASK_BEGIN},
    {"task_end",        TK::TASK_END},
    {"task_wait",       TK::TASK_WAIT},
    {"task_lock",       TK::TASK_LOCK},
    {"task_unlock",     TK::TASK_UNLOCK},
    // special
    {"halt_now",        TK::HALT_NOW},
    // bool values
    {"VERUM",           TK::VERUM},
    {"FALSUM",          TK::FALSUM},
    {"VOIDUM",          TK::VOIDUM},
    // comparison / logical operators (uppercase identifiers)
    {"IS",              TK::IS},
    {"IS_NOT",          TK::IS_NOT},
    {"GT",              TK::GT},
    {"LT",              TK::LT},
    {"GT_EQ",           TK::GT_EQ},
    {"LT_EQ",           TK::LT_EQ},
    {"AND_W",           TK::AND_W},
    {"OR_W",            TK::OR_W},
    {"NOT_W",           TK::NOT_W},
    // loop helper keywords (lexed as plain idents, parser checks .lexeme)
    {"FROM",            TK::IDENT},
    {"TO",              TK::IDENT},
    {"STEP",            TK::IDENT},
    {"IN",              TK::IDENT},
};

// ─────────────────────────────────────────────────────────────────────────────
Lexer::Lexer(const std::string& source, std::string fname)
    : src(source), filename(std::move(fname)) {}

char Lexer::peek(int off) const {
    int i = pos + off;
    return (i >= 0 && i < (int)src.size()) ? src[i] : '\0';
}

bool Lexer::isExprEnd(TK t) const {
    return t == TK::RPAREN || t == TK::RBRACKET ||
           t == TK::INT_LIT || t == TK::FLOAT_LIT ||
           t == TK::STR_LIT || t == TK::IDENT ||
           t == TK::VERUM   || t == TK::FALSUM;
}

void Lexer::skipWhitespace() {
    while (pos < (int)src.size() &&
           (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r'))
        pos++;
}

Token Lexer::makeTok(TK type, const std::string& lex) const {
    Token t; t.type = type; t.lexeme = lex; t.line = line;
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    tokens.reserve(512);

    while (pos < (int)src.size()) {
        skipWhitespace();
        if (pos >= (int)src.size()) break;

        // Comment: ~~
        if (peek(0) == '~' && peek(1) == '~') {
            while (pos < (int)src.size() && src[pos] != '\n') pos++;
            continue;
        }

        char c = src[pos];

        if (c == '\n') {
            tokens.push_back(makeTok(TK::NEWLINE, "\n"));
            pos++; line++;
            continue;
        }

        if (c == '"')  { tokens.push_back(lexString()); continue; }
        if (c == '\'') { tokens.push_back(lexChar());   continue; }

        // Negative number: only if previous token makes a unary minus context
        if (c == '-' && isdigit(peek(1)) &&
            (tokens.empty() || !isExprEnd(tokens.back().type))) {
            tokens.push_back(lexNumber()); continue;
        }
        if (isdigit(c)) { tokens.push_back(lexNumber()); continue; }
        if (isalpha(c) || c == '_') { tokens.push_back(lexIdent()); continue; }

        // Multi-char operators
        if (c == ':' && peek(1) == ':') { tokens.push_back(makeTok(TK::COLCOL, "::")); pos+=2; continue; }
        if (c == '-' && peek(1) == '>') { tokens.push_back(makeTok(TK::ARROW,  "->")); pos+=2; continue; }
        if (c == '+' && peek(1) == '=') { tokens.push_back(makeTok(TK::PLUS_EQ, "+=")); pos+=2; continue; }
        if (c == '-' && peek(1) == '=') { tokens.push_back(makeTok(TK::MINUS_EQ,"-=")); pos+=2; continue; }
        if (c == '*' && peek(1) == '=') { tokens.push_back(makeTok(TK::STAR_EQ, "*=")); pos+=2; continue; }
        if (c == '/' && peek(1) == '=') { tokens.push_back(makeTok(TK::SLASH_EQ,"/=")); pos+=2; continue; }
        if (c == '*' && peek(1) == '*') { tokens.push_back(makeTok(TK::POWER,   "**")); pos+=2; continue; }

        // Single-char
        switch (c) {
            case '+': tokens.push_back(makeTok(TK::PLUS,     "+")); pos++; break;
            case '-': tokens.push_back(makeTok(TK::MINUS,    "-")); pos++; break;
            case '*': tokens.push_back(makeTok(TK::STAR,     "*")); pos++; break;
            case '/': tokens.push_back(makeTok(TK::SLASH,    "/")); pos++; break;
            case '%': tokens.push_back(makeTok(TK::PERCENT,  "%")); pos++; break;
            case ',': tokens.push_back(makeTok(TK::COMMA,    ",")); pos++; break;
            case '(': tokens.push_back(makeTok(TK::LPAREN,   "(")); pos++; break;
            case ')': tokens.push_back(makeTok(TK::RPAREN,   ")")); pos++; break;
            case '[': tokens.push_back(makeTok(TK::LBRACKET, "[")); pos++; break;
            case ']': tokens.push_back(makeTok(TK::RBRACKET, "]")); pos++; break;
            case '{': tokens.push_back(makeTok(TK::LBRACE,   "{")); pos++; break;
            case '}': tokens.push_back(makeTok(TK::RBRACE,   "}")); pos++; break;
            case '.': tokens.push_back(makeTok(TK::DOT,      ".")); pos++; break;
            case ':': pos++; break;   // lone colon: consumed silently
            case '\r': pos++; break;
            default:
                throw LXError(std::string("[LX LEX] Unexpected character '" +
                    std::string(1, c) + "' at line " + std::to_string(line)));
        }
    }
    tokens.push_back(makeTok(TK::END_OF_FILE, ""));
    return tokens;
}

// ─────────────────────────────────────────────────────────────────────────────
Token Lexer::lexString() {
    int startLine = line;
    pos++; // skip "
    std::string val;
    while (pos < (int)src.size() && src[pos] != '"') {
        if (src[pos] == '\\') {
            pos++;
            if (pos >= (int)src.size()) break;
            switch (src[pos]) {
                case 'n':  val += '\n'; break;
                case 't':  val += '\t'; break;
                case '"':  val += '"';  break;
                case '\\': val += '\\'; break;
                case '0':  val += '\0'; break;
                case 'r':  val += '\r'; break;
                default:   val += src[pos]; break;
            }
        } else {
            if (src[pos] == '\n') line++;
            val += src[pos];
        }
        pos++;
    }
    if (pos >= (int)src.size())
        throw LXError(std::string("[LX LEX] Unterminated string at line " + std::to_string(startLine)));
    pos++; // skip closing "
    Token t; t.type = TK::STR_LIT; t.lexeme = "\"" + val + "\"";
    t.sval = val; t.line = startLine;
    return t;
}

Token Lexer::lexChar() {
    int startLine = line;
    pos++; // skip '
    if (pos >= (int)src.size())
        throw LXError(std::string("[LX LEX] Unterminated char literal at line " + std::to_string(startLine)));
    char c = src[pos++];
    if (c == '\\' && pos < (int)src.size()) {
        char esc = src[pos++];
        switch (esc) {
            case 'n': c = '\n'; break; case 't': c = '\t'; break;
            case '\\': c = '\\'; break; case '\'': c = '\''; break;
            default: c = esc; break;
        }
    }
    if (pos < (int)src.size() && src[pos] == '\'') pos++;
    Token t; t.type = TK::CHAR_LIT; t.lexeme = std::string(1, c);
    t.cval = c; t.line = startLine;
    return t;
}

Token Lexer::lexNumber() {
    std::string num;
    if (src[pos] == '-') { num += '-'; pos++; }
    bool isFloat = false;
    while (pos < (int)src.size() && (isdigit(src[pos]) || src[pos] == '.')) {
        if (src[pos] == '.') {
            // make sure it's not a field access (1.method)
            if (isFloat) break; // second dot — stop
            isFloat = true;
        }
        num += src[pos++];
    }
    Token t; t.line = line; t.lexeme = num;
    if (isFloat) { t.type = TK::FLOAT_LIT; t.dval = std::stod(num); }
    else          { t.type = TK::INT_LIT;   t.ival = std::stoll(num); }
    return t;
}

Token Lexer::lexIdent() {
    std::string id;
    while (pos < (int)src.size() && (isalnum(src[pos]) || src[pos] == '_'))
        id += src[pos++];
    Token t; t.line = line; t.lexeme = id;
    auto it = KEYWORDS.find(id);
    if (it != KEYWORDS.end()) {
        t.type = it->second;
        if (t.type == TK::VERUM)  { t.type = TK::BOOL_LIT; t.bval = true; }
        if (t.type == TK::FALSUM) { t.type = TK::BOOL_LIT; t.bval = false; }
    } else {
        t.type = TK::IDENT;
    }
    return t;
}
