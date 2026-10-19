#include "shader_translator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

// Field layouts and instruction semantics: Xenia's src/xenia/gpu/ucode.h (BSD-3-Clause).

namespace tt2::gpu
{
    namespace
    {
        constexpr uint32_t bits(uint64_t v, uint32_t lo, uint32_t n) { return uint32_t((v >> lo) & ((1ull << n) - 1)); }
        constexpr int32_t sbits(uint64_t v, uint32_t lo, uint32_t n)
        {
            const uint32_t u = bits(v, lo, n);
            return int32_t(u << (32 - n)) >> (32 - n);
        }

        const char kPrelude[] = R"(cbuffer FloatConsts : register(b0) { float4 c[256]; };
cbuffer IntConsts : register(b1) { uint4 bools[2]; uint4 loops[8]; };
cbuffer DrawConsts : register(b2)
{
    uint4 vfetch[48];  // per vf#: byte offset of its data in vb, endian
    float4 ndc_scale;
    float4 ndc_offset;
    uint4 draw_flags;  // x: bit 0 xy pre-divided, bit 1 z pre-divided, bit 2 the shader wrote 1/w
                       // y: pixel shader parameter register + 1 (0: none)
                       // z: alpha test function | enable << 3
    float4 alpha_ref;  // x
    uint4 tex_swizzle[8]; // per tf#: 3 bits per component, 0-3 sampled channel, 4 zero, 5 one
    float4 point_size; // xy: half size in pixels, zw: 2 / render target size
};

float4 apply_swizzle(float4 v, uint s)
{
    float4 r;
    [unroll] for (uint i = 0; i < 4; i++)
    {
        uint c = (s >> (i * 3)) & 7;
        r[i] = c < 4 ? v[c] : (c == 5 ? 1.0f : 0.0f);
    }
    return r;
}
uint tex_swz(uint slot) { return tex_swizzle[slot >> 2][slot & 3]; }
bool alpha_test(float a)
{
    uint f = draw_flags.z;
    if (!(f & 8u)) return true;
    float r = alpha_ref.x;
    switch (f & 7u)
    {
    case 0: return false;
    case 1: return a < r;
    case 2: return a == r;
    case 3: return a <= r;
    case 4: return a > r;
    case 5: return a != r;
    case 6: return a >= r;
    default: return true;
    }
}

bool bool_const(uint i) { return ((bools[i >> 7][(i >> 5) & 3] >> (i & 31)) & 1) != 0; }
uint loop_const(uint i) { return loops[i >> 2][i & 3]; }

// Direct3D 9 multiplication: 0 * anything (including Inf and NaN) is +0.
float4 lmul(float4 a, float4 b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }
float lmul(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }
float ldp4(float4 a, float4 b) { float4 p = lmul(a, b); return p.x + p.y + p.z + p.w; }
float ldp3(float4 a, float4 b) { float4 p = lmul(a, b); return p.x + p.y + p.z; }
float ldp2add(float4 a, float4 b, float4 c) { return lmul(a.x, b.x) + lmul(a.y, b.y) + c.x; }
float4 vmax(float4 a, float4 b) { return a >= b ? a : b; }
float4 vmin(float4 a, float4 b) { return a < b ? a : b; }
float vmax4(float4 a)
{
    if (a.x > a.y && a.x > a.z && a.x > a.w) return a.x;
    if (a.y > a.z && a.y > a.w) return a.y;
    if (a.z > a.w) return a.z;
    return a.w;
}
float4 vcube(float4 s0)
{
    float x = s0.z, y = s0.w, z = s0.x;
    float tc, sc, ma, id;
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) { tc = -y; sc = z < 0.0f ? -x : x; ma = 2.0f * z; id = z < 0.0f ? 5.0f : 4.0f; }
    else if (abs(y) >= abs(x)) { tc = y < 0.0f ? -z : z; sc = x; ma = 2.0f * y; id = y < 0.0f ? 3.0f : 2.0f; }
    else { tc = -y; sc = x < 0.0f ? z : -z; ma = 2.0f * x; id = x < 0.0f ? 1.0f : 0.0f; }
    return float4(tc, sc, ma, id);
}
// Xenos cube coordinates (s and t in [1, 2], face index) to a direction.
float3 cube_dir(float3 c)
{
    float sc = (c.x - 1.5f) * 2.0f, tc = (c.y - 1.5f) * 2.0f;
    uint face = uint(c.z) % 6;
    if (face == 0) return float3(1.0f, -tc, -sc);
    if (face == 1) return float3(-1.0f, -tc, sc);
    if (face == 2) return float3(sc, 1.0f, tc);
    if (face == 3) return float3(sc, -1.0f, -tc);
    if (face == 4) return float3(sc, -tc, 1.0f);
    return float3(-sc, -tc, -1.0f);
}
uint swap_endian(uint v, uint e)
{
    if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2u || e == 3u) v = (v << 16) | (v >> 16);
    return v;
}
static const float FLT_MAX_ = 3.402823466e+38f;
)";

        enum CfOp
        {
            kNop = 0, kExec, kExecEnd, kCondExec, kCondExecEnd, kCondExecPred, kCondExecPredEnd,
            kLoopStart, kLoopEnd, kCondCall, kReturn, kCondJmp, kAlloc, kCondExecPredClean,
            kCondExecPredCleanEnd, kMarkVsFetchDone
        };

        struct Cf
        {
            uint64_t v;
            uint32_t op() const { return bits(v, 44, 4); }
            bool is_exec() const
            {
                const uint32_t o = op();
                return o == kExec || o == kExecEnd || o == kCondExec || o == kCondExecEnd || o == kCondExecPred ||
                       o == kCondExecPredEnd || o == kCondExecPredClean || o == kCondExecPredCleanEnd;
            }
            bool ends() const
            {
                const uint32_t o = op();
                return o == kExecEnd || o == kCondExecEnd || o == kCondExecPredEnd || o == kCondExecPredCleanEnd;
            }
            uint32_t exec_address() const { return bits(v, 0, 12); }
            uint32_t exec_count() const { return bits(v, 12, 3); }
            uint32_t sequence() const { return bits(v, 16, 12); }
            uint32_t bool_address() const { return bits(v, 34, 8); }
            bool condition() const { return bits(v, 42, 1) != 0; }
            uint32_t address() const { return bits(v, 0, 13); } // loops, calls, jumps
            bool repeat() const { return bits(v, 13, 1) != 0; }
            bool unconditional() const { return bits(v, 13, 1) != 0; }
            bool predicated() const { return bits(v, 14, 1) != 0; }
            uint32_t loop_id() const { return bits(v, 16, 5); }
            bool predicated_break() const { return bits(v, 21, 1) != 0; }
        };

        std::string num(int64_t v) { return std::to_string(v); }
        std::string fnum(float f)
        {
            if (std::isnan(f))
                return "asfloat(0x7FC00000u)";
            if (std::isinf(f))
                return f > 0 ? "asfloat(0x7F800000u)" : "asfloat(0xFF800000u)";
            char b[48];
            snprintf(b, sizeof(b), "%.9g", f);
            std::string s = b;
            if (s.find_first_of(".e") == std::string::npos)
                s += ".0"; // "2f" is not an HLSL literal, "2.0f" is
            return s + "f";
        }

        const char kComp[] = "xyzw";

        class Translator
        {
        public:
            Translator(ShaderStage stage, const uint32_t* words_be, uint32_t count, bool no_gradients)
                : stage_(stage), no_gradients_(no_gradients)
            {
                words_.resize(count);
                for (uint32_t i = 0; i < count; i++)
                    words_[i] = __builtin_bswap32(words_be[i]);
                result_.stage = stage;
            }

            TranslatedShader run()
            {
                if (!parse_cf())
                    return std::move(result_);
                complex_ = std::any_of(cfs_.begin(), cfs_.end(), [](const Cf& c) {
                    const uint32_t o = c.op();
                    return o == kLoopStart || o == kLoopEnd || o == kCondCall || o == kReturn || o == kCondJmp;
                });
                if (complex_ && stage_ == ShaderStage::Pixel)
                    no_gradients_ = true;

                indent_ = 1;
                if (complex_)
                    emit_complex();
                else
                    emit_simple();
                if (!result_.error.empty())
                    return std::move(result_);
                assemble();
                result_.ok = true;
                return std::move(result_);
            }

        private:
            ShaderStage stage_;
            bool no_gradients_;
            bool complex_ = false;
            std::vector<uint32_t> words_;
            std::vector<Cf> cfs_;
            std::string body_;
            int indent_ = 1;
            TranslatedShader result_;
            std::map<uint32_t, uint32_t> textures_; // slot -> dimension
            std::set<uint32_t> vslots_;
            std::map<uint32_t, uint32_t> vstrides_, vextents_; // dwords
            bool temps_relative_ = false;
            uint32_t temps_used_ = 1;
            uint32_t vf_last_slot_ = 0, vf_last_stride_ = 0;

            void line(const std::string& s)
            {
                body_.append(size_t(indent_) * 4, ' ');
                body_ += s;
                body_ += '\n';
            }
            void fail(const std::string& e)
            {
                if (result_.error.empty())
                    result_.error = e;
            }
            bool vs() const { return stage_ == ShaderStage::Vertex; }

            // ------------------------------------------------------------ control flow

            bool parse_cf()
            {
                const uint32_t triples = uint32_t(words_.size() / 3);
                uint32_t limit = triples; // the instructions start at the lowest exec address
                for (uint32_t t = 0; t < limit && t < triples; t++)
                {
                    const uint32_t* d = &words_[t * 3];
                    const Cf a{ uint64_t(d[0]) | (uint64_t(d[1] & 0xFFFF) << 32) };
                    const Cf b{ uint64_t((d[1] >> 16) | (d[2] << 16)) | (uint64_t(d[2] >> 16) << 32) };
                    for (const Cf& c : { a, b })
                    {
                        cfs_.push_back(c);
                        if (c.is_exec() && c.exec_count())
                            limit = std::min(limit, c.exec_address());
                    }
                }
                if (cfs_.empty())
                {
                    fail("no control flow");
                    return false;
                }
                return true;
            }

            std::string exec_condition(const Cf& c)
            {
                switch (c.op())
                {
                case kCondExec:
                case kCondExecEnd:
                case kCondExecPredClean:
                case kCondExecPredCleanEnd:
                    return std::string(c.condition() ? "" : "!") + "bool_const(" + num(c.bool_address()) + ")";
                case kCondExecPred:
                case kCondExecPredEnd:
                    return std::string(c.condition() ? "" : "!") + "p0";
                default:
                    return "";
                }
            }

            void emit_simple()
            {
                line("do");
                line("{");
                indent_++;
                for (const Cf& c : cfs_)
                {
                    if (!c.is_exec())
                        continue;
                    const std::string cond = exec_condition(c);
                    if (!cond.empty())
                    {
                        line("if (" + cond + ")");
                        line("{");
                        indent_++;
                    }
                    emit_exec(c);
                    if (c.ends())
                        line("break;");
                    if (!cond.empty())
                    {
                        indent_--;
                        line("}");
                    }
                    else if (c.ends())
                        break;
                }
                indent_--;
                line("} while (false);");
            }

            void emit_complex()
            {
                line("uint cf_pc = 0, guard = 0;");
                line("uint rets[4] = { 0, 0, 0, 0 }; uint ret_sp = 0;");
                line("uint lcnt[4] = { 0, 0, 0, 0 }; int lsave[4] = { 0, 0, 0, 0 };");
                line("[loop] while (cf_pc != 0xFFFFu && guard++ < 65536u)");
                line("{");
                indent_++;
                // An if/else chain on the CF index rather than a switch: the HLSL compiler fails
                // with an internal error ("l-value expected") on some shaders' switch form.
                line("const uint cur = cf_pc;");
                int depth = 0;
                for (uint32_t k = 0; k < cfs_.size(); k++)
                {
                    const Cf& c = cfs_[k];
                    const std::string next = num(k + 1);
                    line(std::string(k == 0 ? "if" : "else if") + " (cur == " + num(k) + "u)");
                    line("{");
                    indent_++;
                    if (c.is_exec())
                    {
                        line("cf_pc = " + next + ";");
                        const std::string cond = exec_condition(c);
                        if (!cond.empty())
                        {
                            line("if (" + cond + ")");
                            line("{");
                            indent_++;
                        }
                        emit_exec(c);
                        if (c.ends())
                            line("cf_pc = 0xFFFFu;");
                        if (!cond.empty())
                        {
                            indent_--;
                            line("}");
                        }
                    }
                    else
                    {
                        switch (c.op())
                        {
                        case kLoopStart:
                        {
                            const std::string d = num(std::min(depth, 3));
                            line("uint lc = loop_const(" + num(c.loop_id()) + ");");
                            line("lsave[" + d + "] = aL; lcnt[" + d + "] = 0;");
                            if (!c.repeat())
                                line("aL = int((lc >> 8) & 0xFF);");
                            line("cf_pc = " + next + ";");
                            line("if ((lc & 0xFF) == 0) { aL = lsave[" + d + "]; cf_pc = " + num(c.address()) + "; }");
                            depth++;
                            break;
                        }
                        case kLoopEnd:
                        {
                            depth = std::max(depth - 1, 0);
                            const std::string d = num(std::min(depth, 3));
                            line("uint lc = loop_const(" + num(c.loop_id()) + ");");
                            line("lcnt[" + d + "]++; aL += int(lc << 8) >> 24;");
                            std::string brk = c.predicated_break() ? std::string(" && ") + (c.condition() ? "!p0" : "p0") : "";
                            line("if (lcnt[" + d + "] < (lc & 0xFF)" + brk + ") cf_pc = " + num(c.address()) + ";");
                            line("else { aL = lsave[" + d + "]; cf_pc = " + next + "; }");
                            break;
                        }
                        case kCondCall:
                        case kCondJmp:
                        {
                            std::string cond = "true";
                            if (!c.unconditional())
                                cond = c.predicated() ? std::string(c.condition() ? "" : "!") + "p0"
                                                      : std::string(c.condition() ? "" : "!") + "bool_const(" + num(c.bool_address()) + ")";
                            if (c.op() == kCondCall)
                                line("if (" + cond + ") { rets[min(ret_sp, 3u)] = " + next + "; ret_sp++; cf_pc = " + num(c.address()) +
                                     "; } else cf_pc = " + next + ";");
                            else
                                line("cf_pc = (" + cond + ") ? " + num(c.address()) + "u : " + next + "u;");
                            break;
                        }
                        case kReturn:
                            line("if (ret_sp > 0) { ret_sp--; cf_pc = rets[min(ret_sp, 3u)]; } else cf_pc = " + next + ";");
                            break;
                        default:
                            line("cf_pc = " + next + ";");
                            break;
                        }
                    }
                    indent_--;
                    line("}");
                }
                line("else");
                line("    cf_pc = 0xFFFFu;");
                indent_--;
                line("}");
            }

            void emit_exec(const Cf& c)
            {
                const uint32_t seq = c.sequence();
                for (uint32_t i = 0; i < c.exec_count(); i++)
                {
                    const uint32_t index = c.exec_address() + i;
                    if ((index + 1) * 3 > words_.size())
                    {
                        fail("exec past the end of the shader");
                        return;
                    }
                    const uint32_t* w = &words_[index * 3];
                    if ((seq >> (i * 2)) & 1)
                        emit_fetch(w);
                    else
                        emit_alu(w);
                }
            }

            // ------------------------------------------------------------ registers

            std::string temp(uint32_t index, bool relative)
            {
                if (relative)
                {
                    temps_relative_ = true;
                    return "r[(" + num(index) + " + aL) & 63]";
                }
                temps_used_ = std::max(temps_used_, index + 1);
                return "r[" + num(index) + "]";
            }

            std::string dest_swizzle_write(const std::string& dst, const std::string& value, uint32_t swiz)
            {
                // fetch destination swizzle: 3 bits per component, 0-3 select, 4 = 0, 5 = 1, 7 keep
                std::string out;
                for (uint32_t i = 0; i < 4; i++)
                {
                    const uint32_t s = (swiz >> (i * 3)) & 7;
                    if (s >= 6)
                        continue;
                    std::string v = s < 4 ? value + "." + kComp[s] : (s == 4 ? "0.0f" : "1.0f");
                    out += dst + "." + kComp[i] + " = " + v + "; ";
                }
                return out;
            }

            // ------------------------------------------------------------ fetch

            void emit_fetch(const uint32_t* w)
            {
                const uint32_t op = bits(w[0], 0, 5);
                const bool predicated = bits(w[1], 31, 1);
                const bool pred_cond = bits(w[2], 31, 1);
                std::string pre;
                if (predicated)
                    pre = std::string("if (") + (pred_cond ? "" : "!") + "p0) ";
                if (op == 0)
                    emit_vfetch(w, pre);
                else
                    emit_tfetch(w, op, pre);
            }

            void emit_vfetch(const uint32_t* w, const std::string& pre)
            {
                if (!vs())
                {
                    fail("vertex fetch in a pixel shader");
                    return;
                }
                const uint32_t src = bits(w[0], 5, 6);
                const bool src_rel = bits(w[0], 11, 1);
                const uint32_t dst = bits(w[0], 12, 6);
                const bool dst_rel = bits(w[0], 18, 1);
                const uint32_t slot_full = bits(w[0], 20, 5) * 3 + bits(w[0], 25, 2);
                const uint32_t src_comp = bits(w[0], 30, 2);
                const uint32_t dst_swiz = bits(w[1], 0, 12);
                const bool is_signed = bits(w[1], 12, 1);
                const bool integer = bits(w[1], 13, 1);
                const bool rounded = bits(w[1], 15, 1);
                const uint32_t format = bits(w[1], 16, 6);
                const int exp_adjust = sbits(w[1], 24, 6);
                const bool mini = bits(w[1], 30, 1);
                const uint32_t stride_full = bits(w[2], 0, 8);
                const int32_t offset = sbits(w[2], 8, 23);

                std::string code = pre + "{ ";
                uint32_t slot = slot_full, stride = stride_full;
                if (!mini)
                {
                    vf_last_slot_ = slot_full;
                    vf_last_stride_ = stride_full;
                    code += "vf_index = int(floor(" + temp(src, src_rel) + "." + kComp[src_comp] + (rounded ? " + 0.5f" : "") + ")); ";
                }
                else
                {
                    slot = vf_last_slot_;
                    stride = vf_last_stride_;
                }
                vslots_.insert(slot);
                vstrides_[slot] = std::max(vstrides_[slot], stride);
                const std::string base = std::string("vfetch[") + num(slot >> 1) + "]." + ((slot & 1) ? "z" : "x");
                const std::string endian = std::string("vfetch[") + num(slot >> 1) + "]." + ((slot & 1) ? "w" : "y");
                code += "uint va = " + base + " + uint(vf_index * " + num(stride) + " + " + num(offset) + ") * 4u; ";

                auto load = [&](int i) { return "swap_endian(vb.Load(va + " + num(i * 4) + "u), " + endian + ")"; };
                auto norm_int = [&](const std::string& u, int n) -> std::string {
                    // u: uint expression holding an n-bit field
                    if (is_signed)
                    {
                        const std::string s = "float(int(" + u + " << " + num(32 - n) + ") >> " + num(32 - n) + ")";
                        if (!integer)
                            return "max(" + s + " / " + fnum(float((1u << (n - 1)) - 1)) + ", -1.0f)";
                        return s;
                    }
                    const std::string f = "float(" + u + ")";
                    if (!integer)
                        return f + " / " + fnum(n == 32 ? 4294967295.0f : float((1ull << n) - 1));
                    return f;
                };
                std::string v;
                bool fixed = true; // exp_adjust applies to fixed-point formats
                switch (format)
                {
                case 6: // 8_8_8_8
                    code += "uint d0 = " + load(0) + "; ";
                    v = "float4(" + norm_int("(d0 & 0xFFu)", 8) + ", " + norm_int("((d0 >> 8) & 0xFFu)", 8) + ", " +
                        norm_int("((d0 >> 16) & 0xFFu)", 8) + ", " + norm_int("(d0 >> 24)", 8) + ")";
                    break;
                case 7: // 2_10_10_10
                    code += "uint d0 = " + load(0) + "; ";
                    v = "float4(" + norm_int("(d0 & 0x3FFu)", 10) + ", " + norm_int("((d0 >> 10) & 0x3FFu)", 10) + ", " +
                        norm_int("((d0 >> 20) & 0x3FFu)", 10) + ", " + norm_int("(d0 >> 30)", 2) + ")";
                    break;
                case 16: // 10_11_11
                    code += "uint d0 = " + load(0) + "; ";
                    v = "float4(" + norm_int("(d0 & 0x7FFu)", 11) + ", " + norm_int("((d0 >> 11) & 0x7FFu)", 11) + ", " +
                        norm_int("(d0 >> 22)", 10) + ", 1.0f)";
                    break;
                case 17: // 11_11_10
                    code += "uint d0 = " + load(0) + "; ";
                    v = "float4(" + norm_int("(d0 & 0x3FFu)", 10) + ", " + norm_int("((d0 >> 10) & 0x7FFu)", 11) + ", " +
                        norm_int("(d0 >> 21)", 11) + ", 1.0f)";
                    break;
                case 25: // 16_16
                    code += "uint d0 = " + load(0) + "; ";
                    v = "float4(" + norm_int("(d0 & 0xFFFFu)", 16) + ", " + norm_int("(d0 >> 16)", 16) + ", 0.0f, 1.0f)";
                    break;
                case 26: // 16_16_16_16
                    code += "uint d0 = " + load(0) + ", d1 = " + load(1) + "; ";
                    v = "float4(" + norm_int("(d0 & 0xFFFFu)", 16) + ", " + norm_int("(d0 >> 16)", 16) + ", " +
                        norm_int("(d1 & 0xFFFFu)", 16) + ", " + norm_int("(d1 >> 16)", 16) + ")";
                    break;
                case 31: // 16_16_FLOAT
                    code += "uint d0 = " + load(0) + "; ";
                    v = "float4(f16tof32(d0 & 0xFFFFu), f16tof32(d0 >> 16), 0.0f, 1.0f)";
                    fixed = false;
                    break;
                case 32: // 16_16_16_16_FLOAT
                    code += "uint d0 = " + load(0) + ", d1 = " + load(1) + "; ";
                    v = "float4(f16tof32(d0 & 0xFFFFu), f16tof32(d0 >> 16), f16tof32(d1 & 0xFFFFu), f16tof32(d1 >> 16))";
                    fixed = false;
                    break;
                case 33: // 32
                    code += "uint d0 = " + load(0) + "; ";
                    v = "float4(" + norm_int("d0", 32) + ", 0.0f, 0.0f, 1.0f)";
                    break;
                case 34: // 32_32
                    code += "uint d0 = " + load(0) + ", d1 = " + load(1) + "; ";
                    v = "float4(" + norm_int("d0", 32) + ", " + norm_int("d1", 32) + ", 0.0f, 1.0f)";
                    break;
                case 35: // 32_32_32_32
                    code += "uint d0 = " + load(0) + ", d1 = " + load(1) + ", d2 = " + load(2) + ", d3 = " + load(3) + "; ";
                    v = "float4(" + norm_int("d0", 32) + ", " + norm_int("d1", 32) + ", " + norm_int("d2", 32) + ", " + norm_int("d3", 32) + ")";
                    break;
                case 36: // 32_FLOAT
                    code += "uint d0 = " + load(0) + "; ";
                    v = "float4(asfloat(d0), 0.0f, 0.0f, 1.0f)";
                    fixed = false;
                    break;
                case 37: // 32_32_FLOAT
                    code += "uint d0 = " + load(0) + ", d1 = " + load(1) + "; ";
                    v = "float4(asfloat(d0), asfloat(d1), 0.0f, 1.0f)";
                    fixed = false;
                    break;
                case 38: // 32_32_32_32_FLOAT
                    code += "uint d0 = " + load(0) + ", d1 = " + load(1) + ", d2 = " + load(2) + ", d3 = " + load(3) + "; ";
                    v = "asfloat(uint4(d0, d1, d2, d3))";
                    fixed = false;
                    break;
                case 57: // 32_32_32_FLOAT
                    code += "uint d0 = " + load(0) + ", d1 = " + load(1) + ", d2 = " + load(2) + "; ";
                    v = "float4(asfloat(d0), asfloat(d1), asfloat(d2), 1.0f)";
                    fixed = false;
                    break;
                default:
                    fail("unsupported vertex format " + num(format));
                    return;
                }
                {
                    // dwords of one element
                    uint32_t words = 1;
                    if (format == 26 || format == 32 || format == 34 || format == 37)
                        words = 2;
                    else if (format == 57)
                        words = 3;
                    else if (format == 35 || format == 38)
                        words = 4;
                    vextents_[slot] = std::max<uint32_t>(vextents_[slot], uint32_t(std::max(offset, 0)) + words);
                }
                code += "float4 v = " + v + "; ";
                if (fixed && exp_adjust)
                    code += "v *= " + fnum(float(std::ldexp(1.0, exp_adjust))) + "; ";
                code += dest_swizzle_write(temp(dst, dst_rel), "v", dst_swiz);
                code += "}";
                line(code);
            }

            void emit_tfetch(const uint32_t* w, uint32_t op, const std::string& pre)
            {
                const uint32_t src = bits(w[0], 5, 6);
                const bool src_rel = bits(w[0], 11, 1);
                const uint32_t dst = bits(w[0], 12, 6);
                const bool dst_rel = bits(w[0], 18, 1);
                const uint32_t slot = bits(w[0], 20, 5);
                const bool denorm = bits(w[0], 25, 1);
                const uint32_t src_swiz = bits(w[0], 26, 6);
                const uint32_t dst_swiz = bits(w[1], 0, 12);
                const bool use_reg_lod = bits(w[1], 29, 1);
                const bool use_reg_grad = bits(w[2], 0, 1);
                const float lod_bias = float(sbits(w[2], 2, 7)) / 16.0f;
                const uint32_t dim = bits(w[2], 14, 2);
                const float off_x = float(sbits(w[2], 16, 5)) * 0.5f;
                const float off_y = float(sbits(w[2], 21, 5)) * 0.5f;
                const float off_z = float(sbits(w[2], 26, 5)) * 0.5f;

                const std::string s = temp(src, src_rel);
                const std::string coord = "float3(" + s + "." + kComp[src_swiz & 3] + ", " + s + "." + kComp[(src_swiz >> 2) & 3] + ", " +
                                          s + "." + kComp[(src_swiz >> 4) & 3] + ")";
                const std::string d = temp(dst, dst_rel);

                switch (op)
                {
                case 24: // setTextureLod
                    line(pre + "lod_reg = " + s + "." + kComp[src_swiz & 3] + ";");
                    return;
                case 25: // setTextureGradientsHorz
                    line(pre + "grad_h = " + coord + ";");
                    return;
                case 26: // setTextureGradientsVert
                    line(pre + "grad_v = " + coord + ";");
                    return;
                case 16: // getTextureBorderColorFrac
                    line(pre + "{ " + dest_swizzle_write(d, "float4(0.0f, 0.0f, 0.0f, 0.0f)", dst_swiz) + "}");
                    return;
                case 1:
                case 17:
                case 18:
                case 19:
                    break;
                default:
                    fail("unsupported fetch opcode " + num(op));
                    return;
                }

                if (auto it = textures_.find(slot); it != textures_.end() && it->second != dim)
                {
                    fail("texture slot " + num(slot) + " used with two dimensions");
                    return;
                }
                textures_[slot] = dim;
                const std::string tex = "tex" + num(slot);
                const std::string smp = "smp" + num(slot);

                std::string code = pre + "{ float3 tc = " + coord + "; ";
                // size of the texture, for unnormalized coordinates and offsets
                const bool need_size = denorm || off_x != 0 || off_y != 0 || off_z != 0 || op == 19;
                if (need_size)
                {
                    if (dim == 2)
                        code += "float3 ts; " + tex + ".GetDimensions(ts.x, ts.y, ts.z); ";
                    else
                        code += "float3 ts = 1.0f; " + tex + ".GetDimensions(ts.x, ts.y); ";
                }
                if (denorm && dim != 3)
                    code += dim == 2 ? "tc /= ts; " : "tc.xy /= ts.xy; ";
                if (off_x != 0 || off_y != 0 || off_z != 0)
                    code += "tc += float3(" + fnum(off_x) + ", " + fnum(off_y) + ", " + fnum(off_z) + ") / ts; ";

                std::string c;
                switch (dim)
                {
                case 0: c = "float2(tc.x, 0.5f)"; break;
                case 1: c = "tc.xy"; break;
                case 2: c = "tc"; break;
                default: c = "cube_dir(tc)"; break;
                }

                std::string value;
                if (op == 1)
                {
                    const bool implicit = !vs() && !no_gradients_;
                    std::string bias = lod_bias != 0 ? " + " + fnum(lod_bias) : "";
                    if (use_reg_lod)
                        value = tex + ".SampleLevel(" + smp + ", " + c + ", lod_reg" + bias + ")";
                    else if (use_reg_grad && dim != 3)
                    {
                        const char* sw = dim == 0 ? ".xx" : (dim == 2 ? ".xyz" : ".xy");
                        value = tex + ".SampleGrad(" + smp + ", " + c + ", grad_h" + sw + ", grad_v" + sw + ")";
                        if (dim == 0)
                            value = tex + ".SampleGrad(" + smp + ", " + c + ", float2(grad_h.x, 0.0f), float2(grad_v.x, 0.0f))";
                    }
                    else if (!implicit)
                        value = tex + ".SampleLevel(" + smp + ", " + c + ", " + (lod_bias != 0 ? fnum(lod_bias) : "0.0f") + ")";
                    else if (lod_bias != 0)
                        value = tex + ".SampleBias(" + smp + ", " + c + ", " + fnum(lod_bias) + ")";
                    else
                        value = tex + ".Sample(" + smp + ", " + c + ")";
                    value = "apply_swizzle(" + value + ", tex_swz(" + num(slot) + "))";
                }
                else if (op == 17) // getTextureComputedLod
                    value = (vs() || no_gradients_) ? "float4(0.0f, 0.0f, 0.0f, 0.0f)"
                                                    : "float4(" + tex + ".CalculateLevelOfDetailUnclamped(" + smp + ", " + c + "), 0.0f, 0.0f, 0.0f)";
                else if (op == 18) // getTextureGradients
                    value = (vs() || no_gradients_) ? "float4(0.0f, 0.0f, 0.0f, 0.0f)" : "float4(ddx(tc.xy), ddy(tc.xy)).xzyw";
                else // 19 getTextureWeights
                    value = "float4(frac(tc * ts - 0.5f), 0.0f)";
                code += "float4 tv = " + value + "; ";
                code += dest_swizzle_write(d, "tv", dst_swiz);
                code += "}";
                line(code);
            }

            // ------------------------------------------------------------ ALU

            struct Alu
            {
                uint32_t w0, w1, w2;
                uint32_t vector_dest() const { return bits(w0, 0, 6); }
                bool vector_dest_rel() const { return bits(w0, 6, 1); }
                bool abs_constants() const { return bits(w0, 7, 1); }
                uint32_t scalar_dest() const { return bits(w0, 8, 6); }
                bool scalar_dest_rel() const { return bits(w0, 14, 1); }
                bool is_export() const { return bits(w0, 15, 1); }
                uint32_t vector_mask() const { return bits(w0, 16, 4); }
                uint32_t scalar_mask() const { return bits(w0, 20, 4); }
                bool vector_clamp() const { return bits(w0, 24, 1); }
                bool scalar_clamp() const { return bits(w0, 25, 1); }
                uint32_t scalar_op() const { return bits(w0, 26, 6); }
                uint32_t swizzle(int i) const { return bits(w1, i == 1 ? 16 : i == 2 ? 8 : 0, 8); }
                bool negate(int i) const { return bits(w1, i == 1 ? 26 : i == 2 ? 25 : 24, 1); }
                bool pred_condition() const { return bits(w1, 27, 1); }
                bool predicated() const { return bits(w1, 28, 1); }
                bool const_a0_relative() const { return bits(w1, 29, 1); }
                bool const1_rel() const { return bits(w1, 30, 1); }
                bool const0_rel() const { return bits(w1, 31, 1); }
                uint32_t reg(int i) const { return bits(w2, i == 1 ? 16 : i == 2 ? 8 : 0, 8); }
                uint32_t vector_op() const { return bits(w2, 24, 5); }
                bool is_temp(int i) const { return bits(w2, i == 1 ? 31 : i == 2 ? 30 : 29, 1); }
                bool const_addressed(int i) const
                {
                    if (i == 1)
                        return const0_rel();
                    if (i == 2)
                        return is_temp(1) ? const0_rel() : const1_rel();
                    return (is_temp(1) && is_temp(2)) ? const0_rel() : const1_rel();
                }
            };

            // The operand's register with abs applied, no swizzle or negation.
            std::string operand_base(const Alu& a, int i)
            {
                const uint32_t reg = a.reg(i);
                if (a.is_temp(i))
                {
                    std::string r = temp(reg & 0x3F, (reg & 0x40) != 0);
                    return (reg & 0x80) ? "abs(" + r + ")" : r;
                }
                std::string r;
                if (a.const_addressed(i))
                    r = "c[(" + num(reg) + " + " + (a.const_a0_relative() ? "a0" : "aL") + ") & 255]";
                else
                    r = "c[" + num(reg) + "]";
                return a.abs_constants() ? "abs(" + r + ")" : r;
            }

            std::string operand(const Alu& a, int i)
            {
                const uint32_t swz = a.swizzle(i);
                std::string s = "(" + operand_base(a, i) + ").";
                for (uint32_t c = 0; c < 4; c++)
                    s += kComp[((swz >> (c * 2)) + c) & 3];
                return a.negate(i) ? "(-" + s + ")" : s;
            }

            void emit_alu(const uint32_t* w)
            {
                const Alu a{ w[0], w[1], w[2] };
                const uint32_t vop = a.vector_op();
                const uint32_t sop = a.scalar_op();

                // Which halves do anything.
                const bool vector_side_effect = (vop >= 20 && vop <= 27) || vop == 29;
                bool vector_used = a.vector_mask() || vector_side_effect;
                const bool scalar_side_effect = sop == 23 || sop == 24 || (sop >= 27 && sop <= 39);
                bool scalar_used = sop != 50 && sop != 41; // retain_prev / unused: ps unchanged
                const uint32_t c0_mask = (a.is_export() && a.scalar_dest_rel()) ? (0xFu & ~(a.vector_mask() | a.scalar_mask())) : 0;
                const uint32_t c1_mask = a.is_export() ? (a.vector_mask() & a.scalar_mask()) : 0;
                const uint32_t vmask = a.is_export() ? (a.vector_mask() & ~a.scalar_mask()) : a.vector_mask();
                const uint32_t smask = a.is_export() ? (a.scalar_mask() & ~a.vector_mask()) : a.scalar_mask();
                if (!vector_used && !scalar_used && !c0_mask && !c1_mask)
                    return;
                (void)scalar_side_effect;

                std::string code;
                if (a.predicated())
                    code += std::string("if (") + (a.pred_condition() ? "" : "!") + "p0) ";
                code += "{ ";
                bool sets_p0 = false, sets_a0 = false;

                // ---- vector operation
                if (vector_used)
                {
                    const std::string A = operand(a, 1);
                    auto B = [&] { return operand(a, 2); };
                    auto C = [&] { return operand(a, 3); };
                    std::string v;
                    switch (vop)
                    {
                    case 0: v = A + " + " + B(); break;
                    case 1: v = "lmul(" + A + ", " + B() + ")"; break;
                    case 2: v = "vmax(" + A + ", " + B() + ")"; break;
                    case 3: v = "vmin(" + A + ", " + B() + ")"; break;
                    case 4: v = "(float4)(" + A + " == " + B() + ")"; break;
                    case 5: v = "(float4)(" + A + " > " + B() + ")"; break;
                    case 6: v = "(float4)(" + A + " >= " + B() + ")"; break;
                    case 7: v = "(float4)(" + A + " != " + B() + ")"; break;
                    case 8: v = "frac(" + A + ")"; break;
                    case 9: v = "trunc(" + A + ")"; break;
                    case 10: v = "floor(" + A + ")"; break;
                    case 11: v = "lmul(" + A + ", " + B() + ") + " + C(); break;
                    case 12: v = "(" + A + " == 0.0f) ? " + B() + " : " + C(); break;
                    case 13: v = "(" + A + " >= 0.0f) ? " + B() + " : " + C(); break;
                    case 14: v = "(" + A + " > 0.0f) ? " + B() + " : " + C(); break;
                    case 15: v = "(float4)ldp4(" + A + ", " + B() + ")"; break;
                    case 16: v = "(float4)ldp3(" + A + ", " + B() + ")"; break;
                    case 17: v = "(float4)ldp2add(" + A + ", " + B() + ", " + C() + ")"; break;
                    case 18: v = "vcube(" + A + ")"; break;
                    case 19: v = "(float4)vmax4(" + A + ")"; break;
                    case 20: case 21: case 22: case 23:
                    {
                        static const char* ops[] = { " == 0.0f", " != 0.0f", " > 0.0f", " >= 0.0f" };
                        const char* o = ops[vop - 20];
                        code += "float4 va = " + A + ", vb_ = " + B() + "; ";
                        code += std::string("bool vp = va.w == 0.0f && vb_.w") + o + "; ";
                        v = std::string("(float4)((va.x == 0.0f && vb_.x") + o + ") ? 0.0f : va.x + 1.0f)";
                        sets_p0 = true;
                        break;
                    }
                    case 24: case 25: case 26: case 27:
                    {
                        static const char* ops[] = { " == ", " > ", " >= ", " != " };
                        code += std::string("bool vk = any(") + A + ops[vop - 24] + B() + "); ";
                        if (!vs())
                        {
                            code += "if (vk) discard; ";
                            result_.kills = true;
                        }
                        v = "(float4)(vk ? 1.0f : 0.0f)";
                        break;
                    }
                    case 28: v = "float4(1.0f, lmul(" + A + ".y, " + B() + ".y), " + A + ".z, " + B() + ".w)"; break;
                    case 29:
                        code += "float4 va = " + A + "; int va0 = int(clamp(floor(va.w + 0.5f), -256.0f, 255.0f)); ";
                        v = "vmax(va, " + B() + ")";
                        sets_a0 = true;
                        break;
                    default:
                        fail("unknown vector opcode " + num(vop));
                        return;
                    }
                    code += "float4 vr = " + (a.vector_clamp() ? "saturate(" + v + ")" : v) + "; ";
                }

                // ---- scalar operation
                bool scalar_p0 = false, scalar_a0 = false;
                if (scalar_used)
                {
                    std::string sa, sb;
                    if (sop >= 42 && sop <= 47)
                    {
                        // constant (component a) op temp (component b)
                        const uint32_t swz = a.swizzle(3);
                        const uint32_t treg = (sop & 1) | (uint32_t(a.is_temp(3)) << 1) | (swz & 0x3C);
                        std::string cst = a.const_addressed(3)
                                              ? "c[(" + num(a.reg(3)) + " + " + (a.const_a0_relative() ? "a0" : "aL") + ") & 255]"
                                              : "c[" + num(a.reg(3)) + "]";
                        std::string tmp = temp(treg, false);
                        if (a.abs_constants())
                        {
                            cst = "abs(" + cst + ")";
                            tmp = "abs(" + tmp + ")";
                        }
                        sa = "(" + cst + ")." + kComp[((swz >> 6) + 3) & 3];
                        sb = "(" + tmp + ")." + kComp[(swz + 0) & 3];
                        if (a.negate(3))
                        {
                            sa = "(-" + sa + ")";
                            sb = "(-" + sb + ")";
                        }
                    }
                    else
                    {
                        const uint32_t swz = a.swizzle(3);
                        const std::string base = "(" + operand_base(a, 3) + ")";
                        sa = base + "." + kComp[((swz >> 6) + 3) & 3];
                        sb = base + "." + kComp[(swz + 0) & 3];
                        if (a.negate(3))
                        {
                            sa = "(-" + sa + ")";
                            sb = "(-" + sb + ")";
                        }
                    }
                    std::string s;
                    auto setp = [&](const std::string& cond) {
                        code += "float sa_ = " + sa + "; bool sp = " + cond + "; ";
                        scalar_p0 = true;
                    };
                    switch (sop)
                    {
                    case 0: s = sa + " + " + sb; break;
                    case 1: s = sa + " + ps"; break;
                    case 2: s = "lmul(" + sa + ", " + sb + ")"; break;
                    case 3: s = "lmul(" + sa + ", ps)"; break;
                    case 4:
                        code += "float sb_ = " + sb + "; ";
                        s = "(ps == -FLT_MAX_ || !isfinite(ps) || !isfinite(sb_) || sb_ <= 0.0f) ? -FLT_MAX_ : lmul(" + sa + ", ps)";
                        break;
                    case 5: code += "float sa_ = " + sa + ", sb_ = " + sb + "; "; s = "sa_ >= sb_ ? sa_ : sb_"; break;
                    case 6: code += "float sa_ = " + sa + ", sb_ = " + sb + "; "; s = "sa_ < sb_ ? sa_ : sb_"; break;
                    case 7: s = "(" + sa + " == 0.0f) ? 1.0f : 0.0f"; break;
                    case 8: s = "(" + sa + " > 0.0f) ? 1.0f : 0.0f"; break;
                    case 9: s = "(" + sa + " >= 0.0f) ? 1.0f : 0.0f"; break;
                    case 10: s = "(" + sa + " != 0.0f) ? 1.0f : 0.0f"; break;
                    case 11: s = "frac(" + sa + ")"; break;
                    case 12: s = "trunc(" + sa + ")"; break;
                    case 13: s = "floor(" + sa + ")"; break;
                    case 14: s = "exp2(" + sa + ")"; break;
                    case 15: code += "float sa_ = " + sa + "; float st = sa_ == 1.0f ? 0.0f : log2(sa_); "; s = "isinf(st) && st < 0.0f ? -FLT_MAX_ : st"; break;
                    case 16: code += "float sa_ = " + sa + "; "; s = "sa_ == 1.0f ? 0.0f : log2(sa_)"; break;
                    case 17: code += "float sa_ = " + sa + "; float st = sa_ == 1.0f ? 1.0f : 1.0f / sa_; "; s = "isinf(st) ? (st < 0.0f ? -FLT_MAX_ : FLT_MAX_) : st"; break;
                    case 18: code += "float sa_ = " + sa + "; float st = sa_ == 1.0f ? 1.0f : 1.0f / sa_; "; s = "isinf(st) ? (st < 0.0f ? -0.0f : 0.0f) : st"; break;
                    case 19: code += "float sa_ = " + sa + "; "; s = "sa_ == 1.0f ? 1.0f : 1.0f / sa_"; break;
                    case 20: code += "float sa_ = " + sa + "; float st = sa_ == 1.0f ? 1.0f : rsqrt(sa_); "; s = "isinf(st) ? (st < 0.0f ? -FLT_MAX_ : FLT_MAX_) : st"; break;
                    case 21: code += "float sa_ = " + sa + "; float st = sa_ == 1.0f ? 1.0f : rsqrt(sa_); "; s = "isinf(st) ? (st < 0.0f ? -0.0f : 0.0f) : st"; break;
                    case 22: code += "float sa_ = " + sa + "; "; s = "sa_ == 1.0f ? 1.0f : rsqrt(sa_)"; break;
                    case 23:
                    case 24:
                        code += "float sa_ = " + sa + ", sb_ = " + sb + "; int sa0 = int(clamp(floor(sa_" + (sop == 23 ? " + 0.5f" : "") +
                                "), -256.0f, 255.0f)); ";
                        s = "sa_ >= sb_ ? sa_ : sb_";
                        scalar_a0 = true;
                        break;
                    case 25: s = sa + " - " + sb; break;
                    case 26: s = sa + " - ps"; break;
                    case 27: setp("sa_ == 0.0f"); s = "sp ? 0.0f : 1.0f"; break;
                    case 28: setp("sa_ != 0.0f"); s = "sp ? 0.0f : 1.0f"; break;
                    case 29: setp("sa_ > 0.0f"); s = "sp ? 0.0f : 1.0f"; break;
                    case 30: setp("sa_ >= 0.0f"); s = "sp ? 0.0f : 1.0f"; break;
                    case 31: setp("sa_ == 1.0f"); s = "sp ? 0.0f : (sa_ == 0.0f ? 1.0f : sa_)"; break;
                    case 32: setp("sa_ - 1.0f <= 0.0f"); s = "sp ? 0.0f : sa_ - 1.0f"; break;
                    case 33: code += "bool sp = false; "; scalar_p0 = true; s = "FLT_MAX_"; break;
                    case 34: setp("sa_ == 0.0f"); s = "sp ? 0.0f : sa_"; break;
                    case 35: case 36: case 37: case 38: case 39:
                    {
                        static const char* ops[] = { " == 0.0f", " > 0.0f", " >= 0.0f", " != 0.0f", " == 1.0f" };
                        code += "bool sk = " + sa + ops[sop - 35] + "; ";
                        if (!vs())
                        {
                            code += "if (sk) discard; ";
                            result_.kills = true;
                        }
                        s = "sk ? 1.0f : 0.0f";
                        break;
                    }
                    case 40: s = "sqrt(" + sa + ")"; break;
                    case 42: case 43: s = "lmul(" + sa + ", " + sb + ")"; break;
                    case 44: case 45: s = sa + " + " + sb; break;
                    case 46: case 47: s = sa + " - " + sb; break;
                    case 48: s = "sin(" + sa + ")"; break;
                    case 49: s = "cos(" + sa + ")"; break;
                    default:
                        fail("unknown scalar opcode " + num(sop));
                        return;
                    }
                    code += "float sr = " + (a.scalar_clamp() ? "saturate(" + s + ")" : s) + "; ";
                }

                // ---- writes (after both halves have read their operands)
                auto mask_str = [](uint32_t m) {
                    std::string s;
                    for (uint32_t i = 0; i < 4; i++)
                        if (m & (1u << i))
                            s += kComp[i];
                    return s;
                };
                if (a.is_export())
                {
                    const std::string e = export_reg(a.vector_dest());
                    if (e.empty())
                        return;
                    if (vector_used && vmask)
                        code += e + "." + mask_str(vmask) + " = vr." + mask_str(vmask) + "; ";
                    if (scalar_used && smask)
                        code += e + "." + mask_str(smask) + " = sr; ";
                    if (c0_mask)
                        code += e + "." + mask_str(c0_mask) + " = 0.0f; ";
                    if (c1_mask)
                        code += e + "." + mask_str(c1_mask) + " = 1.0f; ";
                }
                else
                {
                    if (vector_used && vmask)
                        code += temp(a.vector_dest(), a.vector_dest_rel()) + "." + mask_str(vmask) + " = vr." + mask_str(vmask) + "; ";
                    if (scalar_used && smask)
                        code += temp(a.scalar_dest(), a.scalar_dest_rel()) + "." + mask_str(smask) + " = sr; ";
                }
                if (scalar_used)
                    code += "ps = sr; ";
                if (sets_p0)
                    code += "p0 = vp; ";
                if (scalar_p0)
                    code += "p0 = sp; ";
                if (sets_a0)
                    code += "a0 = va0; ";
                if (scalar_a0)
                    code += "a0 = sa0; ";
                code += "}";
                line(code);
            }

            std::string export_reg(uint32_t index)
            {
                if (vs())
                {
                    if (index < 16)
                        return "o_t[" + num(index) + "]";
                    if (index == 62)
                        return "o_pos";
                    if (index == 63)
                        return "o_pts";
                }
                else
                {
                    if (index < 4)
                    {
                        result_.color_targets |= 1u << index;
                        return "o_c[" + num(index) + "]";
                    }
                    if (index == 61)
                    {
                        result_.writes_depth = true;
                        return "o_depth";
                    }
                }
                if (index >= 32 && index <= 37)
                {
                    result_.memexport = true;
                    return "o_mem";
                }
                fail("unknown export register " + num(index));
                return "";
            }

            // ------------------------------------------------------------ the whole shader

            void assemble()
            {
                std::string s = kPrelude;
                for (auto& [slot, dim] : textures_)
                {
                    static const char* types[] = { "Texture2D", "Texture2D", "Texture3D", "TextureCube" };
                    s += std::string(types[dim]) + " tex" + num(slot) + " : register(t" + num(slot) + ");\n";
                    s += "SamplerState smp" + num(slot) + " : register(s" + num(slot) + ");\n";
                    result_.textures.push_back({ slot, dim });
                }
                result_.vertex_slots.assign(vslots_.begin(), vslots_.end());
                for (uint32_t slot : result_.vertex_slots)
                {
                    result_.vertex_strides.push_back(vstrides_[slot]);
                    result_.vertex_extents.push_back(vextents_[slot]);
                }
                const uint32_t ntemps = temps_relative_ ? 64 : temps_used_;

                if (vs())
                {
                    s += R"(ByteAddressBuffer vb : register(t32);
struct VSOut { float4 pos : SV_Position; float4 t[16] : TEXCOORD0; float4 pts : TEXCOORD16; };
float4 xform(float4 p)
{
    if (draw_flags.x & 4u) p.w = 1.0f / p.w;
    if (draw_flags.x & 1u) p.xy *= p.w;
    if (draw_flags.x & 2u) p.z *= p.w;
    p.xyz = p.xyz * ndc_scale.xyz + ndc_offset.xyz * p.w;
    return p;
}
VSOut main(uint vid : SV_VertexID)
{
)";
                    s += "    float4 r[" + num(ntemps) + "];\n";
                    s += "    for (uint i = 0; i < " + num(ntemps) + "; i++) r[i] = 0.0f;\n";
                    s += "    r[0].x = float(vid);\n";
                    s += "    float4 o_pos = float4(0.0f, 0.0f, 0.0f, 1.0f), o_pts = 0.0f, o_mem = 0.0f;\n";
                    s += "    float4 o_t[16]; for (uint j = 0; j < 16; j++) o_t[j] = 0.0f;\n";
                }
                else
                {
                    s += "struct PSIn { float4 pos : SV_Position; float4 t[16] : TEXCOORD0; float4 pts : TEXCOORD16; };\n";
                    s += "struct PSOut\n{\n";
                    for (uint32_t i = 0; i < 4; i++)
                        if (result_.color_targets & (1u << i))
                            s += "    float4 c" + num(i) + " : SV_Target" + num(i) + ";\n";
                    if (result_.writes_depth)
                        s += "    float depth : SV_Depth;\n";
                    if (!result_.color_targets && !result_.writes_depth)
                        s += "    float4 c0 : SV_Target0;\n";
                    s += "};\n";
                    s += "PSOut main(PSIn input)\n{\n";
                    s += "    float4 r[" + num(std::max(ntemps, 16u)) + "];\n";
                    // Interpolators in r0-r15, the rest zero (separate loops: the compiler unrolls
                    // a combined one and rejects the out-of-range input.t[i] reads).
                    s += "    for (uint i = 0; i < 16; i++) r[i] = input.t[i];\n";
                    if (ntemps > 16)
                        s += "    for (uint i2 = 16; i2 < " + num(ntemps) + "; i2++) r[i2] = 0.0f;\n";
                    s += "    float4 o_c[4]; for (uint j = 0; j < 4; j++) o_c[j] = 0.0f;\n";
                    s += "    float4 o_depth = input.pos.zzzz, o_mem = 0.0f;\n";
                    // Parameter generation: screen position and point sprite coordinate.
                    s += "    if (draw_flags.y != 0u) { [unroll] for (uint k = 0; k < " + num(std::max(ntemps, 16u)) +
                         "; k++) if (k == draw_flags.y - 1u) r[k] = float4(input.pos.xy, input.pts.zw); }\n";
                }
                s += "    float ps = 0.0f; bool p0 = false; int a0 = 0; int aL = 0; int vf_index = 0;\n";
                s += "    float lod_reg = 0.0f; float3 grad_h = 0.0f, grad_v = 0.0f;\n";
                s += body_;
                if (vs())
                    s += "    VSOut o; o.pos = xform(o_pos); o.t = o_t; o.pts = o_pts;\n    return o;\n}\n";
                else
                {
                    s += "    if (!alpha_test(o_c[0].a)) discard;\n";
                    s += "    PSOut o;\n";
                    for (uint32_t i = 0; i < 4; i++)
                        if (result_.color_targets & (1u << i))
                            s += "    o.c" + num(i) + " = o_c[" + num(i) + "];\n";
                    if (result_.writes_depth)
                        s += "    o.depth = o_depth.x;\n";
                    if (!result_.color_targets && !result_.writes_depth)
                        s += "    o.c0 = 0.0f;\n";
                    s += "    return o;\n}\n";
                }
                result_.hlsl = std::move(s);
            }
        };
    }

    TranslatedShader translate_shader(ShaderStage stage, const uint32_t* words_be, uint32_t count, bool no_gradients)
    {
        return Translator(stage, words_be, count, no_gradients).run();
    }
}
