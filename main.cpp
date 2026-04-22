/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  LX 3.0 — main.cpp                                                  ║
 * ║  CLI driver: run / build / exec / dump                              ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */
#include "lx_lexer.h"
#include "lx_parser.h"
#include "lx_compiler.h"
#include "lx_vm.h"
#include "lx_import.h"

static void printUsage() {
    std::cerr <<
        "LX Language Runtime v3.0\n"
        "─────────────────────────────────────\n"
        "Usage:\n"
        "  lx run   <file.lx>     compile + run source\n"
        "  lx build <file.lx>     compile → <file.lxc>\n"
        "  lx exec  <file.lxc>    run pre-compiled bytecode\n"
        "  lx dump  <file.lx>     print bytecode disassembly\n"
        "  lx <file.lx>           shorthand for 'run'\n";
}

static std::string baseDirOf(const std::string& path) {
    auto s = path.find_last_of("/\\");
    return s != std::string::npos ? path.substr(0, s) : "";
}

static bool compileSrc(const std::string& file,
                        std::map<std::string, Chunk>& chunks,
                        std::map<std::string, BlueprintMeta>& blueprints) {
    // Load source
    std::string source;
    try { source = readFileStr(file); }
    catch (LXIOError& e) { std::cerr << e.what() << "\n"; return false; }

    // Resolve imports
    std::vector<std::string> visited = {file};
    source = resolveImports(source, baseDirOf(file), visited);

    // Lex
    std::vector<Token> tokens;
    try {
        Lexer lexer(source, file);
        tokens = lexer.tokenize();
    } catch (LXError& e) {
        std::cerr << e.what() << "\n"; return false;
    }

    // Parse
    ASTPtr ast;
    try {
        Parser parser(std::move(tokens));
        ast = parser.parseProgram();
    } catch (LXError& e) {
        std::cerr << e.what() << "\n"; return false;
    }

    // Compile
    try {
        Compiler compiler;
        compiler.compile(ast);
        chunks     = compiler.chunks;
        blueprints = compiler.blueprints;
    } catch (LXError& e) {
        std::cerr << e.what() << "\n"; return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) { printUsage(); return 1; }

    std::string mode = "run";
    std::string file;
    if (argc == 2) { file = argv[1]; }
    else { mode = argv[1]; file = argv[2]; }

    std::map<std::string, Chunk>        chunks;
    std::map<std::string, BlueprintMeta> blueprints;

    if (mode == "exec") {
        if (!loadChunks(chunks, file)) {
            std::cerr << "[LX] Failed to load bytecode: " << file << "\n"; return 1;
        }
        // blueprints not serialised yet — exec works for non-OOP code
    } else {
        if (!compileSrc(file, chunks, blueprints)) return 1;
    }

    if (mode == "dump") {
        disassemble(chunks, std::cout); return 0;
    }
    if (mode == "build") {
        std::string out = file;
        auto dot = out.rfind('.');
        if (dot != std::string::npos) out = out.substr(0, dot);
        out += ".lxc";
        if (saveChunks(chunks, out))
            std::cout << "[LX] Bytecode written: " << out << "\n";
        else
            std::cerr << "[LX] Failed to write: " << out << "\n";
        return 0;
    }

    // run
    try {
        VM vm;
        vm.run(chunks, blueprints);
    } catch (std::exception& e) {
        std::cerr << "[LX FATAL] " << e.what() << "\n"; return 1;
    }
    return 0;
}
