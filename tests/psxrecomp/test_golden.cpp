// Golden test: compare our disassembly of the game's main text against GNU objdump (mips:3000).
//
// Requires two git-ignored files under game/work/ (the test is skipped when either is missing):
//   main.bin          raw 0xE0000-byte text of the exe, loaded at 0x80010000
//   main.objdump.txt  mips64-linux-gnu-objdump -D -z -b binary -m mips:3000 -EL --adjust-vma=0x80010000 main.bin
//
// Words objdump itself rejects (".word") or decodes as a non-R3000 instruction (COP1/COP3, LWC0, jalx, ...) are
// "non-R3000" and are not compared textually: the only requirement is that we do not mis-handle valid words.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "psxrecomp/mips.h"

using namespace psxrecomp;

#ifndef DW3_GAME_DIR
#define DW3_GAME_DIR "game"
#endif

namespace {

constexpr uint32_t kBase = 0x80010000;

// Mnemonics objdump may print for words that are valid R3000 instructions (including its pseudo-ops).
const std::set<std::string>& r3000_mnemonics() {
    static const std::set<std::string> s = {
        "sll", "srl", "sra", "sllv", "srlv", "srav", "jr", "jalr", "syscall", "break", "mfhi", "mthi", "mflo", "mtlo",
        "mult", "multu", "div", "divu", "add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu",
        "bltz", "bgez", "bltzal", "bgezal", "j", "jal", "beq", "bne", "blez", "bgtz", "addi", "addiu", "slti",
        "sltiu", "andi", "ori", "xori", "lui", "mfc0", "mtc0", "rfe", "mfc2", "cfc2", "mtc2", "ctc2", "c2", "lb",
        "lh", "lwl", "lw", "lbu", "lhu", "lwr", "sb", "sh", "swl", "sw", "swr", "lwc2", "swc2",
        "nop", "ssnop", "ehb", "move", "li", "neg", "negu", "b", "bal", "beqz", "bnez",
    };
    return s;
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

struct GoldenLine {
    uint32_t addr = 0;
    uint32_t word = 0;
    std::string text;  // "mnemonic operands" with a single space
};

// Parses "80010000:\t80011168 \tlb\tat,4456(zero)".
bool parse_line(const std::string& line, GoldenLine& g) {
    unsigned addr = 0, word = 0;
    int n = 0;
    if (std::sscanf(line.c_str(), "%x:\t%x %n", &addr, &word, &n) != 2 || n == 0) return false;
    size_t p = line.find('\t', static_cast<size_t>(n) - 1);
    if (p == std::string::npos) return false;
    std::string rest = line.substr(p + 1);
    size_t t = rest.find('\t');
    if (t != std::string::npos) rest[t] = ' ';
    // Strip trailing comments / symbolic annotations if objdump ever adds them.
    size_t c = rest.find("\t#");
    if (c != std::string::npos) rest.resize(c);
    while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\r')) rest.pop_back();
    g.addr = addr;
    g.word = word;
    g.text = rest;
    return true;
}

}  // namespace

TEST(Golden, MainBinMatchesObjdump) {
    std::string dir = DW3_GAME_DIR;
    std::string bin, dump;
    if (!read_file(dir + "/work/main.bin", bin)) GTEST_SKIP() << "game/work/main.bin not present";
    if (!read_file(dir + "/work/main.objdump.txt", dump)) GTEST_SKIP() << "game/work/main.objdump.txt not present";
    ASSERT_EQ(bin.size() % 4, 0u);

    std::vector<GoldenLine> lines;
    std::istringstream ss(dump);
    std::string line;
    while (std::getline(ss, line)) {
        GoldenLine g;
        if (parse_line(line, g)) lines.push_back(g);
    }
    ASSERT_EQ(lines.size(), bin.size() / 4) << "objdump line count != word count (was it run with -z?)";

    size_t match = 0, both_invalid = 0, lenient = 0, mismatch = 0;
    std::map<std::string, size_t> lenient_by_mnemonic;
    std::map<std::string, size_t> mismatch_by_mnemonic;
    std::vector<std::string> samples;

    for (size_t i = 0; i < lines.size(); ++i) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(bin.data()) + i * 4;
        uint32_t word = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24);
        uint32_t pc = kBase + static_cast<uint32_t>(i) * 4;
        const GoldenLine& g = lines[i];
        ASSERT_EQ(g.addr, pc);
        ASSERT_EQ(g.word, word);

        std::string mn = g.text.substr(0, g.text.find(' '));
        bool objdump_valid = r3000_mnemonics().count(mn) != 0;
        Instruction in = decode(word, pc);
        std::string ours = disassemble(in);

        if (objdump_valid) {
            if (ours == g.text) {
                ++match;
            } else {
                ++mismatch;
                ++mismatch_by_mnemonic[mn];
                if (samples.size() < 40) {
                    char buf[256];
                    std::snprintf(buf, sizeof buf, "%08x: %08x objdump='%s' ours='%s'", pc, word, g.text.c_str(),
                                  ours.c_str());
                    samples.push_back(buf);
                }
            }
        } else if (in.op == Op::Invalid) {
            ++both_invalid;
        } else {
            // objdump rejects (reserved bits set, or a non-R3000 form) but we decode leniently, like the CPU.
            ++lenient;
            ++lenient_by_mnemonic[mn];
        }
    }

    size_t total = lines.size();
    size_t comparable = match + mismatch;
    std::printf("[golden] words=%zu comparable(valid in objdump)=%zu match=%zu mismatch=%zu (%.4f%%)\n", total,
                comparable, match, mismatch, comparable ? 100.0 * match / comparable : 0.0);
    std::printf("[golden] both-invalid=%zu lenient-decode=%zu\n", both_invalid, lenient);
    for (auto& [m, n] : lenient_by_mnemonic) std::printf("[golden]   lenient (objdump '%s'): %zu\n", m.c_str(), n);
    for (auto& [m, n] : mismatch_by_mnemonic) std::printf("[golden]   MISMATCH (objdump '%s'): %zu\n", m.c_str(), n);
    for (auto& s : samples) std::printf("[golden]   %s\n", s.c_str());

    EXPECT_EQ(mismatch, 0u);
    EXPECT_GT(match, 100000u);
}
