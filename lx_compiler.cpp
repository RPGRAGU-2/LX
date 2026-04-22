/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  LX 3.0 — lx_compiler.cpp                                           ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */
#include "lx_compiler.h"

std::string Compiler::uniqueName(const std::string& base, int line) {
    return base + "_" + std::to_string(line) + "_" + std::to_string(chunks.size());
}

// ─────────────────────────────────────────────────────────────────────────────
void Compiler::compile(ASTPtr program) {
    chunks["__main__"] = Chunk();
    chunks["__main__"].name = "__main__";
    cur = &chunks["__main__"];

    hoistBlueprints(program);
    hoistFunctions(program);
    compileProgram(program);
    cur->emit(OP::HALT);
    resolveJumps();
}

// ─── Pass 1: hoist blueprint definitions ────────────────────────────────────
void Compiler::hoistBlueprints(ASTPtr prog) {
    for (auto& stmt : prog->children) {
        if (stmt->kind == NT::BLUEPRINT_DEF) {
            compileBlueprint(stmt);
        }
    }
}

// ─── Pass 2: hoist top-level function defs ──────────────────────────────────
void Compiler::hoistFunctions(ASTPtr prog) {
    for (auto& stmt : prog->children) {
        if (stmt->kind == NT::FUNC_DEF) {
            compileFunc(stmt);
        }
    }
}

// ─── Main pass: top-level statements ────────────────────────────────────────
void Compiler::compileProgram(ASTPtr prog) {
    cur = &chunks["__main__"];
    for (auto& stmt : prog->children) {
        if (stmt->kind != NT::FUNC_DEF && stmt->kind != NT::BLUEPRINT_DEF) {
            compileStmt(stmt);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void Compiler::compileBlueprint(ASTPtr node) {
    BlueprintMeta meta;
    meta.name   = node->name;
    meta.parent = node->parent;

    std::string saved = currentBlueprint;
    currentBlueprint = node->name;

    // Compile __init__
    if (node->initBlock) {
        std::string chunkName = node->name + ".__init__";
        compileFunc(node->initBlock, chunkName, node->name);
        meta.methodChunks["__init__"] = chunkName;
    }

    // Compile methods
    for (auto& [mname, mnode] : node->methods) {
        std::string chunkName = node->name + "." + mname;
        compileFunc(mnode, chunkName, node->name);
        meta.methodChunks[mname] = chunkName;
    }

    blueprints[node->name] = meta;
    currentBlueprint = saved;
}

// ─────────────────────────────────────────────────────────────────────────────
void Compiler::compileFunc(ASTPtr node, const std::string& nameOverride,
                            const std::string& bpOwner) {
    std::string chunkName = nameOverride.empty() ? node->name : nameOverride;
    Chunk fc;
    fc.name = chunkName;
    fc.blueprintOwner = bpOwner;
    for (auto& p : node->params) fc.params.push_back(p->name);
    if (!bpOwner.empty()) fc.params.insert(fc.params.begin(), "self");

    Chunk* saved = cur;
    chunks[chunkName] = fc;
    cur = &chunks[chunkName];
    compileBlock(node->alt[0]);
    cur->emit(OP::PUSH_VOID, node->line);
    cur->emit(OP::RET, node->line);
    cur = saved;
}

// ─────────────────────────────────────────────────────────────────────────────
void Compiler::compileBlock(ASTPtr block) {
    if (!block) return;
    for (auto& s : block->children) compileStmt(s);
}

// ─────────────────────────────────────────────────────────────────────────────
void Compiler::compileStmt(ASTPtr n) {
    if (!n) return;
    int ln = n->line;

    switch (n->kind) {
        case NT::DECL: {
            compileExpr(n->children[0]);
            cur->emitS(OP::STORE_NEW, n->name, ln);
            break;
        }
        case NT::ASSIGN: {
            if (n->op == "::") {
                compileExpr(n->children[0]);
            } else {
                cur->emitS(OP::LOAD, n->name, ln);
                compileExpr(n->children[0]);
                if      (n->op == "+=") cur->emit(OP::AUG_ADD, ln);
                else if (n->op == "-=") cur->emit(OP::AUG_SUB, ln);
                else if (n->op == "*=") cur->emit(OP::AUG_MUL, ln);
                else if (n->op == "/=") cur->emit(OP::AUG_DIV, ln);
            }
            cur->emitS(OP::STORE, n->name, ln);
            break;
        }
        case NT::ATTR_ASSIGN: {
            // reassign: obj.field :: expr  OR  reassign: self.field :: expr
            if (n->name == "self")
                cur->emit(OP::LOAD_SELF, ln);
            else
                cur->emitS(OP::LOAD, n->name, ln);
            compileExpr(n->children[0]);
            cur->emitS(OP::SET_ATTR, n->sval, ln);
            break;
        }
        case NT::PRINT: {
            compileExpr(n->children[0]);
            if (n->sval == "err")  cur->emit(OP::PRINT_ERR, ln);
            else if (n->bval)      cur->emit(OP::PRINT_RAW, ln);
            else                   cur->emit(OP::PRINT_NL,  ln);
            break;
        }
        case NT::IF_CHAIN:    compileIfChain(n);  break;
        case NT::WHILE_LOOP:  compileWhile(n);    break;
        case NT::FOR_LOOP:    compileFor(n);       break;
        case NT::EACH_LOOP:   compileEach(n);      break;
        case NT::BREAK: {
            if (loopStack.empty())
                throw LXError(std::string("[LX COMPILE] break_out outside loop at line " + std::to_string(ln)));
            int idx = cur->emitI(OP::JMP, 0, ln);
            loopStack.back().breakPatches.push_back(idx);
            break;
        }
        case NT::CONTINUE: {
            if (loopStack.empty())
                throw LXError(std::string("[LX COMPILE] skip_over outside loop at line " + std::to_string(ln)));
            int idx = cur->emitI(OP::JMP, 0, ln);
            loopStack.back().contPatches.push_back(idx);
            break;
        }
        case NT::FUNC_DEF:      /* already hoisted */  break;
        case NT::BLUEPRINT_DEF: /* already hoisted */  break;
        case NT::RETURN_STMT: {
            compileExpr(n->children[0]);
            cur->emit(OP::RET, ln);
            break;
        }
        case NT::CALL_STMT: {
            compileExpr(n->children[0]);
            cur->emit(OP::POP, ln);
            break;
        }
        case NT::LABEL_STMT: {
            labelMap[n->name] = cur->here();
            break;
        }
        case NT::JUMP_STMT: {
            auto it = labelMap.find(n->name);
            if (it != labelMap.end()) {
                cur->emitI(OP::JMP, it->second, ln);
            } else {
                int idx = cur->emitI(OP::JMP, 0, ln);
                pendingJumps.push_back({idx, n->name});
            }
            break;
        }
        case NT::JUMP_IF_STMT: {
            compileExpr(n->children[0]);
            auto it = labelMap.find(n->name);
            if (it != labelMap.end()) {
                cur->emitI(OP::JMP_TRUE, it->second, ln);
            } else {
                int idx = cur->emitI(OP::JMP_TRUE, 0, ln);
                pendingJumps.push_back({idx, n->name});
            }
            break;
        }
        case NT::IMPORT_STMT: break; // handled before compilation
        case NT::HALT_STMT:   cur->emit(OP::HALT, ln); break;
        case NT::TRY_CATCH:   compileTryCatch(n);  break;
        case NT::THROW_STMT: {
            compileExpr(n->children[0]);
            cur->emit(OP::THROW, ln);
            break;
        }
        case NT::MATCH_STMT:  compileMatch(n);    break;
        case NT::TASK_STMT:   compileTaskStmt(n); break;
        case NT::TASK_WAIT_STMT: {
            cur->emitS(OP::CALL, "__task_wait__", ln);
            cur->code.back().sarg  = n->name;
            cur->code.back().iarg2 = 0;
            break;
        }
        case NT::BLOCK: compileBlock(n); break;
        default:
            throw LXError(std::string("[LX COMPILE] Unknown statement at line " + std::to_string(ln)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void Compiler::compileExpr(ASTPtr n) {
    if (!n) return;
    int ln = n->line;
    switch (n->kind) {
        case NT::INT_LIT:   cur->emitI(OP::PUSH_INT,   n->ival, ln); break;
        case NT::FLOAT_LIT: cur->emitD(OP::PUSH_FLOAT, n->dval, ln); break;
        case NT::STR_LIT:   cur->emitS(OP::PUSH_STR,   n->sval, ln); break;
        case NT::CHAR_LIT: {
            Instr ins; ins.op=OP::PUSH_CHAR; ins.cval=n->cval; ins.line=ln;
            cur->code.push_back(ins); break;
        }
        case NT::BOOL_LIT: {
            Instr ins; ins.op=OP::PUSH_BOOL; ins.bval=n->bval; ins.line=ln;
            cur->code.push_back(ins); break;
        }
        case NT::VOID_LIT:  cur->emit(OP::PUSH_VOID, ln); break;
        case NT::IDENT:
            if (n->name == "self") cur->emit(OP::LOAD_SELF, ln);
            else cur->emitS(OP::LOAD, n->name, ln);
            break;
        case NT::SELF_EXPR: cur->emit(OP::LOAD_SELF, ln); break;

        case NT::BINOP: {
            // short-circuit for AND_W / OR_W
            if (n->op == "AND_W") {
                compileExpr(n->children[0]);
                int sc = cur->emitI(OP::JMP_FALSE, 0, ln);
                compileExpr(n->children[1]);
                cur->emit(OP::AND, ln);
                cur->patch(sc, cur->here() - 1);
                break;
            }
            if (n->op == "OR_W") {
                compileExpr(n->children[0]);
                compileExpr(n->children[1]);
                cur->emit(OP::OR, ln);
                break;
            }
            compileExpr(n->children[0]);
            compileExpr(n->children[1]);
            if      (n->op=="+")    cur->emit(OP::ADD,ln);
            else if (n->op=="-")    cur->emit(OP::SUB,ln);
            else if (n->op=="*")    cur->emit(OP::MUL,ln);
            else if (n->op=="/")    cur->emit(OP::DIV,ln);
            else if (n->op=="%")    cur->emit(OP::MOD,ln);
            else if (n->op=="**")   cur->emit(OP::POW,ln);
            else if (n->op=="IS")   cur->emit(OP::EQ, ln);
            else if (n->op=="IS_NOT") cur->emit(OP::NEQ,ln);
            else if (n->op=="GT")   cur->emit(OP::GT, ln);
            else if (n->op=="LT")   cur->emit(OP::LT, ln);
            else if (n->op=="GT_EQ") cur->emit(OP::GTE,ln);
            else if (n->op=="LT_EQ") cur->emit(OP::LTE,ln);
            break;
        }
        case NT::UNOP: {
            compileExpr(n->children[0]);
            if      (n->op=="-")      cur->emit(OP::NEG,ln);
            else if (n->op=="NOT_W")  cur->emit(OP::NOT,ln);
            break;
        }
        case NT::CALL_EXPR: {
            for (auto& a : n->children) compileExpr(a);
            cur->emitCall(n->name, (int)n->children.size(), ln);
            break;
        }
        case NT::ATTR_EXPR: {
            // children[0] = object, sval = field/method name
            // bval = true  → method call  (children[1..] = args)
            // bval = false → field access (no extra children)
            compileExpr(n->children[0]); // push object
            if (n->bval) {
                // Method call: push args then CALL_METHOD
                for (size_t i = 1; i < n->children.size(); i++)
                    compileExpr(n->children[i]);
                cur->emitMethod(n->sval, (int)n->children.size() - 1, ln);
            } else {
                // Field access: GET_ATTR
                cur->emitS(OP::GET_ATTR, n->sval, ln);
            }
            break;
        }
        case NT::NEW_EXPR: {
            for (auto& a : n->children) compileExpr(a);
            Instr ins; ins.op=OP::NEW_OBJ; ins.sarg=n->name;
            ins.iarg2=(int)n->children.size(); ins.line=ln;
            cur->code.push_back(ins);
            break;
        }
        case NT::SUPER_EXPR: {
            // super_call: methodName(args) — implicit self is already in scope
            cur->emit(OP::LOAD_SELF, ln);
            for (auto& a : n->children) compileExpr(a);
            Instr ins; ins.op=OP::SUPER_CALL; ins.sarg=n->name;
            ins.iarg2=(int)n->children.size(); ins.line=ln;
            cur->code.push_back(ins);
            break;
        }
        case NT::INDEX_EXPR: {
            compileExpr(n->children[0]);
            compileExpr(n->children[1]);
            cur->emit(OP::INDEX_GET, ln);
            break;
        }
        case NT::LIST_LITERAL: {
            for (auto& e : n->children) compileExpr(e);
            cur->emitI(OP::BUILD_LIST, (int)n->children.size(), ln);
            break;
        }
        case NT::MAP_LITERAL: {
            for (size_t i=0; i < n->children.size(); i+=2) {
                compileExpr(n->children[i]);
                compileExpr(n->children[i+1]);
            }
            cur->emitI(OP::BUILD_MAP, (int)n->children.size()/2, ln);
            break;
        }
        default:
            throw LXError(std::string("[LX COMPILE] Cannot compile expression kind=" +
                std::to_string((int)n->kind) + " at line " + std::to_string(ln)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void Compiler::compileIfChain(ASTPtr n) {
    int ln = n->line;
    size_t numConds = n->children.size();
    std::vector<int> endPatches;
    for (size_t i = 0; i < numConds; i++) {
        compileExpr(n->children[i]);
        int fj = cur->emitI(OP::JMP_FALSE, 0, ln);
        compileBlock(n->alt[i]);
        endPatches.push_back(cur->emitI(OP::JMP, 0, ln));
        cur->patch(fj, cur->here());
    }
    if (n->alt.back()) compileBlock(n->alt.back());
    int endAddr = cur->here();
    for (int idx : endPatches) cur->patch(idx, endAddr);
}

void Compiler::compileWhile(ASTPtr n) {
    int ln = n->line;
    loopStack.push_back({});
    int loopStart = cur->here();
    compileExpr(n->children[0]);
    int exitJump = cur->emitI(OP::JMP_FALSE, 0, ln);
    compileBlock(n->alt[0]);
    cur->emitI(OP::JMP, loopStart, ln);
    int loopEnd = cur->here();
    cur->patch(exitJump, loopEnd);
    for (int idx : loopStack.back().breakPatches) cur->patch(idx, loopEnd);
    for (int idx : loopStack.back().contPatches)  cur->patch(idx, loopStart);
    loopStack.pop_back();
}

void Compiler::compileFor(ASTPtr n) {
    int ln = n->line;
    compileExpr(n->children[0]);
    cur->emitS(OP::STORE_NEW, n->name, ln);
    loopStack.push_back({});
    int loopStart = cur->here();
    cur->emitS(OP::LOAD, n->name, ln);
    compileExpr(n->children[1]);
    cur->emit(OP::LTE, ln);
    int exitJump = cur->emitI(OP::JMP_FALSE, 0, ln);
    compileBlock(n->alt[0]);
    int contAddr = cur->here();
    cur->emitS(OP::LOAD, n->name, ln);
    compileExpr(n->children[2]);
    cur->emit(OP::ADD, ln);
    cur->emitS(OP::STORE, n->name, ln);
    cur->emitI(OP::JMP, loopStart, ln);
    int loopEnd = cur->here();
    cur->patch(exitJump, loopEnd);
    for (int idx : loopStack.back().breakPatches) cur->patch(idx, loopEnd);
    for (int idx : loopStack.back().contPatches)  cur->patch(idx, contAddr);
    loopStack.pop_back();
}

void Compiler::compileEach(ASTPtr n) {
    int ln = n->line;
    std::string colVar = "__col_" + n->name + "_" + std::to_string(ln) + "__";
    std::string idxVar = "__idx_" + n->name + "_" + std::to_string(ln) + "__";
    compileExpr(n->children[0]);
    cur->emitS(OP::STORE_NEW, colVar, ln);
    cur->emitI(OP::PUSH_INT, 0, ln);
    cur->emitS(OP::STORE_NEW, idxVar, ln);
    loopStack.push_back({});
    int loopStart = cur->here();
    cur->emitS(OP::LOAD, idxVar, ln);
    cur->emitS(OP::LOAD, colVar, ln);
    cur->emit(OP::LEN, ln);
    cur->emit(OP::LT, ln);
    int exitJump = cur->emitI(OP::JMP_FALSE, 0, ln);
    cur->emitS(OP::LOAD, colVar, ln);
    cur->emitS(OP::LOAD, idxVar, ln);
    cur->emit(OP::INDEX_GET, ln);
    cur->emitS(OP::STORE_NEW, n->name, ln);
    compileBlock(n->alt[0]);
    int contAddr = cur->here();
    cur->emitS(OP::LOAD, idxVar, ln);
    cur->emitI(OP::PUSH_INT, 1, ln);
    cur->emit(OP::ADD, ln);
    cur->emitS(OP::STORE, idxVar, ln);
    cur->emitI(OP::JMP, loopStart, ln);
    int loopEnd = cur->here();
    cur->patch(exitJump, loopEnd);
    for (int idx : loopStack.back().breakPatches) cur->patch(idx, loopEnd);
    for (int idx : loopStack.back().contPatches)  cur->patch(idx, contAddr);
    loopStack.pop_back();
}

void Compiler::compileTryCatch(ASTPtr n) {
    int ln = n->line;
    // Emit TRY_SETUP with placeholder catch address
    int tryBeginInstr = cur->emitI(OP::TRY_SETUP, 0, ln);
    compileBlock(n->alt[0]); // try body
    cur->emit(OP::TRY_POP, ln); // normal exit: pop handler
    int afterTryJump = cur->emitI(OP::JMP, 0, ln);

    // For now we compile all catches sequentially (first wins)
    int catchAddr = cur->here();
    cur->patch(tryBeginInstr, catchAddr);

    if (!n->catches.empty()) {
        auto& cc = n->catches[0];
        if (!cc.errorVar.empty())
            cur->emitS(OP::STORE_NEW, cc.errorVar, ln);
        else
            cur->emit(OP::POP, ln);
        for (auto& s : cc.body) compileStmt(s);
    } else {
        cur->emit(OP::POP, ln);
    }
    cur->patch(afterTryJump, cur->here());
}

void Compiler::compileMatch(ASTPtr n) {
    int ln = n->line;
    std::string subjVar = "__match_" + std::to_string(ln) + "__";
    compileExpr(n->children[0]);
    cur->emitS(OP::STORE_NEW, subjVar, ln);
    std::vector<int> endPatches;
    for (auto& arm : n->arms) {
        if (arm.condition) {
            cur->emitS(OP::LOAD, subjVar, ln);
            compileExpr(arm.condition);
            cur->emit(OP::EQ, ln);
            int skipJump = cur->emitI(OP::JMP_FALSE, 0, ln);
            for (auto& s : arm.body) compileStmt(s);
            endPatches.push_back(cur->emitI(OP::JMP, 0, ln));
            cur->patch(skipJump, cur->here());
        } else {
            for (auto& s : arm.body) compileStmt(s);
            endPatches.push_back(cur->emitI(OP::JMP, 0, ln));
        }
    }
    int endAddr = cur->here();
    for (int idx : endPatches) cur->patch(idx, endAddr);
}

void Compiler::compileTaskStmt(ASTPtr n) {
    // Compile task body as a separate chunk, call it via __task_spawn__
    std::string taskChunkName = "__task_" + n->name + "_" + std::to_string(n->line) + "__";
    Chunk tc; tc.name = taskChunkName;
    Chunk* saved = cur;
    chunks[taskChunkName] = tc;
    cur = &chunks[taskChunkName];
    compileBlock(n->alt[0]);
    cur->emit(OP::PUSH_VOID);
    cur->emit(OP::RET);
    cur = saved;

    // At runtime: call __task_spawn__(taskChunkName, taskName)
    cur->emitS(OP::PUSH_STR, taskChunkName, n->line);
    cur->emitS(OP::PUSH_STR, n->name, n->line);
    Instr ins; ins.op=OP::CALL; ins.sarg="__task_spawn__"; ins.iarg2=2; ins.line=n->line;
    cur->code.push_back(ins);
    cur->emit(OP::POP, n->line);
}

// ─────────────────────────────────────────────────────────────────────────────
void Compiler::resolveJumps() {
    for (auto& pj : pendingJumps) {
        auto it = labelMap.find(pj.label);
        if (it == labelMap.end())
            throw LXError(std::string("[LX COMPILE] Undefined label: " + pj.label));
        chunks["__main__"].patch(pj.instrIdx, it->second);
    }
    pendingJumps.clear();
}
