#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <functional>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <regex>
#include <filesystem>

// ─────────────────────────────────────────────
//  LX Language Interpreter
//  Extension: .lx
//  Author: LX Runtime Engine
// ─────────────────────────────────────────────

using Value = std::variant<long long, double, std::string, char, bool>;

// ── Value helpers ──────────────────────────────
std::string valueToString(const Value& v) {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) return arg;
        else if constexpr (std::is_same_v<T, char>)   return std::string(1, arg);
        else if constexpr (std::is_same_v<T, bool>)   return arg ? "VERUM" : "FALSUM";
        else if constexpr (std::is_same_v<T, double>)  {
            std::ostringstream oss;
            oss << arg;
            return oss.str();
        }
        else return std::to_string(arg);
    }, v);
}

double valueToDouble(const Value& v) {
    return std::visit([](auto&& arg) -> double {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) return std::stod(arg);
        else if constexpr (std::is_same_v<T, char>)   return (double)arg;
        else if constexpr (std::is_same_v<T, bool>)   return arg ? 1.0 : 0.0;
        else return (double)arg;
    }, v);
}

long long valueToInt(const Value& v) {
    return std::visit([](auto&& arg) -> long long {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) return std::stoll(arg);
        else if constexpr (std::is_same_v<T, char>)   return (long long)arg;
        else if constexpr (std::is_same_v<T, bool>)   return arg ? 1LL : 0LL;
        else if constexpr (std::is_same_v<T, double>) return (long long)arg;
        else return arg;
    }, v);
}

bool valueToBool(const Value& v) {
    return std::visit([](auto&& arg) -> bool {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) return !arg.empty();
        else if constexpr (std::is_same_v<T, bool>)   return arg;
        else if constexpr (std::is_same_v<T, double>) return arg != 0.0;
        else if constexpr (std::is_same_v<T, char>)   return arg != '\0';
        else return arg != 0LL;
    }, v);
}

// ── Utility ────────────────────────────────────
std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

std::vector<std::string> splitByComma(const std::string& s) {
    std::vector<std::string> result;
    int depth = 0;
    std::string cur;
    for (char c : s) {
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') depth--;
        if (c == ',' && depth == 0) {
            result.push_back(trim(cur));
            cur.clear();
        } else cur += c;
    }
    if (!cur.empty()) result.push_back(trim(cur));
    return result;
}

// ── Function definition ─────────────────────────
struct FunctionDef {
    std::string name;
    std::vector<std::string> params;
    std::vector<std::string> body; // raw lines
    int startLine; // absolute line in loaded file
};

// ── LX Interpreter Class ────────────────────────
class LXInterpreter {
public:
    std::vector<std::string>        lines;
    std::map<std::string, Value>    variables;
    std::map<std::string, FunctionDef> functions;
    std::map<std::string, int>      labels; // label -> line index
    int pc = 0; // program counter
    bool halted = false;
    int callDepth = 0;
    static const int MAX_DEPTH = 256;

    // ── Load file ──────────────────────────────
    bool loadFile(const std::string& filename) {
        std::ifstream f(filename);
        if (!f.is_open()) {
            std::cerr << "[LX ERROR] Cannot open file: " << filename << "\n";
            return false;
        }
        std::string line;
        while (std::getline(f, line))
            lines.push_back(line);
        return true;
    }

    // ── Pre-pass: scan labels, functions, imports ──
    void prePass() {
        // Resolve with_get imports first
        std::vector<std::string> expanded;
        for (auto& ln : lines) {
            std::string t = trim(ln);
            if (t.rfind("with_get", 0) == 0) {
                std::string fname = trim(t.substr(8));
                // strip < >
                if (!fname.empty() && fname.front() == '<') fname = fname.substr(1);
                if (!fname.empty() && fname.back() == '>')  fname.pop_back();
                fname = trim(fname);
                // try as-is, then .lx
                std::ifstream f(fname);
                if (!f.is_open()) f.open(fname + ".lx");
                if (f.is_open()) {
                    std::string il;
                    while (std::getline(f, il)) expanded.push_back(il);
                } else {
                    std::cerr << "[LX WARN] Import not found: " << fname << "\n";
                }
            } else {
                expanded.push_back(ln);
            }
        }
        lines = expanded;

        // Scan labels:  label_mark: LABELNAME
        for (int i = 0; i < (int)lines.size(); i++) {
            std::string t = trim(lines[i]);
            if (t.rfind("label_mark:", 0) == 0) {
                std::string lname = trim(t.substr(11));
                labels[lname] = i;
            }
        }

        // Scan function definitions:  func_begin: NAME (params)
        for (int i = 0; i < (int)lines.size(); i++) {
            std::string t = trim(lines[i]);
            if (t.rfind("func_begin:", 0) == 0) {
                std::string rest = trim(t.substr(11));
                // parse NAME(params)
                size_t paren = rest.find('(');
                FunctionDef fd;
                if (paren != std::string::npos) {
                    fd.name = trim(rest.substr(0, paren));
                    size_t cparen = rest.find(')', paren);
                    std::string paramStr = rest.substr(paren+1, cparen - paren - 1);
                    if (!trim(paramStr).empty())
                        fd.params = splitByComma(paramStr);
                } else {
                    fd.name = rest;
                }
                fd.startLine = i;
                // collect body until func_end:
                int j = i + 1;
                while (j < (int)lines.size() && trim(lines[j]) != "func_end:") {
                    fd.body.push_back(lines[j]);
                    j++;
                }
                functions[fd.name] = fd;
            }
        }
    }

    // ── Expression evaluator ───────────────────
    Value evalExpr(const std::string& expr) {
        std::string e = trim(expr);

        // String literal
        if (e.size() >= 2 && e.front() == '"' && e.back() == '"')
            return e.substr(1, e.size()-2);

        // Char literal
        if (e.size() == 3 && e.front() == '\'' && e.back() == '\'')
            return e[1];

        // Bool literals
        if (e == "VERUM")  return true;
        if (e == "FALSUM") return false;

        // Null/empty
        if (e == "VOIDUM") return std::string("");

        // Function call inside expression:  FUNCNAME(args)
        {
            std::regex funcRe(R"(^([A-Za-z_][A-Za-z0-9_]*)\((.*)\)$)");
            std::smatch m;
            if (std::regex_match(e, m, funcRe)) {
                std::string fname = m[1];
                std::string argStr = m[2];
                if (functions.count(fname)) {
                    auto args = argStr.empty() ? std::vector<std::string>{} : splitByComma(argStr);
                    return callFunction(fname, args);
                }
            }
        }

        // Built-in functions
        if (e.rfind("len(", 0) == 0 && e.back() == ')') {
            Value inner = evalExpr(e.substr(4, e.size()-5));
            return (long long)valueToString(inner).size();
        }
        if (e.rfind("num(", 0) == 0 && e.back() == ')') {
            Value inner = evalExpr(e.substr(4, e.size()-5));
            try { return (long long)std::stoll(valueToString(inner)); }
            catch(...) { return (double)std::stod(valueToString(inner)); }
        }
        if (e.rfind("str(", 0) == 0 && e.back() == ')') {
            return valueToString(evalExpr(e.substr(4, e.size()-5)));
        }
        if (e.rfind("sqrt(", 0) == 0 && e.back() == ')') {
            return std::sqrt(valueToDouble(evalExpr(e.substr(5, e.size()-6))));
        }
        if (e.rfind("abs(", 0) == 0 && e.back() == ')') {
            double v = valueToDouble(evalExpr(e.substr(4, e.size()-5)));
            return std::abs(v);
        }
        if (e.rfind("input(", 0) == 0 && e.back() == ')') {
            std::string prompt = valueToString(evalExpr(e.substr(6, e.size()-7)));
            std::cout << prompt;
            std::string inp; std::getline(std::cin, inp);
            return inp;
        }

        // Comparison / logical operators (low precedence)
        // We handle these with a simple recursive descent

        // Try binary ops: check for lowest-precedence operators first
        // Operators: OR_W, AND_W, IS_NOT, IS, GT_EQ, LT_EQ, GT, LT, +, -, *, /, %

        // Split on "OR_W"
        {
            int depth = 0;
            for (int i = (int)e.size()-1; i >= 0; i--) {
                if (e[i] == ')') depth++;
                else if (e[i] == '(') depth--;
                if (depth == 0 && i+4 <= (int)e.size() && e.substr(i,4) == "OR_W") {
                    // check word boundary
                    bool leftOk  = (i == 0 || !isalnum(e[i-1]));
                    bool rightOk = (i+4 >= (int)e.size() || !isalnum(e[i+4]));
                    if (leftOk && rightOk) {
                        bool l = valueToBool(evalExpr(e.substr(0,i)));
                        bool r = valueToBool(evalExpr(e.substr(i+4)));
                        return l || r;
                    }
                }
            }
        }
        {
            int depth = 0;
            for (int i = (int)e.size()-1; i >= 0; i--) {
                if (e[i] == ')') depth++;
                else if (e[i] == '(') depth--;
                if (depth == 0 && i+5 <= (int)e.size() && e.substr(i,5) == "AND_W") {
                    bool leftOk  = (i == 0 || !isalnum(e[i-1]));
                    bool rightOk = (i+5 >= (int)e.size() || !isalnum(e[i+5]));
                    if (leftOk && rightOk) {
                        bool l = valueToBool(evalExpr(e.substr(0,i)));
                        bool r = valueToBool(evalExpr(e.substr(i+5)));
                        return l && r;
                    }
                }
            }
        }
        // NOT_W
        if (e.rfind("NOT_W ", 0) == 0)
            return !valueToBool(evalExpr(e.substr(6)));

        // IS_NOT
        {
            size_t pos = e.find(" IS_NOT ");
            if (pos != std::string::npos) {
                Value l = evalExpr(e.substr(0, pos));
                Value r = evalExpr(e.substr(pos+8));
                return valueToString(l) != valueToString(r);
            }
        }
        // IS
        {
            size_t pos = e.find(" IS ");
            if (pos != std::string::npos) {
                Value l = evalExpr(e.substr(0, pos));
                Value r = evalExpr(e.substr(pos+4));
                // numeric equality if both numeric
                try {
                    double ld = valueToDouble(l), rd = valueToDouble(r);
                    return ld == rd;
                } catch(...) {}
                return valueToString(l) == valueToString(r);
            }
        }
        // GT_EQ
        {
            size_t pos = e.find(" GT_EQ ");
            if (pos != std::string::npos) {
                double l = valueToDouble(evalExpr(e.substr(0,pos)));
                double r = valueToDouble(evalExpr(e.substr(pos+7)));
                return l >= r;
            }
        }
        // LT_EQ
        {
            size_t pos = e.find(" LT_EQ ");
            if (pos != std::string::npos) {
                double l = valueToDouble(evalExpr(e.substr(0,pos)));
                double r = valueToDouble(evalExpr(e.substr(pos+7)));
                return l <= r;
            }
        }
        // GT
        {
            size_t pos = e.find(" GT ");
            if (pos != std::string::npos) {
                double l = valueToDouble(evalExpr(e.substr(0,pos)));
                double r = valueToDouble(evalExpr(e.substr(pos+4)));
                return l > r;
            }
        }
        // LT
        {
            size_t pos = e.find(" LT ");
            if (pos != std::string::npos) {
                double l = valueToDouble(evalExpr(e.substr(0,pos)));
                double r = valueToDouble(evalExpr(e.substr(pos+4)));
                return l < r;
            }
        }

        // Arithmetic: right-to-left scan for + and -
        {
            int depth = 0;
            for (int i = (int)e.size()-1; i >= 1; i--) {
                if (e[i] == ')') depth++;
                else if (e[i] == '(') depth--;
                if (depth == 0 && (e[i] == '+' || e[i] == '-') && e[i-1] != '*' && e[i-1] != '/' && e[i-1] != '%') {
                    // make sure it's not a unary minus
                    if (i == 0) continue;
                    char op = e[i];
                    std::string left  = trim(e.substr(0,i));
                    std::string right = trim(e.substr(i+1));
                    if (left.empty()) continue;
                    Value lv = evalExpr(left);
                    Value rv = evalExpr(right);
                    // string concat for +
                    if (op == '+') {
                        bool lStr = std::holds_alternative<std::string>(lv);
                        bool rStr = std::holds_alternative<std::string>(rv);
                        if (lStr || rStr) return valueToString(lv) + valueToString(rv);
                        bool lDbl = std::holds_alternative<double>(lv) || std::holds_alternative<double>(rv);
                        if (lDbl) return valueToDouble(lv) + valueToDouble(rv);
                        return valueToInt(lv) + valueToInt(rv);
                    } else {
                        bool lDbl = std::holds_alternative<double>(lv) || std::holds_alternative<double>(rv);
                        if (lDbl) return valueToDouble(lv) - valueToDouble(rv);
                        return valueToInt(lv) - valueToInt(rv);
                    }
                }
            }
        }
        // * / %
        {
            int depth = 0;
            for (int i = (int)e.size()-1; i >= 1; i--) {
                if (e[i] == ')') depth++;
                else if (e[i] == '(') depth--;
                if (depth == 0 && (e[i] == '*' || e[i] == '/' || e[i] == '%')) {
                    char op = e[i];
                    std::string left  = trim(e.substr(0,i));
                    std::string right = trim(e.substr(i+1));
                    if (left.empty() || right.empty()) continue;
                    Value lv = evalExpr(left);
                    Value rv = evalExpr(right);
                    double ld = valueToDouble(lv), rd = valueToDouble(rv);
                    if (op == '*') {
                        bool isInt = std::holds_alternative<long long>(lv) && std::holds_alternative<long long>(rv);
                        return isInt ? Value(valueToInt(lv)*valueToInt(rv)) : Value(ld*rd);
                    }
                    if (op == '/') { if (rd == 0) throw std::runtime_error("Division by zero"); return ld/rd; }
                    if (op == '%') { return valueToInt(lv) % valueToInt(rv); }
                }
            }
        }

        // Unary minus
        if (!e.empty() && e[0] == '-') {
            Value v = evalExpr(e.substr(1));
            if (std::holds_alternative<double>(v)) return -std::get<double>(v);
            return -valueToInt(v);
        }

        // Parenthesised expression
        if (!e.empty() && e.front() == '(' && e.back() == ')') {
            return evalExpr(e.substr(1, e.size()-2));
        }

        // Variable lookup
        if (variables.count(e)) return variables[e];

        // Number literal
        try {
            size_t pos;
            long long iv = std::stoll(e, &pos);
            if (pos == e.size()) return iv;
        } catch(...) {}
        try {
            size_t pos;
            double dv = std::stod(e, &pos);
            if (pos == e.size()) return dv;
        } catch(...) {}

        // Bare word → string
        return e;
    }

    // ── Execute a single line ──────────────────
    // Returns: -1 = normal, -2 = halt, >=0 = jump to line, -3 = return (with value)
    Value returnValue;
    bool  hasReturn = false;

    int executeLine(const std::string& rawLine) {
        std::string line = trim(rawLine);

        // Blank / comment
        if (line.empty() || line.rfind("~~", 0) == 0) return -1;

        // label_mark: — skip at runtime
        if (line.rfind("label_mark:", 0) == 0) return -1;

        // func_begin: — skip function definitions at runtime
        if (line.rfind("func_begin:", 0) == 0) {
            // skip to func_end:
            pc++;
            while (pc < (int)lines.size() && trim(lines[pc]) != "func_end:") pc++;
            return -1;
        }
        if (line == "func_end:") return -1;

        // ── with_text: ─────────────────────────
        if (line.rfind("with_text:", 0) == 0) {
            std::string expr = trim(line.substr(10));
            std::cout << valueToString(evalExpr(expr)) << "\n";
            return -1;
        }

        // ── with_text_raw: (no newline) ─────────
        if (line.rfind("with_text_raw:", 0) == 0) {
            std::string expr = trim(line.substr(14));
            std::cout << valueToString(evalExpr(expr));
            return -1;
        }

        // ── d_type: TYPE VARNAME :: EXPR ────────
        if (line.rfind("d_type:", 0) == 0) {
            std::string rest = trim(line.substr(7));
            // find type
            size_t sp = rest.find(' ');
            if (sp == std::string::npos) throw std::runtime_error("Bad d_type declaration: " + line);
            std::string dtype = trim(rest.substr(0, sp));
            rest = trim(rest.substr(sp+1));
            // find ::
            size_t colcol = rest.find("::");
            if (colcol == std::string::npos) throw std::runtime_error("Missing :: in d_type: " + line);
            std::string varname = trim(rest.substr(0, colcol));
            std::string expr    = trim(rest.substr(colcol+2));
            Value val = evalExpr(expr);
            // coerce by dtype
            if (dtype == "short" || dtype == "long" || dtype == "int")
                val = valueToInt(val);
            else if (dtype == "float" || dtype == "double")
                val = valueToDouble(val);
            else if (dtype == "char")
                val = (char)valueToString(val)[0];
            else if (dtype == "bool")
                val = valueToBool(val);
            // string stays as string
            variables[varname] = val;
            return -1;
        }

        // ── reassign: VARNAME :: EXPR ───────────
        if (line.rfind("reassign:", 0) == 0) {
            std::string rest = trim(line.substr(9));
            size_t colcol = rest.find("::");
            if (colcol == std::string::npos) throw std::runtime_error("Missing :: in reassign: " + line);
            std::string varname = trim(rest.substr(0, colcol));
            std::string expr    = trim(rest.substr(colcol+2));
            if (!variables.count(varname)) throw std::runtime_error("Variable not declared: " + varname);
            Value val = evalExpr(expr);
            // preserve original type
            Value& existing = variables[varname];
            if (std::holds_alternative<long long>(existing))       val = valueToInt(val);
            else if (std::holds_alternative<double>(existing))     val = valueToDouble(val);
            else if (std::holds_alternative<char>(existing))       val = (char)valueToString(val)[0];
            else if (std::holds_alternative<bool>(existing))       val = valueToBool(val);
            variables[varname] = val;
            return -1;
        }

        // ── jump: LABELNAME ────────────────────
        if (line.rfind("jump:", 0) == 0) {
            std::string lname = trim(line.substr(5));
            if (!labels.count(lname)) throw std::runtime_error("Unknown label: " + lname);
            return labels[lname]; // jump to that line
        }

        // ── jump_if: COND -> LABELNAME ──────────
        if (line.rfind("jump_if:", 0) == 0) {
            std::string rest = trim(line.substr(8));
            size_t arrow = rest.find("->");
            if (arrow == std::string::npos) throw std::runtime_error("Bad jump_if syntax: " + line);
            std::string cond  = trim(rest.substr(0, arrow));
            std::string lname = trim(rest.substr(arrow+2));
            if (valueToBool(evalExpr(cond))) {
                if (!labels.count(lname)) throw std::runtime_error("Unknown label: " + lname);
                return labels[lname];
            }
            return -1;
        }

        // ── maybe: COND [then_block] ────────────
        // maybe: COND  (single-line if)
        //   ... body lines ...
        // perhaps_not:
        //   ... else lines ...
        // end_maybe:
        if (line.rfind("maybe:", 0) == 0) {
            std::string cond = trim(line.substr(6));
            bool condVal = valueToBool(evalExpr(cond));
            // collect then_lines and else_lines by scanning forward
            std::vector<std::string> thenLines, elseLines;
            bool inElse = false;
            int depth = 1;
            pc++;
            while (pc < (int)lines.size()) {
                std::string lt = trim(lines[pc]);
                if (lt.rfind("maybe:", 0) == 0) depth++;
                if (lt == "end_maybe:") { depth--; if (depth == 0) break; }
                if (depth == 1 && lt == "perhaps_not:") { inElse = true; pc++; continue; }
                if (!inElse) thenLines.push_back(lines[pc]);
                else          elseLines.push_back(lines[pc]);
                pc++;
            }
            auto& chosen = condVal ? thenLines : elseLines;
            // run chosen block
            LXInterpreter sub;
            sub.variables = variables;
            sub.functions = functions;
            sub.labels    = labels;
            sub.lines     = chosen;
            sub.run();
            // merge variables back
            for (auto& [k,v] : sub.variables) variables[k] = v;
            if (sub.hasReturn) { hasReturn = true; returnValue = sub.returnValue; return -3; }
            return -1;
        }

        // ── loop_while: COND ───────────────────
        if (line.rfind("loop_while:", 0) == 0) {
            std::string cond = trim(line.substr(11));
            std::vector<std::string> bodyLines;
            int depth = 1;
            pc++;
            while (pc < (int)lines.size()) {
                std::string lt = trim(lines[pc]);
                if (lt.rfind("loop_while:", 0) == 0) depth++;
                if (lt == "end_loop:") { depth--; if (depth == 0) break; }
                bodyLines.push_back(lines[pc]);
                pc++;
            }
            int maxIter = 1000000;
            while (valueToBool(evalExpr(cond)) && maxIter-- > 0) {
                LXInterpreter sub;
                sub.variables = variables;
                sub.functions = functions;
                sub.labels    = labels;
                sub.lines     = bodyLines;
                sub.run();
                for (auto& [k,v] : sub.variables) variables[k] = v;
                if (sub.hasReturn) { hasReturn = true; returnValue = sub.returnValue; return -3; }
                if (sub.halted) { halted = true; return -2; }
            }
            return -1;
        }

        // ── loop_from: VAR FROM start TO end [STEP s] ──
        if (line.rfind("loop_from:", 0) == 0) {
            std::string rest = trim(line.substr(10));
            // parse:  VAR FROM expr TO expr [STEP expr]
            std::regex loopRe(R"((\w+)\s+FROM\s+(.+?)\s+TO\s+(.+?)(?:\s+STEP\s+(.+))?\s*$)");
            std::smatch m;
            if (!std::regex_match(rest, m, loopRe))
                throw std::runtime_error("Bad loop_from syntax: " + line);
            std::string varN  = m[1];
            long long start   = valueToInt(evalExpr(m[2]));
            long long end_    = valueToInt(evalExpr(m[3]));
            long long step    = m[4].matched ? valueToInt(evalExpr(m[4])) : 1;
            std::vector<std::string> bodyLines;
            int depth = 1;
            pc++;
            while (pc < (int)lines.size()) {
                std::string lt = trim(lines[pc]);
                if (lt.rfind("loop_from:", 0) == 0) depth++;
                if (lt == "end_loop:") { depth--; if (depth == 0) break; }
                bodyLines.push_back(lines[pc]);
                pc++;
            }
            variables[varN] = start;
            for (long long i = start; (step>0 ? i<=end_ : i>=end_); i += step) {
                variables[varN] = i;
                LXInterpreter sub;
                sub.variables = variables;
                sub.functions = functions;
                sub.labels    = labels;
                sub.lines     = bodyLines;
                sub.run();
                for (auto& [k,v] : sub.variables) variables[k] = v;
                if (sub.hasReturn) { hasReturn = true; returnValue = sub.returnValue; return -3; }
                if (sub.halted) { halted = true; return -2; }
            }
            return -1;
        }

        // ── call: FUNCNAME(args) ────────────────
        if (line.rfind("call:", 0) == 0) {
            std::string rest = trim(line.substr(5));
            std::regex funcRe(R"(([A-Za-z_][A-Za-z0-9_]*)\((.*)\))");
            std::smatch m;
            if (!std::regex_match(rest, m, funcRe))
                throw std::runtime_error("Bad call syntax: " + line);
            std::string fname = m[1];
            std::string argStr = m[2];
            auto args = argStr.empty() ? std::vector<std::string>{} : splitByComma(argStr);
            callFunction(fname, args);
            return -1;
        }

        // ── give_back: EXPR ────────────────────
        if (line.rfind("give_back:", 0) == 0) {
            returnValue = evalExpr(trim(line.substr(10)));
            hasReturn = true;
            return -3;
        }

        // ── halt_now: ──────────────────────────
        if (line == "halt_now:") {
            halted = true;
            return -2;
        }

        // ── scream: EXPR (print to stderr) ──────
        if (line.rfind("scream:", 0) == 0) {
            std::cerr << "[LX RUNTIME] " << valueToString(evalExpr(trim(line.substr(7)))) << "\n";
            return -1;
        }

        throw std::runtime_error("Unknown statement: " + line);
    }

    // ── Call a function ──────────────────────
    Value callFunction(const std::string& fname, const std::vector<std::string>& argExprs) {
        if (!functions.count(fname))
            throw std::runtime_error("Undefined function: " + fname);
        if (++callDepth > MAX_DEPTH) throw std::runtime_error("Stack overflow");
        FunctionDef& fd = functions[fname];
        LXInterpreter sub;
        sub.functions = functions;
        sub.labels    = labels;
        sub.variables = variables; // inherit globals
        sub.callDepth = callDepth;
        // bind params
        for (size_t i = 0; i < fd.params.size(); i++) {
            std::string pname = trim(fd.params[i]);
            Value val = (i < argExprs.size()) ? evalExpr(argExprs[i]) : Value(0LL);
            sub.variables[pname] = val;
        }
        sub.lines = fd.body;
        sub.run();
        // write back globals (not params)
        for (auto& [k,v] : sub.variables) {
            bool isParam = false;
            for (auto& p : fd.params) if (trim(p) == k) { isParam = true; break; }
            if (!isParam) variables[k] = v;
        }
        callDepth--;
        return sub.hasReturn ? sub.returnValue : Value(0LL);
    }

    // ── Main run loop ──────────────────────────
    void run() {
        pc = 0;
        while (pc < (int)lines.size() && !halted && !hasReturn) {
            int result = executeLine(lines[pc]);
            if (result == -2) break;           // halt
            if (result == -3) break;           // return
            if (result >= 0)  pc = result;     // jump
            else              pc++;            // normal advance
        }
    }
};

// ── Entry point ────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: lx <file.lx | file.txt>\n";
        return 1;
    }
    std::string filename = argv[1];

    LXInterpreter interp;
    if (!interp.loadFile(filename)) return 1;
    interp.prePass();

    try {
        interp.run();
    } catch (const std::exception& e) {
        std::cerr << "[LX FATAL] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
