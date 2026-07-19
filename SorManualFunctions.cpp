#include "Sor.hpp"
#include "SorCheats.hpp"

#include <cstdio>

namespace {

constexpr m_long kVBlankMailbox = 0xFFFFFA00u;

template <typename T> constexpr m_byte BYTE(T value) {
    return static_cast<m_byte>(value);
}

template <typename T> constexpr m_word WORD(T value) {
    return static_cast<m_word>(value);
}

template <typename T> constexpr m_long LONG(T value) {
    return static_cast<m_long>(value);
}

} // namespace

void Sor::sub_0041ea(m_long entry_) {
    // Manual reimplementation of the punch-damage path. IRQs may wait until
    // the whole routine finishes; no per-instruction service/pace.
    traceEnter(0x41EAu);

    auto addByteToD0 = [this](m_byte source) {
        const m_byte destination = BYTE(cpu().d[0] & 0xFFu);
        const m_word sum         = WORD(destination) + WORD(source);
        const m_byte result      = BYTE(sum);
        const bool   carry       = (sum & 0x100u) != 0;
        const bool   overflow    = ((~(destination ^ source) & (destination ^ result)) & 0x80u) != 0;
        cpu().setNZVCX(result, 0x80u, overflow, carry, carry);
        cpu().d[0] = LONG((cpu().d[0] & 0xFFFFFF00u) | LONG(result));
    };

    switch (entry_) {
        case 0x41EEu:
            goto L0041ee;
        case 0x41F2u:
            goto L0041f2;
        case 0x41F4u:
            goto L0041f4;
        case 0x41F8u:
            goto L0041f8;
        case 0x41FCu:
            goto L0041fc;
        case 0x4200u:
            goto L004200;
        case 0x4204u:
            goto L004204;
        case 0x4208u:
            goto L004208;
        case 0x420Cu:
            goto L00420c;
        case 0x4210u:
            goto L004210;
        case 0x4212u:
            goto L004212;
        case 0x4214u:
            goto L004214;
        case 0x4218u:
            goto L004218;
        case 0x421Cu:
            goto L00421c;
        case 0x421Eu:
            goto L00421e;
        case 0x4220u:
            goto L004220;
        case 0x4222u:
            goto L004222;
        case 0x4224u:
            goto L004224;
        case 0x4228u:
            goto L004228;
        case 0x422Eu:
            goto L00422e;
        case 0x4230u:
            goto L004230;
        case 0x4236u:
            goto L004236;
        case 0x423Au:
            goto L00423a;
        default:
            break;
    }

    // $0041EA lea.l $00423C, a1
    cpu().a[1] = LONG(0x423Cu);

L0041ee:
    // $0041EE move.w $8(a0), d0
    {
        const m_word value = memory().readWord(cpu().a[0] + 8);
        cpu().d[0]         = LONG((cpu().d[0] & 0xFFFF0000u) | LONG(value));
        cpu().setNZClearVC(value, 0x8000u);
    }

L0041f2:
    // $0041F2 lsr.w #$1, d0
    {
        m_long     value = LONG(WORD(cpu().d[0] & 0xFFFFu));
        const bool carry = (value & 1u) != 0;
        value >>= 1;
        cpu().setVCX(false, carry, carry);
        cpu().setNZ(value, 0x8000u);
        cpu().d[0] = LONG((cpu().d[0] & 0xFFFF0000u) | LONG(WORD(value)));
    }

L0041f4:
    // $0041F4 andi.b #$00FE, d0
    {
        const m_byte value = BYTE(cpu().d[0] & 0xFFu) & 0xFEu;
        cpu().setNZClearVC(value, 0x80u);
        cpu().d[0] = LONG((cpu().d[0] & 0xFFFFFF00u) | LONG(value));
    }

L0041f8:
    // $0041F8 adda.w $0(a1,d0.w), a1
    {
        const auto   index  = static_cast<int32_t>(static_cast<int16_t>(WORD(cpu().d[0] & 0xFFFFu)));
        const m_word offset = memory().readWord(cpu().a[1] + LONG(index));
        cpu().a[1]          = LONG(cpu().a[1] + LONG(static_cast<int32_t>(static_cast<int16_t>(offset))));
    }

L0041fc:
    // $0041FC move.b $50(a0), d0
    {
        const m_byte value = memory().readByte(cpu().a[0] + 80);
        cpu().d[0]         = LONG((cpu().d[0] & 0xFFFFFF00u) | LONG(value));
        cpu().setNZClearVC(value, 0x80u);
    }

L004200:
    // $004200 move.b $0(a1,d0.w), d0
    {
        const auto   index = static_cast<int32_t>(static_cast<int16_t>(WORD(cpu().d[0] & 0xFFFFu)));
        const m_byte value = memory().readByte(cpu().a[1] + LONG(index));
        cpu().d[0]         = LONG((cpu().d[0] & 0xFFFFFF00u) | LONG(value));
        cpu().setNZClearVC(value, 0x80u);
    }

L004204:
    // $004204 add.b $A(a0), d0
    addByteToD0(memory().readByte(cpu().a[0] + 10));

L004208:
    // $004208 move.b $3(a1,d0.w), d0
    {
        const auto   index = static_cast<int32_t>(static_cast<int16_t>(WORD(cpu().d[0] & 0xFFFFu)));
        const m_byte value = memory().readByte(cpu().a[1] + 3 + LONG(index));
        cpu().d[0]         = LONG((cpu().d[0] & 0xFFFFFF00u) | LONG(value));
        cpu().setNZClearVC(value, 0x80u);
    }

L00420c:
    // $00420C tst.b $00FA43
    {
        const m_byte value = memory().readByte(0xFFFFFA43u);
        cpu().setNZClearVC(value, 0x80u);
    }

L004210:
    // $004210 beq.s $004224
    if (cpu().condition(7))
        goto L004224;

L004212:
    // $004212 move.b d0, d1
    {
        const m_byte value = BYTE(cpu().d[0] & 0xFFu);
        cpu().d[1]         = LONG((cpu().d[1] & 0xFFFFFF00u) | LONG(value));
        cpu().setNZClearVC(value, 0x80u);
    }

L004214:
    // $004214 andi.b #$00F0, d1
    {
        const m_byte value = BYTE(cpu().d[1] & 0xFFu) & 0xF0u;
        cpu().setNZClearVC(value, 0x80u);
        cpu().d[1] = LONG((cpu().d[1] & 0xFFFFFF00u) | LONG(value));
    }

L004218:
    // $004218 andi.b #$000F, d0
    {
        const m_byte value = BYTE(cpu().d[0] & 0xFFu) & 0x0Fu;
        cpu().setNZClearVC(value, 0x80u);
        cpu().d[0] = LONG((cpu().d[0] & 0xFFFFFF00u) | LONG(value));
    }

L00421c:
    // $00421C move.b d0, d2
    {
        const m_byte value = BYTE(cpu().d[0] & 0xFFu);
        cpu().d[2]         = LONG((cpu().d[2] & 0xFFFFFF00u) | LONG(value));
        cpu().setNZClearVC(value, 0x80u);
    }

L00421e:
    // $00421E add.b d0, d0
    addByteToD0(BYTE(cpu().d[0] & 0xFFu));

L004220:
    // $004220 add.b d2, d0
    addByteToD0(BYTE(cpu().d[2] & 0xFFu));

L004222:
    // $004222 add.b d1, d0
    addByteToD0(BYTE(cpu().d[1] & 0xFFu));

L004224:
    // $004224 move.b d0, $34(a0)
    {
        const m_byte value = BYTE(cpu().d[0] & 0xFFu);
        memory().writeByte(cpu().a[0] + 52, value);
        cpu().setNZClearVC(value, 0x80u);
    }

L004228:
    // $004228 andi.b #$000F, $34(a0)
    {
        const m_long damageAddress = cpu().a[0] + 52;
        const m_byte baseDamage    = memory().readByte(damageAddress) & 0x0Fu;
        cpu().setNZClearVC(baseDamage, 0x80u);
        memory().writeByte(damageAddress, baseDamage);

        // Host-only cheat: change only P1's damage nibble. The upper nibble in
        // d0 still drives the game's normal hit reaction at object+$42.
        const m_byte adjustedDamage =
            SorCheats::adjustP1PunchDamage(cpu().a[0], baseDamage, SorCheats::p1PunchPowerEnabled());
        if (adjustedDamage != baseDamage)
            memory().writeByte(damageAddress, adjustedDamage);
    }

L00422e:
    // $00422E lsr.b #$4, d0
    {
        m_long value = LONG(BYTE(cpu().d[0] & 0xFFu));
        bool   carry = false;
        for (int i = 0; i < 4; ++i) {
            carry = (value & 1u) != 0;
            value >>= 1;
        }
        cpu().setVCX(false, carry, carry);
        cpu().setNZ(value, 0x80u);
        cpu().d[0] = LONG((cpu().d[0] & 0xFFFFFF00u) | LONG(BYTE(value)));
    }

L004230:
    // $004230 andi.b #$00FE, $42(a0)
    {
        const m_long address = cpu().a[0] + 66;
        const m_byte value   = memory().readByte(address) & 0xFEu;
        cpu().setNZClearVC(value, 0x80u);
        memory().writeByte(address, value);
    }

L004236:
    // $004236 or.b d0, $42(a0)
    {
        const m_long address = cpu().a[0] + 66;
        const m_byte value   = memory().readByte(address) | BYTE(cpu().d[0] & 0xFFu);
        cpu().setNZClearVC(value, 0x80u);
        memory().writeByte(address, value);
    }

L00423a:
    // $00423A rts
    cpu().ssp += 4;
    if ((cpu().ssp & 0x00FFFFFFu) > 0x00FFFF00u) {
        std::fprintf(stderr,
                     "[RTS] ssp=$%06X fn=$%06X\n",
                     static_cast<unsigned>(cpu().ssp & 0x00FFFFFFu),
                     static_cast<unsigned>(lastFunction() & 0x00FFFFFFu));
    }
}

void Sor::sync_z80_1(m_long entry_) {
    // Wait for VBlank via $FFFA00. IRQs only need servicing inside the wait
    // loop; they can lag slightly around the setup/teardown.
    traceEnter(0x00010502u);
    (void)entry_;

    constexpr m_byte command = 1;
    memory().writeByte(kVBlankMailbox, command);
    cpu().setNZClearVC(command, 0x80u);
    cpu().setStatus(0x2500u);

    const m_byte finalValue = memory().waitForByteValue(kVBlankMailbox, 0, [this] {
        waitForInterrupt();
        if (irqLevel() > cpu().interruptMask())
            serviceIRQ();
        return !shouldQuit();
    });
    cpu().setNZClearVC(finalValue, 0x80u);

    cpu().ssp += 4;
    if ((cpu().ssp & 0x00FFFFFFu) > 0x00FFFF00u) {
        std::fprintf(stderr,
                     "[RTS] ssp=$%06X fn=$%06X\n",
                     static_cast<unsigned>(cpu().ssp & 0x00FFFFFFu),
                     static_cast<unsigned>(lastFunction() & 0x00FFFFFFu));
    }
}

void Sor::sync_z80_2(m_long entry_) {
    // Wait for VBlank via $FFFA00. IRQs only need servicing inside the wait
    // loop; they can lag slightly around the setup/teardown.
    traceEnter(0x00010514u);
    (void)entry_;

    constexpr m_byte command = 2;
    memory().writeByte(kVBlankMailbox, command);
    cpu().setNZClearVC(command, 0x80u);
    cpu().setStatus(0x2500u);

    const m_byte finalValue = memory().waitForByteValue(kVBlankMailbox, 0, [this] {
        waitForInterrupt();
        if (irqLevel() > cpu().interruptMask())
            serviceIRQ();
        return !shouldQuit();
    });
    cpu().setNZClearVC(finalValue, 0x80u);

    cpu().ssp += 4;
    if ((cpu().ssp & 0x00FFFFFFu) > 0x00FFFF00u) {
        std::fprintf(stderr,
                     "[RTS] ssp=$%06X fn=$%06X\n",
                     static_cast<unsigned>(cpu().ssp & 0x00FFFFFFu),
                     static_cast<unsigned>(lastFunction() & 0x00FFFFFFu));
    }
}
