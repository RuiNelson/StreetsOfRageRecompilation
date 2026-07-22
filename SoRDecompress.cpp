#include "Sor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaxDecodedBytes = 1u << 20;

constexpr m_long kArtQueue                 = 0xFFFFDCD0u;
constexpr m_long kArtVramDestination       = 0xFFFFDCD4u;
constexpr m_long kIncrementalWriter        = 0xFFFFDD10u;
constexpr m_long kIncrementalScratch0      = 0xFFFFDD14u;
constexpr m_long kIncrementalScratch1      = 0xFFFFDD18u;
constexpr m_long kIncrementalXorRow        = 0xFFFFDD1Cu;
constexpr m_long kIncrementalBitBuffer     = 0xFFFFDD20u;
constexpr m_long kIncrementalBitsRemaining = 0xFFFFDD24u;
constexpr m_long kIncrementalTiles         = 0xFFFFDD28u;
constexpr m_long kIncrementalTileBudget    = 0xFFFFDD2Au;

constexpr m_long kLevel                  = 0xFFFFFF02u;
constexpr m_long kLevel3AnimationFlag    = 0xFFFFFA0Cu;
constexpr m_long kLevel3AnimationTimers  = 0xFFFFFA0Au;
constexpr m_long kLevel3AnimationCounter = 0xFFFFFB1Au;

constexpr m_long      kZ80DacDriverSource = 0x000795A2u;
constexpr std::size_t kZ80DriverCopyBytes = 0x1EC7u;

struct DecodeResult {
    std::vector<m_byte> data;
    m_long              sourceEnd{};
};

struct NemesisResult : DecodeResult {
    m_word tileCount{};
    m_word initialBitBuffer{};
    m_long initialPayloadCursor{};
    bool   xorMode{};
};

template <typename ReadByte> class ByteReader {
    public:
    ByteReader(ReadByte readByte, m_long position) : readByte_(std::move(readByte)), position_(position) {
    }

    m_byte readU8() {
        const m_byte value = readByte_(position_);
        ++position_;
        return value;
    }

    m_word readBE16() {
        return static_cast<m_word>((static_cast<m_word>(readU8()) << 8) | readU8());
    }

    m_word readLE16() {
        const m_word low = readU8();
        return static_cast<m_word>(low | (static_cast<m_word>(readU8()) << 8));
    }

    m_long position() const {
        return position_;
    }

    private:
    ReadByte readByte_;
    m_long   position_;
};

template <typename Reader> class MsbBitReader {
    public:
    MsbBitReader(Reader &reader, std::uint64_t buffer = 0, unsigned bits = 0)
        : reader_(reader), buffer_(buffer), bits_(bits) {
    }

    std::uint32_t peek(unsigned count) {
        fill(count);
        return static_cast<std::uint32_t>((buffer_ >> (bits_ - count)) & mask(count));
    }

    std::uint32_t read(unsigned count) {
        if (count == 0)
            return 0;
        const std::uint32_t value = peek(count);
        bits_ -= count;
        buffer_ &= mask(bits_);
        return value;
    }

    private:
    static std::uint64_t mask(unsigned bits) {
        return bits == 0 ? 0 : ((std::uint64_t{1} << bits) - 1);
    }

    void fill(unsigned count) {
        while (bits_ < count) {
            buffer_ = (buffer_ << 8) | reader_.readU8();
            bits_ += 8;
        }
    }

    Reader       &reader_;
    std::uint64_t buffer_{};
    unsigned      bits_{};
};

template <typename Reader> class KosinskiDescriptorReader {
    public:
    explicit KosinskiDescriptorReader(Reader &reader) : reader_(reader), descriptor_(reader.readLE16()) {
    }

    unsigned read() {
        const unsigned bit = descriptor_ & 1u;
        descriptor_ >>= 1;
        if (--remaining_ == 0) {
            // The cartridge eagerly fetches the next descriptor before reading
            // the token bytes belonging to the sixteenth bit.
            descriptor_ = reader_.readLE16();
            remaining_  = 16;
        }
        return bit;
    }

    private:
    Reader  &reader_;
    m_word   descriptor_{};
    unsigned remaining_ = 16;
};

void ensureOutputFits(std::size_t size) {
    if (size > kMaxDecodedBytes)
        throw std::runtime_error("decompressed output exceeds the host buffer limit");
}

void appendWord(std::vector<m_byte> &output, m_word value) {
    ensureOutputFits(output.size() + 2);
    output.push_back(static_cast<m_byte>(value >> 8));
    output.push_back(static_cast<m_byte>(value));
}

void appendLong(std::vector<m_byte> &output, m_long value) {
    ensureOutputFits(output.size() + 4);
    output.push_back(static_cast<m_byte>(value >> 24));
    output.push_back(static_cast<m_byte>(value >> 16));
    output.push_back(static_cast<m_byte>(value >> 8));
    output.push_back(static_cast<m_byte>(value));
}

template <typename ReadByte> NemesisResult decodeNemesis(ReadByte readByte, m_long source) {
    ByteReader reader(std::move(readByte), source);

    const m_word header    = reader.readBE16();
    const bool   xorMode   = (header & 0x8000u) != 0;
    const m_word tileCount = header & 0x7FFFu;
    ensureOutputFits(static_cast<std::size_t>(tileCount) * 32u);

    struct TableEntry {
        m_byte length{};
        m_byte token{};
        bool   valid{};
    };
    std::array<TableEntry, 256> table{};

    m_byte group = reader.readU8();
    while (group != 0xFFu) {
        const m_byte pixel = group & 0x0Fu;
        for (;;) {
            const m_byte descriptor = reader.readU8();
            if (descriptor >= 0x80u) {
                group = descriptor;
                break;
            }

            const unsigned length = descriptor & 0x0Fu;
            if (length == 0 || length > 8)
                throw std::runtime_error("invalid Nemesis prefix length");

            const m_byte token = static_cast<m_byte>((descriptor & 0x70u) | pixel);
            const m_byte code  = reader.readU8();
            if (code >= (1u << length))
                throw std::runtime_error("invalid Nemesis prefix code");

            const unsigned first = static_cast<unsigned>(code) << (8u - length);
            const unsigned count = 1u << (8u - length);
            for (unsigned index = first; index < first + count; ++index) {
                if (table[index].valid)
                    throw std::runtime_error("overlapping Nemesis prefix code");
                table[index] = {static_cast<m_byte>(length), token, true};
            }
        }
    }

    const m_word initialBits          = reader.readBE16();
    const m_long initialPayloadCursor = reader.position();
    MsbBitReader bits(reader, initialBits, 16);

    std::vector<m_byte> output;
    output.reserve(static_cast<std::size_t>(tileCount) * 32u);

    std::size_t rowsRemaining = static_cast<std::size_t>(tileCount) * 8u;
    m_long      previousRow{};
    m_long      row{};
    unsigned    nibbles{};

    while (rowsRemaining != 0) {
        m_byte         token{};
        const unsigned prefix = bits.peek(8);
        if (prefix >= 0xFCu) {
            bits.read(6);
            token = static_cast<m_byte>(bits.read(7));
        } else {
            const TableEntry entry = table[prefix];
            if (!entry.valid)
                throw std::runtime_error("undefined Nemesis prefix code");
            bits.read(entry.length);
            token = entry.token;
        }

        const m_byte value = token & 0x0Fu;
        for (unsigned repeat = (token >> 4) + 1u; repeat != 0; --repeat) {
            row = (row << 4) | value;
            if (++nibbles != 8)
                continue;

            if (xorMode) {
                row ^= previousRow;
                previousRow = row;
            }
            appendLong(output, row);
            --rowsRemaining;
            row     = 0;
            nibbles = 0;
            if (rowsRemaining == 0)
                break;
        }
    }

    NemesisResult result;
    result.data                 = std::move(output);
    result.sourceEnd            = reader.position();
    result.tileCount            = tileCount;
    result.initialBitBuffer     = initialBits;
    result.initialPayloadCursor = initialPayloadCursor;
    result.xorMode              = xorMode;
    return result;
}

template <typename ReadByte> DecodeResult decodeEnigma(ReadByte readByte, m_long source, m_word baseTile) {
    ByteReader reader(std::move(readByte), source);

    const unsigned indexBits     = reader.readU8();
    const m_byte   attributeMask = reader.readU8();
    if (indexBits > 16)
        throw std::runtime_error("invalid Enigma tile-index width");

    m_word       incrementing = static_cast<m_word>(reader.readBE16() + baseTile);
    const m_word common       = static_cast<m_word>(reader.readBE16() + baseTile);
    MsbBitReader bits(reader);

    std::vector<m_byte> output;

    const auto inlineWord = [&]() {
        m_word                          word = baseTile;
        constexpr std::array<m_byte, 5> maskBits{0x10u, 0x08u, 0x04u, 0x02u, 0x01u};
        constexpr std::array<m_word, 5> tileBits{0x8000u, 0x4000u, 0x2000u, 0x1000u, 0x0800u};
        for (std::size_t i = 0; i < maskBits.size(); ++i) {
            if ((attributeMask & maskBits[i]) == 0 || bits.read(1) == 0)
                continue;
            if (tileBits[i] == 0x4000u || tileBits[i] == 0x2000u)
                word = static_cast<m_word>(word + tileBits[i]);
            else
                word |= tileBits[i];
        }
        if (indexBits != 0)
            word = static_cast<m_word>(word + bits.read(indexBits));
        return word;
    };

    for (;;) {
        const unsigned opcode = bits.read(1) == 0 ? bits.read(1) : 4u + bits.read(2);
        const unsigned count  = bits.read(4);
        if (opcode == 7u && count == 0x0Fu) {
            if ((reader.position() & 1u) != 0)
                reader.readU8();
            return {std::move(output), reader.position()};
        }

        const unsigned run = count + 1u;
        switch (opcode) {
            case 0:
                for (unsigned i = 0; i < run; ++i) {
                    appendWord(output, incrementing);
                    ++incrementing;
                }
                break;
            case 1:
                for (unsigned i = 0; i < run; ++i)
                    appendWord(output, common);
                break;
            case 4: {
                const m_word word = inlineWord();
                for (unsigned i = 0; i < run; ++i)
                    appendWord(output, word);
                break;
            }
            case 5: {
                m_word word = inlineWord();
                for (unsigned i = 0; i < run; ++i) {
                    appendWord(output, word);
                    ++word;
                }
                break;
            }
            case 6: {
                m_word word = inlineWord();
                for (unsigned i = 0; i < run; ++i) {
                    appendWord(output, word);
                    --word;
                }
                break;
            }
            case 7:
                for (unsigned i = 0; i < run; ++i)
                    appendWord(output, inlineWord());
                break;
            default:
                throw std::runtime_error("invalid Enigma opcode");
        }
    }
}

template <typename ReadByte> DecodeResult decodeKosinski(ReadByte readByte, m_long source) {
    ByteReader               reader(std::move(readByte), source);
    KosinskiDescriptorReader descriptor(reader);
    std::vector<m_byte>      output;

    const auto copyMatch = [&](int displacement, unsigned length) {
        if (displacement >= 0 || static_cast<std::size_t>(-displacement) > output.size())
            throw std::runtime_error("invalid Kosinski back-reference");
        ensureOutputFits(output.size() + length);
        for (unsigned i = 0; i < length; ++i) {
            const auto sourceIndex = static_cast<std::ptrdiff_t>(output.size()) + displacement;
            output.push_back(output[static_cast<std::size_t>(sourceIndex)]);
        }
    };

    for (;;) {
        if (descriptor.read() != 0) {
            ensureOutputFits(output.size() + 1);
            output.push_back(reader.readU8());
            continue;
        }

        if (descriptor.read() == 0) {
            const unsigned length = ((descriptor.read() << 1) | descriptor.read()) + 2u;
            copyMatch(static_cast<int>(reader.readU8()) - 0x100, length);
            continue;
        }

        const m_byte   low          = reader.readU8();
        const m_byte   high         = reader.readU8();
        const unsigned encoded      = 0xE000u | ((static_cast<unsigned>(high) & 0xF8u) << 5) | low;
        const int      displacement = static_cast<int>(encoded) - 0x10000;
        const unsigned lengthCode   = high & 0x07u;
        if (lengthCode != 0) {
            copyMatch(displacement, lengthCode + 2u);
            continue;
        }

        const m_byte extension = reader.readU8();
        if (extension == 0)
            return {std::move(output), reader.position()};
        if (extension != 1)
            copyMatch(displacement, static_cast<unsigned>(extension) + 1u);
    }
}

void writeToWorkRam(SystemMemory &memory, m_long destination, std::vector<m_byte> &data) {
    if (!data.empty())
        memory.writeFromBuffer(data.data(), destination, static_cast<int>(data.size()));
}

m_long vramWriteCommand(m_word byteAddress) {
    return 0x40000000u | (static_cast<m_long>(byteAddress & 0x3FFFu) << 16) |
           static_cast<m_long>((byteAddress & 0xC000u) >> 14);
}

struct IncrementalNemesisState {
    std::vector<m_byte> decoded;
    std::size_t         uploadedTiles{};
    m_long              sourceEnd{};
    bool                xorMode{};
};

// The 68000 CPU and its IRQ handler execute on one host thread. Keying the
// resumable host buffer by Sor instance keeps independent environments apart.
thread_local std::unordered_map<Sor *, IncrementalNemesisState> incrementalNemesis;

} // namespace

// ---------------------------------------------------------------------------
// $8192/$81A4 — blocking Nemesis to VDP or 68000 work RAM.
// ---------------------------------------------------------------------------
void Sor::nemesisdec_vram(m_long entry_) {
    const bool ramDestination = entry_ == 0x81A4u;
    traceEnter(ramDestination ? 0x81A4u : 0x8192u);

    const auto readByte = [this](m_long address) {
        return memory().readByte(address);
    };
    auto result = decodeNemesis(readByte, cpu().a[0]);

    if (ramDestination) {
        writeToWorkRam(memory(), cpu().a[4], result.data);
    } else {
        for (std::size_t offset = 0; offset < result.data.size(); offset += 4) {
            const m_long row = (static_cast<m_long>(result.data[offset]) << 24) |
                               (static_cast<m_long>(result.data[offset + 1]) << 16) |
                               (static_cast<m_long>(result.data[offset + 2]) << 8) | result.data[offset + 3];
            vdp().writeDataPort(static_cast<m_word>(row >> 16));
            vdp().writeDataPort(static_cast<m_word>(row));
        }
    }

    // The original MOVEM restores every decoder register. Its final MOVE.W
    // observes a zero tile-row counter, leaving Z set and N/V/C clear.
    cpu().setNZClearVC(0, 0x8000u);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $82D2 — copy the plane header, then share the Enigma decoder stack frame.
// ---------------------------------------------------------------------------
void Sor::enigmadec_with_plane_header(m_long /*entry_*/) {
    traceEnter(0x82D2u);

    const auto readByte = [this](m_long address) {
        return memory().readByte(address);
    };
    const m_long source      = cpu().a[0];
    const m_long destination = cpu().a[1];

    std::array<m_byte, 8> header{};
    for (std::size_t i = 0; i < header.size(); ++i)
        header[i] = readByte(source + static_cast<m_long>(i));
    memory().writeFromBuffer(header.data(), destination, static_cast<int>(header.size()));

    auto result = decodeEnigma(readByte, source + 8, cpu().dw(0));
    writeToWorkRam(memory(), destination + 8, result.data);

    cpu().a[0] = result.sourceEnd;
    cpu().a[1] = destination + 8; // Saved by Enigma after the two header longs.
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $82D6, plus the real tail-entry wrappers at $112C0 and $12832.
// ---------------------------------------------------------------------------
void Sor::enigmadec(m_long entry_) {
    traceEnter(entry_);

    if (entry_ == 0x000112C0u) {
        if (memory().readWord(kLevel) != 2u) {
            memory().writeByte(kLevel3AnimationFlag, 0);
            cpu().setNZClearVC(0, 0x80u);
            cpu().ssp += 4;
            return;
        }
        memory().writeByte(kLevel3AnimationFlag, 1);
        memory().writeWord(kLevel3AnimationTimers, 0x0505u);
        memory().writeWord(kLevel3AnimationCounter, 0);
        cpu().d[0] = 0;
        cpu().a[0] = 0x00065976u;
        cpu().a[1] = 0x00FF8000u;
    } else if (entry_ == 0x00012832u) {
        cpu().d[0] = 0;
        cpu().a[0] = 0x00012842u;
        cpu().a[1] = 0xFFFFFF4Au;
    }

    const auto readByte = [this](m_long address) {
        return memory().readByte(address);
    };
    const m_long destination = cpu().a[1];
    auto         result      = decodeEnigma(readByte, cpu().a[0], cpu().dw(0));
    writeToWorkRam(memory(), destination, result.data);

    cpu().a[0] = result.sourceEnd;
    // A1 and D0-D7/A2-A5 are restored by the original MOVEM.
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $84BA — build a host-owned incremental Nemesis stream.
// ---------------------------------------------------------------------------
void Sor::begin_incremental_nemesis_decode(m_long /*entry_*/) {
    traceEnter(0x84BAu);

    const m_long source = memory().readLong(kArtQueue);
    if (source == 0) {
        if (memory().readWord(kIncrementalTiles) == 0)
            incrementalNemesis.erase(this);
        cpu().ssp += 4;
        return;
    }
    if (memory().readWord(kIncrementalTiles) != 0) {
        cpu().ssp += 4;
        return;
    }

    const auto readByte = [this](m_long address) {
        return memory().readByte(address);
    };
    auto result = decodeNemesis(readByte, source);

    IncrementalNemesisState state;
    state.decoded            = std::move(result.data);
    state.sourceEnd          = result.sourceEnd;
    state.xorMode            = result.xorMode;
    state.uploadedTiles      = 0;
    incrementalNemesis[this] = std::move(state);

    const m_long writer = result.xorMode ? 0x0000825Eu : 0x00008254u;
    memory().writeWord(kIncrementalTiles, result.tileCount);
    memory().writeLong(kArtQueue, result.initialPayloadCursor);
    memory().writeLong(kIncrementalWriter, writer);
    memory().writeLong(kIncrementalScratch0, 0);
    memory().writeLong(kIncrementalScratch1, 0);
    memory().writeLong(kIncrementalXorRow, 0);
    memory().writeLong(kIncrementalBitBuffer, result.initialBitBuffer);
    memory().writeLong(kIncrementalBitsRemaining, 16);

    cpu().a[0] = result.initialPayloadCursor;
    cpu().a[1] = 0xFFFFF600u;
    cpu().a[3] = writer;
    cpu().setDw(2, result.tileCount);
    cpu().d[5] = result.initialBitBuffer;
    cpu().d[6] = 16;
    cpu().d[0] = 0;
    cpu().setNZClearVC(cpu().d[6], 0x80000000u);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $8510 — upload at most five decoded tiles through the public VDP ports.
// ---------------------------------------------------------------------------
void Sor::continue_incremental_nemesis_decode(m_long /*entry_*/) {
    traceEnter(0x8510u);

    m_word remaining = memory().readWord(kIncrementalTiles);
    if (remaining == 0) {
        cpu().ssp += 4;
        return;
    }

    const auto stateIt = incrementalNemesis.find(this);
    if (stateIt == incrementalNemesis.end())
        throw std::runtime_error("incremental Nemesis host state is missing");
    auto &state = stateIt->second;

    const m_word destination = memory().readWord(kArtVramDestination);
    const m_long command     = vramWriteCommand(destination);
    vdp().writeControlPort(static_cast<m_word>(command >> 16));
    vdp().writeControlPort(static_cast<m_word>(command));

    m_word budget = 5;
    memory().writeWord(kIncrementalTileBudget, budget);
    unsigned uploadedThisCall = 0;
    while (remaining != 0 && uploadedThisCall < 5) {
        const std::size_t tileOffset = state.uploadedTiles * 32u;
        if (tileOffset + 32u > state.decoded.size())
            throw std::runtime_error("incremental Nemesis tile count exceeds host output");

        for (std::size_t offset = tileOffset; offset < tileOffset + 32u; offset += 2) {
            const m_word word =
                static_cast<m_word>((static_cast<m_word>(state.decoded[offset]) << 8) | state.decoded[offset + 1]);
            vdp().writeDataPort(word);
        }

        ++state.uploadedTiles;
        ++uploadedThisCall;
        --remaining;
        memory().writeWord(kIncrementalTiles, remaining);
        if (remaining == 0)
            break;
        --budget;
        memory().writeWord(kIncrementalTileBudget, budget);
    }

    if (state.xorMode && state.uploadedTiles != 0) {
        const std::size_t lastRow = state.uploadedTiles * 32u - 4u;
        const m_long      row     = (static_cast<m_long>(state.decoded[lastRow]) << 24) |
                                    (static_cast<m_long>(state.decoded[lastRow + 1]) << 16) |
                                    (static_cast<m_long>(state.decoded[lastRow + 2]) << 8) | state.decoded[lastRow + 3];
        memory().writeLong(kIncrementalXorRow, row);
    }

    if (remaining != 0) {
        memory().writeWord(kArtVramDestination, static_cast<m_word>(destination + 0x00A0u));
        // The decoder's bit-level state is host-owned. Keep the RAM cursor
        // nonzero for queue producers and diagnostics while the stream is live.
        memory().writeLong(kArtQueue, state.sourceEnd);
        cpu().ssp += 4;
        return;
    }

    // Shift seven six-byte records plus the zero sentinel over the completed
    // queue head, exactly matching the original twelve-longword copy.
    for (m_long offset = 0; offset < 48; offset += 4)
        memory().writeLong(kArtQueue + offset, memory().readLong(kArtQueue + offset + 6));
    incrementalNemesis.erase(stateIt);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $85A2 — blocking Kosinski to 68000 work RAM.
// ---------------------------------------------------------------------------
void Sor::kosinskidec(m_long /*entry_*/) {
    traceEnter(0x85A2u);

    const auto readByte = [this](m_long address) {
        return memory().readByte(address);
    };
    const m_long destination = cpu().a[1];
    auto         result      = decodeKosinski(readByte, cpu().a[0]);
    writeToWorkRam(memory(), destination, result.data);

    cpu().a[0] = result.sourceEnd;
    cpu().a[1] = destination + static_cast<m_long>(result.data.size());
    cpu().setDb(1, 0); // Terminator extension byte.
    cpu().setNZClearVC(0, 0x80u);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $1061C — decode the DAC driver in host memory and write Z80 RAM directly.
// ---------------------------------------------------------------------------
void Sor::load_z80_dac_driver(m_long /*entry_*/) {
    traceEnter(0x0001061Cu);

    z80().setReset(false);
    z80().setBusRequest(true);
    while (!z80().busRequestAcked() && !shouldQuit())
        std::this_thread::yield();

    if (shouldQuit()) {
        z80().setBusRequest(false);
        cpu().ssp += 4;
        return;
    }

    const auto readByte = [this](m_long address) {
        return memory().readByte(address);
    };
    auto result = decodeKosinski(readByte, kZ80DacDriverSource);
    if (result.data.size() < kZ80DriverCopyBytes)
        throw std::runtime_error("Kosinski DAC driver is shorter than the cartridge copy count");

    for (std::size_t offset = 0; offset < kZ80DriverCopyBytes; ++offset)
        z80().writeRAMFor68K(static_cast<std::uint16_t>(offset), result.data[offset]);

    constexpr std::array<m_byte, 4> sampleBank{0x00u, 0x80u, 0x07u, 0x80u};
    for (std::size_t i = 0; i < sampleBank.size(); ++i)
        z80().writeRAMFor68K(static_cast<std::uint16_t>(0x1FF8u + i), sampleBank[i]);

    z80().setReset(true);
    z80().setReset(false);
    z80().setBusRequest(false);

    cpu().a[0] = result.sourceEnd;
    cpu().a[1] = 0x00A01FFCu;
    cpu().a[2] = 0x00FF7000u + static_cast<m_long>(kZ80DriverCopyBytes);
    cpu().setDw(2, 0xFFFFu);
    cpu().setNZClearVC(0, 0x8000u);
    cpu().ssp += 4;
}
