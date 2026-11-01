#pragma once

#include "SoR.hpp"

#include <cstdint>

// ---------------------------------------------------------------------------
// What the hand-written ports of small ROM routines share: 68000 operations with the condition codes the
// ROM instruction leaves, and the soft `jsr` into another routine.
// ---------------------------------------------------------------------------

namespace sorops {

constexpr m_long maskOf(unsigned bits) {
    return bits >= 32u ? 0xFFFFFFFFu : ((1u << bits) - 1u);
}

constexpr m_long signOf(unsigned bits) {
    return 1u << (bits - 1u);
}

inline m_long sext8(m_byte value) {
    return static_cast<m_long>(static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
}

inline m_long sext16(m_word value) {
    return static_cast<m_long>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

// ext.w: the byte as a sign-extended word.
inline m_word sext8to16(m_byte value) {
    return static_cast<m_word>(static_cast<std::int16_t>(static_cast<std::int8_t>(value)));
}

inline bool lessSigned16(m_word left, m_word right) {
    return static_cast<std::int16_t>(left) < static_cast<std::int16_t>(right);
}

// move, tst, moveq, clr (the flags of a value): N and Z from the value, V and C clear, X untouched.
inline void tst(CPU68K &cpu, m_long value, unsigned bits) {
    cpu.setNZClearVC(value & maskOf(bits), signOf(bits));
}

// add / sub with the flags of the ROM instruction; the result is returned masked to the width.
inline m_long addValue(CPU68K &cpu, m_long dst, m_long src, unsigned bits, bool setX = true) {
    const m_long mask   = maskOf(bits);
    const m_long d      = dst & mask;
    const m_long s      = src & mask;
    const std::uint64_t wide = static_cast<std::uint64_t>(d) + s;
    const m_long result = static_cast<m_long>(wide) & mask;
    const bool carry    = wide > mask;
    const bool overflow = ((~(d ^ s)) & (d ^ result) & signOf(bits)) != 0;
    if (setX)
        cpu.setNZVCX(result, signOf(bits), overflow, carry, carry);
    else
        cpu.setNZVC(result, signOf(bits), overflow, carry);
    return result;
}

inline m_long subValue(CPU68K &cpu, m_long dst, m_long src, unsigned bits, bool setX = true) {
    const m_long mask   = maskOf(bits);
    const m_long d      = dst & mask;
    const m_long s      = src & mask;
    const m_long result = (d - s) & mask;
    const bool carry    = s > d;
    const bool overflow = (((d ^ s) & (d ^ result)) & signOf(bits)) != 0;
    if (setX)
        cpu.setNZVCX(result, signOf(bits), overflow, carry, carry);
    else
        cpu.setNZVC(result, signOf(bits), overflow, carry);
    return result;
}

// cmp: the flags of dst - src, X untouched.
inline void cmpValue(CPU68K &cpu, m_long dst, m_long src, unsigned bits) {
    subValue(cpu, dst, src, bits, false);
}

inline m_long negValue(CPU68K &cpu, m_long value, unsigned bits) {
    const m_long mask   = maskOf(bits);
    const m_long v      = value & mask;
    const m_long result = (0u - v) & mask;
    const bool carry    = v != 0;
    cpu.setNZVCX(result, signOf(bits), v == signOf(bits), carry, carry);
    return result;
}

// Shifts by a count of 1 to `bits`: C and X are the last bit out, V clear.
inline m_long lslValue(CPU68K &cpu, m_long value, unsigned count, unsigned bits) {
    const m_long mask   = maskOf(bits);
    const m_long v      = value & mask;
    const bool out      = count <= bits ? ((v >> (bits - count)) & 1u) != 0 : false;
    const m_long result = count >= bits ? 0u : ((v << count) & mask);
    cpu.setNZVCX(result, signOf(bits), false, out, out);
    return result;
}

inline m_long lsrValue(CPU68K &cpu, m_long value, unsigned count, unsigned bits) {
    const m_long mask   = maskOf(bits);
    const m_long v      = value & mask;
    const bool out      = ((v >> (count - 1u)) & 1u) != 0;
    const m_long result = count >= bits ? 0u : (v >> count);
    cpu.setNZVCX(result, signOf(bits), false, out, out);
    return result;
}

inline m_long asrValue(CPU68K &cpu, m_long value, unsigned count, unsigned bits) {
    const m_long mask   = maskOf(bits);
    const m_long v      = value & mask;
    const std::int64_t signedValue = (v & signOf(bits)) ? static_cast<std::int64_t>(v) - (static_cast<std::int64_t>(mask) + 1) : static_cast<std::int64_t>(v);
    const bool out      = ((signedValue >> (count - 1u)) & 1) != 0;
    const m_long result = static_cast<m_long>(signedValue >> count) & mask;
    cpu.setNZVCX(result, signOf(bits), false, out, out);
    return result;
}

// bset / bclr / bchg / btst on a byte in memory: only Z changes (set when the bit was clear).
inline bool testBitMem(CPU68K &cpu, SystemMemory &mem, m_long address, unsigned bit) {
    const bool set = ((mem.readByte(address) >> bit) & 1u) != 0;
    cpu.setFlag(CPU68K::FlagZ, !set);
    return set;
}

inline bool clearBitMem(CPU68K &cpu, SystemMemory &mem, m_long address, unsigned bit) {
    const m_byte value = mem.readByte(address);
    const bool set     = ((value >> bit) & 1u) != 0;
    cpu.setFlag(CPU68K::FlagZ, !set);
    mem.writeByte(address, static_cast<m_byte>(value & ~(1u << bit)));
    return set;
}

inline bool setBitMem(CPU68K &cpu, SystemMemory &mem, m_long address, unsigned bit) {
    const m_byte value = mem.readByte(address);
    const bool set     = ((value >> bit) & 1u) != 0;
    cpu.setFlag(CPU68K::FlagZ, !set);
    mem.writeByte(address, static_cast<m_byte>(value | (1u << bit)));
    return set;
}

} // namespace sorops

// Soft 68000 jsr/bsr into another routine: push the return PC, call, and leave this routine if the callee
// unwound past it (the same contract as CALL in generated code).
#define SOR_CALL(fn, retPc, target)                                              \
    do {                                                                         \
        const m_long spBefore_ = cpu().ssp;                                      \
        logCallFromReturn((retPc), (target));                                    \
        cpu().ssp -= 4;                                                          \
        memory().writeLong(cpu().ssp, (retPc));                                  \
        (fn)();                                                                  \
        if ((cpu().ssp & 0x00FFFFFFu) > (spBefore_ & 0x00FFFFFFu))               \
            return;                                                              \
    } while (0)
