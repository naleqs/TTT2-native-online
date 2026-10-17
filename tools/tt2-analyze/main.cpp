// tt2-analyze: generates the XenonRecomp configuration for the player's own copy of
// Tekken Tag Tournament 2 (Default.xex). Nothing game specific is shipped in this
// repository; every address is found in the executable at setup time.
//
//   tt2-analyze <default.xex> <out_dir> [--explore]
//
// Writes:
//   <out_dir>/tt2_recomp.toml      main recompiler config (paths relative to out_dir)
//   <out_dir>/tt2_switch_tables.toml jump tables found by tt2's own detector
//   <out_dir>/tt2_analysis.json    summary consumed by the setup script and the runtime build
//
// XenonAnalyse's built-in jump table patterns were written for an older XDK compiler and
// find nothing in this game, so the detector here works backwards from every
// `mtctr; bctr` pair instead of matching fixed instruction sequences.

#include <file.h>
#include <image.h>
#include <disasm.h>
#include <xbox.h>
#include <function.h>

#include <fmt/core.h>
#include <fmt/os.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <xex.h>

namespace fs = std::filesystem;

// Ordinal -> "__imp__Name" tables defined in XenonUtils/xex.cpp
extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

namespace
{
    uint32_t load_be32(const void* p)
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return __builtin_bswap32(v);
    }

    struct CodeView
    {
        const Image& image;

        std::optional<uint32_t> word(size_t address) const
        {
            for (const auto& s : image.sections)
            {
                if (address >= s.base && address + 4 <= s.base + s.size)
                    return load_be32(s.data + (address - s.base));
            }
            return std::nullopt;
        }

        bool decode(size_t address, ppc_insn& insn) const
        {
            const auto* p = static_cast<const uint8_t*>(image.Find(address));
            if (p == nullptr)
                return false;
            ppc::Disassemble(p, address, insn);
            return insn.opcode != nullptr;
        }
    };

    // ---------------------------------------------------------------- register save/restore helpers

    struct HelperPattern
    {
        const char* key;
        std::vector<uint32_t> words;
    };

    // Byte patterns documented in the XenonRecomp README.
    const HelperPattern kHelpers[] = {
        { "restgprlr_14_address", { 0xE9C1FF68 } },
        { "savegprlr_14_address", { 0xF9C1FF68 } },
        { "restfpr_14_address",   { 0xC9CCFF70 } },
        { "savefpr_14_address",   { 0xD9CCFF70 } },
        { "restvmx_14_address",   { 0x3960FEE0, 0x7DCB60CE } },
        { "savevmx_14_address",   { 0x3960FEE0, 0x7DCB61CE } },
        { "restvmx_64_address",   { 0x3960FC00, 0x100B60CB } },
        { "savevmx_64_address",   { 0x3960FC00, 0x100B61CB } },
    };

    std::map<std::string, std::vector<size_t>> find_helpers(const Image& image)
    {
        std::map<std::string, std::vector<size_t>> found;
        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code))
                continue;

            for (size_t off = 0; off + 8 <= s.size; off += 4)
            {
                for (const auto& h : kHelpers)
                {
                    bool match = true;
                    for (size_t i = 0; i < h.words.size() && match; i++)
                        match = off + 4 * (i + 1) <= s.size && load_be32(s.data + off + 4 * i) == h.words[i];
                    if (match)
                        found[h.key].push_back(s.base + off);
                }
            }
        }
        return found;
    }

    // ---------------------------------------------------------------- jump tables

    enum class TableKind
    {
        Absolute,   // lwzx rX, table, idx*4 ; mtctr rX
        ByteOffset, // lbzx  + base (+ shift)
        HalfOffset, // lhzx  + base (+ shift)
    };

    struct JumpTable
    {
        size_t bctr{};
        size_t base{}; // address of the first instruction the recompiler reads (lis of the table)
        uint32_t index_reg{};
        size_t default_label{};
        std::vector<size_t> labels;
        TableKind kind{};
    };

    // Instruction records are walked backwards from the bctr; this is enough context for the
    // XDK 2010-2012 compilers in practice (the bounds check is never far from the dispatch).
    constexpr int kLookback = 48;

    struct Insn
    {
        size_t address;
        ppc_insn insn;
    };

    std::vector<Insn> window_before(const CodeView& code, size_t address, int count)
    {
        std::vector<Insn> out;
        for (int i = 1; i <= count; i++)
        {
            Insn rec{ address - 4 * i, {} };
            if (!code.decode(rec.address, rec.insn))
                break;
            out.push_back(rec);
        }
        return out; // nearest first
    }

    bool is(const ppc_insn& i, uint32_t id) { return i.opcode && i.opcode->id == id; }

    // True when the instruction writes general purpose register `reg` (its first operand).
    // Stores, compares, branches, moves to SPRs, traps, cache ops and nops write no GPR; a
    // `nop` (ori r0,r0,0) in particular must not count as a write to r0.
    bool writes_gpr(const ppc_insn& in, uint32_t reg)
    {
        if (!in.opcode || in.operands[0] != reg)
            return false;
        const std::string_view name = in.opcode->name;
        // Floating point and vector instructions name FPRs/VRs in that slot (lfs f31 is not r31).
        if (name.starts_with("f") || name.starts_with("lf") || name.starts_with("v") || name.starts_with("lv") ||
            name.starts_with("mffs") || name.starts_with("mtfs"))
            return false;
        return !(name == "nop" || name.starts_with("st") || name.starts_with("cmp") || name.starts_with("b") ||
                 name.starts_with("mt") || name.starts_with("tw") || name.starts_with("dcb") || name.starts_with("sync") ||
                 name.starts_with("eieio") || name.starts_with("isync"));
    }

    // Resolves `lis rA, hi ; addi rA, rA, lo` (or `ori`) that defines `reg` before position `from`
    // in the backwards window. Returns the address of the lis together with the value.
    std::optional<std::pair<size_t, uint32_t>> resolve_constant(const std::vector<Insn>& w, size_t from, uint32_t reg)
    {
        std::optional<uint32_t> lo;
        for (size_t k = from; k < w.size(); k++)
        {
            const auto& in = w[k].insn;
            if ((is(in, PPC_INST_ADDI) || is(in, PPC_INST_ORI)) && in.operands[0] == reg && in.operands[1] == reg && !lo)
            {
                lo = is(in, PPC_INST_ADDI) ? uint32_t(int32_t(int16_t(in.operands[2]))) : in.operands[2];
            }
            else if (is(in, PPC_INST_LIS) && in.operands[0] == reg)
            {
                return std::make_pair(w[k].address, (in.operands[1] << 16) + lo.value_or(0));
            }
            else if (writes_gpr(in, reg) && lo)
            {
                return std::nullopt; // redefined by something we don't understand
            }
        }
        return std::nullopt;
    }

    // Function containing an address, from .pdata: [start, end)
    using FunctionOf = std::function<std::optional<std::pair<size_t, size_t>>(size_t)>;

    std::optional<JumpTable> analyse_bctr(const CodeView& code, size_t bctr, const FunctionOf& function_of)
    {
        auto w = window_before(code, bctr, kLookback);
        if (w.empty() || !is(w[0].insn, PPC_INST_MTCTR))
            return std::nullopt;

        JumpTable t{};
        t.bctr = bctr;
        uint32_t target = w[0].insn.operands[0];

        size_t k = 1;
        std::optional<uint32_t> add_base_reg;
        uint32_t shift = 0;

        // Optional: add rT, rBase, rOff  (offset tables)
        if (k < w.size() && is(w[k].insn, PPC_INST_ADD) && w[k].insn.operands[0] == target)
        {
            add_base_reg = w[k].insn.operands[1];
            target = w[k].insn.operands[2];
            k++;
            // the base register's lis/addi may sit between here and the load
        }

        // Optional shift of the loaded offset: rlwinm rT, rT, sh, 0, 31-sh  (slwi)
        auto skip_to_def = [&](uint32_t reg) {
            while (k < w.size() && !writes_gpr(w[k].insn, reg))
                k++;
        };

        skip_to_def(target);
        if (k < w.size() && is(w[k].insn, PPC_INST_RLWINM) && w[k].insn.operands[0] == target && w[k].insn.operands[3] == 0)
        {
            shift = w[k].insn.operands[2];
            target = w[k].insn.operands[1];
            k++;
            skip_to_def(target);
        }

        if (k >= w.size())
            return std::nullopt;

        const auto& load = w[k].insn;
        uint32_t table_reg, index_reg;
        if (is(load, PPC_INST_LWZX) && !add_base_reg)
            t.kind = TableKind::Absolute;
        else if (is(load, PPC_INST_LBZX) && add_base_reg)
            t.kind = TableKind::ByteOffset;
        else if (is(load, PPC_INST_LHZX) && add_base_reg)
            t.kind = TableKind::HalfOffset;
        else
            return std::nullopt;

        table_reg = load.operands[1];
        index_reg = load.operands[2];
        const size_t load_pos = k;

        auto table = resolve_constant(w, load_pos + 1, table_reg);
        if (!table)
        {
            // operands may be swapped (lwzx rT, idx, table)
            std::swap(table_reg, index_reg);
            table = resolve_constant(w, load_pos + 1, table_reg);
            if (!table)
                return std::nullopt;
        }

        // The index register is usually scaled from the switch variable: rlwinm rI, rV, 2, 0, 29
        uint32_t switch_reg = index_reg;
        for (size_t j = load_pos + 1; j < w.size(); j++)
        {
            const auto& in = w[j].insn;
            if (in.operands[0] != switch_reg)
                continue;
            if (is(in, PPC_INST_RLWINM) || is(in, PPC_INST_EXTSW) || is(in, PPC_INST_CLRLWI))
                switch_reg = in.operands[1];
            break;
        }

        // Bounds check: cmplwi crN, rV, max ; bgt crN, default
        std::optional<uint32_t> cr;
        size_t count = 0;
        size_t cmp_pos = 0;
        for (size_t j = 0; j < w.size(); j++)
        {
            const auto& in = w[j].insn;
            if (!cr && (is(in, PPC_INST_BGT) || is(in, PPC_INST_BLE) || is(in, PPC_INST_BGTLR) || is(in, PPC_INST_BLELR)))
            {
                cr = in.operands[0];
                if (in.opcode->operands[1] != 0)
                    t.default_label = in.operands[1];
            }
            else if (cr && is(in, PPC_INST_CMPLWI) && in.operands[0] == *cr)
            {
                switch_reg = in.operands[1];
                count = size_t(in.operands[2]) + 1;
                cmp_pos = j;
                break;
            }
        }
        if (count > 4096)
            return std::nullopt;

        if (count > 0)
        {
            // The recompiler emits `switch (rV)` at the bctr, so rV must still hold the case
            // index there. A write between the compare and the bctr is only accepted when it
            // recomputes the same value (the compiler sometimes repeats `addi rV, rX, -base`).
            uint32_t original = 0;
            for (size_t j = cmp_pos + 1; j < w.size(); j++)
                if (writes_gpr(w[j].insn, switch_reg))
                {
                    original = w[j].insn.instruction;
                    break;
                }
            for (size_t j = 0; j < cmp_pos; j++)
                if (writes_gpr(w[j].insn, switch_reg) && w[j].insn.instruction != original)
                    return std::nullopt;
        }

        std::optional<uint32_t> add_base;
        if (add_base_reg)
        {
            auto b = resolve_constant(w, 1, *add_base_reg);
            if (!b)
                return std::nullopt;
            add_base = b->second;
        }

        auto entry_at = [&](size_t i) -> std::optional<size_t> {
            switch (t.kind)
            {
            case TableKind::Absolute:
                if (auto v = code.word(table->second + 4 * uint32_t(i)))
                    return size_t(*v);
                return std::nullopt;
            case TableKind::ByteOffset:
                if (const auto* p = static_cast<const uint8_t*>(code.image.Find(table->second + i)))
                    return *add_base + (size_t(*p) << shift);
                return std::nullopt;
            case TableKind::HalfOffset:
                if (const auto* p = static_cast<const uint8_t*>(code.image.Find(table->second + 2 * i)))
                    return *add_base + (size_t(uint16_t((p[0] << 8) | p[1])) << shift);
                return std::nullopt;
            }
            return std::nullopt;
        };

        if (count == 0)
        {
            // No bounds check in reach (the compiler proved the range). A byte-sized index
            // (loaded with lbz) has exactly 256 cases; otherwise size the table by its entries,
            // which must all land inside the function that contains the dispatch. Extra
            // entries are harmless; too few would hit the recompiler's unreachable default.
            bool byte_index = false;
            for (size_t j = 1; j < w.size(); j++)
                if (writes_gpr(w[j].insn, switch_reg))
                {
                    byte_index = is(w[j].insn, PPC_INST_LBZ) || is(w[j].insn, PPC_INST_LBZX) || is(w[j].insn, PPC_INST_CLRLWI);
                    break;
                }
            const auto range = function_of ? function_of(bctr) : std::nullopt;
            if (!range)
                return std::nullopt;
            const size_t limit = byte_index ? 256 : 512;
            while (count < limit)
            {
                const auto e = entry_at(count);
                if (!e || *e < range->first || *e >= range->second || (*e & 3))
                    break;
                count++;
            }
            if (count < 2)
                return std::nullopt;
        }

        t.index_reg = switch_reg;
        // The recompiler identifies the switch by the address of its dispatch sequence start.
        t.base = std::min(table->first, add_base ? resolve_constant(w, 1, *add_base_reg)->first : table->first);
        t.labels.resize(count);
        for (size_t i = 0; i < count; i++)
        {
            const auto e = entry_at(i);
            if (!e)
                return std::nullopt;
            t.labels[i] = *e;
        }

        // Sanity: every label must be code near the dispatch.
        for (size_t l : t.labels)
        {
            ppc_insn tmp;
            if (!code.decode(l, tmp) || (l > bctr ? l - bctr : bctr - l) > 0x10000)
                return std::nullopt;
        }
        return t;
    }

    struct BctrScan
    {
        std::vector<JumpTable> tables;
        std::vector<size_t> unresolved;
    };

    BctrScan scan_jump_tables(const Image& image)
    {
        CodeView code{ image };
        BctrScan out;

        std::map<size_t, size_t> pdata; // start -> end
        if (const Section* p = image.Find(".pdata"))
            for (size_t i = 0; i < p->size / 8; i++)
            {
                const uint32_t begin = load_be32(p->data + i * 8);
                const uint32_t length = ((load_be32(p->data + i * 8 + 4) >> 8) & 0x3FFFFF) * 4;
                if (begin && length)
                    pdata[begin] = begin + length;
            }
        const FunctionOf function_of = [&](size_t address) -> std::optional<std::pair<size_t, size_t>> {
            auto it = pdata.upper_bound(address);
            if (it == pdata.begin())
                return std::nullopt;
            --it;
            if (address >= it->second)
                return std::nullopt;
            return std::make_pair(it->first, it->second);
        };
        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code))
                continue;
            for (size_t off = 0; off + 4 <= s.size; off += 4)
            {
                if (load_be32(s.data + off) != 0x4E800420) // bctr
                    continue;
                const size_t addr = s.base + off;
                ppc_insn prev;
                if (!code.decode(addr - 4, prev) || !is(prev, PPC_INST_MTCTR))
                    continue; // virtual/indirect calls through ctr loaded elsewhere
                if (auto t = analyse_bctr(code, addr, function_of))
                    out.tables.push_back(std::move(*t));
                else
                    out.unresolved.push_back(addr);
            }
        }
        return out;
    }

    // ---------------------------------------------------------------- function boundaries

    struct FunctionRange
    {
        size_t base;
        size_t size;
    };

    // Mirrors XenonRecomp's own discovery (.pdata, then bl targets, then a linear sweep) so we
    // know which function the recompiler will put each jump table in.
    std::vector<FunctionRange> discover_functions(const Image& image, std::vector<size_t>& pdata_starts)
    {
        std::map<size_t, size_t> fns;
        const Section* pdata = image.Find(".pdata");
        if (pdata)
        {
            const size_t count = pdata->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION);
            for (size_t i = 0; i < count; i++)
            {
                const uint32_t begin = load_be32(pdata->data + i * 8);
                const uint32_t data = load_be32(pdata->data + i * 8 + 4);
                if (begin == 0)
                    continue;
                const size_t length = ((data >> 8) & 0x3FFFFF) * 4;
                fns.emplace(begin, length);
                pdata_starts.push_back(begin);
            }
        }
        std::sort(pdata_starts.begin(), pdata_starts.end());

        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code))
                continue;

            for (size_t off = 0; off + 4 <= s.size; off += 4)
            {
                const uint32_t w = load_be32(s.data + off);
                if ((w >> 26) != 18 || (w & 3) != 1) // bl
                    continue;
                const int32_t li = int32_t(w << 6) >> 6 & ~3;
                const size_t target = s.base + off + li;
                if (target >= s.base && target < s.base + s.size && !fns.contains(target))
                {
                    auto fn = Function::Analyze(s.data + (target - s.base), s.base + s.size - target, target);
                    fns.emplace(fn.base, fn.size);
                }
            }

            size_t addr = s.base;
            while (addr < s.base + s.size)
            {
                if (auto it = fns.find(addr); it != fns.end() && it->second != 0)
                {
                    addr += it->second;
                    continue;
                }
                if (load_be32(s.data + (addr - s.base)) == 0) // padding (invalid_instructions)
                {
                    addr += 4;
                    continue;
                }
                auto fn = Function::Analyze(s.data + (addr - s.base), s.base + s.size - addr, addr);
                fns[fn.base] = fn.size;
                addr += std::max<size_t>(fn.size, 4);
            }
        }

        std::vector<FunctionRange> out;
        for (auto [b, sz] : fns)
            out.push_back({ b, sz });
        return out;
    }

    // Control-flow extent of a function, following switch labels (which Function::Analyze cannot
    // see: it takes the dispatch `bctr` for a tail call and ends the function there).
    size_t follow_extent(const Image& image, size_t start, size_t limit, const std::map<size_t, const JumpTable*>& tables)
    {
        std::set<size_t> visited;
        std::vector<size_t> work{ start };
        size_t end = start;

        while (!work.empty())
        {
            size_t addr = work.back();
            work.pop_back();
            for (; addr < limit; addr += 4)
            {
                if (!visited.insert(addr).second)
                    break;
                const auto* p = static_cast<const uint8_t*>(image.Find(addr));
                if (!p)
                    break;
                end = std::max(end, addr + 4);
                const uint32_t w = load_be32(p);
                const uint32_t op = w >> 26;
                const bool lk = w & 1;

                if (op == 18) // b / bl
                {
                    if (lk)
                        continue;
                    const int32_t li = int32_t(w << 6) >> 6 & ~3;
                    const size_t target = (w & 2) ? size_t(uint32_t(li)) : addr + li;
                    if (target >= start && target < limit)
                        work.push_back(target);
                    break;
                }
                if (op == 16) // bc
                {
                    if (lk)
                        continue;
                    const int32_t bd = int16_t(w & 0xFFFC);
                    const size_t target = (w & 2) ? size_t(uint32_t(bd)) : addr + bd;
                    if (target >= start && target < limit)
                        work.push_back(target);
                    if ((((w >> 21) & 0x1F) & 0x14) == 0x14)
                        break;
                    continue;
                }
                if (op == 19)
                {
                    const uint32_t xo = (w >> 1) & 0x3FF;
                    const bool always = (((w >> 21) & 0x1F) & 0x14) == 0x14;
                    if ((xo == 16 || xo == 528) && !lk)
                    {
                        if (xo == 528)
                        {
                            if (auto t = tables.find(addr); t != tables.end())
                            {
                                for (size_t l : t->second->labels)
                                    if (l >= start && l < limit)
                                        work.push_back(l);
                            }
                        }
                        if (always)
                            break;
                    }
                }
            }
        }
        return end;
    }

    std::vector<FunctionRange> fix_switch_functions(const Image& image, const std::vector<JumpTable>& tables)
    {
        std::vector<size_t> pdata_starts;
        const auto fns = discover_functions(image, pdata_starts);

        std::map<size_t, const JumpTable*> by_bctr;
        for (const auto& t : tables)
            by_bctr[t.bctr] = &t;

        auto containing = [&](size_t addr) -> const FunctionRange* {
            auto it = std::upper_bound(fns.begin(), fns.end(), addr, [](size_t a, const FunctionRange& f) { return a < f.base; });
            return it == fns.begin() ? nullptr : &*std::prev(it);
        };

        std::map<size_t, size_t> fixes;
        for (const auto& t : tables)
        {
            const FunctionRange* fn = containing(t.bctr);
            if (!fn)
                continue;
            const auto [lo, hi] = std::minmax_element(t.labels.begin(), t.labels.end());
            if (*lo >= fn->base && *hi < fn->base + fn->size)
                continue; // already inside

            // Function::Analyze ends a function at a switch dispatch, and the linear sweep then
            // starts a fragment right after it. A label below the fragment start means the real
            // function begins earlier: walk back to the fragment holding the lowest label.
            size_t start = fn->base;
            for (int guard = 0; guard < 64 && *lo < start; guard++)
            {
                const FunctionRange* prev = containing(*lo);
                if (!prev || prev->base >= start)
                    break;
                start = prev->base;
            }
            if (*lo < start)
                continue;

            // Never grow past the next function that has unwind data.
            size_t limit = start + 0x100000;
            if (auto p = std::upper_bound(pdata_starts.begin(), pdata_starts.end(), start); p != pdata_starts.end())
                limit = std::min(limit, *p);
            if (const Section* sec = image.Find(".text"); sec)
                limit = std::min(limit, sec->base + sec->size);

            const size_t end = follow_extent(image, start, limit, by_bctr);
            if (end <= t.bctr || end <= *hi)
                continue; // the dispatch is not reachable from this start; leave it to the recompiler's warning
            fixes[start] = std::max(fixes[start], end - start);
        }

        // The other switch idiom of this compiler: `mtctr rV` followed by a chain of
        // `bdzf eq, case_N` that counts CTR down. Function::Analyze stops early there too, so
        // grow any function whose forward conditional branches land past its end.
        for (size_t i = 0; i < fns.size(); i++)
        {
            const auto& fn = fns[i];
            size_t limit = fn.base + 0x10000;
            if (auto p = std::upper_bound(pdata_starts.begin(), pdata_starts.end(), fn.base); p != pdata_starts.end())
                limit = std::min(limit, *p);

            bool escapes = false;
            for (size_t a = fn.base; a < fn.base + fn.size && !escapes; a += 4)
            {
                const auto* p = static_cast<const uint8_t*>(image.Find(a));
                if (!p)
                    break;
                const uint32_t w = load_be32(p);
                if ((w >> 26) != 16 || (w & 3) != 0)
                    continue;
                const uint32_t bo = (w >> 21) & 0x1F;
                if (bo & 0x04)
                    continue; // only CTR-decrementing branches (bdz/bdnz forms)
                const size_t target = a + int16_t(w & 0xFFFC);
                escapes = target >= fn.base + fn.size && target < limit;
            }
            if (!escapes)
                continue;

            const size_t end = follow_extent(image, fn.base, limit, by_bctr);
            if (end > fn.base + fn.size)
                fixes[fn.base] = std::max(fixes[fn.base], end - fn.base);
        }

        // Fragments inside an enlarged function are covered by it.
        std::vector<FunctionRange> out;
        for (auto [b, sz] : fixes)
        {
            if (!out.empty() && b < out.back().base + out.back().size)
            {
                out.back().size = std::max(out.back().size, b + sz - out.back().base);
                continue;
            }
            out.push_back({ b, sz });
        }
        return out;
    }

    void explore_site(const CodeView& code, size_t addr)
    {
        fmt::println("---- {:08X}", addr);
        auto w = window_before(code, addr + 4, 20);
        std::reverse(w.begin(), w.end());
        for (const auto& r : w)
            fmt::println("  {:08X}  {:<8} {}", r.address, r.insn.opcode->name, r.insn.op_str);
    }

    std::string hex(size_t v) { return fmt::format("0x{:X}", v); }

    // ---------------------------------------------------------------- runtime data

    struct VariableImport
    {
        std::string module;
        uint32_t ordinal;
        std::string name;
        uint32_t slot;
    };

    // Data imports (type 0 records). Image::ParseImage replaces function thunks with stubs and
    // byte-swaps the remaining records in place; the runtime fills these slots at startup.
    std::vector<VariableImport> variable_imports(const std::vector<uint8_t>& xex, Image& image)
    {
        std::vector<VariableImport> out;
        auto* imports = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(xex.data(), XEX_HEADER_IMPORT_LIBRARIES));
        if (!imports)
            return out;

        std::vector<std::string> names;
        const char* strings = reinterpret_cast<const char*>(imports + 1);
        size_t off = 0;
        for (uint32_t i = 0; i < imports->numImports; i++)
        {
            names.emplace_back(strings + off);
            off += ((names.back().size() + 1) + 3) & ~size_t(3);
        }

        auto* lib = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const char*>(imports) + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
        for (uint32_t l = 0; l < imports->numImports; l++)
        {
            const uint16_t name_index = lib->name;
            const std::string& module = names[name_index < names.size() ? name_index : 0];
            const auto* table = module == "xam.xex" ? &XamExports : &XboxKernelExports;
            auto* desc = reinterpret_cast<const Xex2ImportDescriptor*>(lib + 1);
            for (uint32_t i = 0; i < lib->numberOfImports; i++)
            {
                const uint32_t slot = desc[i].firstThunk;
                auto* p = static_cast<uint8_t*>(const_cast<void*>(image.Find(slot)));
                if (!p)
                    continue;
                uint32_t v;
                std::memcpy(&v, p, 4); // host order after ParseImage's in-place swap
                if (v == 0x00000060)
                    continue; // function thunk (now a stub the recompiler turned into a call)
                const uint32_t type = v >> 24;
                const uint32_t ordinal = v & 0xFFFF;
                if (type != 0)
                    continue;
                auto it = table->find(ordinal);
                std::string name = it != table->end() ? std::string(it->second) : fmt::format("ordinal_{}", ordinal);
                if (name.starts_with("__imp__"))
                    name = name.substr(7);
                out.push_back({ module, ordinal, name, slot });
                std::memset(p, 0, 4);
            }
            lib = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const char*>(lib + 1) + lib->numberOfImports * sizeof(Xex2ImportDescriptor));
        }
        return out;
    }

    void write_runtime_data(const std::vector<uint8_t>& xex, Image& image, const fs::path& out_dir)
    {
        const auto vars = variable_imports(xex, image);

        uint32_t stack_size = 0x40000;
        if (auto* p = getOptHeaderPtr(xex.data(), XEX_HEADER_DEFAULT_STACK_SIZE))
            stack_size = load_be32(p); // value stored inline in the header entry
        uint32_t tls_slots = 0, tls_raw = 0, tls_data_size = 0, tls_raw_size = 0;
        if (auto* p = static_cast<const uint8_t*>(getOptHeaderPtr(xex.data(), XEX_HEADER_TLS_INFO)))
        {
            tls_slots = load_be32(p);
            tls_raw = load_be32(p + 4);
            tls_data_size = load_be32(p + 8);
            tls_raw_size = load_be32(p + 12);
        }

        {
            // Binary mode: a text-mode stream would turn every 0x0A byte into CR LF on Windows.
            std::ofstream bin(out_dir / "tt2_image.bin", std::ios::binary | std::ios::trunc);
            bin.write(reinterpret_cast<const char*>(image.data.get()), image.size);
        }

        auto f = fmt::output_file((out_dir / "tt2_image.json").string());
        f.print("{{\n  \"base\": {},\n  \"size\": {},\n  \"entry\": {},\n  \"stack_size\": {},\n", image.base, image.size, image.entry_point, stack_size);
        f.print("  \"tls\": {{ \"slots\": {}, \"raw_address\": {}, \"data_size\": {}, \"raw_size\": {} }},\n", tls_slots, tls_raw, tls_data_size, tls_raw_size);
        // Optional headers for RtlImageXexHeaderField: inline values, or the data blob.
        f.print("  \"xex_headers\": [\n");
        {
            const auto* header = reinterpret_cast<const Xex2Header*>(xex.data());
            const auto* opt = reinterpret_cast<const Xex2OptHeader*>(header + 1);
            for (uint32_t i = 0; i < header->headerCount; i++)
            {
                const uint32_t key = opt[i].key;
                f.print("    {{ \"key\": {}", key);
                if ((key & 0xFF) == 0 || (key & 0xFF) == 1)
                    f.print(", \"value\": {}", uint32_t(opt[i].value));
                else
                {
                    const uint32_t offset = opt[i].offset;
                    const uint32_t size = (key & 0xFF) == 0xFF ? load_be32(xex.data() + offset) : (key & 0xFF) * 4;
                    f.print(", \"data\": \"");
                    for (uint32_t b = 0; b < size && offset + b < xex.size(); b++)
                        f.print("{:02x}", xex[offset + b]);
                    f.print("\"");
                }
                f.print(" }}{}\n", i + 1 < header->headerCount ? "," : "");
            }
        }
        f.print("  ],\n");
        f.print("  \"variable_imports\": [\n");
        for (size_t i = 0; i < vars.size(); i++)
            f.print("    {{ \"module\": \"{}\", \"ordinal\": {}, \"name\": \"{}\", \"slot\": {} }}{}\n", vars[i].module, vars[i].ordinal, vars[i].name, vars[i].slot, i + 1 < vars.size() ? "," : "");
        f.print("  ]\n}}\n");
        fmt::println("runtime data: image {:X} bytes, {} variable imports, stack {:X}, tls slots {}", image.size, vars.size(), stack_size, tls_slots);
    }
}


int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fmt::println("usage: tt2-analyze <default.xex> <out_dir> [--explore N]");
        return 2;
    }

    const fs::path xex_path = argv[1];
    const fs::path out_dir = argv[2];
    int explore = 0;
    size_t explore_at = 0;
    for (int i = 3; i + 1 < argc; i++)
    {
        if (std::string(argv[i]) == "--explore")
            explore = std::atoi(argv[i + 1]);
        if (std::string(argv[i]) == "--explore-at") // disassemble around one address
            explore_at = std::strtoul(argv[i + 1], nullptr, 16);
    }

    const auto file = LoadFile(xex_path.string().c_str());
    if (file.empty())
    {
        fmt::println(stderr, "error: cannot read {}", xex_path.string());
        return 1;
    }

    Image image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr)
    {
        fmt::println(stderr, "error: {} is not a valid XEX", xex_path.string());
        return 1;
    }

    fmt::println("image base {:08X} size {:X} entry {:08X}", image.base, image.size, image.entry_point);
    for (const auto& s : image.sections)
        fmt::println("  section {:<8} {:08X} +{:X} {}", s.name, s.base, s.size, (s.flags & SectionFlags_Code) ? "code" : "data");

    if (explore_at)
    {
        explore_site(CodeView{ image }, explore_at);
        return 0;
    }

    const auto helpers = find_helpers(image);
    std::map<std::string, size_t> chosen;
    bool helpers_ok = true;
    for (const auto& h : kHelpers)
    {
        auto it = helpers.find(h.key);
        if (it == helpers.end())
        {
            fmt::println(stderr, "error: {} not found", h.key);
            helpers_ok = false;
            continue;
        }
        if (it->second.size() > 1)
            fmt::println("warning: {} has {} matches, using the first", h.key, it->second.size());
        chosen[h.key] = it->second.front();
        fmt::println("  {:<22} {:08X}", h.key, it->second.front());
    }

    const auto scan = scan_jump_tables(image);
    fmt::println("jump tables: {} resolved, {} unresolved mtctr/bctr sites", scan.tables.size(), scan.unresolved.size());

    if (explore > 0)
    {
        CodeView code{ image };
        // Unresolved dispatches whose target register was not loaded with a plain
        // `lwz rT, d(rA)` (vtable / function pointer) may be switches we do not recognise.
        std::vector<size_t> suspicious;
        for (size_t site : scan.unresolved)
        {
            auto w = window_before(code, site, 12);
            if (w.empty())
                continue;
            const uint32_t reg = w[0].insn.operands[0];
            for (size_t k = 1; k < w.size(); k++)
            {
                if (!writes_gpr(w[k].insn, reg))
                    continue;
                if (!is(w[k].insn, PPC_INST_LWZ))
                    suspicious.push_back(site);
                break;
            }
        }
        fmt::println("unresolved: {} total, {} not loaded from memory (possible switches)", scan.unresolved.size(), suspicious.size());
        for (int i = 0; i < explore && i < int(suspicious.size()); i++)
            explore_site(code, suspicious[size_t(i)]);
        return 0;
    }

    if (explore < 0)
    {
        CodeView code{ image };
        for (int i = 0; i < explore && i < int(scan.unresolved.size()); i++)
            explore_site(code, scan.unresolved[size_t(i) * scan.unresolved.size() / size_t(explore)]);
        for (int i = 0; i < 3 && i < int(scan.tables.size()); i++)
            explore_site(code, scan.tables[size_t(i)].bctr);
        return 0;
    }

    if (!helpers_ok)
        return 1;

    fs::create_directories(out_dir);

    {
        auto f = fmt::output_file((out_dir / "tt2_switch_tables.toml").string());
        f.print("# Generated by tt2-analyze from the player's own Default.xex. Do not commit.\n\n");
        for (const auto& t : scan.tables)
        {
            f.print("[[switch]]\nbase = {}\nr = {}\ndefault = {}\nlabels = [\n", hex(t.base), t.index_reg, hex(t.default_label));
            for (size_t l : t.labels)
                f.print("    {},\n", hex(l));
            f.print("]\n\n");
        }
    }

    {
        auto f = fmt::output_file((out_dir / "tt2_recomp.toml").string());
        f.print("# Generated by tt2-analyze. Paths are relative to this file.\n[main]\n");
        f.print("file_path = \"default.xex\"\n");
        f.print("out_directory_path = \"ppc\"\n");
        f.print("switch_table_file_path = \"tt2_switch_tables.toml\"\n\n");
        // Optimisations stay off until the recompilation is known to run correctly.
        f.print("skip_lr = false\nskip_msr = false\nctr_as_local = false\nxer_as_local = false\n");
        f.print("reserved_as_local = false\ncr_as_local = false\nnon_argument_as_local = false\nnon_volatile_as_local = false\n\n");
        for (const auto& [k, v] : chosen)
            f.print("{} = {}\n", k, hex(v));
        f.print("\n");

        const auto fixes = fix_switch_functions(image, scan.tables);
        fmt::println("function boundary fixes for jump tables: {}", fixes.size());
        f.print("functions = [\n");
        for (const auto& fn : fixes)
            f.print("    {{ address = {}, size = {} }},\n", hex(fn.base), hex(fn.size));
        f.print("]\n\n");
        // Hand-maintained additions (mid-asm hooks for the frame boundary and input, function
        // boundary fixes) are appended by the setup script from config/recomp/tt2_hooks.toml.
    }

    write_runtime_data(file, image, out_dir);

    {
        auto f = fmt::output_file((out_dir / "tt2_analysis.json").string());
        f.print("{{\n  \"image_base\": {},\n  \"image_size\": {},\n  \"entry_point\": {},\n", image.base, image.size, image.entry_point);
        f.print("  \"jump_tables\": {},\n  \"unresolved_bctr\": {}\n}}\n", scan.tables.size(), scan.unresolved.size());
    }

    fmt::println("wrote {}", out_dir.string());
    return 0;
}
