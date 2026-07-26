#include "Sor.hpp"
#include "SoRTransitions.hpp"

namespace {

constexpr m_long kPaletteFadeCounter = 0xFFFFFB0Cu;
constexpr m_long kTargetPaletteBuffer = 0xFFFFF480u;
constexpr m_long kActivePalette = 0xFFFFF480u;
constexpr m_long kSpriteAttributeTableBuffer = 0xFFDA00u;

constexpr m_long signExtendWord(m_word value) {
    return static_cast<m_long>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

} // namespace

// A hand-written equivalent of generated/Sor.cpp's CALL/CALL_DISPATCH.
#define SOR_CALL_68K(expression, returnPc)                                                                             \
    do {                                                                                                               \
        const m_long sorCallSp = cpu().ssp;                                                                            \
        cpu().ssp -= 4;                                                                                                \
        memory().writeLong(cpu().ssp, static_cast<m_long>(returnPc));                                                  \
        expression;                                                                                                    \
        if ((cpu().ssp & 0x00FFFFFFu) > (sorCallSp & 0x00FFFFFFu))                                                     \
            return;                                                                                                    \
    } while (false)

void StreetsOfRage::clear_vram_and_fade_out() {
    traceEnter(0xCustom1);
    
    // Set fade counter to start fade out
    memory().writeWord(kPaletteFadeCounter, 0x40);
    
    // Set fade out flag
    memory().writeByte(0xFFFA71u, 1);
    
    // Process fade out steps
    while (memory().readWord(kPaletteFadeCounter) != 0) {
        const m_word fadeCounter = memory().readWord(kPaletteFadeCounter);
        const m_byte component = static_cast<m_byte>(fadeCounter & 0x03u);
        
        // Process all 64 palette entries
        for (int i = 0; i < 64; ++i) {
            const m_word paletteEntry = memory().readWord(kActivePalette + i * 2);
            m_word newEntry = paletteEntry;
            
            // Darken the selected RGB component
            switch (component) {
                case 0: { // Red component (bits 4-7)
                    m_byte r = static_cast<m_byte>((paletteEntry >> 4) & 0x0F);
                    if (r > 0) {
                        r = static_cast<m_byte>(r - 1);
                    }
                    newEntry = static_cast<m_word>((paletteEntry & 0x0F0F) | (static_cast<m_word>(r) << 4));
                    break;
                }
                case 1: { // Green component (bits 8-11)
                    m_byte g = static_cast<m_byte>((paletteEntry >> 8) & 0x0F);
                    if (g > 0) {
                        g = static_cast<m_byte>(g - 1);
                    }
                    newEntry = static_cast<m_word>((paletteEntry & 0xF0FF) | (static_cast<m_word>(g) << 8));
                    break;
                }
                case 2: { // Blue component (bits 0-3)
                    m_byte b = static_cast<m_byte>(paletteEntry & 0x0F);
                    if (b > 0) {
                        b = static_cast<m_byte>(b - 1);
                    }
                    newEntry = static_cast<m_word>((paletteEntry & 0xFFF0) | b);
                    break;
                }
            }
            
            memory().writeWord(kActivePalette + i * 2, newEntry);
        }
        
        // Decrement fade counter
        memory().writeWord(kPaletteFadeCounter, static_cast<m_word>(memory().readWord(kPaletteFadeCounter) - 1));
        
        // Wait a frame
        SOR_CALL_68K(wait_vblank_without_graphics_upload(), 0xCustom2);
    }
    
    // Now clear all VRAM using DMA fill
    // Use the vdp_fill_stride function at 0x808C
    // Parameters: d0=VDP address, d1=columns, d2=rows, d4=fill word
    cpu().d[0] = 0x00000000u; // VDP address (VRAM start)
    cpu().d[1] = 0x0040u;    // Width in words (64 tiles per row)
    cpu().d[2] = 0x0200u;    // Height in rows (512 rows for full VRAM)
    cpu().d[4] = 0x0000u;    // Fill word (0 = blank tile)
    SOR_CALL_68K(dispatch(0x0000808Cu), 0xCustom3);
    
    // Clear the palette to black
    for (int i = 0; i < 64; ++i) {
        memory().writeWord(kActivePalette + i * 2, 0x0000u); // Black
    }
    
    // Clear sprite attribute table buffer in RAM
    for (int i = 0; i < 80; ++i) {
        memory().writeLong(kSpriteAttributeTableBuffer + i * 8, 0x00000000u);
    }
    
    cpu().ssp += 4;
}

void StreetsOfRage::fade_in_from_black() {
    traceEnter(0xCustom4);
    
    // Set fade counter for fade in
    memory().writeWord(kPaletteFadeCounter, 0x40);
    
    // Process fade in steps
    while (memory().readWord(kPaletteFadeCounter) != 0) {
        const m_word fadeCounter = memory().readWord(kPaletteFadeCounter);
        const m_byte component = static_cast<m_byte>(fadeCounter & 0x03u);
        
        for (int i = 0; i < 64; ++i) {
            const m_word currentEntry = memory().readWord(kActivePalette + i * 2);
            const m_word targetEntry = memory().readWord(kTargetPaletteBuffer + i * 2);
            
            m_word newEntry = currentEntry;
            
            // Brighten the selected RGB component towards target
            switch (component) {
                case 0: { // Red
                    m_byte currentR = static_cast<m_byte>((currentEntry >> 4) & 0x0F);
                    m_byte targetR = static_cast<m_byte>((targetEntry >> 4) & 0x0F);
                    if (currentR < targetR) {
                        currentR = static_cast<m_byte>(currentR + 1);
                    }
                    newEntry = static_cast<m_word>((currentEntry & 0xFF0F) | (static_cast<m_word>(currentR) << 4));
                    break;
                }
                case 1: { // Green
                    m_byte currentG = static_cast<m_byte>((currentEntry >> 8) & 0x0F);
                    m_byte targetG = static_cast<m_byte>((targetEntry >> 8) & 0x0F);
                    if (currentG < targetG) {
                        currentG = static_cast<m_byte>(currentG + 1);
                    }
                    newEntry = static_cast<m_word>((currentEntry & 0xF0FF) | (static_cast<m_word>(currentG) << 8));
                    break;
                }
                case 2: { // Blue
                    m_byte currentB = static_cast<m_byte>(currentEntry & 0x0F);
                    m_byte targetB = static_cast<m_byte>(targetEntry & 0x0F);
                    if (currentB < targetB) {
                        currentB = static_cast<m_byte>(currentB + 1);
                    }
                    newEntry = static_cast<m_word>((currentEntry & 0xFFF0) | currentB);
                    break;
                }
            }
            
            memory().writeWord(kActivePalette + i * 2, newEntry);
        }
        
        // Decrement fade counter
        memory().writeWord(kPaletteFadeCounter, static_cast<m_word>(memory().readWord(kPaletteFadeCounter) - 1));
        
        // Wait a frame
        SOR_CALL_68K(wait_vblank_without_graphics_upload(), 0xCustom5);
    }
    
    cpu().ssp += 4;
}

#undef SOR_CALL_68K
