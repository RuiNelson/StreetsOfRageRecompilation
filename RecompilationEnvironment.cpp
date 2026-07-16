/**
 * @file RecompilationEnvironment.cpp
 * @brief 68000-register diagnostics for recompiled cartridge dispatch failures.
 */

#include "RecompilationEnvironment.hpp"

#include <cstdio>

void RecompilationEnvironment::dumpUnhandledDispatchCpuState() {
    std::fprintf(stderr,
                 "[dispatch] SR=$%04X SSP=$%06X USP=$%06X PC=$%06X\n",
                 static_cast<unsigned>(cpu_.status()),
                 static_cast<unsigned>(cpu_.ssp & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu_.usp & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu_.pc & 0x00FFFFFFu));
    std::fprintf(stderr,
                 "[dispatch] D0=$%08X D1=$%08X D2=$%08X D3=$%08X "
                 "D4=$%08X D5=$%08X D6=$%08X D7=$%08X\n",
                 static_cast<unsigned>(cpu_.d[0]),
                 static_cast<unsigned>(cpu_.d[1]),
                 static_cast<unsigned>(cpu_.d[2]),
                 static_cast<unsigned>(cpu_.d[3]),
                 static_cast<unsigned>(cpu_.d[4]),
                 static_cast<unsigned>(cpu_.d[5]),
                 static_cast<unsigned>(cpu_.d[6]),
                 static_cast<unsigned>(cpu_.d[7]));
    std::fprintf(stderr,
                 "[dispatch] A0=$%06X A1=$%06X A2=$%06X A3=$%06X "
                 "A4=$%06X A5=$%06X A6=$%06X\n",
                 static_cast<unsigned>(cpu_.a[0] & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu_.a[1] & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu_.a[2] & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu_.a[3] & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu_.a[4] & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu_.a[5] & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu_.a[6] & 0x00FFFFFFu));

    const m_long a0 = cpu_.a[0] & 0x00FFFFFFu;
    std::fprintf(stderr,
                 "[dispatch] object@A0 type=%02X flags=%02X state30=%02X next31=%02X "
                 "ptr4=%08X anim8=%04X timerE=%04X stack=%08X %08X %08X\n",
                 static_cast<unsigned>(memory().readByte(a0 + 0)),
                 static_cast<unsigned>(memory().readByte(a0 + 1)),
                 static_cast<unsigned>(memory().readByte(a0 + 0x30)),
                 static_cast<unsigned>(memory().readByte(a0 + 0x31)),
                 static_cast<unsigned>(memory().readLong(a0 + 4)),
                 static_cast<unsigned>(memory().readWord(a0 + 8)),
                 static_cast<unsigned>(memory().readWord(a0 + 0x0E)),
                 static_cast<unsigned>(memory().readLong(cpu_.ssp)),
                 static_cast<unsigned>(memory().readLong(cpu_.ssp + 4)),
                 static_cast<unsigned>(memory().readLong(cpu_.ssp + 8)));
}
