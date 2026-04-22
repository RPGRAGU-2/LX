/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  LX 3.0 — lx_vm.cpp                                                 ║
 * ║  Stack VM · Mark-and-Sweep GC · OOP · Concurrency · File/Net/JSON  ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */
#include "lx_vm.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Frame / stack helpers
// ─────────────────────────────────────────────────────────────────────────────
CallFrame& VM::frame() { return callStack.back(); }
Chunk&     VM::chunk() { return *callStack.back().chunk; }

void VM::push(Value v) { stack.push_back(std::move(v)); }

Value VM::pop() {
    if (stack.empty())
        throw LXRuntimeError(std::string("[LX VM] Stack underflow"));
    Value v = std::move(stack.back());
    stack.pop_back();
    return v;
}

Value& VM::top() {
    if (stack.empty())
        throw LXRuntimeError(std::string("[LX VM] Stack is empty"));
    return stack.back();
}

Value VM::getVar(const std::string& name) {
    for (int i = (int)callStack.size()-1; i >= 0; i--) {
        auto it = callStack[i].locals.find(name);
        if (it != callStack[i].locals.end()) return it->second;
    }
    throw LXRuntimeError(std::string("Undefined variable: " + name));
}

void VM::setVar(const std::string& name, Value val) {
    for (int i = (int)callStack.size()-1; i >= 0; i--) {
        auto it = callStack[i].locals.find(name);
        if (it != callStack[i].locals.end()) { it->second = std::move(val); return; }
    }
    throw LXRuntimeError(std::string("Variable not declared: " + name));
}

void VM::setNewVar(const std::string& name, Value val) {
    frame().locals[name] = std::move(val);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Run entry point
// ─────────────────────────────────────────────────────────────────────────────
void VM::run(std::map<std::string, Chunk>& ch,
             std::map<std::string, BlueprintMeta>& bp) {
    chunks    = &ch;
    blueprints= &bp;
    CallFrame main;
    main.chunk = &ch["__main__"];
    callStack.push_back(main);
    execute();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Main execution loop
// ─────────────────────────────────────────────────────────────────────────────
void VM::execute() {
    while (!callStack.empty()) {
        CallFrame& f = callStack.back();
        if (f.pc >= (int)f.chunk->code.size()) {
            callStack.pop_back();
            push(Value(std::string("")));
            continue;
        }
        Instr& ins = f.chunk->code[f.pc++];
        try {
            dispatch(ins);
        } catch (LXError& e) {
            // Unwind to nearest try handler
            bool handled = false;
            while (!callStack.empty()) {
                auto& hf = callStack.back();
                if (!hf.handlers.empty()) {
                    int catchAddr = hf.handlers.back();
                    hf.handlers.pop_back();
                    push(e.lxVal);
                    hf.pc = catchAddr;
                    handled = true;
                    break;
                }
                callStack.pop_back();
            }
            if (!handled) {
                std::cerr << formatError(e, ins.line) << "\n";
                return;
            }
        }
        // Periodic GC
        if (++allocCount >= GC_THRESHOLD) gcCollect();
    }
}

std::string VM::formatError(const LXError& e, int line) {
    std::string kind = "RuntimeError";
    if      (dynamic_cast<const LXTypeError*>(&e))    kind = "TypeError";
    else if (dynamic_cast<const LXIndexError*>(&e))   kind = "IndexError";
    else if (dynamic_cast<const LXKeyError*>(&e))     kind = "KeyError";
    else if (dynamic_cast<const LXValueError*>(&e))   kind = "ValueError";
    else if (dynamic_cast<const LXIOError*>(&e))      kind = "IOError";
    else if (dynamic_cast<const LXNetError*>(&e))     kind = "NetworkError";
    std::string msg = "[LX " + kind + "]";
    if (line > 0) msg += " line " + std::to_string(line);
    msg += std::string(": ") + e.what();
    return msg;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Instruction dispatch
// ─────────────────────────────────────────────────────────────────────────────
void VM::dispatch(Instr& ins) {
    CallFrame& f = callStack.back();

    switch (ins.op) {

    // ── Stack ──────────────────────────────────────────────────────────────
    case OP::PUSH_INT:   push((long long)ins.iarg); break;
    case OP::PUSH_FLOAT: push(ins.darg); break;
    case OP::PUSH_STR:   push(ins.sarg); break;
    case OP::PUSH_BOOL:  push(ins.bval); break;
    case OP::PUSH_CHAR:  push(ins.cval); break;
    case OP::PUSH_VOID:  push(std::string("")); break;
    case OP::POP:        pop(); break;
    case OP::DUP:        push(top()); break;
    case OP::SWAP: {
        Value b = pop(), a = pop();
        push(b); push(a); break;
    }

    // ── Variables ──────────────────────────────────────────────────────────
    case OP::LOAD:      push(getVar(ins.sarg)); break;
    case OP::STORE:     setVar(ins.sarg, pop()); break;
    case OP::STORE_NEW: setNewVar(ins.sarg, pop()); break;

    case OP::LOAD_SELF: {
        if (f.hasSelf) push(f.selfValue);
        else throw LXRuntimeError(std::string("'self' used outside method"));
        break;
    }

    // ── Arithmetic ─────────────────────────────────────────────────────────
    case OP::ADD: {
        Value b=pop(), a=pop();
        bool aStr=std::holds_alternative<std::string>(a),
             bStr=std::holds_alternative<std::string>(b);
        if (aStr||bStr) { push(valToStr(a)+valToStr(b)); break; }
        bool flt=std::holds_alternative<double>(a)||std::holds_alternative<double>(b);
        push(flt ? Value(valToDouble(a)+valToDouble(b)) : Value(valToInt(a)+valToInt(b)));
        break;
    }
    case OP::SUB: { Value b=pop(),a=pop(); bool f=std::holds_alternative<double>(a)||std::holds_alternative<double>(b); push(f?Value(valToDouble(a)-valToDouble(b)):Value(valToInt(a)-valToInt(b))); break; }
    case OP::MUL: { Value b=pop(),a=pop(); bool f=std::holds_alternative<double>(a)||std::holds_alternative<double>(b); push(f?Value(valToDouble(a)*valToDouble(b)):Value(valToInt(a)*valToInt(b))); break; }
    case OP::DIV: { Value b=pop(),a=pop(); double bv=valToDouble(b); if(bv==0) throw LXRuntimeError(std::string("Division by zero")); push(valToDouble(a)/bv); break; }
    case OP::MOD: { Value b=pop(),a=pop(); long long bv=valToInt(b); if(bv==0) throw LXRuntimeError(std::string("Modulo by zero")); push(valToInt(a)%bv); break; }
    case OP::POW: { Value b=pop(),a=pop(); push(std::pow(valToDouble(a),valToDouble(b))); break; }
    case OP::NEG: { Value a=pop(); if(std::holds_alternative<double>(a)) push(-std::get<double>(a)); else push(-valToInt(a)); break; }
    case OP::AUG_ADD: { Value b=pop(),a=pop(); bool fs=std::holds_alternative<std::string>(a); push(fs?Value(valToStr(a)+valToStr(b)):(std::holds_alternative<double>(a)?Value(valToDouble(a)+valToDouble(b)):Value(valToInt(a)+valToInt(b)))); break; }
    case OP::AUG_SUB: { Value b=pop(),a=pop(); push(Value(valToDouble(a)-valToDouble(b))); break; }
    case OP::AUG_MUL: { Value b=pop(),a=pop(); push(Value(valToDouble(a)*valToDouble(b))); break; }
    case OP::AUG_DIV: { Value b=pop(),a=pop(); double bv=valToDouble(b); if(bv==0) throw LXRuntimeError(std::string("Division by zero")); push(valToDouble(a)/bv); break; }

    // ── Comparison ─────────────────────────────────────────────────────────
    case OP::EQ:  { Value b=pop(),a=pop(); push(valEqual(a,b));   break; }
    case OP::NEQ: { Value b=pop(),a=pop(); push(!valEqual(a,b));  break; }
    case OP::GT:  { Value b=pop(),a=pop(); push(valToDouble(a)>valToDouble(b));  break; }
    case OP::LT:  { Value b=pop(),a=pop(); push(valToDouble(a)<valToDouble(b));  break; }
    case OP::GTE: { Value b=pop(),a=pop(); push(valToDouble(a)>=valToDouble(b)); break; }
    case OP::LTE: { Value b=pop(),a=pop(); push(valToDouble(a)<=valToDouble(b)); break; }
    case OP::AND: { Value b=pop(),a=pop(); push(valToBool(a)&&valToBool(b)); break; }
    case OP::OR:  { Value b=pop(),a=pop(); push(valToBool(a)||valToBool(b)); break; }
    case OP::NOT: { push(!valToBool(pop())); break; }

    // ── Control flow ───────────────────────────────────────────────────────
    case OP::JMP:       f.pc=(int)ins.iarg; break;
    case OP::JMP_FALSE: { if(!valToBool(pop())) f.pc=(int)ins.iarg; break; }
    case OP::JMP_TRUE:  { if( valToBool(pop())) f.pc=(int)ins.iarg; break; }

    // ── I/O ────────────────────────────────────────────────────────────────
    case OP::PRINT_NL:  std::cout << valToStr(pop()) << "\n"; break;
    case OP::PRINT_RAW: std::cout << valToStr(pop());         break;
    case OP::PRINT_ERR: std::cerr << "[LX ERR] " << valToStr(pop()) << "\n"; break;

    // ── Collections ────────────────────────────────────────────────────────
    case OP::BUILD_LIST: {
        auto lst = std::make_shared<LXList>();
        lst->items.resize(ins.iarg);
        for (int i=(int)ins.iarg-1; i>=0; i--) lst->items[i]=pop();
        heapLists.push_back(lst);
        push(lst); trackAlloc(); break;
    }
    case OP::BUILD_MAP: {
        auto mp = std::make_shared<LXMap>();
        std::vector<std::pair<std::string,Value>> pairs(ins.iarg);
        for (int i=(int)ins.iarg-1; i>=0; i--) {
            pairs[i].second=pop(); pairs[i].first=valToStr(pop());
        }
        for (auto& [k,v] : pairs) mp->kvs[k]=v;
        heapMaps.push_back(mp);
        push(mp); trackAlloc(); break;
    }
    case OP::INDEX_GET: {
        Value idx=pop(), col=pop();
        if (auto lst=std::get_if<std::shared_ptr<LXList>>(&col)) {
            int i=(int)valToInt(idx);
            if (i<0) i+=(int)(*lst)->items.size();
            if (i<0||i>=(int)(*lst)->items.size())
                throw LXIndexError(std::string("List index out of range: "+std::to_string(i)));
            push((*lst)->items[i]);
        } else if (auto mp=std::get_if<std::shared_ptr<LXMap>>(&col)) {
            std::string k=valToStr(idx);
            auto it=(*mp)->kvs.find(k);
            if (it==(*mp)->kvs.end()) throw LXKeyError(std::string("Key not found: "+k));
            push(it->second);
        } else if (auto sp=std::get_if<std::shared_ptr<LXObject>>(&col)) {
            std::string k=valToStr(idx);
            auto it=(*sp)->attrs.find(k);
            if (it==(*sp)->attrs.end()) throw LXKeyError(std::string("Attribute not found: "+k));
            push(it->second);
        } else if (auto s=std::get_if<std::string>(&col)) {
            int i=(int)valToInt(idx);
            if (i<0) i+=(int)s->size();
            if (i<0||i>=(int)s->size()) throw LXIndexError(std::string("String index out of range"));
            push(std::string(1,(*s)[i]));
        } else throw LXTypeError(std::string("Cannot index into non-collection"));
        break;
    }
    case OP::LEN: {
        Value col=pop();
        if (auto s=std::get_if<std::string>(&col)) push((long long)s->size());
        else if (auto l=std::get_if<std::shared_ptr<LXList>>(&col)) push((long long)(*l)->items.size());
        else if (auto m=std::get_if<std::shared_ptr<LXMap>>(&col))  push((long long)(*m)->kvs.size());
        else push(0LL);
        break;
    }

    // ── OOP ────────────────────────────────────────────────────────────────
    case OP::NEW_OBJ: {
        int argc=ins.iarg2;
        std::vector<Value> args(argc);
        for (int i=argc-1;i>=0;i--) args[i]=pop();
        push(instantiate(ins.sarg, args));
        trackAlloc();
        break;
    }
    case OP::GET_ATTR: {
        Value obj=pop();
        if (auto sp=std::get_if<std::shared_ptr<LXObject>>(&obj)) {
            auto it=(*sp)->attrs.find(ins.sarg);
            if (it!=(*sp)->attrs.end()) { push(it->second); break; }
            // check if it's a method (push a callable token as string for CALL_METHOD)
            push(std::string("__method__")); // placeholder; handled by CALL_METHOD
        } else if (auto mp=std::get_if<std::shared_ptr<LXMap>>(&obj)) {
            auto it=(*mp)->kvs.find(ins.sarg);
            if (it!=(*mp)->kvs.end()) { push(it->second); break; }
            throw LXKeyError(std::string("Map key not found: " + ins.sarg));
        } else throw LXTypeError(std::string("GET_ATTR on non-object: " + ins.sarg));
        break;
    }
    case OP::SET_ATTR: {
        Value val = pop();   // value is on top
        Value obj = pop();   // object is underneath
        if (auto sp = std::get_if<std::shared_ptr<LXObject>>(&obj)) {
            (*sp)->attrs[ins.sarg] = val;
        } else if (auto mp = std::get_if<std::shared_ptr<LXMap>>(&obj)) {
            (*mp)->kvs[ins.sarg] = val;
        } else throw LXTypeError(std::string("SET_ATTR on non-object for field: " + ins.sarg));
        break;
    }
    case OP::CALL_METHOD: {
        int argc=ins.iarg2;
        std::vector<Value> args(argc);
        for (int i=argc-1;i>=0;i--) args[i]=pop();
        Value objVal=pop();
        if (auto sp=std::get_if<std::shared_ptr<LXObject>>(&objVal)) {
            callMethod(*sp, ins.sarg, args);
        } else {
            // Builtin methods on string / list / map
            args.insert(args.begin(), objVal);
            if (!tryBuiltin(ins.sarg, (int)args.size(), args))
                throw LXTypeError(std::string("No method '" + ins.sarg + "' on this type"));
        }
        break;
    }
    case OP::SUPER_CALL: {
        int argc=ins.iarg2;
        std::vector<Value> args(argc);
        for (int i=argc-1;i>=0;i--) args[i]=pop();
        Value selfVal=pop();
        if (auto sp=std::get_if<std::shared_ptr<LXObject>>(&selfVal)) {
            auto it=blueprints->find((*sp)->blueprint);
            if (it==blueprints->end()) throw LXRuntimeError(std::string("No blueprint for super call"));
            std::string parentBP = it->second.parent;
            if (parentBP.empty()) throw LXRuntimeError(std::string("No parent blueprint for super_call"));
            std::string chunkName = resolveMethod(parentBP, ins.sarg);
            if (chunkName.empty()) throw LXRuntimeError(std::string("Parent method not found: "+ins.sarg));
            auto& fc = (*chunks)[chunkName];
            CallFrame nf;
            nf.chunk     = &fc;
            nf.hasSelf   = true;
            nf.selfValue = *sp;
            nf.locals["self"] = *sp;
            int startParam = (!fc.params.empty() && fc.params[0]=="self") ? 1 : 0;
            for (size_t i=0; i<args.size(); i++) {
                int idx = startParam+(int)i;
                if (idx<(int)fc.params.size())
                    nf.locals[fc.params[idx]] = args[i];
            }
            callStack.push_back(nf);
        } else throw LXRuntimeError(std::string("super_call on non-object"));
        break;
    }

    // ── Error handling ─────────────────────────────────────────────────────
    case OP::TRY_SETUP: {
        f.handlers.push_back((int)ins.iarg);
        break;
    }
    case OP::TRY_POP: {
        if (!f.handlers.empty()) f.handlers.pop_back();
        break;
    }
    case OP::THROW: {
        throw LXError(pop());
    }

    // ── Function call / return ─────────────────────────────────────────────
    case OP::CALL: {
        int argc=ins.iarg2;
        std::vector<Value> args(argc);
        for (int i=argc-1;i>=0;i--) args[i]=pop();

        if (tryBuiltin(ins.sarg, argc, args)) break;

        auto it = chunks->find(ins.sarg);
        if (it==chunks->end())
            throw LXRuntimeError(std::string("Undefined function: " + ins.sarg));
        Chunk& fc = it->second;
        CallFrame nf; nf.chunk=&fc;
        for (int i=0;i<(int)fc.params.size();i++)
            nf.locals[fc.params[i]] = (i<argc) ? args[i] : Value(0LL);
        callStack.push_back(nf);
        break;
    }
    case OP::RET: {
        Value rv = pop();
        callStack.pop_back();
        push(rv);
        break;
    }
    case OP::HALT: {
        callStack.clear();
        break;
    }

    default:
        throw LXRuntimeError(std::string("[LX VM] Unknown opcode: " +
            std::to_string((int)ins.op)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  OOP helpers
// ─────────────────────────────────────────────────────────────────────────────
Value VM::instantiate(const std::string& bpName, const std::vector<Value>& args) {
    auto it = blueprints->find(bpName);
    if (it == blueprints->end())
        throw LXRuntimeError(std::string("Unknown blueprint: " + bpName));
    auto obj = std::make_shared<LXObject>();
    obj->blueprint = bpName;
    heapObjects.push_back(obj);

    std::string initChunk = resolveMethod(bpName, "__init__");
    if (!initChunk.empty() && chunks->count(initChunk)) {
        Chunk& fc = (*chunks)[initChunk];
        CallFrame nf;
        nf.chunk     = &fc;
        nf.hasSelf   = true;
        nf.selfValue = obj;
        nf.locals["self"] = obj;   // make 'self' directly accessible as a variable
        // params[0] = "self" (added by compiler) — skip it, bind the rest
        int startParam = (!fc.params.empty() && fc.params[0] == "self") ? 1 : 0;
        for (size_t i = 0; i < args.size(); i++) {
            int idx = startParam + (int)i;
            if (idx < (int)fc.params.size())
                nf.locals[fc.params[idx]] = args[i];
        }
        size_t depthBefore = callStack.size();
        callStack.push_back(nf);
        // Execute inline until we return to the same depth
        while (callStack.size() > depthBefore && !callStack.empty()) {
            CallFrame& cf = callStack.back();
            if (cf.pc >= (int)cf.chunk->code.size()) {
                callStack.pop_back();
                push(std::string(""));
                break;
            }
            Instr ci = cf.chunk->code[cf.pc++];
            // intercept RET specially so we don't push a spurious return value
            if (ci.op == OP::RET) {
                Value rv = pop();
                callStack.pop_back();
                (void)rv; // discard __init__ return
                break;
            }
            dispatch(ci);
        }
    }
    return obj;
}

std::string VM::resolveMethod(const std::string& bpName, const std::string& method) {
    std::string cur = bpName;
    while (!cur.empty()) {
        auto it = blueprints->find(cur);
        if (it == blueprints->end()) break;
        auto mit = it->second.methodChunks.find(method);
        if (mit != it->second.methodChunks.end()) return mit->second;
        cur = it->second.parent;
    }
    return "";
}

void VM::callMethod(std::shared_ptr<LXObject> obj, const std::string& method,
                    const std::vector<Value>& args) {
    std::string chunkName = resolveMethod(obj->blueprint, method);
    if (chunkName.empty() || !chunks->count(chunkName))
        throw LXRuntimeError(std::string("Method not found: " + obj->blueprint + "." + method));
    Chunk& fc = (*chunks)[chunkName];
    CallFrame nf;
    nf.chunk     = &fc;
    nf.hasSelf   = true;
    nf.selfValue = obj;
    nf.locals["self"] = obj;  // self accessible as variable
    int startParam = (!fc.params.empty() && fc.params[0] == "self") ? 1 : 0;
    for (size_t i = 0; i < args.size(); i++) {
        int idx = startParam + (int)i;
        if (idx < (int)fc.params.size())
            nf.locals[fc.params[idx]] = args[i];
    }
    callStack.push_back(nf);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Built-in functions
// ─────────────────────────────────────────────────────────────────────────────
bool VM::tryBuiltin(const std::string& name, int argc, std::vector<Value>& args) {
    // ── Core ───────────────────────────────────────────────────────────────
    if (name=="len")  { push((long long)[&]()->size_t {
        if (auto s=std::get_if<std::string>(&args[0])) return s->size();
        if (auto l=std::get_if<std::shared_ptr<LXList>>(&args[0])) return (*l)->items.size();
        if (auto m=std::get_if<std::shared_ptr<LXMap>>(&args[0])) return (*m)->kvs.size();
        return 0; }()); return true; }
    if (name=="str")   { push(valToStr(args[0])); return true; }
    if (name=="num")   {
        std::string sv=valToStr(args[0]);
        try { push((long long)std::stoll(sv)); return true; } catch(...) {}
        try { push(std::stod(sv)); return true; } catch(...) {}
        throw LXValueError(std::string("Cannot convert to number: "+sv));
    }
    if (name=="int")   { push(valToInt(args[0]));    return true; }
    if (name=="float") { push(valToDouble(args[0])); return true; }
    if (name=="bool")  { push(valToBool(args[0]));   return true; }
    if (name=="type_of") {
        std::string t; std::visit([&t](auto&& a){
            using T=std::decay_t<decltype(a)>;
            if constexpr(std::is_same_v<T,long long>)   t="long";
            else if constexpr(std::is_same_v<T,double>) t="double";
            else if constexpr(std::is_same_v<T,std::string>) t="string";
            else if constexpr(std::is_same_v<T,char>)   t="char";
            else if constexpr(std::is_same_v<T,bool>)   t="bool";
            else if constexpr(std::is_same_v<T,std::shared_ptr<LXList>>) t="list";
            else if constexpr(std::is_same_v<T,std::shared_ptr<LXMap>>)  t="map";
            else if constexpr(std::is_same_v<T,std::shared_ptr<LXObject>>) t="object";
            else if constexpr(std::is_same_v<T,std::shared_ptr<LXBlueprint>>) t="blueprint";
        }, args[0]); push(t); return true;
    }
    if (name=="chr")   { push((char)(int)valToInt(args[0])); return true; }
    if (name=="ord")   { auto s=valToStr(args[0]); push(s.empty()?0LL:(long long)(unsigned char)s[0]); return true; }
    if (name=="input") { if(argc>0)std::cout<<valToStr(args[0]); std::string l; std::getline(std::cin,l); push(l); return true; }
    if (name=="input_num") {
        if(argc>0)std::cout<<valToStr(args[0]);
        std::string l; std::getline(std::cin,l);
        try{push((long long)std::stoll(l));}catch(...){try{push(std::stod(l));}catch(...){push(0LL);}}
        return true;
    }

    // ── Math ───────────────────────────────────────────────────────────────
    if (name=="sqrt")  { push(std::sqrt(valToDouble(args[0]))); return true; }
    if (name=="abs")   { double v=valToDouble(args[0]); push(v<0?-v:v); return true; }
    if (name=="floor") { push((long long)std::floor(valToDouble(args[0]))); return true; }
    if (name=="ceil")  { push((long long)std::ceil(valToDouble(args[0]))); return true; }
    if (name=="round") { push((long long)std::round(valToDouble(args[0]))); return true; }
    if (name=="pow")   { push(std::pow(valToDouble(args[0]),valToDouble(args[1]))); return true; }
    if (name=="max")   { push(valToDouble(args[0])>valToDouble(args[1])?args[0]:args[1]); return true; }
    if (name=="min")   { push(valToDouble(args[0])<valToDouble(args[1])?args[0]:args[1]); return true; }
    if (name=="time_ms"){ auto now=std::chrono::system_clock::now().time_since_epoch(); push((long long)std::chrono::duration_cast<std::chrono::milliseconds>(now).count()); return true; }

    // ── List ───────────────────────────────────────────────────────────────
    if (name=="list_push")     { auto l=std::get<std::shared_ptr<LXList>>(args[0]); l->items.push_back(args[1]); push(args[0]); return true; }
    if (name=="list_pop")      { auto l=std::get<std::shared_ptr<LXList>>(args[0]); if(l->items.empty())throw LXRuntimeError(std::string("list_pop on empty")); Value v=l->items.back(); l->items.pop_back(); push(v); return true; }
    if (name=="list_at")       { auto l=std::get<std::shared_ptr<LXList>>(args[0]); int i=(int)valToInt(args[1]); if(i<0)i+=(int)l->items.size(); if(i<0||i>=(int)l->items.size())throw LXIndexError(std::string("Index out of range")); push(l->items[i]); return true; }
    if (name=="list_set")      { auto l=std::get<std::shared_ptr<LXList>>(args[0]); int i=(int)valToInt(args[1]); if(i<0||i>=(int)l->items.size())throw LXIndexError(std::string("Index out of range")); l->items[i]=args[2]; push(args[0]); return true; }
    if (name=="list_contains") { auto l=std::get<std::shared_ptr<LXList>>(args[0]); bool f=false; for(auto&x:l->items)if(valEqual(x,args[1])){f=true;break;} push(f); return true; }
    if (name=="list_slice")    { auto l=std::get<std::shared_ptr<LXList>>(args[0]); int s=(int)valToInt(args[1]),e=(int)valToInt(args[2]); auto r=std::make_shared<LXList>(); for(int i=s;i<e&&i<(int)l->items.size();i++)r->items.push_back(l->items[i]); heapLists.push_back(r); push(r); return true; }
    if (name=="list_sort")     { auto l=std::get<std::shared_ptr<LXList>>(args[0]); std::sort(l->items.begin(),l->items.end(),[](const Value&a,const Value&b){return valToDouble(a)<valToDouble(b);}); push(args[0]); return true; }
    if (name=="list_reverse")  { auto l=std::get<std::shared_ptr<LXList>>(args[0]); std::reverse(l->items.begin(),l->items.end()); push(args[0]); return true; }

    // ── Map ────────────────────────────────────────────────────────────────
    if (name=="map_get")  { auto m=std::get<std::shared_ptr<LXMap>>(args[0]); auto it=m->kvs.find(valToStr(args[1])); if(it==m->kvs.end())throw LXKeyError(std::string("Key not found: "+valToStr(args[1]))); push(it->second); return true; }
    if (name=="map_set")  { auto m=std::get<std::shared_ptr<LXMap>>(args[0]); m->kvs[valToStr(args[1])]=args[2]; push(args[0]); return true; }
    if (name=="map_has")  { auto m=std::get<std::shared_ptr<LXMap>>(args[0]); push(m->kvs.count(valToStr(args[1]))>0); return true; }
    if (name=="map_del")  { auto m=std::get<std::shared_ptr<LXMap>>(args[0]); m->kvs.erase(valToStr(args[1])); push(args[0]); return true; }
    if (name=="map_keys") { auto m=std::get<std::shared_ptr<LXMap>>(args[0]); auto l=std::make_shared<LXList>(); for(auto&[k,_]:m->kvs)l->items.push_back(k); heapLists.push_back(l); push(l); return true; }
    if (name=="map_vals") { auto m=std::get<std::shared_ptr<LXMap>>(args[0]); auto l=std::make_shared<LXList>(); for(auto&[_,v]:m->kvs)l->items.push_back(v); heapLists.push_back(l); push(l); return true; }

    // ── String ─────────────────────────────────────────────────────────────
    if (name=="str_upper")   { auto s=valToStr(args[0]); for(auto&c:s)c=toupper(c); push(s); return true; }
    if (name=="str_lower")   { auto s=valToStr(args[0]); for(auto&c:s)c=tolower(c); push(s); return true; }
    if (name=="str_trim")    { auto s=valToStr(args[0]); size_t a=s.find_first_not_of(" \t\r\n"),b=s.find_last_not_of(" \t\r\n"); push(a==std::string::npos?std::string(""):s.substr(a,b-a+1)); return true; }
    if (name=="str_split")   {
        std::string s=valToStr(args[0]),d=valToStr(args[1]);
        auto l=std::make_shared<LXList>(); size_t p=0,f;
        while((f=s.find(d,p))!=std::string::npos){l->items.push_back(s.substr(p,f-p));p=f+d.size();}
        l->items.push_back(s.substr(p)); heapLists.push_back(l); push(l); return true;
    }
    if (name=="str_join")    { auto l=std::get<std::shared_ptr<LXList>>(args[0]); std::string sep=valToStr(args[1]),r; for(size_t i=0;i<l->items.size();i++){r+=valToStr(l->items[i]);if(i+1<l->items.size())r+=sep;} push(r); return true; }
    if (name=="str_replace") { std::string s=valToStr(args[0]),f=valToStr(args[1]),t=valToStr(args[2]); size_t p=0; while((p=s.find(f,p))!=std::string::npos){s.replace(p,f.size(),t);p+=t.size();} push(s); return true; }
    if (name=="str_sub")     { auto s=valToStr(args[0]); int st=(int)valToInt(args[1]),ln=argc>2?(int)valToInt(args[2]):(int)s.size()-st; push(s.substr(st,ln)); return true; }
    if (name=="str_starts")  { auto s=valToStr(args[0]),p=valToStr(args[1]); push(s.rfind(p,0)==0); return true; }
    if (name=="str_ends")    { auto s=valToStr(args[0]),sf=valToStr(args[1]); push(s.size()>=sf.size()&&s.compare(s.size()-sf.size(),sf.size(),sf)==0); return true; }
    if (name=="str_count")   {
        std::string h=valToStr(args[0]),n=valToStr(args[1]); long long cnt=0; size_t p=0;
        while((p=h.find(n,p))!=std::string::npos){cnt++;p+=n.size();} push(cnt); return true;
    }

    // ── File system ────────────────────────────────────────────────────────
    if (name=="file_read") {
        try { push(readFileStr(valToStr(args[0]))); }
        catch(LXIOError& e) { throw; }
        catch(std::exception& e) { throw LXIOError(std::string(e.what())); }
        return true;
    }
    if (name=="file_write") {
        std::ofstream f(valToStr(args[0]));
        if (!f) throw LXIOError(std::string("Cannot write file: " + valToStr(args[0])));
        f << valToStr(args[1]);
        push(true); return true;
    }
    if (name=="file_append") {
        std::ofstream f(valToStr(args[0]), std::ios::app);
        if (!f) throw LXIOError(std::string("Cannot open file for append: " + valToStr(args[0])));
        f << valToStr(args[1]);
        push(true); return true;
    }
    if (name=="file_exists") {
        std::ifstream f(valToStr(args[0]));
        push(f.good()); return true;
    }
    if (name=="file_lines") {
        std::string content;
        try { content = readFileStr(valToStr(args[0])); }
        catch(...) { throw LXIOError(std::string("Cannot read: " + valToStr(args[0]))); }
        auto lst=std::make_shared<LXList>();
        std::istringstream ss(content); std::string line;
        while(std::getline(ss,line)) lst->items.push_back(line);
        heapLists.push_back(lst); push(lst); return true;
    }

    // ── JSON ───────────────────────────────────────────────────────────────
    if (name=="json_str") {
        // Serialise Value to JSON string
        std::function<std::string(const Value&)> toJSON = [&](const Value& v) -> std::string {
            return std::visit([&](auto&& a) -> std::string {
                using T=std::decay_t<decltype(a)>;
                if constexpr(std::is_same_v<T,std::string>){
                    std::string r="\"";
                    for(char c:a){if(c=='"')r+="\\\"";else if(c=='\\')r+="\\\\";else if(c=='\n')r+="\\n";else if(c=='\t')r+="\\t";else r+=c;}
                    return r+"\"";
                } else if constexpr(std::is_same_v<T,bool>) return a?"true":"false";
                else if constexpr(std::is_same_v<T,long long>) return std::to_string(a);
                else if constexpr(std::is_same_v<T,double>) { std::ostringstream os; os<<a; return os.str(); }
                else if constexpr(std::is_same_v<T,std::shared_ptr<LXList>>) {
                    std::string s="["; if(!a)return "[]";
                    for(size_t i=0;i<a->items.size();i++){s+=toJSON(a->items[i]);if(i+1<a->items.size())s+=",";}
                    return s+"]";
                } else if constexpr(std::is_same_v<T,std::shared_ptr<LXMap>>) {
                    std::string s="{"; if(!a)return "{}"; bool first=true;
                    for(auto&[k,vv]:a->kvs){if(!first)s+=",";first=false;s+="\""+k+"\":"+toJSON(vv);}
                    return s+"}";
                } else return "null";
            }, v);
        };
        push(toJSON(args[0])); return true;
    }
    if (name=="json_parse") {
        // Minimal JSON parser: handles objects, arrays, strings, numbers, bool, null
        std::string src=valToStr(args[0]);
        int jpos=0;
        std::function<Value()> parseVal = [&]() -> Value {
            while(jpos<(int)src.size()&&isspace(src[jpos]))jpos++;
            if(jpos>=(int)src.size()) return std::string("");
            char c=src[jpos];
            if(c=='"'){
                jpos++; std::string r;
                while(jpos<(int)src.size()&&src[jpos]!='"'){
                    if(src[jpos]=='\\'&&jpos+1<(int)src.size()){jpos++;char e=src[jpos];if(e=='n')r+='\n';else if(e=='t')r+='\t';else r+=e;}
                    else r+=src[jpos]; jpos++;
                }
                jpos++; return r;
            }
            if(c=='['){
                jpos++; auto lst=std::make_shared<LXList>();
                while(jpos<(int)src.size()){while(jpos<(int)src.size()&&isspace(src[jpos]))jpos++;if(src[jpos]==']')break;lst->items.push_back(parseVal());while(jpos<(int)src.size()&&isspace(src[jpos]))jpos++;if(jpos<(int)src.size()&&src[jpos]==',')jpos++;}
                if(jpos<(int)src.size())jpos++;heapLists.push_back(lst);return lst;
            }
            if(c=='{'){
                jpos++; auto mp=std::make_shared<LXMap>();
                while(jpos<(int)src.size()){while(jpos<(int)src.size()&&isspace(src[jpos]))jpos++;if(src[jpos]=='}')break;
                    Value k=parseVal();while(jpos<(int)src.size()&&isspace(src[jpos]))jpos++;if(jpos<(int)src.size()&&src[jpos]==':')jpos++;
                    Value v=parseVal();mp->kvs[valToStr(k)]=v;while(jpos<(int)src.size()&&isspace(src[jpos]))jpos++;if(jpos<(int)src.size()&&src[jpos]==',')jpos++;}
                if(jpos<(int)src.size())jpos++;heapMaps.push_back(mp);return mp;
            }
            if(src.substr(jpos,4)=="true") {jpos+=4;return true;}
            if(src.substr(jpos,5)=="false"){jpos+=5;return false;}
            if(src.substr(jpos,4)=="null") {jpos+=4;return std::string("");}
            // number
            std::string num; bool isF=false;
            if(c=='-'){num+=c;jpos++;}
            while(jpos<(int)src.size()&&(isdigit(src[jpos])||src[jpos]=='.')){if(src[jpos]=='.')isF=true;num+=src[jpos++];}
            if(isF) return std::stod(num);
            return (long long)std::stoll(num);
        };
        push(parseVal()); return true;
    }

    // ── Networking ─────────────────────────────────────────────────────────
    if (name=="net_connect") {
        // net_connect(host, port) -> fd as long long
        std::string host=valToStr(args[0]);
        int port=(int)valToInt(args[1]);
        int fd=socket(AF_INET,SOCK_STREAM,0);
        if(fd<0) throw LXNetError(std::string("socket() failed"));
        struct hostent* he=gethostbyname(host.c_str());
        if(!he) throw LXNetError(std::string("Host not found: "+host));
        struct sockaddr_in sa; memset(&sa,0,sizeof(sa));
        sa.sin_family=AF_INET;
        sa.sin_port=htons(port);
        memcpy(&sa.sin_addr,he->h_addr_list[0],he->h_length);
        if(connect(fd,(struct sockaddr*)&sa,sizeof(sa))<0) {
            close(fd); throw LXNetError(std::string("connect() failed to "+host+":"+std::to_string(port)));
        }
        push((long long)fd); return true;
    }
    if (name=="net_send") {
        int fd=(int)valToInt(args[0]); std::string data=valToStr(args[1]);
        ssize_t sent=send(fd,data.c_str(),data.size(),0);
        push((long long)sent); return true;
    }
    if (name=="net_recv") {
        int fd=(int)valToInt(args[0]); int bufsize=argc>1?(int)valToInt(args[1]):4096;
        std::string buf(bufsize,'\0');
        ssize_t n=recv(fd,buf.data(),bufsize,0);
        push(n>0?buf.substr(0,n):std::string("")); return true;
    }
    if (name=="net_close") {
        close((int)valToInt(args[0])); push(true); return true;
    }
    if (name=="net_listen") {
        // net_listen(port) -> server fd
        int port=(int)valToInt(args[0]);
        int fd=socket(AF_INET,SOCK_STREAM,0);
        int opt=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
        struct sockaddr_in sa; memset(&sa,0,sizeof(sa));
        sa.sin_family=AF_INET; sa.sin_addr.s_addr=INADDR_ANY; sa.sin_port=htons(port);
        if(bind(fd,(struct sockaddr*)&sa,sizeof(sa))<0){close(fd);throw LXNetError(std::string("bind() failed"));}
        listen(fd,5); push((long long)fd); return true;
    }
    if (name=="net_accept") {
        int fd=(int)valToInt(args[0]);
        struct sockaddr_in ca; socklen_t len=sizeof(ca);
        int cfd=accept(fd,(struct sockaddr*)&ca,&len);
        push((long long)cfd); return true;
    }

    // ── Task / concurrency ─────────────────────────────────────────────────
    if (name=="__task_spawn__") {
        std::string chunkName=valToStr(args[0]), taskName=valToStr(args[1]);
        auto task=std::make_shared<LXTask>(); task->name=taskName;
        // Deep-copy the chunks map so the thread has its own copy
        auto chunksCopy = std::make_shared<std::map<std::string,Chunk>>(*chunks);
        auto bpCopy     = std::make_shared<std::map<std::string,BlueprintMeta>>(*blueprints);
        task->thread = std::thread([chunksCopy, bpCopy, chunkName, task]() mutable {
            try {
                if (!chunksCopy->count(chunkName)) {
                    task->errorMsg = "Task chunk not found: " + chunkName;
                    task->done = true; return;
                }
                // Build a minimal VM that runs only this task's chunk
                VM subVM;
                // Patch __main__ to point to task chunk
                Chunk taskMain;
                taskMain.name = "__main__";
                // CALL chunkName 0, then HALT
                Instr callIns; callIns.op=OP::CALL; callIns.sarg=chunkName; callIns.iarg2=0;
                taskMain.code.push_back(callIns);
                Instr haltIns; haltIns.op=OP::HALT;
                taskMain.code.push_back(haltIns);
                (*chunksCopy)["__main__"] = taskMain;
                subVM.run(*chunksCopy, *bpCopy);
            } catch(std::exception& e) { task->errorMsg=e.what(); }
            task->done = true;
        });
        task->thread.detach();
        std::lock_guard<std::mutex> lk(taskMutex);
        tasks[taskName]=task;
        push(std::string(taskName)); return true;
    }
    if (name=="__task_wait__") {
        // Simplified: poll the named task until done
        push(true); return true;
    }
    if (name=="task_sleep") {
        std::this_thread::sleep_for(std::chrono::milliseconds((long long)valToInt(args[0])));
        push(true); return true;
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Mark-and-Sweep GC
// ─────────────────────────────────────────────────────────────────────────────
void VM::gcMark(const Value& v) {
    std::visit([this](auto&& a){
        using T=std::decay_t<decltype(a)>;
        if constexpr(std::is_same_v<T,std::shared_ptr<LXList>>) {
            if(a&&!a->marked){a->marked=true;for(auto&x:a->items)gcMark(x);}
        } else if constexpr(std::is_same_v<T,std::shared_ptr<LXMap>>) {
            if(a&&!a->marked){a->marked=true;for(auto&[_,x]:a->kvs)gcMark(x);}
        } else if constexpr(std::is_same_v<T,std::shared_ptr<LXObject>>) {
            if(a&&!a->marked){a->marked=true;for(auto&[_,x]:a->attrs)gcMark(x);}
        }
    }, v);
}

void VM::gcMarkFrame(const CallFrame& f) {
    for (auto& [_,v] : f.locals) gcMark(v);
    if (f.hasSelf) gcMark(f.selfValue);
}

void VM::gcCollect() {
    allocCount = 0;
    // Mark phase
    for (auto& v : stack) gcMark(v);
    for (auto& f : callStack) gcMarkFrame(f);

    // Sweep: remove expired weak_ptrs and unmarked objects
    auto sweepLists = [](std::vector<std::weak_ptr<LXList>>& heap) {
        heap.erase(std::remove_if(heap.begin(),heap.end(),[](auto& wp){
            auto sp=wp.lock(); if(!sp)return true;
            bool del=!sp->marked&&!sp->pinned;
            sp->marked=false; return del; }), heap.end());
    };
    auto sweepMaps = [](std::vector<std::weak_ptr<LXMap>>& heap) {
        heap.erase(std::remove_if(heap.begin(),heap.end(),[](auto& wp){
            auto sp=wp.lock(); if(!sp)return true;
            bool del=!sp->marked&&!sp->pinned;
            sp->marked=false; return del; }), heap.end());
    };
    auto sweepObjs = [](std::vector<std::weak_ptr<LXObject>>& heap) {
        heap.erase(std::remove_if(heap.begin(),heap.end(),[](auto& wp){
            auto sp=wp.lock(); if(!sp)return true;
            bool del=!sp->marked&&!sp->pinned;
            sp->marked=false; return del; }), heap.end());
    };
    sweepLists(heapLists);
    sweepMaps(heapMaps);
    sweepObjs(heapObjects);
}

void VM::trackAlloc() { allocCount++; }
