// Headless FFCC GBA client harness using Dolphin's bundled libmGBA.
//
// This loads a retail ffcc_cli.bin as a GBA multiboot image and exposes the
// four GameCube-side Joybus operations.  It is intentionally independent of
// Dolphin's GameCube emulation so malformed parent transcripts can be replayed
// deterministically.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio.h>
}

namespace
{
constexpr std::uint32_t EWRAM_BASE = 0x02000000;
constexpr std::uint32_t IWRAM_BASE = 0x03000000;
constexpr std::uint32_t G_LINK_STARTED_JP = 0x030029A4;
constexpr std::uint32_t G_RECONNECT_PENDING_JP = 0x03002994;
constexpr std::uint32_t G_WAS_CONNECTED_JP = 0x030029A0;
constexpr std::uint32_t G_SCREEN_JP = 0x03003090;
constexpr std::uint32_t G_MODE_JP = 0x03003094;
constexpr std::uint32_t G_SAVED_SCREEN_JP = 0x030032E8;
constexpr std::uint32_t G_LINK_ESTABLISHED_JP = 0x030032F0;
constexpr std::uint32_t G_SCREEN_PHASE_JP = 0x030032F4;
constexpr std::uint32_t S_LINK_CONNECTED_JP = 0x03000018;
constexpr std::uint32_t S_TX_QUEUE_JP = 0x03000038;
constexpr std::uint32_t S_TX_COUNT_JP = 0x03000138;
constexpr std::uint32_t LINK_WRITE_JP = 0x020014C0;
constexpr std::uint32_t LINK_WRITE_EN = 0x02002698;
constexpr std::uint32_t LINK_SEND_CMAKE_NAME_JP = 0x02001FAC;
constexpr std::uint32_t LINK_SEND_LETTER_REPLY_JP = 0x02001E30;
constexpr std::uint32_t LINK_SEND_EVENT_JP = 0x02001E00;
constexpr std::uint32_t LINK_SEND_ITEM_OP_JP = 0x02001F38;
constexpr std::uint32_t LINK_SEND_EQUIP_SLOT_JP = 0x020020F8;
constexpr std::uint32_t LINK_SEND_CMD_SLOT_JP = 0x02002128;
constexpr std::uint32_t RADAR_DRAW_MAP_JP = 0x02003F94;
constexpr std::uint32_t RADAR_DRAW_MAP_EN = 0x0200197C;
constexpr std::uint32_t G_DATA_FLAGS_JP = 0x03002AEC;
constexpr std::uint32_t S_BASE_POS_JP = 0x030008C4;
constexpr std::uint32_t S_MAP_HEADER_JP = 0x03001174;
constexpr std::uint32_t S_MAP_INFO_JP = 0x03001178;
constexpr std::uint32_t S_MAP_ROW_LEN_JP = 0x0300117C;
constexpr std::uint32_t S_MAP_DATA_JP = 0x03001180;
constexpr std::uint32_t S_BASE_POS_EN = 0x03002754;
constexpr std::uint32_t S_MAP_HEADER_EN = 0x03000054;
constexpr std::uint32_t S_MAP_INFO_EN = 0x03000058;
constexpr std::uint32_t S_MAP_ROW_LEN_EN = 0x0300005C;
constexpr std::uint32_t S_MAP_DATA_EN = 0x03000060;
constexpr std::uint32_t S_TX_QUEUE_EN = 0x03000090;
constexpr std::uint32_t S_TX_COUNT_EN = 0x03000190;
constexpr std::uint32_t LIST_BUF_JP = 0x02032800;
constexpr std::uint32_t DETAIL_BUF_JP = 0x02035800;
constexpr std::uint32_t REG_JOY_TRANS = 0x04000154;
constexpr std::uint32_t REG_JOYSTAT = 0x04000158;

mLogger s_quiet_logger = {
    [](mLogger*, int, mLogLevel, const char*, va_list) {}, nullptr};

std::uint32_t ParseU32(const std::string& text)
{
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 0);
    if (consumed != text.size() || value > 0xFFFFFFFFULL)
        throw std::runtime_error("invalid 32-bit integer: " + text);
    return static_cast<std::uint32_t>(value);
}

std::vector<std::uint8_t> ParseHexBytes(const std::string& text)
{
    std::string digits;
    digits.reserve(text.size());
    for (const char c : text)
    {
        if (c == ' ' || c == '\t' || c == '_' || c == ':')
            continue;
        digits.push_back(c);
    }
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
        digits.erase(0, 2);
    if (digits.empty() || (digits.size() & 1) != 0)
        throw std::runtime_error("hex byte string must contain an even number of digits");

    std::vector<std::uint8_t> result;
    result.reserve(digits.size() / 2);
    for (std::size_t i = 0; i < digits.size(); i += 2)
    {
        const auto byte_text = digits.substr(i, 2);
        std::size_t consumed = 0;
        const auto value = std::stoul(byte_text, &consumed, 16);
        if (consumed != 2)
            throw std::runtime_error("invalid hex byte: " + byte_text);
        result.push_back(static_cast<std::uint8_t>(value));
    }
    return result;
}

std::vector<std::uint8_t> ReadFileBytes(const std::string& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("could not open payload file: " + path);

    const auto end = input.tellg();
    if (end < 0)
        throw std::runtime_error("could not determine payload size: " + path);
    input.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
    if (!data.empty() &&
        !input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(end)))
        throw std::runtime_error("could not read payload file: " + path);
    return data;
}

std::uint16_t Crc16(const std::vector<std::uint8_t>& data)
{
    std::uint16_t crc = 0xFFFF;
    for (const auto byte : data)
    {
        crc ^= static_cast<std::uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return static_cast<std::uint16_t>(~crc);
}

std::uint32_t PackWord(std::uint8_t a, std::uint8_t b = 0, std::uint8_t c = 0,
                       std::uint8_t d = 0)
{
    return static_cast<std::uint32_t>(a) |
           (static_cast<std::uint32_t>(b) << 8) |
           (static_cast<std::uint32_t>(c) << 16) |
           (static_cast<std::uint32_t>(d) << 24);
}

std::string Hex(std::uint32_t value, int width = 8)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(width) << value;
    return out.str();
}

class Harness
{
public:
    explicit Harness(const char* image_path)
    {
        mLogSetDefaultLogger(&s_quiet_logger);
        m_core = mCoreCreate(mPLATFORM_GBA);
        if (!m_core || !m_core->init(m_core))
            throw std::runtime_error("mGBA core initialization failed");

        mCoreInitConfig(m_core, "ffcc-joybus-harness");
        mCoreConfigSetIntValue(&m_core->config, "useBios", 0);
        mCoreConfigSetIntValue(&m_core->config, "skipBios", 1);
        mCoreConfigSetValue(&m_core->config, "idleOptimization", "remove");

        if (!mCoreLoadFile(m_core, image_path))
            throw std::runtime_error(std::string("could not load multiboot image: ") + image_path);

        GBASIOJOYCreate(&m_joybus);
        auto* gba = static_cast<GBA*>(m_core->board);
        GBASIOSetDriver(&gba->sio, &m_joybus, SIO_JOYBUS);

        m_video.resize(GBA_VIDEO_HORIZONTAL_PIXELS * GBA_VIDEO_VERTICAL_PIXELS);
        m_core->setVideoBuffer(m_core, m_video.data(), GBA_VIDEO_HORIZONTAL_PIXELS);
        m_core->reset(m_core);
    }

    ~Harness()
    {
        if (m_core)
        {
            mCoreConfigDeinit(&m_core->config);
            m_core->deinit(m_core);
        }
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    void RunFrames(unsigned count)
    {
        for (unsigned i = 0; i < count; ++i)
            m_core->runFrame(m_core);
    }

    void SetKeys(std::uint32_t keys)
    {
        m_core->setKeys(m_core, keys);
    }

    std::vector<std::uint8_t> SaveState() const
    {
        std::vector<std::uint8_t> state(m_core->stateSize(m_core));
        if (!m_core->saveState(m_core, state.data()))
            throw std::runtime_error("could not save mGBA state");
        return state;
    }

    void LoadState(const std::vector<std::uint8_t>& state)
    {
        if (!m_core->loadState(m_core, state.data()))
            throw std::runtime_error("could not restore mGBA state");
    }

    std::vector<std::uint8_t> Command(GBASIOJOYCommand command, std::uint32_t word = 0)
    {
        std::array<std::uint8_t, 6> data{};
        data[0] = static_cast<std::uint8_t>(word);
        data[1] = static_cast<std::uint8_t>(word >> 8);
        data[2] = static_cast<std::uint8_t>(word >> 16);
        data[3] = static_cast<std::uint8_t>(word >> 24);
        const int size = GBASIOJOYSendCommand(&m_joybus, command, data.data());
        RunFrames(1);
        return {data.begin(), data.begin() + size};
    }

    std::uint8_t Poll()
    {
        const auto response = Command(JOY_POLL);
        if (response.size() != 3)
            throw std::runtime_error("short JOY_POLL response");
        return response[2];
    }

    std::uint32_t Read()
    {
        const auto response = Command(JOY_TRANS);
        if (response.size() != 5)
            throw std::runtime_error("short JOY_TRANS response");
        return static_cast<std::uint32_t>(response[0]) |
               (static_cast<std::uint32_t>(response[1]) << 8) |
               (static_cast<std::uint32_t>(response[2]) << 16) |
               (static_cast<std::uint32_t>(response[3]) << 24);
    }

    void Write(std::uint32_t word)
    {
        Command(JOY_RECV, word);
    }

    void ResetJoybus()
    {
        Command(JOY_RESET);
    }

    void SendMessage(const std::vector<std::uint8_t>& data)
    {
        if (data.empty() || data.size() > 0xFFFF)
            throw std::runtime_error("message payload size must be 1..65535 bytes");

        const auto continuation_count = (data.size() - 1 + 2) / 3;
        const auto total_words = continuation_count + 2;
        if (total_words > 0xFF)
            throw std::runtime_error("message needs more than 255 Joybus words");

        const auto crc = Crc16(data);
        Write(PackWord(0x05, static_cast<std::uint8_t>(total_words),
                       static_cast<std::uint8_t>(crc), static_cast<std::uint8_t>(crc >> 8)));
        Write(PackWord(0x45, static_cast<std::uint8_t>(data.size()),
                       static_cast<std::uint8_t>(data.size() >> 8), data[0]));

        std::size_t pos = 1;
        for (std::size_t i = 0; i < continuation_count; ++i)
        {
            const auto b = pos < data.size() ? data[pos++] : 0;
            const auto c = pos < data.size() ? data[pos++] : 0;
            const auto d = pos < data.size() ? data[pos++] : 0;
            Write(PackWord(0x85, b, c, d));
        }
        std::cout << "send-message bytes=" << data.size() << " words=" << total_words
                  << " crc=" << Hex(crc, 4) << '\n';
    }

    void SendBulk(std::uint8_t type, const std::vector<std::uint8_t>& data)
    {
        constexpr std::size_t max_chunk_size = 0x2FD;
        if (type > 15)
            throw std::runtime_error("bulk type must be 0..15");
        if (data.empty() || data.size() > 0xFFFF)
            throw std::runtime_error("bulk payload size must be 1..65535 bytes");

        const auto block_count = (data.size() + max_chunk_size - 1) / max_chunk_size;
        const auto total_crc = Crc16(data);
        Write(PackWord(0x0B, type, static_cast<std::uint8_t>(block_count),
                       static_cast<std::uint8_t>(block_count >> 8)));
        Write(PackWord(0x0B, type, static_cast<std::uint8_t>(data.size()),
                       static_cast<std::uint8_t>(data.size() >> 8)));
        Write(PackWord(0x0B, type, static_cast<std::uint8_t>(total_crc),
                       static_cast<std::uint8_t>(total_crc >> 8)));

        for (std::size_t block = 0; block < block_count; ++block)
        {
            const auto begin = block * max_chunk_size;
            const auto remaining = data.size() - begin;
            const auto chunk_size = remaining < max_chunk_size ? remaining : max_chunk_size;
            const auto word_count = (chunk_size + 2) / 3;
            const std::vector<std::uint8_t> chunk(data.begin() + begin,
                                                  data.begin() + begin + chunk_size);
            const auto chunk_crc = Crc16(chunk);

            Write(PackWord(0x4B, static_cast<std::uint8_t>(word_count),
                           static_cast<std::uint8_t>(chunk_size),
                           static_cast<std::uint8_t>(chunk_size >> 8)));
            Write(PackWord(0x4B, static_cast<std::uint8_t>(block),
                           static_cast<std::uint8_t>(chunk_crc),
                           static_cast<std::uint8_t>(chunk_crc >> 8)));

            for (std::size_t pos = 0; pos < chunk_size; pos += 3)
            {
                const auto a = data[begin + pos];
                const auto b = pos + 1 < chunk_size ? data[begin + pos + 1] : 0;
                const auto c = pos + 2 < chunk_size ? data[begin + pos + 2] : 0;
                Write(PackWord(0x8B, a, b, c));
            }

            bool acknowledged = false;
            for (unsigned attempt = 0; attempt < 256; ++attempt)
            {
                if ((Poll() & 8) == 0)
                {
                    RunFrames(1);
                    continue;
                }
                const auto reply = Read();
                const auto command = static_cast<std::uint8_t>(reply) & 0x3F;
                if (command == 7)
                    throw std::runtime_error("GBA rejected bulk block " + std::to_string(block) +
                                             " with " + Hex(reply));
                if (command == 6 && static_cast<std::uint8_t>(reply >> 8) == type)
                {
                    acknowledged = true;
                    break;
                }
            }
            if (!acknowledged)
                throw std::runtime_error("no matching ACK for bulk block " + std::to_string(block));
        }

        std::cout << "send-bulk type=" << static_cast<unsigned>(type)
                  << " bytes=" << data.size() << " blocks=" << block_count
                  << " crc=" << Hex(total_crc, 4) << '\n';
    }

    void Handshake(std::uint32_t game_code = 0x4A434347, std::uint32_t host_id = 0x12345678,
                   std::uint8_t player = 0)
    {
        // Allow AgbMain to initialize LinkWork, then reproduce JoyBus::InitialCode.
        RunFrames(2);
        ResetJoybus();
        const auto idle = Read();
        Write(game_code);
        const auto context = Read();
        const auto old_host = Read();
        Write(host_id);
        Write(1U | (static_cast<std::uint32_t>(player & 0xF) << 8));

        std::cout << "handshake idle=" << Hex(idle) << " context=" << Hex(context)
                  << " old_host=" << Hex(old_host) << " status=" << Hex(Poll(), 2) << '\n';
    }

    std::uint8_t Read8(std::uint32_t address) const
    {
        return static_cast<std::uint8_t>(m_core->rawRead8(m_core, address, -1));
    }

    std::uint16_t Read16(std::uint32_t address) const
    {
        return static_cast<std::uint16_t>(m_core->rawRead16(m_core, address, -1));
    }

    std::uint32_t Read32(std::uint32_t address) const
    {
        return m_core->rawRead32(m_core, address, -1);
    }

    void Write8(std::uint32_t address, std::uint8_t value)
    {
        m_core->rawWrite8(m_core, address, -1, value);
    }

    void Write16(std::uint32_t address, std::uint16_t value)
    {
        m_core->rawWrite16(m_core, address, -1, value);
    }

    void Write32(std::uint32_t address, std::uint32_t value)
    {
        m_core->rawWrite32(m_core, address, -1, value);
    }

    void QuiesceJoybusRegisters()
    {
        // rawWrite intentionally honors the hardware's write-one/retain rules;
        // a test baseline needs to clear the emulated latch itself.
        auto* gba = static_cast<GBA*>(m_core->board);
        gba->memory.io[(REG_JOY_TRANS & 0x3FF) >> 1] = 0;
        gba->memory.io[((REG_JOY_TRANS & 0x3FF) >> 1) + 1] = 0;
        gba->memory.io[(REG_JOYSTAT & 0x3FF) >> 1] = 0;
    }

    std::uint32_t Register(const char* name) const
    {
        std::uint32_t value = 0;
        if (!m_core->readRegister(m_core, name, &value))
            throw std::runtime_error(std::string("could not read register ") + name);
        return value;
    }

    void SetRegister(const char* name, std::uint32_t value)
    {
        if (!m_core->writeRegister(m_core, name, &value))
            throw std::runtime_error(std::string("could not write register ") + name);
    }

    void CallThumb(std::uint32_t address, std::uint32_t r0, std::uint32_t r1 = 0)
    {
        constexpr std::uint32_t return_stub = EWRAM_BASE + 0x3FF00;
        Write16(return_stub, 0xE7FE); // b .
        SetRegister("r0", r0);
        SetRegister("r1", r1);
        SetRegister("lr", return_stub | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", address);
        for (int i = 0; i < 10000; ++i)
        {
            const auto pc = Register("pc") & ~1U;
            if (pc == return_stub || pc == return_stub + 2)
                return;
            m_core->step(m_core);
        }
        throw std::runtime_error("Thumb call did not return: pc=" + Hex(Register("pc")) +
                                 " lr=" + Hex(Register("lr")) +
                                 " cpsr=" + Hex(Register("cpsr")));
    }

    void PrintState() const
    {
        std::cout << "pc=" << Hex(Register("pc")) << " lr=" << Hex(Register("lr"))
                  << " cpsr=" << Hex(Register("cpsr"))
                  << " linkStarted=" << static_cast<unsigned>(Read8(G_LINK_STARTED_JP))
                  << " linkEstablished=" << static_cast<unsigned>(Read8(G_LINK_ESTABLISHED_JP))
                  << " screen=" << static_cast<int>(static_cast<std::int8_t>(Read8(G_SCREEN_JP)))
                  << " mode=" << static_cast<unsigned>(Read8(G_MODE_JP))
                  << " savedScreen=" << static_cast<int>(static_cast<std::int8_t>(Read8(G_SAVED_SCREEN_JP)))
                  << '\n';
    }

    void Dump(std::uint32_t address, std::uint32_t size) const
    {
        for (std::uint32_t offset = 0; offset < size; offset += 16)
        {
            std::cout << Hex(address + offset) << ':';
            for (std::uint32_t i = 0; i < 16 && offset + i < size; ++i)
                std::cout << ' ' << Hex(Read8(address + offset + i), 2).substr(2);
            std::cout << '\n';
        }
    }

    std::vector<std::uint32_t> Drain(unsigned limit = 64)
    {
        std::vector<std::uint32_t> words;
        while (words.size() < limit && (Poll() & 8) != 0)
            words.push_back(Read());
        return words;
    }

    void SweepSmith()
    {
        // Suppress the ordinary per-frame LINK_PAD packet so a candidate's
        // writes to the link are distinguishable.  The sweep still runs the
        // real client and screen dispatcher; only the parent/link baseline is
        // made quiescent.
        Write8(S_LINK_CONNECTED_JP, 0);
        Write8(G_WAS_CONNECTED_JP, 0);
        Write8(G_RECONNECT_PENDING_JP, 0);
        Write32(S_TX_COUNT_JP, 0);
        QuiesceJoybusRegisters();
        const auto baseline = SaveState();
        const std::array<std::uint16_t, 8> key_sets = {
            0, 1, 2, 4, 8, 0x10, 0x40, 0x80,
        };

        unsigned transmitting_cases = 0;
        unsigned noncode_cases = 0;
        for (int phase = 0; phase <= 2; ++phase)
        {
            for (int screen = -128; screen <= 127; ++screen)
            {
                for (const auto keys : key_sets)
                {
                    LoadState(baseline);
                    Write32(G_MODE_JP, 3);
                    Write32(G_SCREEN_JP, static_cast<std::uint32_t>(screen));
                    Write32(G_SCREEN_PHASE_JP, static_cast<std::uint32_t>(phase));
                    Write8(G_LINK_STARTED_JP, 1);
                    Write8(G_LINK_ESTABLISHED_JP, 0);
                    SetKeys(keys);
                    RunFrames(1);

                    const auto pc = Register("pc");
                    if (!((pc >= EWRAM_BASE && pc < EWRAM_BASE + 0x40000) ||
                          (pc >= IWRAM_BASE && pc < IWRAM_BASE + 0x8000) || pc < 0x4000))
                    {
                        ++noncode_cases;
                    }

                    const auto joy_status = Read16(REG_JOYSTAT);
                    const auto tx_count = Read32(S_TX_COUNT_JP);
                    const auto tx_word = Read32(REG_JOY_TRANS);
                    if ((joy_status & 8) != 0 || tx_count != 0)
                    {
                        ++transmitting_cases;
                        std::cout << "tx smith screen=" << screen << " phase=" << phase
                                  << " keys=" << Hex(keys, 4) << " pc=" << Hex(pc)
                                  << " joy_status=" << Hex(joy_status, 4)
                                  << " presented=" << Hex(tx_word)
                                  << " queued=" << tx_count;
                        for (std::uint32_t i = 0; i < tx_count && i < 8; ++i)
                            std::cout << ' ' << Hex(Read32(S_TX_QUEUE_JP + i * 4));
                        std::cout << '\n';
                    }
                }
            }
        }
        std::cout << "sweep-smith transmitting_cases=" << transmitting_cases
                  << " noncode_pc_cases=" << noncode_cases << " total=6144\n";
        LoadState(baseline);
        SetKeys(0);
    }

    void ProbeQueueReset()
    {
        // Model the vulnerable ordering without requiring a frame-perfect GC.
        // "A" + the JP name-entry music-note character is 41 81 F4.  If the
        // header is lost, the first continuation makes ChkCMakeName store F4
        // in m_playerSlot (signed -12).
        constexpr std::uint32_t name = EWRAM_BASE + 0x3FE00;
        Write8(S_LINK_CONNECTED_JP, 0);
        Write8(G_WAS_CONNECTED_JP, 0);
        Write8(G_RECONNECT_PENDING_JP, 0);
        Write32(S_TX_COUNT_JP, 0);
        QuiesceJoybusRegisters();
        for (int i = 0; i < 16; ++i)
            Write8(name + i, 0);
        Write8(name + 0, 0x41);
        Write8(name + 1, 0x81);
        Write8(name + 2, 0xF4);
        CallThumb(LINK_SEND_CMAKE_NAME_JP, name);

        std::cout << "before-reset presented=" << Hex(Read32(REG_JOY_TRANS))
                  << " queued=" << Read32(S_TX_COUNT_JP);
        for (std::uint32_t i = 0; i < Read32(S_TX_COUNT_JP) && i < 8; ++i)
            std::cout << ' ' << Hex(Read32(S_TX_QUEUE_JP + i * 4));
        std::cout << '\n';

        ResetJoybus();
        std::cout << "after-reset presented=" << Hex(Read32(REG_JOY_TRANS))
                  << " queued=" << Read32(S_TX_COUNT_JP) << '\n';

        const auto idle = Read();
        Write(0x4A434347);
        const auto context = Read();
        const auto old_host = Read();
        Write(0x12345678);
        Write(1);

        std::cout << "reconnect idle=" << Hex(idle) << " context=" << Hex(context)
                  << " old_host=" << Hex(old_host) << " words=";
        for (int i = 0; i < 7; ++i)
            std::cout << (i == 0 ? "" : ",") << Hex(Read());
        std::cout << " queued=" << Read32(S_TX_COUNT_JP) << '\n';
    }

    void ProbeLetterReplyReset()
    {
        // A letter reply is header + middle + final.  Discard the header with
        // one Joybus reset, let the parent consume only the middle word, then
        // reset again before it consumes the final word.  The GC receiver
        // appends that middle word to its persistent receive buffer but never
        // reaches the final-word CRC check.
        Write8(S_LINK_CONNECTED_JP, 0);
        Write8(G_WAS_CONNECTED_JP, 0);
        Write8(G_RECONNECT_PENDING_JP, 0);
        Write32(S_TX_COUNT_JP, 0);
        QuiesceJoybusRegisters();

        SetRegister("r2", 0);          // item attachment, not gil
        SetRegister("r3", 0x12345678); // attachment value
        CallThumb(LINK_SEND_LETTER_REPLY_JP, 7, 2);

        std::cout << "reply-before-reset presented=" << Hex(Read32(REG_JOY_TRANS))
                  << " queued=" << Read32(S_TX_COUNT_JP);
        for (std::uint32_t i = 0; i < Read32(S_TX_COUNT_JP) && i < 8; ++i)
            std::cout << ' ' << Hex(Read32(S_TX_QUEUE_JP + i * 4));
        std::cout << '\n';

        ResetJoybus();
        const auto idle = Read();
        Write(0x4A434347);
        const auto context = Read();
        const auto old_host = Read();
        Write(0x12345678);
        Write(1);

        const auto stale_host = Read();
        const auto middle = Read();
        std::cout << "reply-middle-only idle=" << Hex(idle)
                  << " context=" << Hex(context) << " old_host=" << Hex(old_host)
                  << " stale_host=" << Hex(stale_host) << " middle=" << Hex(middle)
                  << " final-presented=" << Hex(Read32(REG_JOY_TRANS))
                  << " queued=" << Read32(S_TX_COUNT_JP) << '\n';

        ResetJoybus();
        std::cout << "reply-after-second-reset presented=" << Hex(Read32(REG_JOY_TRANS))
                  << " queued=" << Read32(S_TX_COUNT_JP) << '\n';
    }

    void ProbeRadarStack()
    {
        // Radar_DrawMap's line[256] starts 0x250 bytes below the saved LR.
        // One 296-halfword RLE run reaches it; two following literal tokens
        // replace LR with a Thumb pointer to an EWRAM sentinel.
        constexpr std::uint32_t map = EWRAM_BASE + 0x2E000;
        constexpr std::uint32_t info = map + 0x100;
        constexpr std::uint32_t rows = info + 8;
        constexpr std::uint32_t data = info + 0x20;
        constexpr std::uint32_t normal_return = EWRAM_BASE + 0x3FF00;
        // Both return-address halfwords must have bit 15 clear or the row
        // decoder treats them as additional RLE tokens.
        constexpr std::uint32_t hijack_target = EWRAM_BASE + 0x30000;

        for (std::uint32_t i = 0; i < 0x200; ++i)
            Write8(map + i, 0);
        Write16(G_DATA_FLAGS_JP, 2);
        Write16(S_BASE_POS_JP + 0, 0);
        Write16(S_BASE_POS_JP + 2, 0);
        Write16(map + 0x14, 0x100); // width
        Write16(map + 0x16, 8); // one decoded row
        Write16(info + 0, static_cast<std::uint16_t>(-80));
        Write16(info + 2, static_cast<std::uint16_t>(-64));
        Write32(S_MAP_HEADER_JP, map);
        Write32(S_MAP_INFO_JP, info);
        Write32(S_MAP_ROW_LEN_JP, rows);
        Write32(S_MAP_DATA_JP, data);
        Write8(rows, 3);
        Write16(data + 0, 0x8128); // RLE: 296 copies of tile 0
        Write16(data + 2, static_cast<std::uint16_t>((hijack_target | 1) & 0xFFFF));
        Write16(data + 4, static_cast<std::uint16_t>((hijack_target | 1) >> 16));
        Write16(normal_return, 0xE7FE); // b .
        Write16(hijack_target, 0xE7FE); // b .

        const auto entry_sp = Register("sp");
        SetRegister("lr", normal_return | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", RADAR_DRAW_MAP_JP);
        for (int i = 0; i < 200000; ++i)
        {
            const auto pc = Register("pc") & ~1U;
            if (pc == hijack_target || pc == hijack_target + 2)
            {
                std::cout << "probe-radar-stack hijacked pc=" << Hex(Register("pc"))
                          << " steps=" << i << '\n';
                return;
            }
            if (pc == normal_return || pc == normal_return + 2)
            {
                std::cout << "probe-radar-stack returned normally pc=" << Hex(Register("pc"))
                          << " steps=" << i << '\n';
                return;
            }
            m_core->step(m_core);
        }
        std::cout << "probe-radar-stack stopped pc=" << Hex(Register("pc"))
                  << " lr=" << Hex(Register("lr")) << " cpsr=" << Hex(Register("cpsr"))
                  << " entry_sp=" << Hex(entry_sp) << '\n';
        Dump(entry_sp - 0x80, 0x90);
    }

    void ProbeRadarRop(int command_kind = 0)
    {
        // The structured smith records can produce 0x02010251 as the first
        // overwritten return address.  In the JP retail client this lands on
        // GilScreen_Main's clean epilogue gadget:
        //
        //   movs r0, #0; add sp, #8; pop {r4-r7}; pop {r1}; bx r1
        //
        // At gadget entry SP is Radar_DrawMap's original entry SP.  For the
        // GilScreen gadget its next PC comes from line[310], fourteen
        // halfwords after the first overwritten PC at line[296], and r4-r7
        // come from line[302..309].  The cmdwrite probe uses a second retail
        // epilogue which first pops r3, shifting those fields by one word.
        // DETAIL_BUF is a separate bulk destination, so a previously
        // downloaded letter body survives the smith-list and map downloads
        // and is an executable second stage.
        constexpr std::uint32_t map = EWRAM_BASE + 0x2E000;
        constexpr std::uint32_t info = map + 0x100;
        constexpr std::uint32_t rows = info + 8;
        constexpr std::uint32_t data = info + 0x20;
        constexpr std::uint32_t normal_return = EWRAM_BASE + 0x3FF00;
        // 0x02010251 enters at "movs r0,#0".  0x02010201 enters an earlier
        // path that plays a sound, sets r0=1, then reaches the same epilogue.
        const bool event_command = command_kind == 0;
        const bool item_command = command_kind == 1;
        const bool cmdslot_command = command_kind == 2;
        const bool letter_command = command_kind == 3 || command_kind == 4;
        const bool route_letter_command = command_kind == 4;
        const bool cmdwrite_command = command_kind == 5;
        const bool equipwrite_command = command_kind == 6;
        const bool route_equipwrite_command = command_kind == 7;
        const bool raw_write_command = command_kind >= 8 && command_kind <= 10;
        const bool en_raw_write_command = command_kind == 9;
        const bool pal_raw_write_command = command_kind == 10;
        const bool western_raw_write_command = en_raw_write_command || pal_raw_write_command;
        const std::uint32_t radar_draw_map = western_raw_write_command ? RADAR_DRAW_MAP_EN : RADAR_DRAW_MAP_JP;
        const std::uint32_t base_pos = western_raw_write_command ? S_BASE_POS_EN : S_BASE_POS_JP;
        const std::uint32_t map_header = western_raw_write_command ? S_MAP_HEADER_EN : S_MAP_HEADER_JP;
        const std::uint32_t map_info = western_raw_write_command ? S_MAP_INFO_EN : S_MAP_INFO_JP;
        const std::uint32_t map_row_len = western_raw_write_command ? S_MAP_ROW_LEN_EN : S_MAP_ROW_LEN_JP;
        const std::uint32_t map_data = western_raw_write_command ? S_MAP_DATA_EN : S_MAP_DATA_JP;
        const std::uint32_t tx_queue = western_raw_write_command ? S_TX_QUEUE_EN : S_TX_QUEUE_JP;
        const std::uint32_t tx_count = western_raw_write_command ? S_TX_COUNT_EN : S_TX_COUNT_JP;
        const std::uint32_t epilogue_gadget = route_equipwrite_command ? EWRAM_BASE + 0x0570C
            : cmdwrite_command ? EWRAM_BASE + 0x02554
            : en_raw_write_command ? EWRAM_BASE + 0x150C4
            : pal_raw_write_command ? EWRAM_BASE + 0x15280
            : EWRAM_BASE + ((event_command || letter_command || raw_write_command) ? 0x10250 : 0x10200);
        const std::uint32_t send_function = event_command ? LINK_SEND_EVENT_JP
            : item_command ? LINK_SEND_ITEM_OP_JP
            : cmdslot_command ? LINK_SEND_CMD_SLOT_JP
            : (equipwrite_command || route_equipwrite_command) ? LINK_SEND_EQUIP_SLOT_JP
            : raw_write_command ? (western_raw_write_command ? LINK_WRITE_EN : LINK_WRITE_JP)
            : LINK_SEND_LETTER_REPLY_JP;
        const std::uint32_t argument = event_command ? 0x42 : item_command ? 0xA6 : 72;
        constexpr std::uint32_t detail_payload = DETAIL_BUF_JP;

        for (std::uint32_t i = 0; i < 0x200; ++i)
            Write8(map + i, 0);
        Write16(G_DATA_FLAGS_JP, 2);
        Write16(base_pos + 0, 0);
        Write16(base_pos + 2, 0);
        Write16(map + 0x14, 0x100);
        Write16(map + 0x16, 8);
        Write16(info + 0, static_cast<std::uint16_t>(-80));
        Write16(info + 2, static_cast<std::uint16_t>(-64));
        Write32(map_header, map);
        Write32(map_info, info);
        Write32(map_row_len, rows);
        Write32(map_data, data);

        std::uint32_t cursor = data;
        auto token = [&](std::uint16_t value) {
            Write16(cursor, value);
            cursor += 2;
        };
        auto token32 = [&](std::uint32_t value) {
            token(static_cast<std::uint16_t>(value));
            token(static_cast<std::uint16_t>(value >> 16));
        };
        if (route_equipwrite_command)
        {
            // Radar's own saved r4-r7 occupy line[288..295].  0200570D moves
            // the restored r4 to r0, pops a replacement r4 and the next PC,
            // then branches.  This gives the route-compatible name an r0=5
            // while retaining controlled r5-r7.
            token(0x8120); // line[0..287]
            token32(5); // saved r4 -> r0 = OOB equip index 5
            token32(LIST_BUF_JP + 0x100 - 52); // saved r5
            token32(0x00004000); // saved r6
            token32(0x02014DB9); // saved r7; sum = 02018DB9
            token32(epilogue_gadget | 1); // line[296..297]
            token32(0); // replacement r4 at line[298..299]
            token32((detail_payload + 2) | 1); // line[300..301]
        }
        else
        {
            token(0x8128); // line[0..295]
            token32(epilogue_gadget | 1);
            for (int i = 0; i < 4; ++i)
                token(0); // line[298..301], skipped by add sp, #8
            if (cmdwrite_command)
                token32(0x02030000); // popped r3
            const std::uint32_t r4 = cmdwrite_command ? 0x00002810 : (send_function | 1);
            token32(r4);
            const std::uint32_t r5 = cmdwrite_command ? LIST_BUF_JP + 0x100 - 52
                : route_letter_command ? LIST_BUF_JP + 0x100 - 52
                : letter_command ? 0x00480001
                : equipwrite_command ? 5 : 0;
            const std::uint32_t r6 = cmdwrite_command ? 0x000040EE
                : route_letter_command ? 2 : letter_command ? 2 : argument;
            const std::uint32_t r6_word = raw_write_command ? 0x78563455
                : equipwrite_command ? 0x9A : r6;
            token32(r5);
            token32(r6_word);
            const std::uint32_t r7 = cmdwrite_command ? 0x02006C61 : 0;
            token32(r7);
            const std::uint32_t second_stage = detail_payload +
                ((route_letter_command || cmdwrite_command) ? 2 : 0);
            token32(second_stage | 1);
        }
        Write8(rows, static_cast<std::uint8_t>((cursor - data) / 2));

        // "1F2F G" is a legal six-character JP name and these bytes are also
        // three Thumb instructions:
        //   mov r1, r6; mov r2, r6; bx r4
        // r0 comes from the selected gadget, so this calls one of:
        // Link_SendEvent(0, 0x42, 0x42), or
        // Link_SendItemOp(1, 0xA6, 0xA6), or
        // Link_SendCmdSlot(1, -72).  The last one installs the Big Chungus
        // hitbox-pointer index directly and bypasses the normal 0..63 list.
        if (route_equipwrite_command)
        {
            // The existing Any% prefix doubles as code: ldr r3,[r5,#52];
            // add r7,r6; bx r7.  The branch enters Smith_EquipResult's tail,
            // which truncates r3 into r1 and calls Link_SendEquipSlot(r0,r1).
            Write16(detail_payload + 0, 0xA682);
            Write16(detail_payload + 2, 0x6B6B);
            Write16(detail_payload + 4, 0x4437);
            Write16(detail_payload + 6, 0x4738);
            Write32(LIST_BUF_JP + 0x100, 0x0000009A);
        }
        else if (cmdwrite_command)
        {
            // "えkk7D8G" preserves the route's 82 A6 6B 6B prefix.  Entering
            // at +2 executes ldr r3,[r5,#52]; add r7,r6; bx r7.  The staged
            // LIST_BUF word supplies r3=02030000.  Popped r7=02006C61 plus
            // r6=000040EE makes 0200AD4F, entering CmdList_SetSlot's tail.
            // It calls Link_SendCmdSlot(low8(r6), *(s16 *)(r3+r4+0xF4));
            // r4=2810 points that load at the next staged LIST_BUF word.
            Write16(detail_payload + 0, 0xA682);
            Write16(detail_payload + 2, 0x6B6B);
            Write16(detail_payload + 4, 0x4437);
            Write16(detail_payload + 6, 0x4738);
            Write32(LIST_BUF_JP + 0x100, 0x02030000);
            Write16(LIST_BUF_JP + 0x104, 0x1234);
        }
        else if (route_letter_command)
        {
            // "えkk3D G" is 82 A6 6B 6B 33 44 20 47.  Entering at its first
            // `k` executes ldr r3,[r5,#52]; add r3,r6; bx r4.  Point r5 at a
            // staged raw word in LIST_BUF, allowing the existing えkk
            // wrong-craft prefix to double as the final letter payload.
            Write16(detail_payload + 0, 0xA682);
            Write16(detail_payload + 2, 0x6B6B);
            Write16(detail_payload + 4, 0x4433);
            Write16(detail_payload + 6, 0x4720);
            Write32(LIST_BUF_JP + 0x100, 0xFFB7FFFF);
        }
        else if (letter_command)
        {
            // "kB3D G": neg r3,r5; add r3,r6; bx r4.  At name entry the
            // gadget/Radar residue already supplies r0=0, low8(r1)=1 and
            // low8(r2)=0.  Starting with r5=0x00480001 and r6=2 constructs
            // r3=0xFFB80001, so Link_SendLetterReply replies to letter zero
            // with item 1 and a signed source slot of -72.
            Write16(detail_payload + 0, 0x426B);
            Write16(detail_payload + 2, 0x4433);
            Write16(detail_payload + 4, 0x4720);
        }
        else if (cmdslot_command)
        {
            // "qB G": neg r1,r6; bx r4.  The radar decoder cannot emit a
            // literal halfword with bit 15 set, so pop +72 and negate it in
            // the name instead of trying to pop 0xFFB8.
            Write16(detail_payload + 0, 0x4271);
            Write16(detail_payload + 2, 0x4720);
        }
        else if (equipwrite_command)
        {
            // "(F1F G": mov r0,r5; mov r1,r6; bx r4.  With r5=5 and
            // r6=0x9A this calls Link_SendEquipSlot(5, 0x9A), producing
            // 1E 05 9A 00.  On the GameCube, signed index 5 aliases
            // m_inventoryItems[0] and signed value 0x9A is item -102.
            Write16(detail_payload + 0, 0x4628);
            Write16(detail_payload + 2, 0x4631);
            Write16(detail_payload + 4, 0x4720);
        }
        else if (raw_write_command)
        {
            // "0F G": mov r0,r6; bx r4.  This calls Link_Write directly
            // with all four bytes supplied by the radar-controlled stack.
            // 55 34 56 78 is deliberately a continuation-shaped word with
            // otherwise unrestricted payload bytes.
            Write16(detail_payload + 0, 0x4630);
            Write16(detail_payload + 2, 0x4720);
        }
        else
        {
            // "1F2F G": mov r1,r6; mov r2,r6; bx r4
            Write16(detail_payload + 0, 0x4631);
            Write16(detail_payload + 2, 0x4632);
            Write16(detail_payload + 4, 0x4720);
        }
        Write16(normal_return, 0xE7FE);
        Write32(tx_count, 0);

        SetRegister("lr", normal_return | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", radar_draw_map);
        std::array<std::uint32_t, 8> name_entry_args{};
        bool saw_name_entry = false;
        for (int i = 0; i < 200000; ++i)
        {
            const auto pc = Register("pc") & ~1U;
            const bool at_second_stage = pc >= detail_payload && pc < detail_payload + 10;
            if (!saw_name_entry && at_second_stage)
            {
                for (int reg = 0; reg < 8; ++reg)
                {
                    const auto name = "r" + std::to_string(reg);
                    name_entry_args[reg] = Register(name.c_str());
                }
                saw_name_entry = true;
            }
            const std::uint32_t queued = Read32(tx_count);
            const std::uint32_t presented = Read32(REG_JOY_TRANS);
            if (queued >= (letter_command ? 3U : 1U) ||
                (raw_write_command && presented == 0x78563455))
            {
                const char* label = event_command ? "probe-radar-rop"
                    : item_command ? "probe-radar-item-rop"
                    : cmdslot_command ? "probe-radar-cmdslot-rop"
                    : cmdwrite_command ? "probe-radar-cmdwrite-rop"
                    : route_equipwrite_command ? "probe-radar-route-equipwrite-rop"
                    : equipwrite_command ? "probe-radar-equipwrite-rop"
                    : en_raw_write_command ? "probe-radar-raw-write-rop-en"
                    : pal_raw_write_command ? "probe-radar-raw-write-rop-pal"
                    : raw_write_command ? "probe-radar-raw-write-rop"
                    : route_letter_command ? "probe-radar-route-letter-rop"
                    : "probe-radar-letter-rop";
                std::cout << label
                          << (queued ? " queued pc=" : " presented pc=")
                          << Hex(Register("pc")) << " packets=";
                for (std::uint32_t packet = 0; packet < queued; ++packet)
                    std::cout << (packet == 0 ? "" : ",") << Hex(Read32(tx_queue + packet * 4));
                if (!queued)
                    std::cout << Hex(presented);
                std::cout << " count=" << queued << " steps=" << i;
                if (saw_name_entry)
                {
                    std::cout << " entry=";
                    for (int reg = 0; reg < 8; ++reg)
                        std::cout << (reg == 0 ? "" : ",") << "r" << reg << "=" << Hex(name_entry_args[reg]);
                }
                std::cout << '\n';
                return;
            }
            if (pc == normal_return || pc == normal_return + 2)
            {
                std::cout << "probe-radar-rop returned normally pc=" << Hex(Register("pc"))
                          << " steps=" << i << '\n';
                return;
            }
            m_core->step(m_core);
        }
        std::cout << "probe-radar-rop stopped pc=" << Hex(Register("pc"))
                  << " lr=" << Hex(Register("lr")) << " r0=" << Hex(Register("r0"))
                  << " tx-count=" << Read32(tx_count);
        if (saw_name_entry)
        {
            std::cout << " entry=";
            for (int reg = 0; reg < 8; ++reg)
                std::cout << (reg == 0 ? "" : ",") << "r" << reg << "=" << Hex(name_entry_args[reg]);
        }
        std::cout << '\n';
    }

private:
    mCore* m_core = nullptr;
    GBASIODriver m_joybus{};
    std::vector<std::uint32_t> m_video;
};

void PrintResponse(const char* operation, const std::vector<std::uint8_t>& response)
{
    std::cout << operation << ':';
    for (const auto byte : response)
        std::cout << ' ' << Hex(byte, 2).substr(2);
    std::cout << '\n';
}

void RunScript(Harness& harness, const char* path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error(std::string("could not open script: ") + path);

    std::string line;
    unsigned line_number = 0;
    while (std::getline(input, line))
    {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos)
            line.resize(comment);
        std::istringstream tokens(line);
        std::string command;
        if (!(tokens >> command))
            continue;

        try
        {
            if (command == "handshake")
            {
                harness.Handshake();
            }
            else if (command == "reset")
            {
                PrintResponse("reset", harness.Command(JOY_RESET));
            }
            else if (command == "poll")
            {
                PrintResponse("poll", harness.Command(JOY_POLL));
            }
            else if (command == "read")
            {
                PrintResponse("read", harness.Command(JOY_TRANS));
            }
            else if (command == "write")
            {
                std::string value;
                tokens >> value;
                harness.Write(ParseU32(value));
            }
            else if (command == "send-message")
            {
                std::string payload;
                tokens >> payload;
                harness.SendMessage(ParseHexBytes(payload));
            }
            else if (command == "send-bulk-file")
            {
                std::string type, payload_path;
                tokens >> type >> std::quoted(payload_path);
                if (type.empty() || payload_path.empty())
                    throw std::runtime_error("send-bulk-file needs TYPE and PATH");
                const auto parsed_type = ParseU32(type);
                if (parsed_type > 15)
                    throw std::runtime_error("bulk type must be 0..15");
                harness.SendBulk(static_cast<std::uint8_t>(parsed_type), ReadFileBytes(payload_path));
            }
            else if (command == "call-thumb")
            {
                std::string address, r0, r1;
                tokens >> address >> r0;
                const auto arg1 = tokens >> r1 ? ParseU32(r1) : 0;
                harness.CallThumb(ParseU32(address), ParseU32(r0), arg1);
            }
            else if (command == "wait")
            {
                std::string count;
                tokens >> count;
                harness.RunFrames(ParseU32(count));
            }
            else if (command == "keys")
            {
                std::string value;
                tokens >> value;
                harness.SetKeys(ParseU32(value));
            }
            else if (command == "drain")
            {
                const auto words = harness.Drain();
                std::cout << "drain:";
                for (const auto word : words)
                    std::cout << ' ' << Hex(word);
                std::cout << '\n';
            }
            else if (command == "quiesce")
            {
                harness.QuiesceJoybusRegisters();
            }
            else if (command == "sweep-smith")
            {
                harness.SweepSmith();
            }
            else if (command == "probe-queue-reset")
            {
                harness.ProbeQueueReset();
            }
            else if (command == "probe-letter-reply-reset")
            {
                harness.ProbeLetterReplyReset();
            }
            else if (command == "probe-radar-stack")
            {
                harness.ProbeRadarStack();
            }
            else if (command == "probe-radar-rop")
            {
                harness.ProbeRadarRop();
            }
            else if (command == "probe-radar-item-rop")
            {
                harness.ProbeRadarRop(1);
            }
            else if (command == "probe-radar-cmdslot-rop")
            {
                harness.ProbeRadarRop(2);
            }
            else if (command == "probe-radar-letter-rop")
            {
                harness.ProbeRadarRop(3);
            }
            else if (command == "probe-radar-route-letter-rop")
            {
                harness.ProbeRadarRop(4);
            }
            else if (command == "probe-radar-cmdwrite-rop")
            {
                harness.ProbeRadarRop(5);
            }
            else if (command == "probe-radar-equipwrite-rop")
            {
                harness.ProbeRadarRop(6);
            }
            else if (command == "probe-radar-route-equipwrite-rop")
            {
                harness.ProbeRadarRop(7);
            }
            else if (command == "probe-radar-raw-write-rop")
            {
                harness.ProbeRadarRop(8);
            }
            else if (command == "probe-radar-raw-write-rop-en")
            {
                harness.ProbeRadarRop(9);
            }
            else if (command == "probe-radar-raw-write-rop-pal")
            {
                harness.ProbeRadarRop(10);
            }
            else if (command == "state")
            {
                harness.PrintState();
            }
            else if (command == "dump")
            {
                std::string address, size;
                tokens >> address >> size;
                harness.Dump(ParseU32(address), ParseU32(size));
            }
            else if (command == "poke8" || command == "poke16" || command == "poke32")
            {
                std::string address, value;
                tokens >> address >> value;
                const auto parsed_address = ParseU32(address);
                const auto parsed_value = ParseU32(value);
                if (command == "poke8")
                    harness.Write8(parsed_address, static_cast<std::uint8_t>(parsed_value));
                else if (command == "poke16")
                    harness.Write16(parsed_address, static_cast<std::uint16_t>(parsed_value));
                else
                    harness.Write32(parsed_address, parsed_value);
            }
            else
            {
                throw std::runtime_error("unknown command: " + command);
            }
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error(std::string(path) + ':' + std::to_string(line_number) + ": " + error.what());
        }
    }
}

void Usage(const char* argv0)
{
    std::cerr << "usage: " << argv0 << " <ffcc_cli.bin> [transcript.txt]\n"
              << "commands: handshake, reset, poll, read, write WORD, wait FRAMES, state,\n"
              << "          send-message HEXBYTES, send-bulk-file TYPE PATH,\n"
              << "          call-thumb ADDRESS R0 [R1], keys MASK, drain,\n"
              << "          quiesce,\n"
              << "          sweep-smith,\n"
              << "          probe-queue-reset, probe-letter-reply-reset, probe-radar-stack,\n"
              << "          probe-radar-rop, probe-radar-item-rop, probe-radar-cmdslot-rop,\n"
              << "          probe-radar-letter-rop,\n"
              << "          probe-radar-route-letter-rop,\n"
              << "          probe-radar-cmdwrite-rop,\n"
              << "          probe-radar-equipwrite-rop,\n"
              << "          probe-radar-route-equipwrite-rop,\n"
              << "          probe-radar-raw-write-rop,\n"
              << "          probe-radar-raw-write-rop-en,\n"
              << "          probe-radar-raw-write-rop-pal,\n"
              << "          dump ADDRESS SIZE,\n"
              << "          poke8/poke16/poke32 ADDRESS VALUE\n";
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
    {
        Usage(argv[0]);
        return 2;
    }

    try
    {
        Harness harness(argv[1]);
        if (argc == 3)
        {
            RunScript(harness, argv[2]);
        }
        else
        {
            harness.Handshake();
            harness.PrintState();
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
