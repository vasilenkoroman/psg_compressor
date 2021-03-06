#include <string>
#include <map>
#include <vector>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <array>

static const uint8_t kEndTrackMarker = 0x0f;
static const int kMaxDelay = 256;
static const int kMaxRefOffset = 16384;
static const int kPsg2iSize = 32;

static const int kMaxTimeForL4 = 930;

enum Flags
{
    none = 0,
    
    cleanRegs  = 2,
    cleanToneA = 4,
    cleanToneB = 8,
    cleanToneC = 16,
    cleanEnvelope = 32,
    cleanEnvForm = 64,
    cleanNoise = 128,
    
    dumpPsg = 256,
    dumpTimings = 512,
    addScf = 1024
};

enum class TimingState
{
    single,
    longFirst,
    first,
    mid,
    last
};

enum CompressionLevel
{
    l0,   //< Maximum speed. Max frame time=802t.
    l1,   //< Same max frame time, avarage frame size worse a little bit, better compression.
    l2,   //< Max frame time about 828t, better compression.
    l3,   //< Max frame time above 900t, better compression.
    l4,   //< Allow recursive refs. It requires slow_psg_player.asm
};

static const int kDefaultFlags = cleanNoise - 1;

using RegMap = std::map<int, int>;
using RegVector = std::array<int, 14>;

auto splitRegs(const RegMap& regs)
{
    int firstRegs = 0;
    int secondRegs = 0;
    for (const auto& reg : regs)
    {
        if (reg.first < 6)
            ++firstRegs;
        else
            ++secondRegs;
    }
    return std::tuple<int, int>(firstRegs, secondRegs);
}

uint8_t reverseBits(uint8_t value)
{
    uint8_t b = value;
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

uint8_t makeRegMask(const RegMap& regs, int from, int to)
{
    uint8_t result = 0;
    uint8_t bit = 0x80;
    for (int i = from; i < to; ++i)
    {
        const auto reg = regs.find(i);
        if (reg == regs.end())
        {
            result += bit;
        }
        bit >>= 1;
    }
    return result;
}

uint16_t longRegMask(const RegMap& regs)
{
    uint8_t mask1 = makeRegMask(regs, 0, 6);
    uint8_t mask2 = makeRegMask(regs, 6, 14);

    mask1 = reverseBits(mask1) << 2;
    mask2 = reverseBits(mask2);

    return mask1 + mask2 * 256;
}

struct Stats
{
    int outPsgFrames = 0;
    int inPsgFrames = 0;

    int emptyCnt = 0;
    int emptyFrames = 0;

    int singleRepeat = 0;
    int allRepeat = 0;
    int allRepeatFrames = 0;

    int ownCnt = 0;
    int ownBytes = 0;

    std::map<int, int> frameRegs;
    std::map<int, int> regsChange;

    std::map<int, int> firstHalfRegs;
    std::map<int, int> secondHalfRegs;

    int unusedToneA = 0;
    int unusedToneB = 0;
    int unusedToneC = 0;
    int unusedEnvelope = 0;
    int unusedEnvForm = 0;
    int unusedNoise = 0;
    bool addScf = false;

    std::map<int, int> maskToUsage;
    std::multimap<int, int> usageToMask;
    std::map<int, int> maskIndex;
    CompressionLevel level = CompressionLevel::l1;
};

bool isPsg2(const RegMap& regs, uint16_t symbol, const Stats& stats)
{
    return regs.size() > 1;
}

struct RefInfo
{
    int refTo = -1;
    int reducedLen = 0;
    int refLen = 0;
    int level = 0;
    int offsetInRef = 0;
};

class TimingsHelper
{
private:
    const Stats& m_stats;
    const std::vector<RefInfo>& m_refInfo;
public:
    TimingsHelper(const Stats& stats, const std::vector<RefInfo>& refInfo):
        m_stats(stats),
        m_refInfo(refInfo)
    {
    }

    int trbRepTimings(int trdRep)
    {
        if (m_stats.level < 4)
        {
            if (trdRep == 0)
                return 7 + 4 + 11;
            int result = 7 + 4 + 5;
            if (trdRep > 1)
            {
                result += 13 + 11;
                return result;
            }

            result += 13 + 5 + 42;
            return result;
        }
        else
        {
            if (trdRep != 1)
                return 4 + 11 + 11;
            return 20+34;
        }
    }

    int frameTimings(const RegMap& regs, int trbRep, uint16_t symbol)
    {
        int result = 0;
        if (m_stats.level < 4)
            result += 28 + 17;  //< before pl_frame
        else
            result += 34+5 +17;  //< before pl_frame
        result += pl0xTimings(regs, symbol);
        if (m_stats.level < 4)
            result += 16;
        else
            result += 59;

        return result + trbRepTimings(trbRep);
    }

    int pause_cont()
    {
        if (m_stats.level < 4)
            return 13 + 16 + 4 + 13 + 10 + 16 + 10 + 10;
        return 114;
    }

    int after_play_frame(int trbRep)
    {
        int result = 0;
        if (m_stats.level < 4)
            result += 16;
        else
            result += 59;
        return result + trbRepTimings(trbRep);
    }

    int delayTimings(TimingState state, int trbRep)
    {
        int pl_pause = m_stats.level < 4 ? 98 : 109;
        int result = 0;
        switch (state)
        {
            case TimingState::single:
                result = pl_pause + 12 + 7 + 6 + 12 + 10 + 10;
                result += after_play_frame(trbRep);
                break;
            case TimingState::longFirst:
                result = pl_pause + 7 + 12 + 6 + 7 + 6 + 12 + pause_cont();
                break;
            case TimingState::first:
                result = pl_pause + 12 + 7 + 6 + 7 + pause_cont();
                break;
            case TimingState::mid:
                result = 12 + 10 + 11 + 11;
                break;
            case TimingState::last:
                result = 12 + 26 + 38;
                if (m_stats.level >= 4)
                    result += 16;
                result += trbRepTimings(trbRep);
                break;
        }
        return result;
    }

    static int play_all_6_13(const RegMap& regs)
    {
        int result = 341;
        if (regs.count(13) == 0)
            result -= 35;
        return result;
    }

    int play_by_mask_13_6(const RegMap& regs)
    {
        int result = 53;
        if (regs.count(13) == 0)
            result -= 34;
        for (int i = 12; i > 6; --i)
        {
            result += 54;
            if (regs.count(i) == 0)
                result -= 34;
        }

        if (regs.count(6) == 0)
        {
            result += 4 + 11;
            if (m_stats.addScf)
                result -= 4; //< Early 'ret c' here. There is no 'scf' overhead.
        }
        else
        {
            result += 55;
        }

        return result;
    }

    int reg_left_6(const RegMap& regs)
    {
        int result = 0;

        if (regs.count(5))
            result += 4 + 7 + 12 + 4 + 16 + 7;
        else
            result += 4+12;

        for (int i = 4; i > 0; --i)
        {
            if (regs.count(i))
                result += 54;
            else
                result += 20;
        }

        if (regs.count(0) == 0)
        {
            result += 4 + 11;
        }
        else
        {
            if (m_stats.addScf)
                result += 4; //< Extra 'scf' here.
            result += 55;
        }

        return result;
    }

    int play_all_0_5_end(const RegMap& regs)
    {
        const auto [firstRegs, secondRegs] = splitRegs(regs);
        int secondRegsExcept13 = secondRegs;
        if (regs.count(13) == 1)
            --secondRegsExcept13;

        int result = 24;

        if (secondRegsExcept13 == 7)
            result += play_all_6_13(regs);
        else
            result += 5 + play_by_mask_13_6(regs);

        return result;
    }

    int pl00TimeForFrame(const RegMap& regs, uint16_t symbol)
    {
        if (regs.size() == 1)
            return 4 + 12 + 4 + 7 + 7 + 7+7+7+4+6+45;

        return 29+53+17 + reg_left_6(regs) + 36 + play_by_mask_13_6(regs);
    }

    int pl0xTimings(const RegMap& regs, uint16_t symbol)
    {
        const auto [firstRegs, secondRegs] = splitRegs(regs);
        int secondRegsExcept13 = secondRegs;
        if (regs.count(13) == 1)
            --secondRegsExcept13;

        uint16_t longMask = longRegMask(regs);
        bool psg2 = isPsg2(regs, symbol, m_stats);
        if (!psg2 || m_stats.maskIndex.count(longMask))
            return 21 + 5 + pl00TimeForFrame(regs, symbol);

        // PSG2 timings
        int result = 44; //< Till jump to play_all_0_5

        if (firstRegs < 6)
        {
            // play_by_mask_0_5
            for (int i = 0; i < 5; ++i)
            {
                if (regs.count(i) == 0)
                    result += 20; //< There is no reg i.
                else
                    result += 54;
            }

            if (regs.count(5) == 0)
            {
                result += 4 + 12; // 'play_all_0_5_end' reached
                result += play_all_0_5_end(regs);
            }
            else
            {
                result += 43 + 24;
                if (secondRegsExcept13 == 7)
                    result += 5 + play_all_6_13(regs);
                else
                    result += 7 + 10 + play_by_mask_13_6(regs);
            }
        }
        else
        {
            result += 5;
            result += 240;
            result += play_all_0_5_end(regs);
        }

        return result;
    }

    int shortRefTimings(const RegMap& regs, uint16_t symbol, int trbRep)
    {
        int result = m_stats.level >= 4 ? 185 : 115;
        result += TimingsHelper::pl0xTimings(regs, symbol);
        if (m_stats.level >= 4)
            result += trbRepTimings(trbRep);
        return result;
    }

    int longRefInitTiming(int pos, const RegMap& regs, uint16_t symbol, int symbolsLeftAtLevel)
    {
        int result = m_stats.level >= 4 ? 269 : 170;

        if (m_stats.level >= 4 && symbolsLeftAtLevel == 1)
        {
            // same level ref
            result -= 26 - 5;
        }

        result += TimingsHelper::pl0xTimings(regs, symbol);
        return result;
    }
};

struct CutRange
{
    int from = -1;
    int to = -1;

    bool isEmpty() const { return from == -1 && to == -1; }
};

class PgsPacker
{
public:

    PgsPacker() : th(stats, refInfo) {}

    struct FrameInfo
    {
        uint16_t symbol = 0;
        RegVector fullState;
        RegMap delta;
    };

    std::map<RegMap, uint16_t> regsToSymbol;
    std::map<uint16_t, RegMap> symbolToRegs;
    std::vector<FrameInfo> ayFrames;

    RegMap changedRegs;

    RegVector lastOrigRegs;
    RegVector lastCleanedRegs;
    RegVector prevCleanedRegs;
    RegVector prevTonePeriod;
    RegVector prevEnvelopePeriod;
    RegVector prevEnvelopeForm;
    RegVector prevNoisePeriod;
    std::map<int, int> symbolsToInflate;

    Stats stats;
    TimingsHelper th;

    std::vector<uint8_t> srcPsgData;
    std::vector<uint8_t> updatedPsgData;
    std::vector<uint8_t> compressedData;
    std::vector<RefInfo> refInfo;
    std::vector<int> frameOffsets;
    int flags = kDefaultFlags;
    bool firstFrame = false;
    std::vector<int> timingsData;

    std::vector<CutRange> cutRanges;
private:

    uint16_t toSymbol(const RegMap& regs)
    {
        auto itr = regsToSymbol.find(regs);
        if (itr != regsToSymbol.end())
            return itr->second;

        uint16_t value = regsToSymbol.size();
        regsToSymbol.emplace(regs, value);
        symbolToRegs.emplace(value, regs);
        return value;
    }

    // This code ported from PHP to cpp from tmk&bfox ayPacker.
    void doCleanRegs()
    {
        // Normalize regs values (only usage bits).

        lastCleanedRegs = lastOrigRegs;

        lastCleanedRegs[1] &= 15;
        lastCleanedRegs[3] &= 15;
        lastCleanedRegs[5] &= 15;
        lastCleanedRegs[6] &= 31;
        lastCleanedRegs[7] &= 63;
        lastCleanedRegs[8] &= 31;
        lastCleanedRegs[9] &= 31;
        lastCleanedRegs[10] &= 31;
        lastCleanedRegs[13] &= 15;

        lastCleanedRegs[7] &= 63;

        // clean volume (do AND_16 if envelope mode)

        for (int i : {8, 9, 10})
        {
            if (lastCleanedRegs[i] & 16)
                lastCleanedRegs[i] = 16;
        }

        // Clean tone period.

        /* toneA */
        if (flags & cleanToneA)
        {
            if (lastOrigRegs[8] == 0 || (lastOrigRegs[7] & 1) != 0)
            {
                lastCleanedRegs[0] = prevTonePeriod[0];
                lastCleanedRegs[1] = prevTonePeriod[1];
                stats.unusedToneA++;
            }
            else
            {
                prevTonePeriod[0] = lastOrigRegs[0];
                prevTonePeriod[1] = lastOrigRegs[1];
            }
        }
        /* toneB */
        if (flags & cleanToneB)
        {
            if (lastOrigRegs[9] == 0 || (lastOrigRegs[7] & 2) != 0)
            {
                lastCleanedRegs[2] = prevTonePeriod[2];
                lastCleanedRegs[3] = prevTonePeriod[3];
                stats.unusedToneB++;
            }
            else
            {
                prevTonePeriod[2] = lastOrigRegs[2];
                prevTonePeriod[3] = lastOrigRegs[3];
            }
        }
        /* toneC */
        if (flags & cleanToneC)
        {
            if (lastOrigRegs[10] == 0 || (lastOrigRegs[7] & 4) != 0)
            {
                lastCleanedRegs[4] = prevTonePeriod[4];
                lastCleanedRegs[5] = prevTonePeriod[5];
                stats.unusedToneC++;
            }
            else
            {
                prevTonePeriod[4] = lastOrigRegs[4];
                prevTonePeriod[5] = lastOrigRegs[5];
            }
        }

        // Clean envelope period.

        if (flags & cleanEnvelope)
        {
            if ((lastOrigRegs[8] & 16) == 0 && (lastOrigRegs[9] & 16) == 0 && (lastOrigRegs[10] & 16) == 0)
            {
                lastCleanedRegs[11] = prevEnvelopePeriod[11];
                lastCleanedRegs[12] = prevEnvelopePeriod[12];
                stats.unusedEnvelope++;
            }
            else
            {
                prevEnvelopePeriod[11] = lastOrigRegs[11];
                prevEnvelopePeriod[12] = lastOrigRegs[12];
            }
        }

        /* clean envelope form */

        if (flags & cleanEnvForm)
        {
            if ((lastOrigRegs[8] & 16) == 0 && (lastOrigRegs[9] & 16) == 0 && (lastOrigRegs[10] & 16) == 0)
            {
                lastCleanedRegs[13] = prevEnvelopeForm[13];
                stats.unusedEnvForm++;
            }
            else
            {
                prevEnvelopeForm[13] = lastOrigRegs[13];
            }
        }

        /* clean noise period */

        if (flags & cleanNoise)
        {
            if ((lastOrigRegs[7] & 8) != 0 && (lastOrigRegs[7] & 16) != 0 && (lastOrigRegs[7] & 32) != 0)
            {
                lastCleanedRegs[6] = prevNoisePeriod[6];
                stats.unusedNoise++;
            }
            else
            {
                prevNoisePeriod[6] = lastCleanedRegs[6];
            }
        }
    }

    void extendToFullChangeIfNeed(int firstThreshold, int secondThreshold)
    {
        decltype(changedRegs) firstReg, secondReg;
        for (const auto& reg : changedRegs)
        {
            if (reg.first < 6)
                firstReg.insert(reg);
            else if (reg.first != 13)
                secondReg.insert(reg);
        }

        if (firstReg.size() >= firstThreshold)
        {
            // Regs are about to full. Extend them to full regs.
            for (int i = 0; i < 6; ++i)
                changedRegs[i] = lastCleanedRegs[i];
        }

        if (secondReg.size() >= secondThreshold)
        {
            // Regs are about to full. Extend them to full regs (exclude reg13)
            for (int i = 6; i < 13; ++i)
                changedRegs[i] = lastCleanedRegs[i];
        }
    }

    bool writeRegs()
    {
        if (changedRegs.empty())
            return false;

        if (prevTonePeriod.empty())
        {
            for (int i = 0; i < 13; ++i)
            {
                changedRegs.emplace(i, 0);
                lastOrigRegs[i] = 0;
            }

            // Initial value
            prevTonePeriod = lastOrigRegs;
            prevEnvelopePeriod = lastOrigRegs;
            prevEnvelopeForm = lastOrigRegs;
            prevNoisePeriod = lastOrigRegs;
        }

        int unusedEnvForm = stats.unusedEnvForm;
        lastCleanedRegs = lastOrigRegs;
        if (flags & cleanRegs)
            doCleanRegs();


        RegMap delta;
        for (int i = 0; i < 14; ++i)
        {
            if (firstFrame || lastCleanedRegs[i] != prevCleanedRegs[i])
                delta[i] = lastCleanedRegs[i];
        }
        firstFrame = false;
        prevCleanedRegs = lastCleanedRegs;

        if (changedRegs.count(13) && !(flags & cleanRegs))
            delta[13] = changedRegs[13]; //< Can be retrig.

        changedRegs = delta;
        if (changedRegs.empty())
            return false;

        if (flags & dumpPsg)
        {
            if (updatedPsgData.empty())
                updatedPsgData.insert(updatedPsgData.end(), srcPsgData.begin(), srcPsgData.begin() + 16);

            updatedPsgData.push_back(0xff);
            for (const auto& reg : changedRegs)
            {
                updatedPsgData.push_back(reg.first);
                updatedPsgData.push_back(reg.second);
            }
        }

        if (stats.level < l3 || symbolsToInflate.count(toSymbol(changedRegs)))
            extendToFullChangeIfNeed(5, 5);
        //else if (stats.level == l4)
        //    extendToFullChangeIfNeed(5, 6);

        uint16_t symbol = toSymbol(changedRegs);
        ayFrames.push_back({ symbol, lastCleanedRegs, changedRegs }); //< Flush previous frame.

        if (changedRegs.size() > 1 && changedRegs.size() <= 6)
        {
            uint16_t mask = longRegMask(changedRegs);
            ++stats.maskToUsage[mask];
        }

        ++stats.outPsgFrames;

        changedRegs.clear();
        return true;
    }

    void writeDelay(int delay)
    {
        if (flags & dumpPsg)
        {
            for (int i = 0; i < delay; ++i)
                updatedPsgData.push_back(0xff);
        }

        if (delay < 1)
            return;

        ++stats.outPsgFrames;

        if (!ayFrames.empty() && ayFrames.rbegin()->symbol <= kMaxDelay)
        {
            // Cleanup regs could wipe out regs chaning at all. That way it could be possible two delay records in a row. Merge them.
            delay += lastDelayValue;
            for (int i = 0; i < lastDelayBytes; ++i)
                ayFrames.pop_back();
        }

        int prevSize = ayFrames.size();
        lastDelayValue = delay;
        while (delay > 0)
        {
            uint16_t d = std::min(kMaxDelay, delay);
            ayFrames.push_back({ d }); //< Special code for delay
            delay -= d;
        }
        lastDelayBytes = ayFrames.size() - prevSize;

    }

    void serializeDelayTimings(int count, int trbRep)
    {
        if (count == 1)
        {
            timingsData.push_back(th.delayTimings(TimingState::single, trbRep));
        }
        else
        {
            auto state = count > 16 ? TimingState::longFirst : TimingState::first;
            timingsData.push_back(th.delayTimings(state, trbRep));
            for (int i = 1; i < count - 1; ++i)
                timingsData.push_back(th.delayTimings(TimingState::mid, trbRep));
            timingsData.push_back(th.delayTimings(TimingState::last, trbRep));
        }
    }

    void serializeDelay(int count)
    {
        if (count > 0)
            serializeDelayTimings(count, 0);

        while (count > 0)
        {
            int value = std::min(kMaxDelay, count);
            if (value > 16)
            {
                compressedData.push_back(0);
                compressedData.push_back((uint8_t)value - 1);
            }
            else
            {
                uint8_t header = 0x10;
                compressedData.push_back(header + value - 1);
            }
            count -= value;
        }
    };

    void serializeRef(uint16_t pos, int len, uint8_t reducedLen)
    {
        int refTiming = serializeRefTimings(pos, len, reducedLen, 0);
        if (stats.level == CompressionLevel::l4)
        {
            const auto symbol = ayFrames[pos].symbol;
            if (refTiming > kMaxTimeForL4)
                ++symbolsToInflate[symbol];
        }

        int offset = frameOffsets[pos];
        int recordSize = len == 1 ? 2 : 3;
        int16_t delta = offset - compressedData.size() - recordSize;
        if (len > 1 && stats.level < CompressionLevel::l4)
            ++delta;
        assert(delta < 0);

        uint8_t* ptr = (uint8_t*)&delta;

        if (len == 1)
            ptr[1] &= ~0x40; // reset 6-th bit

        // Serialize in network byte order
        compressedData.push_back(ptr[1]);
        compressedData.push_back(ptr[0]);

        if (len > 1)
            compressedData.push_back(reducedLen);
    };

    int shortRefTiming(int pos, int trbRep)
    {
        auto symbol = ayFrames[pos].symbol;
        auto regs = symbolToRegs[symbol];

        return th.shortRefTimings(regs, symbol, trbRep);
    }

    int longRefInitTiming(int pos, int symbolsLeftAtLevel)
    {
        auto symbol = ayFrames[pos].symbol;
        auto regs = symbolToRegs[symbol];
        return th.longRefInitTiming(pos, regs, symbol, symbolsLeftAtLevel);
    }

    bool isNestedShortRef(int pos)
    {
        bool result = refInfo[pos].refLen == 1;
        return result;
    }

    bool isNestedLongRefStart(int pos)
    {
        bool result = refInfo[pos].refLen > 1 && refInfo[pos].refTo >= 0;
        return result;
    }

    int serializeRefTimings(int pos, int len, int reducedLen, int prevReducedLen)
    {
        if (len == 1)
        {
            timingsData.push_back(shortRefTiming(pos, reducedLen)); // First frame
            return *timingsData.rbegin();
        }

        const int endPos = pos + len;

        int result = longRefInitTiming(pos, prevReducedLen);
        timingsData.push_back(result); // First frame
        ++pos;
        for (; pos < endPos; ++pos)
        {
            auto symbol = ayFrames[pos].symbol;
            if (symbol <= kMaxDelay)
            {
                serializeDelayTimings(symbol, reducedLen);
            }
            else if (isNestedShortRef(pos))
            {
                timingsData.push_back(shortRefTiming(refInfo[pos].refTo, reducedLen));
                if (stats.level < CompressionLevel::l4)
                    continue; //< skip decrement reducedLen
            }
            else if (isNestedLongRefStart(pos))
            {
                serializeRefTimings(refInfo[pos].refTo, refInfo[pos].refLen, refInfo[pos].reducedLen, reducedLen);
                pos += refInfo[pos].refLen - 1;
            }
            else
            {
                auto regs = symbolToRegs[symbol];
                int result = th.frameTimings(regs, reducedLen, symbol);
                timingsData.push_back(result);
            }
            --reducedLen;
        }
        assert(reducedLen == 0);
        assert(pos == endPos);
        return result;
    }


    void serializeFrame(uint16_t pos)
    {
        int prevSize = compressedData.size();

        uint16_t symbol = ayFrames[pos].symbol;
        auto regs = symbolToRegs[symbol];

        timingsData.push_back(th.frameTimings(regs, 0, symbol));

        uint8_t header1 = 0;

        uint16_t longMask = longRegMask(regs);
        bool usePsg2 = isPsg2(regs, symbol, stats);


        auto itr = stats.maskIndex.find(longMask);
        if (usePsg2)
        {
            if (itr != stats.maskIndex.end())
            {
                header1 = 0x20 + itr->second;
            }
            else
            {
                auto mask = (makeRegMask(regs, 0, 6) >> 2);
                header1 = 0x40 + mask;
            }
            compressedData.push_back(header1);

            int firstsHalfRegs = 0; //< Statistics
            if (itr != stats.maskIndex.end())
            {
                for (auto itr = regs.rbegin(); itr != regs.rend(); ++itr)
                {
                    if (itr->first < 6)
                        compressedData.push_back(itr->second);
                }
            }
            else
            {
                for (const auto& reg : regs)
                {
                    if (reg.first < 6)
                    {
                        compressedData.push_back(reg.second); // reg value
                        ++firstsHalfRegs;
                    }
                }
            }
            ++stats.firstHalfRegs[firstsHalfRegs];
            ++stats.secondHalfRegs[regs.size() - firstsHalfRegs];

            uint8_t header2 = makeRegMask(regs, 6, 14);
            header2 = reverseBits(header2);
            if (itr == stats.maskIndex.end())
                compressedData.push_back(header2);

            if ((header2 & 0x7f) == 0 && itr == stats.maskIndex.end())
            {
                // play_all branch. Serialize regs in regular order
                for (auto itr = regs.begin(); itr != regs.end(); ++itr)
                {
                    if (itr->first >= 6)
                        compressedData.push_back(itr->second);
                }
            }
            else
            {
                // play_by_mask branch. Serialize regs in backward order
                for (auto itr = regs.rbegin(); itr != regs.rend(); ++itr)
                {
                    if (itr->first >= 6)
                        compressedData.push_back(itr->second); // reg value
                }
            }
        }
        else
        {
            assert(regs.size() == 1);
            for (const auto& reg : regs)
            {
                compressedData.push_back(reg.first + 1);
                compressedData.push_back(reg.second); // reg value
                header1 = 0;
            }
        }

        stats.ownBytes += compressedData.size() - prevSize;
    }

    int serializedFrameSize(uint16_t pos)
    {
        const uint16_t symbol = ayFrames[pos].symbol;
        if (symbol <= kMaxDelay)
            return symbol <= 16 ? 1 : 2;

        auto regs = symbolToRegs[symbol];

        if (isPsg2(regs, symbol, stats))
        {
            int headerSize = 2;
            uint16_t mask = longRegMask(regs);
            if (stats.maskIndex.count(mask))
                --headerSize;

            return headerSize + regs.size();
        }


        return regs.size() * 2;
    };

    bool isFrameCover(const FrameInfo& master, const FrameInfo& slave)
    {
        if (master.symbol == slave.symbol)
            return true;

        if (stats.level < l1)
            return false;

        if (slave.symbol <= kMaxDelay || master.delta.size() < slave.delta.size())
            return false;

        auto itr = master.delta.begin();
        for (const auto& reg : slave.delta)
        {
            while (itr != master.delta.end() && itr->first < reg.first)
                ++itr;
            if (itr == master.delta.end() || itr->first != reg.first || itr->second != reg.second)
                return false;
        }
        for (const auto& reg : master.delta)
        {
            if (slave.fullState[reg.first] != reg.second)
                return false;
        }
        if (master.delta.count(13) == 1 && slave.delta.count(13) == 0)
            return false;

        return true;
    }

    auto findRef(int pos)
    {
        const int maxLength = std::min(255, (int)ayFrames.size() - pos);

        int maxChainLen = -1;
        int chainPos = -1;
        int bestBenifit = 0;
        int maxReducedLen = -1;

        int maxAllowedReducedLen = stats.level < l4 ? 128 : 255;

        for (int i = 0; i < pos; ++i)
        {
            if (frameOffsets[pos] - frameOffsets[i] + 3 > kMaxRefOffset)
                continue;

            if (isFrameCover(ayFrames[i], ayFrames[pos]) && refInfo[i].refLen == 0)
            {
                int chainLen = 0;
                int reducedLen = 0;
                int serializedSize = 0;
                std::vector<int> sizes;

                for (int j = 0; j < maxLength && i + j < pos && reducedLen < maxAllowedReducedLen; ++j)
                {
                    if ((refInfo[i + j].refLen > 1 && stats.level < l4) || !isFrameCover(ayFrames[i + j], ayFrames[pos + j]))
                        break;
                    ++chainLen;
                    const auto& ref = refInfo[i + j];
                    if (ref.refLen == 0 || (ref.refLen > 1 && ref.refTo >= 0))
                    {
                        ++reducedLen;
                    }
                    else if (ref.refLen == 1)
                    {
                        // Don't count 1-symbol refs during ref serialization for Levels [0..3]
                        if (stats.level >= l4)
                            ++reducedLen;
                    }

                    serializedSize += serializedFrameSize(pos + j);
                    sizes.push_back(serializedSize);
                }

                bool truncateLastRef2 = false;
                while (chainLen > 0 && refInfo[i + chainLen - 1].refLen > 1
                    && refInfo[i + chainLen - 1].offsetInRef < refInfo[i + chainLen - 1].refLen - 1)
                {
                    sizes.pop_back();
                    --chainLen;
                    truncateLastRef2 = true;
                }
                if (truncateLastRef2)
                    --reducedLen;

                if (stats.level < l4)
                {
                    while (chainLen > 0 && refInfo[i + chainLen - 1].refLen == 1)
                    {
                        sizes.pop_back();
                        --chainLen;
                    }
                }

                int benifit = *sizes.rbegin() - (chainLen == 1 ? 2 : 3);
                if (benifit > bestBenifit)
                {
                    bestBenifit = benifit;
                    maxChainLen = chainLen;
                    maxReducedLen = reducedLen;
                    chainPos = i;
                }
            }
        }
        if (stats.level < l2)
        {
            if (maxChainLen > 1)
            {

                const auto symbol = ayFrames[chainPos].symbol;
                const auto regs = symbolToRegs[symbol];
                int t = th.pl0xTimings(regs, symbol);
                int overrun = (168 - 141) - (661 - t);
                if (overrun > 0)
                    return std::tuple<int, int, int> { -1, -1, -1}; //< Long refs is slower
            }
        }

        return std::tuple<int, int, int> { chainPos, maxChainLen, maxReducedLen - 1};
    }

public:

    void updateRefInfo(int i, int pos, int len, int reducedLen)
    {
        refInfo[i].refTo = pos;
        refInfo[i].reducedLen = reducedLen;
        for (int j = i; j < i + len; ++j)
        {
            assert(refInfo[j].refLen == 0);
            refInfo[j].refLen = len;
            refInfo[j].offsetInRef = j - i;
        }
        if (len > 1)
            updateNestedLevel(pos, len, 1);
    }

    void updateNestedLevel(int pos, int len, int level)
    {
        for (int j = pos; j < pos + len; ++j)
            refInfo[j].level = std::max(refInfo[j].level, level);
        for (int j = pos; j < pos + len; ++j)
        {
            if (refInfo[j].refTo >= 0 && refInfo[j].refLen > 1)
                updateNestedLevel(refInfo[j].refTo, refInfo[j].refLen, level + 1);
        }
    }

    int cutDelay(const CutRange& range, int v)
    {
        if (!range.isEmpty())
        {
            v = std::min(range.to - stats.inPsgFrames, v);
            if (stats.inPsgFrames < range.from)
            {
                if (stats.inPsgFrames + v >= range.from)
                    v = std::min(range.from - stats.inPsgFrames, v);
                else
                    v = 0;
            }
        }
        return v;
    }

    int parsePsg(const std::string& inputFileName)
    {
        using namespace std;

        ifstream fileIn;
        fileIn.open(inputFileName, std::ios::binary);
        if (!fileIn.is_open())
        {
            std::cerr << "Can't open input file " << inputFileName << std::endl;
            return -1;
        }

        fileIn.seekg(0, ios::end);
        int fileSize = fileIn.tellg();
        fileIn.seekg(0, ios::beg);

        srcPsgData.resize(fileSize);
        fileIn.read((char*)srcPsgData.data(), fileSize);
        firstFrame = true;

        const uint8_t* pos = srcPsgData.data() + 16;
        const uint8_t* end = srcPsgData.data() + srcPsgData.size();

        for (int i = 0; i <= kMaxDelay; ++i)
        {
            RegMap fakeRegs;
            fakeRegs[-1] = i;
            regsToSymbol.emplace(fakeRegs, i);
            symbolToRegs.emplace(i, fakeRegs);
        }

        int delayCounter = 0;


        CutRange range;
        if (!cutRanges.empty())
        {
            range = cutRanges[0];
            cutRanges.erase(cutRanges.begin());
        }

        while (pos < end)
        {
            if (!range.isEmpty() && stats.inPsgFrames >= range.to)
            {
                if (!cutRanges.empty())
                {
                    range = cutRanges[0];
                    cutRanges.erase(cutRanges.begin());
                    continue;
                }
                else
                {
                    break;
                }
            }

            uint8_t value = *pos;
            if (value >= 0xfe)
            {
                bool needSkip = !range.isEmpty() && stats.inPsgFrames < range.from;
                if (!changedRegs.empty())
                {
                    if (!needSkip)
                    {
                        if (!writeRegs())
                            ++delayCounter; //< Regs were cleaned up.
                    }
                }

                if (value == 0xff)
                {
                    if (!needSkip)
                        ++delayCounter;
                    ++stats.inPsgFrames;
                    ++pos;
                }
                else
                {
                    int v = pos[1] * 4;
                    v = cutDelay(range, v);
                    stats.inPsgFrames += pos[1] * 4;
                    delayCounter += v;
                    pos += 2;
                }
            }
            else if (value == 0xfd)
            {
                break;
            }
            else
            {
                writeDelay(delayCounter - 1);
                delayCounter = 0;

                assert(value <= 13);
                changedRegs[value] = pos[1];
                lastOrigRegs[value] = pos[1];
                ++stats.regsChange[value];
                pos += 2;
            }
        }

        if (!changedRegs.empty())
        {
            if (!writeRegs())
                ++delayCounter; //< Regs were cleaned up.
        }
        delayCounter = cutDelay(range, delayCounter);
        writeDelay(delayCounter);

        for (const auto& v: stats.maskToUsage)
            stats.usageToMask.emplace(v.second, v.first);
        while (stats.usageToMask.size() > kPsg2iSize)
            stats.usageToMask.erase(stats.usageToMask.begin());
        stats.maskToUsage.clear();
        int i = 0;
        for (const auto& v: stats.usageToMask)
        {
            stats.maskToUsage[v.second] = v.first;
            stats.maskIndex[v.second] = i++;
        }

        return 0;
    }

    int packPsg(const std::string& outputFileName)
    {
        using namespace std;

        ofstream fileOut;
        fileOut.open(outputFileName, std::ios::binary | std::ios::trunc);
        if (!fileOut.is_open())
        {
            std::cerr << "Can't open output file " << outputFileName << std::endl;
            return -1;
        }

        compressedData.resize(kPsg2iSize * 2);
        for (const auto& value: stats.maskIndex)
        {
            const int offset = value.second * 2;
            compressedData[offset] = (uint8_t)value.first;
            compressedData[offset+1] = (value.first >> 8);
        }

        // compressData
        refInfo.resize(ayFrames.size());

        for (int i = 0; i < ayFrames.size();)
        {
            while (frameOffsets.size() <= i)
                frameOffsets.push_back(compressedData.size());

            if (ayFrames[i].symbol <= kMaxDelay)
            {
                serializeDelay(ayFrames[i].symbol);
                stats.emptyFrames += ayFrames[i].symbol;
                ++stats.emptyCnt;
                ++i;
            }
            else
            {
                const auto symbol = ayFrames[i].symbol;

                const auto [pos, len, reducedLen] = findRef(i);
                if (len > 0)
                {
                    serializeRef(pos, len, reducedLen);
                    updateRefInfo(i, pos, len, reducedLen);

                    i += len;
                    if (len == 1)
                        stats.singleRepeat++;
                    stats.allRepeat++;
                    stats.allRepeatFrames += len;
                }
                else
                {
                    serializeFrame(i);
                    ++i;
                    ++stats.ownCnt;
                }
            }
        }

        compressedData.push_back(kEndTrackMarker);

        for (const auto& v : symbolToRegs)
            ++stats.frameRegs[v.second.size()];


        fileOut.write((const char*)compressedData.data(), compressedData.size());
        fileOut.close();

        return 0;
    }

    int writeRawPsg(const std::string& outputFileName)
    {
        using namespace std;

        ofstream fileOut;
        fileOut.open(outputFileName, std::ios::binary | std::ios::trunc);
        if (!fileOut.is_open())
        {
            std::cerr << "Can't open output file " << outputFileName << std::endl;
            return -1;
        }

        fileOut.write((const char*) updatedPsgData.data(), updatedPsgData.size());

        return 0;
    }

    int writeTimingsFile(const std::string& outputFileName)
    {
        if (flags & addScf)
        {
            for (auto& t: timingsData)
                t += 4;
        }

        using namespace std;

        ofstream fileOut;
        fileOut.open(outputFileName, std::ios::trunc);
        if (!fileOut.is_open())
        {
            std::cerr << "Can't open output file " << outputFileName << std::endl;
            return -1;
        }

        fileOut << "frame; timings; with call" << std::endl;
        for (int i = 0; i < timingsData.size(); ++i)
        {
            fileOut << i << ";" << timingsData[i] << ";" << timingsData[i]+10 << ";" << std::endl;
            
        }

        return 0;
    }

    int maxNestedLevel() const 
    {
        int result = 0;
        for (const auto& ref : refInfo)
            result = std::max(result, ref.level);
        return result;
    }       

    private:
        int lastDelayValue = 0;
        int lastDelayBytes = 0;

};

bool hasShortOpt(const std::string& s, char option)
{
    if (s.size() >= 2 && s[0] == '-' && s[1] == '-')
        return false;
    if (s.empty() || s[0] != '-')
        return false;
    return s.find(option) != std::string::npos;
}

CutRange parseRange(const std::string& value)
{
    CutRange result;
    int pos = value.find(',');
    if (pos == -1)
    {
        result.to = std::stoi(value);
    }
    else
    {
        auto s1 = value.substr(0, pos);
        auto s2 = value.substr(pos+1);
        result.from = std::stoi(s1);
        result.to = std::stoi(s2);
    }
    return result;
}

int parseArgs(int argc, char** argv, PgsPacker* packer)
{
    for (int i = 1; i < argc - 2; ++i)
    {
        const std::string s = argv[i];
        if (hasShortOpt(s, 'l') || s == "--level")
        {
            if (i == argc - 1)
            {
                std::cerr << "It need to define compression leven in range [0..4] after the argument '--level'" << std::endl;
                return -1;
            }
            int value = atoi(argv[i + 1]);
            if (value < 0 || value > 5)
            {
                std::cerr << "Invalid compression level " << value << ". Expected value in range [0..5]" << std::endl;
                return -1;
            }
            packer->stats.level = (CompressionLevel)value;
        }
        if (s == "--cut")
        {
            if (i == argc - 1)
            {
                std::cerr << "It need to define cut value in frames after the argument '--cut'. Example: 0,1000." << std::endl;
                return -1;
            }
            auto range = parseRange(argv[i + 1]);
            packer->cutRanges.push_back(range);
        }
        if (hasShortOpt(s, 'c') || s == "--clean")
        {
            packer->flags |= cleanRegs;
        }
        if (hasShortOpt(s, 'k') || s == "--keep")
        {
            packer->flags &= ~cleanRegs;
        }
        if (hasShortOpt(s, 'd') || s == "--dump")
        {
            packer->flags |= dumpPsg;
        }
        if (hasShortOpt(s, 'i') || s == "--info")
        {
            packer->flags |= dumpTimings;
        }
        if (s == "--scf")
        {
            // Undocumented option. Ensure 'c' flag is always on after the player. This option affects timings calculating only. It reffers to the internal player version from zx_scrool
            packer->flags |= addScf;
            packer->stats.addScf = true;
        }
    }
    return 0;
}

int main(int argc, char** argv)
{
    std::unique_ptr<PgsPacker> packer(new PgsPacker());

    std::cout << "Fast PSG packer v.0.9b" << std::endl;
    if (argc < 3)
    {
        std::cout << "Usage: psg_pack [OPTION] input_file output_file" << std::endl;
        std::cout << "Example: psg_pack --level 1 file1.psg packetd.mus" << std::endl;
        std::cout << "Recomended compression levels are level 1 (fast play, up to 799t) and level 4 (small size, up to 930t)" << std::endl;
        std::cout << "Default options: --level 1 --clean" << std::endl;
        std::cout << "" << std::endl;
        std::cout << "Options:" << std::endl;
        std::cout << "-l, --level\t Compression level:" << std::endl;

        std::cout << "\t  0\tMaximum speed. Max frame time=799t" << std::endl;
        std::cout << "\t* 1\tSame max frame time 799t, avarage frame size worse a little bit, better compression (default)" << std::endl;
        std::cout << "\t  2\tMax frame time about 827t, better compression" << std::endl;
        std::cout << "\t  3\tMax frame time above 900t, better compression" << std::endl;
        std::cout << "\t* 4\tMax frame time up to 930t, signitifally better compression. Requires 'l4_psg_player.asm'" << std::endl;
        std::cout << "\t  5\tMax frame time up to 1032t, a bit better compression. Requires 'l4_psg_player.asm'" << std::endl;

        std::cout << "-c, --clean\t Clean AY registers before packing. Improve compression level but incompatible with some tracks." << std::endl;
        std::cout << "-k, --keep\t --Don't clean AY regiaters." << std::endl;
        std::cout << "-i, --info\t Print timings info for each compresed frame." << std::endl;
        std::cout << "-d, --dump\t Dump uncompressed PSG frame to the separate file." << std::endl;
        std::cout << "--cut <range>\t Cut source track. Include frames [N1..N2). Example: --cut 0,1000. The option '--cut <range>' can be repeated several times." << std::endl;
        return -1;
    }
    
    int result = parseArgs(argc, argv, packer.get());
    if (result != 0)
        return result;

    using namespace std::chrono;

    std::cout << "Starting compression at level " << packer->stats.level << std::endl;
    auto timeBegin = std::chrono::steady_clock::now();
    auto prevSymbolsToInflate = packer->symbolsToInflate;
    result = packer->parsePsg(argv[argc-2]);
    if (result == 0)
        result = packer->packPsg(argv[argc - 1]);
    if (result != 0)
        return result;

    while (packer->symbolsToInflate.size() != prevSymbolsToInflate.size())
    {
        prevSymbolsToInflate = packer->symbolsToInflate;
        for (auto& s : prevSymbolsToInflate)
            s.second = 0;

        // Timings are fail. Pack again.
        packer.reset(new PgsPacker());
        parseArgs(argc, argv, packer.get());
        packer->symbolsToInflate = prevSymbolsToInflate;

        result = packer->parsePsg(argv[argc - 2]);
        if (result == 0)
            result = packer->packPsg(argv[argc - 1]);
        if (result != 0)
            return result;
    }


    if (packer->flags & dumpPsg)
        packer->writeRawPsg(std::string(argv[argc - 1]) + ".psg");
    if (packer->flags & dumpTimings)
        packer->writeTimingsFile(std::string(argv[argc - 1]) + ".csv");

    auto timeEnd = steady_clock::now();

    std::cout << "Compression done in " << duration_cast<milliseconds>(timeEnd - timeBegin).count() / 1000.0 << " second(s)" << std::endl;
    std::cout << "Input size:\t" << packer->srcPsgData.size() << std::endl;
    std::cout << "Packed size:\t" << packer->compressedData.size() << std::endl;
    std::cout << "1-byte refs:\t" << packer->stats.singleRepeat << std::endl;
    std::cout << "Total refs:\t" << packer->stats.allRepeat << std::endl;
    std::cout << "Packed frames:\t" << packer->ayFrames.size() << std::endl;
    std::cout << "Empty frames:\t" << packer->stats.emptyCnt << std::endl;
    std::cout << "Frames in refs:\t" << packer->stats.allRepeatFrames << std::endl;
    std::cout << "Total frames:\t" << packer->stats.outPsgFrames << std::endl;
    if (packer->stats.level >= 4)
        std::cout << "Nested level:\t" << packer->maxNestedLevel() << std::endl;
    

    int pos = 0;
    int t = 0;
    int totalTicks = 0;
    for (int i = 0; i < packer->timingsData.size(); ++i)
    {
        if (packer->timingsData[i] > t)
        {
            pos = i;
            t = packer->timingsData[i];
        }
        totalTicks += packer->timingsData[i];
    }

    std::string comment;
    std::cout << "The longest frame: " << t << "t" << comment << ", pos " << pos << ". Avarage frame: " << totalTicks / (packer->timingsData.size()) << "t" << std::endl;

    return 0;
}
