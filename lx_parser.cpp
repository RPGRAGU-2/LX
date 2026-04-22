/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  LX 3.0 — lx_parser.cpp  (clean rewrite)                           ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */
#include "lx_parser.h"

Parser::Parser(std::vector<Token> toks) : tokens(std::move(toks)) {}

Token& Parser::cur()         { return tokens[pos]; }
Token& Parser::peek(int off) { return tokens[std::min(pos+off,(int)tokens.size()-1)]; }
bool   Parser::atEnd() const { return tokens[pos].type == TK::END_OF_FILE; }

void Parser::skipNewlines()     { while (!atEnd() && cur().type==TK::NEWLINE) pos++; }
bool Parser::match(TK t)        { if(cur().type==t){pos++;return true;}return false; }
bool Parser::check(TK t) const  { return tokens[pos].type==t; }
void Parser::expectNewlineOrEnd(){ if(!atEnd()&&cur().type==TK::NEWLINE) pos++; }

Token Parser::consume(TK expected, const std::string& msg) {
    if (cur().type != expected)
        throw LXError(std::string("[LX PARSE] Expected " + msg +
            " but got '" + cur().lexeme + "' at line " + std::to_string(cur().line)));
    return tokens[pos++];
}

ASTPtr Parser::makeBlock(const ASTList& stmts, int ln) {
    auto b = makeNode(NT::BLOCK, ln); b->children = stmts; return b;
}

void Parser::parseBlock(ASTList& stmts, std::vector<TK> stopAt) {
    skipNewlines();
    while (!atEnd()) {
        TK ct = cur().type;
        for (TK s : stopAt) if (ct == s) return;
        stmts.push_back(parseStatement());
        skipNewlines();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
ASTPtr Parser::parseProgram() {
    auto prog = makeNode(NT::PROGRAM);
    skipNewlines();
    while (!atEnd()) {
        prog->children.push_back(parseStatement());
        skipNewlines();
    }
    return prog;
}

ASTPtr Parser::parseStatement() {
    skipNewlines();
    int ln = cur().line;
    switch (cur().type) {
        case TK::D_TYPE:          return parseDecl();
        case TK::REASSIGN:        return parseReassign();
        case TK::WITH_TEXT:       return parsePrint(false,false);
        case TK::WITH_TEXT_RAW:   return parsePrint(true, false);
        case TK::SCREAM:          return parsePrint(false,true);
        case TK::MAYBE:           return parseIfChain();
        case TK::LOOP_WHILE:      return parseWhile();
        case TK::LOOP_FROM:       return parseFor();
        case TK::LOOP_EACH:       return parseEach();
        case TK::BREAK_OUT:       { pos++; expectNewlineOrEnd(); return makeNode(NT::BREAK,ln); }
        case TK::SKIP_OVER:       { pos++; expectNewlineOrEnd(); return makeNode(NT::CONTINUE,ln); }
        case TK::FUNC_BEGIN:      return parseFuncDef();
        case TK::GIVE_BACK:       return parseReturn();
        case TK::CALL:            return parseCallStmt();
        case TK::LABEL_MARK:      return parseLabelMark();
        case TK::JUMP:            return parseJump();
        case TK::JUMP_IF:         return parseJumpIf();
        case TK::WITH_GET:        return parseImport();
        case TK::HALT_NOW:        { pos++; expectNewlineOrEnd(); return makeNode(NT::HALT_STMT,ln); }
        case TK::ATTEMPT:         return parseTryCatch();
        case TK::THROW_ERR:       return parseThrow();
        case TK::MATCH_ON:        return parseMatch();
        case TK::BLUEPRINT_BEGIN: return parseBlueprintDef();
        case TK::TASK_BEGIN:      return parseTaskStmt();
        case TK::TASK_WAIT:       return parseTaskWait();
        default:
            throw LXError(std::string("[LX PARSE] Unexpected token '" +
                cur().lexeme + "' at line " + std::to_string(ln)));
    }
}

// ── d_type: TYPE NAME :: EXPR ─────────────────────────────────────────────────
ASTPtr Parser::parseDecl() {
    int ln = cur().line; pos++;
    std::string dtype = cur().lexeme; pos++;
    std::string name  = consume(TK::IDENT, "variable name").lexeme;
    consume(TK::COLCOL, "::");
    ASTPtr val = parseExpr();
    expectNewlineOrEnd();
    auto n = makeNode(NT::DECL, ln);
    n->name=name; n->dtype=dtype; n->children.push_back(val);
    return n;
}

// ── reassign: NAME [.FIELD] OP EXPR ──────────────────────────────────────────
ASTPtr Parser::parseReassign() {
    int ln = cur().line; pos++;

    // Could be: reassign: self.field :: expr
    bool isSelf = check(TK::SELF);
    std::string name;
    if (isSelf) {
        pos++;  // consume 'self'
        name = "self";
    } else {
        name = consume(TK::IDENT,"variable name").lexeme;
    }

    // Attribute assignment: reassign: obj.field :: expr
    if (check(TK::DOT)) {
        pos++;
        std::string field = consume(TK::IDENT,"field name").lexeme;
        consume(TK::COLCOL,"::");
        ASTPtr val = parseExpr(); expectNewlineOrEnd();
        auto n = makeNode(NT::ATTR_ASSIGN,ln);
        n->name=name; n->sval=field; n->children.push_back(val);
        return n;
    }

    auto n = makeNode(NT::ASSIGN,ln); n->name=name;
    if      (check(TK::COLCOL))   { pos++; n->op="::"; }
    else if (check(TK::PLUS_EQ))  { pos++; n->op="+="; }
    else if (check(TK::MINUS_EQ)) { pos++; n->op="-="; }
    else if (check(TK::STAR_EQ))  { pos++; n->op="*="; }
    else if (check(TK::SLASH_EQ)) { pos++; n->op="/="; }
    else throw LXError(std::string("[LX PARSE] Expected assignment operator at line "+std::to_string(ln)));
    n->children.push_back(parseExpr());
    expectNewlineOrEnd();
    return n;
}

// ── with_text / scream ────────────────────────────────────────────────────────
ASTPtr Parser::parsePrint(bool raw, bool err) {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::PRINT,ln);
    n->bval=raw; n->sval=err?"err":"out";
    n->children.push_back(parseExpr());
    expectNewlineOrEnd(); return n;
}

// ── maybe / perhaps_elif / perhaps_not / end_maybe ───────────────────────────
ASTPtr Parser::parseIfChain() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::IF_CHAIN,ln);
    n->children.push_back(parseExpr());
    expectNewlineOrEnd(); skipNewlines();
    ASTList body;
    parseBlock(body, {TK::PERHAPS_ELIF, TK::PERHAPS_NOT, TK::END_MAYBE});
    n->alt.push_back(makeBlock(body,ln));

    while (check(TK::PERHAPS_ELIF)) {
        pos++;
        n->children.push_back(parseExpr());
        expectNewlineOrEnd(); skipNewlines();
        ASTList eb;
        parseBlock(eb, {TK::PERHAPS_ELIF, TK::PERHAPS_NOT, TK::END_MAYBE});
        n->alt.push_back(makeBlock(eb,ln));
    }
    if (check(TK::PERHAPS_NOT)) {
        pos++; expectNewlineOrEnd(); skipNewlines();
        ASTList elb;
        parseBlock(elb, {TK::END_MAYBE});
        n->alt.push_back(makeBlock(elb,ln));
    } else {
        n->alt.push_back(nullptr);
    }
    consume(TK::END_MAYBE,"end_maybe"); expectNewlineOrEnd();
    return n;
}

ASTPtr Parser::parseWhile() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::WHILE_LOOP,ln);
    n->children.push_back(parseExpr());
    expectNewlineOrEnd(); skipNewlines();
    ASTList body; parseBlock(body,{TK::END_LOOP});
    n->alt.push_back(makeBlock(body,ln));
    consume(TK::END_LOOP,"end_loop"); expectNewlineOrEnd();
    return n;
}

ASTPtr Parser::parseFor() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::FOR_LOOP,ln);
    n->name = consume(TK::IDENT,"loop variable").lexeme;
    if (cur().lexeme!="FROM")
        throw LXError(std::string("[LX PARSE] Expected FROM at line "+std::to_string(ln)));
    pos++;
    n->children.push_back(parseExpr());
    if (cur().lexeme!="TO")
        throw LXError(std::string("[LX PARSE] Expected TO at line "+std::to_string(ln)));
    pos++;
    n->children.push_back(parseExpr());
    if (!atEnd() && cur().lexeme=="STEP") {
        pos++; n->children.push_back(parseExpr());
    } else {
        auto one=makeNode(NT::INT_LIT,ln); one->ival=1; n->children.push_back(one);
    }
    expectNewlineOrEnd(); skipNewlines();
    ASTList body; parseBlock(body,{TK::END_LOOP});
    n->alt.push_back(makeBlock(body,ln));
    consume(TK::END_LOOP,"end_loop"); expectNewlineOrEnd();
    return n;
}

ASTPtr Parser::parseEach() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::EACH_LOOP,ln);
    n->name = consume(TK::IDENT,"loop variable").lexeme;
    if (cur().lexeme!="IN")
        throw LXError(std::string("[LX PARSE] Expected IN at line "+std::to_string(ln)));
    pos++;
    n->children.push_back(parseExpr());
    expectNewlineOrEnd(); skipNewlines();
    ASTList body; parseBlock(body,{TK::END_LOOP});
    n->alt.push_back(makeBlock(body,ln));
    consume(TK::END_LOOP,"end_loop"); expectNewlineOrEnd();
    return n;
}

ASTPtr Parser::parseFuncDef() {
    int ln=cur().line; pos++;
    std::string fname = consume(TK::IDENT,"function name").lexeme;
    consume(TK::LPAREN,"(");
    std::vector<std::string> params;
    while (!check(TK::RPAREN)&&!atEnd()) {
        params.push_back(consume(TK::IDENT,"parameter").lexeme);
        if (!check(TK::RPAREN)) consume(TK::COMMA,",");
    }
    consume(TK::RPAREN,")");
    expectNewlineOrEnd(); skipNewlines();
    ASTList body; parseBlock(body,{TK::FUNC_END});
    consume(TK::FUNC_END,"func_end"); expectNewlineOrEnd();
    auto n=makeNode(NT::FUNC_DEF,ln); n->name=fname;
    for (auto& p:params){auto pn=makeNode(NT::IDENT,ln);pn->name=p;n->params.push_back(pn);}
    n->alt.push_back(makeBlock(body,ln));
    return n;
}

ASTPtr Parser::parseReturn() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::RETURN_STMT,ln);
    if (!check(TK::NEWLINE)&&!atEnd())
        n->children.push_back(parseExpr());
    else { auto v=makeNode(NT::VOID_LIT,ln); n->children.push_back(v); }
    expectNewlineOrEnd(); return n;
}

ASTPtr Parser::parseCallStmt() {
    int ln=cur().line; pos++;
    ASTPtr callExpr = parseCallExprOrAttr(ln);
    expectNewlineOrEnd();
    auto n=makeNode(NT::CALL_STMT,ln); n->children.push_back(callExpr);
    return n;
}

ASTPtr Parser::parseLabelMark() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::LABEL_STMT,ln);
    n->name=consume(TK::IDENT,"label name").lexeme;
    expectNewlineOrEnd(); return n;
}
ASTPtr Parser::parseJump() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::JUMP_STMT,ln);
    n->name=consume(TK::IDENT,"label name").lexeme;
    expectNewlineOrEnd(); return n;
}
ASTPtr Parser::parseJumpIf() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::JUMP_IF_STMT,ln);
    n->children.push_back(parseExpr());
    consume(TK::ARROW,"->"); n->name=consume(TK::IDENT,"label").lexeme;
    expectNewlineOrEnd(); return n;
}
ASTPtr Parser::parseImport() {
    int ln=cur().line; pos++;
    std::string fname;
    while (!check(TK::NEWLINE)&&!atEnd()){fname+=cur().lexeme;pos++;}
    if (!fname.empty()&&fname.front()=='<') fname=fname.substr(1);
    if (!fname.empty()&&fname.back()== '>') fname.pop_back();
    expectNewlineOrEnd();
    auto n=makeNode(NT::IMPORT_STMT,ln); n->name=trim(fname); return n;
}

// ── attempt / catch_err / catch_type / end_attempt ───────────────────────────
ASTPtr Parser::parseTryCatch() {
    int ln=cur().line; pos++;
    expectNewlineOrEnd(); skipNewlines();
    ASTList tryBody;
    parseBlock(tryBody,{TK::CATCH_ERR,TK::CATCH_TYPE,TK::END_ATTEMPT});
    auto n=makeNode(NT::TRY_CATCH,ln);
    n->alt.push_back(makeBlock(tryBody,ln));

    while (check(TK::CATCH_ERR)||check(TK::CATCH_TYPE)) {
        bool isTyped=check(TK::CATCH_TYPE); pos++;
        CatchClause cc;
        if (check(TK::LPAREN)) {
            pos++;
            cc.errorVar=consume(TK::IDENT,"error variable").lexeme;
            if (isTyped&&check(TK::COMMA)){pos++;cc.errorType=consume(TK::IDENT,"error type").lexeme;}
            consume(TK::RPAREN,")");
        }
        expectNewlineOrEnd(); skipNewlines();
        parseBlock(cc.body,{TK::CATCH_ERR,TK::CATCH_TYPE,TK::END_ATTEMPT});
        n->catches.push_back(cc);
    }
    consume(TK::END_ATTEMPT,"end_attempt"); expectNewlineOrEnd();
    return n;
}
ASTPtr Parser::parseThrow() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::THROW_STMT,ln);
    n->children.push_back(parseExpr()); expectNewlineOrEnd(); return n;
}

// ── match_on / when_is / when_else / end_match ────────────────────────────────
ASTPtr Parser::parseMatch() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::MATCH_STMT,ln);
    n->children.push_back(parseExpr()); expectNewlineOrEnd(); skipNewlines();
    while (check(TK::WHEN_IS)||check(TK::WHEN_ELSE)) {
        MatchArm arm;
        if (check(TK::WHEN_IS)) { pos++; arm.condition=parseExpr(); }
        else                    { pos++; arm.condition=nullptr; }
        expectNewlineOrEnd(); skipNewlines();
        parseBlock(arm.body,{TK::WHEN_IS,TK::WHEN_ELSE,TK::END_MATCH});
        n->arms.push_back(std::move(arm)); skipNewlines();
    }
    consume(TK::END_MATCH,"end_match"); expectNewlineOrEnd();
    return n;
}

// ── blueprint_begin: NAME [inherits: PARENT] ... blueprint_end: ──────────────
ASTPtr Parser::parseBlueprintDef() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::BLUEPRINT_DEF,ln);
    n->name=consume(TK::IDENT,"blueprint name").lexeme;

    // inherits: can be on same line OR the very next line
    if (check(TK::INHERITS)) {
        pos++; n->parent=consume(TK::IDENT,"parent name").lexeme;
    }
    expectNewlineOrEnd(); skipNewlines();
    // Also check if inherits: is on the next line (common style)
    if (check(TK::INHERITS)) {
        pos++; n->parent=consume(TK::IDENT,"parent name").lexeme;
        expectNewlineOrEnd(); skipNewlines();
    }

    while (!check(TK::BLUEPRINT_END)&&!atEnd()) {
        skipNewlines();
        if (check(TK::BLUEPRINT_END)) break;
        if (check(TK::INIT_BEGIN)) {
            pos++;
            std::vector<std::string> params;
            if (check(TK::LPAREN)) {
                pos++;
                while (!check(TK::RPAREN)&&!atEnd()) {
                    params.push_back(consume(TK::IDENT,"param").lexeme);
                    if (!check(TK::RPAREN)) consume(TK::COMMA,",");
                }
                consume(TK::RPAREN,")");
            }
            expectNewlineOrEnd(); skipNewlines();
            ASTList body; parseBlock(body,{TK::INIT_END});
            consume(TK::INIT_END,"init_end"); expectNewlineOrEnd();
            auto fn=makeNode(NT::FUNC_DEF,ln); fn->name="__init__";
            for (auto& p:params){auto pn=makeNode(NT::IDENT,ln);pn->name=p;fn->params.push_back(pn);}
            fn->alt.push_back(makeBlock(body,ln)); n->initBlock=fn;
        } else if (check(TK::METHOD_BEGIN)) {
            pos++;
            std::string mname=consume(TK::IDENT,"method name").lexeme;
            consume(TK::LPAREN,"(");
            std::vector<std::string> params;
            while (!check(TK::RPAREN)&&!atEnd()) {
                params.push_back(consume(TK::IDENT,"param").lexeme);
                if (!check(TK::RPAREN)) consume(TK::COMMA,",");
            }
            consume(TK::RPAREN,")");
            expectNewlineOrEnd(); skipNewlines();
            ASTList body; parseBlock(body,{TK::METHOD_END});
            consume(TK::METHOD_END,"method_end"); expectNewlineOrEnd();
            auto fn=makeNode(NT::FUNC_DEF,ln); fn->name=mname;
            for (auto& p:params){auto pn=makeNode(NT::IDENT,ln);pn->name=p;fn->params.push_back(pn);}
            fn->alt.push_back(makeBlock(body,ln)); n->methods.push_back({mname,fn});
        } else {
            if (!atEnd()) pos++;
        }
    }
    consume(TK::BLUEPRINT_END,"blueprint_end"); expectNewlineOrEnd();
    return n;
}

ASTPtr Parser::parseTaskStmt() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::TASK_STMT,ln);
    n->name=consume(TK::IDENT,"task name").lexeme;
    expectNewlineOrEnd(); skipNewlines();
    ASTList body; parseBlock(body,{TK::TASK_END});
    n->alt.push_back(makeBlock(body,ln));
    consume(TK::TASK_END,"task_end"); expectNewlineOrEnd();
    return n;
}
ASTPtr Parser::parseTaskWait() {
    int ln=cur().line; pos++;
    auto n=makeNode(NT::TASK_WAIT_STMT,ln);
    n->name=consume(TK::IDENT,"task name").lexeme;
    expectNewlineOrEnd(); return n;
}

// ── Call statement: either funcName(args) or obj.method(args) ─────────────────
ASTPtr Parser::parseCallExprOrAttr(int ln) {
    std::string firstName=consume(TK::IDENT,"name").lexeme;
    if (check(TK::DOT)) {
        pos++;
        std::string method=consume(TK::IDENT,"method name").lexeme;
        consume(TK::LPAREN,"(");
        auto n=makeNode(NT::ATTR_EXPR,ln);
        n->sval=method;
        n->bval=true;  // method call
        auto obj=makeNode(NT::IDENT,ln); obj->name=firstName;
        n->children.push_back(obj);
        while (!check(TK::RPAREN)&&!atEnd()){
            n->children.push_back(parseExpr());
            if (!check(TK::RPAREN)) consume(TK::COMMA,",");
        }
        consume(TK::RPAREN,")"); return n;
    }
    consume(TK::LPAREN,"(");
    auto n=makeNode(NT::CALL_EXPR,ln); n->name=firstName;
    while (!check(TK::RPAREN)&&!atEnd()) {
        n->children.push_back(parseExpr());
        if (!check(TK::RPAREN)) consume(TK::COMMA,",");
    }
    consume(TK::RPAREN,")"); return n;
}

ASTPtr Parser::parseCallExpr(int ln) {
    std::string fname=consume(TK::IDENT,"function name").lexeme;
    consume(TK::LPAREN,"(");
    auto n=makeNode(NT::CALL_EXPR,ln); n->name=fname;
    while (!check(TK::RPAREN)&&!atEnd()){
        n->children.push_back(parseExpr());
        if (!check(TK::RPAREN)) consume(TK::COMMA,",");
    }
    consume(TK::RPAREN,")"); return n;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Expression parsing (Pratt)
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr Parser::parseExpr()       { return parseOr(); }
ASTPtr Parser::parseOr() {
    auto left=parseAnd();
    while (check(TK::OR_W)){
        int ln=cur().line; pos++;
        auto n=makeNode(NT::BINOP,ln); n->op="OR_W";
        n->children.push_back(left); n->children.push_back(parseAnd()); left=n;
    } return left;
}
ASTPtr Parser::parseAnd() {
    auto left=parseNot();
    while (check(TK::AND_W)){
        int ln=cur().line; pos++;
        auto n=makeNode(NT::BINOP,ln); n->op="AND_W";
        n->children.push_back(left); n->children.push_back(parseNot()); left=n;
    } return left;
}
ASTPtr Parser::parseNot() {
    if (check(TK::NOT_W)){
        int ln=cur().line; pos++;
        auto n=makeNode(NT::UNOP,ln); n->op="NOT_W";
        n->children.push_back(parseNot()); return n;
    } return parseComparison();
}
ASTPtr Parser::parseComparison() {
    auto left=parseAddSub();
    static const std::vector<TK> cops={TK::IS,TK::IS_NOT,TK::GT,TK::LT,TK::GT_EQ,TK::LT_EQ};
    while (std::find(cops.begin(),cops.end(),cur().type)!=cops.end()){
        int ln=cur().line; std::string op=cur().lexeme; pos++;
        auto n=makeNode(NT::BINOP,ln); n->op=op;
        n->children.push_back(left); n->children.push_back(parseAddSub()); left=n;
    } return left;
}
ASTPtr Parser::parseAddSub() {
    auto left=parseMulDiv();
    while (check(TK::PLUS)||check(TK::MINUS)){
        int ln=cur().line; std::string op=cur().lexeme; pos++;
        auto n=makeNode(NT::BINOP,ln); n->op=op;
        n->children.push_back(left); n->children.push_back(parseMulDiv()); left=n;
    } return left;
}
ASTPtr Parser::parseMulDiv() {
    auto left=parsePower();
    while (check(TK::STAR)||check(TK::SLASH)||check(TK::PERCENT)){
        int ln=cur().line; std::string op=cur().lexeme; pos++;
        auto n=makeNode(NT::BINOP,ln); n->op=op;
        n->children.push_back(left); n->children.push_back(parsePower()); left=n;
    } return left;
}
ASTPtr Parser::parsePower() {
    auto left=parseUnary();
    if (check(TK::POWER)){
        int ln=cur().line; pos++;
        auto n=makeNode(NT::BINOP,ln); n->op="**";
        n->children.push_back(left); n->children.push_back(parsePower()); return n;
    } return left;
}
ASTPtr Parser::parseUnary() {
    if (check(TK::MINUS)){
        int ln=cur().line; pos++;
        auto n=makeNode(NT::UNOP,ln); n->op="-";
        n->children.push_back(parsePostfix()); return n;
    } return parsePostfix();
}
ASTPtr Parser::parsePostfix() {
    auto base=parsePrimary();
    while (true) {
        if (check(TK::LBRACKET)){
            int ln=cur().line; pos++;
            auto n=makeNode(NT::INDEX_EXPR,ln);
            n->children.push_back(base); n->children.push_back(parseExpr());
            consume(TK::RBRACKET,"]"); base=n;
        } else if (check(TK::DOT)){
            int ln=cur().line; pos++;
            std::string field=consume(TK::IDENT,"field/method").lexeme;
            if (check(TK::LPAREN)){
                pos++;
                auto n=makeNode(NT::ATTR_EXPR,ln); n->sval=field;
                n->bval = true;  // ← marks as method call
                n->children.push_back(base);
                while (!check(TK::RPAREN)&&!atEnd()){
                    n->children.push_back(parseExpr());
                    if (!check(TK::RPAREN)) consume(TK::COMMA,",");
                }
                consume(TK::RPAREN,")"); base=n;
            } else {
                auto n=makeNode(NT::ATTR_EXPR,ln); n->sval=field;
                n->bval = false; // ← marks as field access
                n->children.push_back(base); base=n;
            }
        } else break;
    }
    return base;
}

ASTPtr Parser::parsePrimary() {
    int ln=cur().line;
    switch (cur().type) {
        case TK::INT_LIT:   { auto n=makeNode(NT::INT_LIT,ln);   n->ival=cur().ival; pos++; return n; }
        case TK::FLOAT_LIT: { auto n=makeNode(NT::FLOAT_LIT,ln); n->dval=cur().dval; pos++; return n; }
        case TK::STR_LIT:   { auto n=makeNode(NT::STR_LIT,ln);   n->sval=cur().sval; pos++; return n; }
        case TK::CHAR_LIT:  { auto n=makeNode(NT::CHAR_LIT,ln);  n->cval=cur().cval; pos++; return n; }
        case TK::BOOL_LIT:  { auto n=makeNode(NT::BOOL_LIT,ln);  n->bval=cur().bval; pos++; return n; }
        case TK::VOIDUM:    { pos++; return makeNode(NT::VOID_LIT,ln); }
        case TK::SELF:      { pos++; return makeNode(NT::SELF_EXPR,ln); }
        case TK::LPAREN:    { pos++; auto e=parseExpr(); consume(TK::RPAREN,")"); return e; }
        case TK::LBRACKET: {
            pos++; auto n=makeNode(NT::LIST_LITERAL,ln);
            while (!check(TK::RBRACKET)&&!atEnd()){
                n->children.push_back(parseExpr());
                if (!check(TK::RBRACKET)) consume(TK::COMMA,",");
            }
            consume(TK::RBRACKET,"]"); return n;
        }
        case TK::LBRACE: {
            pos++; auto n=makeNode(NT::MAP_LITERAL,ln);
            while (!check(TK::RBRACE)&&!atEnd()){
                n->children.push_back(parseExpr()); consume(TK::COLCOL,"::");
                n->children.push_back(parseExpr());
                if (!check(TK::RBRACE)) consume(TK::COMMA,",");
            }
            consume(TK::RBRACE,"}"); return n;
        }
        case TK::NEW_OBJ: {
            pos++; auto n=makeNode(NT::NEW_EXPR,ln);
            n->name=consume(TK::IDENT,"blueprint name").lexeme;
            consume(TK::LPAREN,"(");
            while (!check(TK::RPAREN)&&!atEnd()){
                n->children.push_back(parseExpr());
                if (!check(TK::RPAREN)) consume(TK::COMMA,",");
            }
            consume(TK::RPAREN,")"); return n;
        }
        case TK::SUPER_CALL: {
            pos++; auto n=makeNode(NT::SUPER_EXPR,ln);
            n->name=consume(TK::IDENT,"method name").lexeme;
            consume(TK::LPAREN,"(");
            while (!check(TK::RPAREN)&&!atEnd()){
                n->children.push_back(parseExpr());
                if (!check(TK::RPAREN)) consume(TK::COMMA,",");
            }
            consume(TK::RPAREN,")"); return n;
        }
        case TK::IDENT: {
            if (peek().type==TK::LPAREN) return parseCallExpr(ln);
            auto n=makeNode(NT::IDENT,ln); n->name=cur().lexeme; pos++; return n;
        }
        default:
            throw LXError(std::string("[LX PARSE] Unexpected token in expression: '"+
                cur().lexeme+"' at line "+std::to_string(ln)));
    }
}
