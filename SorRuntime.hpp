#pragma once

#include "Sor.hpp"

/// Hand-written host integration for the generated Streets of Rage cartridge.
/// Kept outside generated/ so --full regeneration preserves debug features.
class SorRuntime final : public Sor {
    public:
    using Sor::Sor;

    protected:
    void handleOptionHotkey(OptionHotkeyCode keyCode) override;
};
