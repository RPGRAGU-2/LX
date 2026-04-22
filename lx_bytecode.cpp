/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  LX 3.0 — lx_bytecode.cpp                                           ║
 * ║  Bytecode serialisation + disassembler                              ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */
#include "lx_bytecode.h"

static const char LXC_MAGIC[] = "LXC\x03";  // version 3

// ─────────────────────────────────────────────────────────────────────────────
static void writeU16(std::ostream& o, uint16_t v) { o.write((char*)&v,2); }
static void writeU32(std::ostream& o, uint32_t v) { o.write((char*)&v,4); }
static void writeI64(std::ostream& o, int64_t  v) { o.write((char*)&v,8); }
static void writeD64(std::ostream& o, double   v) { o.write((char*)&v,8); }
static void writeStr(std::ostream& o, const std::string& s) {
    writeU32(o,(uint32_t)s.size()); o.write(s.data(),s.size());
}

static uint16_t readU16(std::istream& i) { uint16_t v; i.read((char*)&v,2); return v; }
static uint32_t readU32(std::istream& i) { uint32_t v; i.read((char*)&v,4); return v; }
static int64_t  readI64(std::istream& i) { int64_t  v; i.read((char*)&v,8); return v; }
static double   readD64(std::istream& i) { double   v; i.read((char*)&v,8); return v; }
static std::string readStr(std::istream& i) {
    uint32_t len = readU32(i);
    std::string s(len,'\0');
    i.read(s.data(),len);
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
static void serChunk(std::ostream& out, const Chunk& c) {
    writeStr(out, c.name);
    writeStr(out, c.blueprintOwner);
    writeU32(out, (uint32_t)c.params.size());
    for (auto& p : c.params) writeStr(out, p);
    writeU32(out, (uint32_t)c.code.size());
    for (auto& ins : c.code) {
        out.write((char*)&ins.op, 1);
        writeI64(out, ins.iarg);
        writeD64(out, ins.darg);
        writeU32(out, (uint32_t)ins.iarg2);
        writeStr(out, ins.sarg);
        writeU32(out, (uint32_t)ins.line);
        char bv = ins.bval ? 1 : 0; out.write(&bv,1);
        out.write(&ins.cval,1);
    }
}

static void deserChunk(std::istream& in, Chunk& c) {
    c.name          = readStr(in);
    c.blueprintOwner= readStr(in);
    uint32_t np = readU32(in);
    for (uint32_t i=0;i<np;i++) c.params.push_back(readStr(in));
    uint32_t nc = readU32(in);
    for (uint32_t i=0;i<nc;i++) {
        Instr ins;
        in.read((char*)&ins.op,1);
        ins.iarg  = (long long)readI64(in);
        ins.darg  = readD64(in);
        ins.iarg2 = (int)readU32(in);
        ins.sarg  = readStr(in);
        ins.line  = (int)readU32(in);
        char bv; in.read(&bv,1); ins.bval = bv != 0;
        in.read(&ins.cval,1);
        c.code.push_back(ins);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
bool saveChunks(const std::map<std::string,Chunk>& chunks, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(LXC_MAGIC, 4);
    writeU32(f, (uint32_t)chunks.size());
    for (auto& [_,c] : chunks) serChunk(f, c);
    return true;
}

bool loadChunks(std::map<std::string,Chunk>& chunks, const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[4]; f.read(magic,4);
    if (std::string(magic,4) != std::string(LXC_MAGIC,4)) return false;
    uint32_t n = readU32(f);
    for (uint32_t i=0;i<n;i++) {
        Chunk c; deserChunk(f,c); chunks[c.name]=c;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void disassemble(const std::map<std::string,Chunk>& chunks, std::ostream& out) {
    for (auto& [name, c] : chunks) {
        out << "\n╔══ CHUNK: " << name;
        if (!c.blueprintOwner.empty()) out << "  [method of " << c.blueprintOwner << "]";
        out << " ══╗\n";
        if (!c.params.empty()) {
            out << "  params: ";
            for (auto& p : c.params) out << p << " ";
            out << "\n";
        }
        for (int i=0;i<(int)c.code.size();i++) {
            auto& ins = c.code[i];
            out << "  " << std::setw(4) << i << "  "
                << std::left << std::setw(14) << opName(ins.op);
            switch (ins.op) {
                case OP::PUSH_INT:   out << " " << ins.iarg; break;
                case OP::PUSH_FLOAT: out << " " << ins.darg; break;
                case OP::PUSH_STR:   out << " \"" << ins.sarg << "\""; break;
                case OP::PUSH_BOOL:  out << " " << (ins.bval?"VERUM":"FALSUM"); break;
                case OP::PUSH_CHAR:  out << " '" << ins.cval << "'"; break;
                case OP::LOAD:
                case OP::STORE:
                case OP::STORE_NEW:
                case OP::GET_ATTR:
                case OP::SET_ATTR:   out << " \"" << ins.sarg << "\""; break;
                case OP::CALL:
                case OP::CALL_METHOD:
                case OP::NEW_OBJ:    out << " " << ins.sarg << " #" << ins.iarg2; break;
                case OP::JMP:
                case OP::JMP_FALSE:
                case OP::JMP_TRUE:
                case OP::TRY_SETUP:  out << " @" << ins.iarg; break;
                case OP::BUILD_LIST:
                case OP::BUILD_MAP:  out << " #" << ins.iarg; break;
                default: break;
            }
            if (ins.line > 0) out << "   ; line " << ins.line;
            out << "\n";
        }
    }
}
