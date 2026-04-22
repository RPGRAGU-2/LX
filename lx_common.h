#pragma once
/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  LX 3.0 — lx_common.h                                               ║
 * ║  Shared value types, error system, GC spine, forward declarations   ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <variant>
#include <memory>
#include <functional>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <optional>
#include <iomanip>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>
#include <cassert>
#include <cstring>
#include <set>

// ─────────────────────────────────────────────────────────────────────────────
//  Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
struct LXList;
struct LXMap;
struct LXObject;
struct LXBlueprint;
struct LXFunction;  // callable closure / method ref

// ─────────────────────────────────────────────────────────────────────────────
//  Value — the universal runtime value type
// ─────────────────────────────────────────────────────────────────────────────
using Value = std::variant<
    long long,                       // long / short / int
    double,                          // float / double
    std::string,                     // string
    char,                            // char
    bool,                            // bool
    std::shared_ptr<LXList>,         // list
    std::shared_ptr<LXMap>,          // map
    std::shared_ptr<LXObject>,       // object instance
    std::shared_ptr<LXBlueprint>     // blueprint (class) as first-class value
>;

// ─────────────────────────────────────────────────────────────────────────────
//  Heap objects  (GC-tracked)
// ─────────────────────────────────────────────────────────────────────────────
struct GCHeader {
    bool marked{false};        // GC mark bit
    bool pinned{false};        // never collect (builtins, globals)
};

struct LXList : GCHeader {
    std::vector<Value> items;
};

struct LXMap : GCHeader {
    std::map<std::string, Value> kvs;
};

// Blueprint = class definition
struct LXBlueprint : GCHeader {
    std::string name;
    std::string parent;        // name of parent blueprint, or ""
    // method name → chunk name (e.g. "Animal.speak")
    std::map<std::string, std::string> methods;
    // field names declared in init_begin / method bodies
    std::vector<std::string>  fields;
};

// Object instance
struct LXObject : GCHeader {
    std::string blueprint;     // name of the blueprint
    std::map<std::string, Value> attrs;   // instance attributes
};

// ─────────────────────────────────────────────────────────────────────────────
//  Value helpers
// ─────────────────────────────────────────────────────────────────────────────
std::string valToStr(const Value& v);   // forward — defined in lx_value.cpp

double      valToDouble(const Value& v);
long long   valToInt   (const Value& v);
bool        valToBool  (const Value& v);
bool        valEqual   (const Value& a, const Value& b);

// ─────────────────────────────────────────────────────────────────────────────
//  LX Error hierarchy
// ─────────────────────────────────────────────────────────────────────────────
struct LXError : public std::runtime_error {
    Value lxVal;
    int   sourceLine{0};
    explicit LXError(const Value& v, int line = 0)
        : std::runtime_error(lxErrMsg(v)), lxVal(v), sourceLine(line) {}

    static std::string lxErrMsg(const Value& v) {
        if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
        // defer to valToStr when available
        try {
            // quick inline for bootstrap
            if (std::holds_alternative<long long>(v)) return std::to_string(std::get<long long>(v));
            if (std::holds_alternative<double>(v)) return std::to_string(std::get<double>(v));
            if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "VERUM" : "FALSUM";
        } catch(...) {}
        return "<error>";
    }
};

// Specific subtypes (caught differently by the VM)
struct LXTypeError    : LXError { using LXError::LXError; };
struct LXIndexError   : LXError { using LXError::LXError; };
struct LXKeyError     : LXError { using LXError::LXError; };
struct LXValueError   : LXError { using LXError::LXError; };
struct LXRuntimeError : LXError { using LXError::LXError; };
struct LXImportError  : LXError { using LXError::LXError; };
struct LXIOError      : LXError { using LXError::LXError; };
struct LXNetError     : LXError { using LXError::LXError; };

// ─────────────────────────────────────────────────────────────────────────────
//  Utility
// ─────────────────────────────────────────────────────────────────────────────
inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

inline std::vector<std::string> splitByComma(const std::string& s) {
    std::vector<std::string> r;
    int depth = 0; std::string cur;
    for (char c : s) {
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        if (c == ',' && depth == 0) { r.push_back(trim(cur)); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) r.push_back(trim(cur));
    return r;
}

inline std::string readFileStr(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        f.open(path + ".lx");
        if (!f.is_open()) throw LXIOError(std::string("Cannot open file: " + path));
    }
    return {(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()};
}
