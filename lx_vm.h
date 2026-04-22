#pragma once
/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  LX 3.0 — lx_vm.h                                                   ║
 * ║  Stack-based Virtual Machine with Mark-and-Sweep GC                 ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */
#include "lx_bytecode.h"
#include "lx_compiler.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Call frame
// ─────────────────────────────────────────────────────────────────────────────
struct CallFrame {
    Chunk*      chunk{nullptr};
    int         pc{0};
    std::unordered_map<std::string, Value> locals;
    Value       selfValue;    // 'self' for method calls (VOIDUM if none)
    bool        hasSelf{false};
    // try/catch handler stack: {catch_addr}
    std::vector<int> handlers;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Task (concurrent unit)
// ─────────────────────────────────────────────────────────────────────────────
struct LXTask {
    std::string name;
    std::thread thread;
    std::atomic<bool> done{false};
    std::string errorMsg;
};

// ─────────────────────────────────────────────────────────────────────────────
//  VM
// ─────────────────────────────────────────────────────────────────────────────
class VM {
public:
    std::map<std::string, Chunk>*         chunks{nullptr};
    std::map<std::string, BlueprintMeta>* blueprints{nullptr};

    void run(std::map<std::string, Chunk>& ch,
             std::map<std::string, BlueprintMeta>& bp);

    // GC interface
    void gcCollect();

private:
    std::vector<Value>     stack;
    std::vector<CallFrame> callStack;
    std::mutex             gcMutex;

    // Task registry
    std::map<std::string, std::shared_ptr<LXTask>> tasks;
    std::mutex taskMutex;

    // GC heap tracking (all shared_ptr objects registered here)
    std::vector<std::weak_ptr<LXList>>      heapLists;
    std::vector<std::weak_ptr<LXMap>>       heapMaps;
    std::vector<std::weak_ptr<LXObject>>    heapObjects;
    size_t allocCount{0};
    static constexpr size_t GC_THRESHOLD = 512;

    // Stack / frame helpers
    CallFrame& frame();
    Chunk&     chunk();
    void       push(Value v);
    Value      pop();
    Value&     top();
    Value      getVar(const std::string& name);
    void       setVar(const std::string& name, Value val);
    void       setNewVar(const std::string& name, Value val);

    // Execution
    void execute();
    void dispatch(Instr& ins);

    // Built-ins
    bool tryBuiltin(const std::string& name, int argc, std::vector<Value>& args);

    // OOP helpers
    Value  instantiate(const std::string& bpName, const std::vector<Value>& args);
    std::string resolveMethod(const std::string& bpName, const std::string& method);
    void   callMethod(std::shared_ptr<LXObject> obj, const std::string& method,
                      const std::vector<Value>& args);

    // GC helpers
    void   gcMark(const Value& v);
    void   gcMarkFrame(const CallFrame& f);
    void   trackAlloc();

    // Error formatting
    std::string formatError(const LXError& e, int line);
};
