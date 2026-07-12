#include "Sor.hpp"

#include <cstdio>

namespace {

constexpr m_long kVBlankMailbox = 0xFFFFFA00u;

} // namespace

void Sor::sync_z80_1(m_long entry_) {
    traceEnter(0x00010502u);
    (void)entry_;

    if (irqLevel() > cpu().interruptMask())
        serviceIRQ();
    pace();
    constexpr m_byte command = 1;
    memory().writeByte(kVBlankMailbox, command);
    cpu().setNZClearVC(command, 0x80u);

    if (irqLevel() > cpu().interruptMask())
        serviceIRQ();
    pace();
    cpu().setStatus(0x2500u);

    const m_byte finalValue = memory().waitForByteValue(kVBlankMailbox, 0, [this] {
        waitForInterrupt();
        if (irqLevel() > cpu().interruptMask())
            serviceIRQ();
        return !shouldQuit();
    });
    cpu().setNZClearVC(finalValue, 0x80u);

    if (irqLevel() > cpu().interruptMask())
        serviceIRQ();
    pace();
    cpu().ssp += 4;
    if ((cpu().ssp & 0x00FFFFFFu) > 0x00FFFF00u) {
        std::fprintf(stderr,
                     "[RTS] ssp=$%06X fn=$%06X\n",
                     static_cast<unsigned>(cpu().ssp & 0x00FFFFFFu),
                     static_cast<unsigned>(lastFunction() & 0x00FFFFFFu));
    }
}

void Sor::sync_z80_2(m_long entry_) {
    traceEnter(0x00010514u);
    (void)entry_;

    if (irqLevel() > cpu().interruptMask())
        serviceIRQ();
    pace();
    constexpr m_byte command = 2;
    memory().writeByte(kVBlankMailbox, command);
    cpu().setNZClearVC(command, 0x80u);

    if (irqLevel() > cpu().interruptMask())
        serviceIRQ();
    pace();
    cpu().setStatus(0x2500u);

    const m_byte finalValue = memory().waitForByteValue(kVBlankMailbox, 0, [this] {
        waitForInterrupt();
        if (irqLevel() > cpu().interruptMask())
            serviceIRQ();
        return !shouldQuit();
    });
    cpu().setNZClearVC(finalValue, 0x80u);

    if (irqLevel() > cpu().interruptMask())
        serviceIRQ();
    pace();
    cpu().ssp += 4;
    if ((cpu().ssp & 0x00FFFFFFu) > 0x00FFFF00u) {
        std::fprintf(stderr,
                     "[RTS] ssp=$%06X fn=$%06X\n",
                     static_cast<unsigned>(cpu().ssp & 0x00FFFFFFu),
                     static_cast<unsigned>(lastFunction() & 0x00FFFFFFu));
    }
}
