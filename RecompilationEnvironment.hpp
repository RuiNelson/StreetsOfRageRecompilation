#pragma once

#include "CPU68K.hpp"
#include "MegaDriveEnvironment.hpp"

/**
 * @file RecompilationEnvironment.hpp
 * @brief MegaDriveEnvironment plus a 68000 register file for recompiled ROMs.
 *
 * Native Mega Drive games subclass MegaDriveEnvironment directly and never see
 * CPU68K. Mechanically recompiled cartridges (Sor) inherit this type so the
 * register file, power-on SR reset, interrupt mask, and dispatch diagnostics
 * stay in the recompilation project.
 */
class RecompilationEnvironment : public MegaDriveEnvironment {
    public:
    using MegaDriveEnvironment::MegaDriveEnvironment;

    protected:
    /// 68000 register file: D0–D7, A0–A6, SSP, USP, PC and SR.
    CPU68K &cpu() {
        return cpu_;
    }
    const CPU68K &cpu() const {
        return cpu_;
    }

    int cpuInterruptMask() const override {
        return cpu_.interruptMask();
    }

    void onPowerOn() override {
        cpu_ = CPU68K{};
        cpu_.setStatus(0x2700);
    }

    void dumpUnhandledDispatchCpuState() override;

    private:
    CPU68K cpu_;
};
