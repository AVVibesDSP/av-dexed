#include "Cartridge.h"
#include "AVDexedLibrary.h"
#include "AVDexedSysex.h"

void exportSysexPgm(uint8_t *dest, const uint8_t *src) {
    uint8_t header[] = { 0xF0, 0x43, 0x00, 0x00, 0x01, 0x1B };

    memcpy(dest, header, 6);

    // copy 1 unpacked voices
    memcpy(dest+6, src, 155);

    // make checksum for dump
    uint8_t footer[] = { sysexChecksum(src, 155), 0xF7 };

    memcpy(dest+161, footer, 2);
}

/**
 * This function normalize data that comes from corrupted sysex.
 * It used to avoid engine crashing upon extreme values
 *
 * AV Dexed: unchanged from Dexed 1.1.0, and used where Dexed uses it: the
 * pitch envelope and the algorithm, whose engine tables have 100 and 32
 * entries. Every other field keeps the value the bank holds (an output level
 * of 102, found in real DX7 banks, plays as in Dexed); rescaling those too,
 * as 0.1.0 did, left such voices nearly mute.
 */
uint8_t normparm(uint8_t value, char max, int id) {
    if ( value <= max )
        return value;
    // if this is beyond the max, we expect a 0-255 range, normalize this
    // to the expected return value; and this value as a random data.
    return ((float)value)/255 * max;
}

namespace {
/** LFO speed and delay above 99: Dexed 1.1.0 reads past its 100-entry speed
    table and shifts by a negative amount for the delay (undefined behaviour,
    an unpredictable LFO). These two fields alone are limited to 99. */
uint8_t avLfoTime(uint8_t value) { return value <= 99 ? value : 99; }
}


void Cartridge::attachAmem(const uint8_t* stream, int size) {
    hasAmem = false;
    const auto contents = AVDexedSysex::scan(stream, size_t(juce::jmin(size, 65535)));
    for (const auto& bank : contents.banks)
        if (bank.hasAmem && std::memcmp(bank.voices.data(), voiceData + 6, AVDexedSysex::bankBytes) == 0) {
            std::memcpy(amemData, bank.amem.data(), sizeof(amemData));
            hasAmem = true;
            return;
        }
}

int Cartridge::load(const uint8_t *stream, int size) {
    hasData = false;
    hasAmem = false;
    memset(voiceData, 0, sizeof(voiceData));
    memset(perfData, 0, sizeof(perfData));

    if (stream == nullptr || size <= 0)
        return -1;

    const uint8_t *pos = stream;

    // AV Dexed: the first bank of what the standard reading below does not
    // find (a cartridge saved without its header, a librarian's header before
    // the messages, the last F7 missing). A file the standard reading accepts
    // is read exactly as before.
    const auto firstScannedBank = [this, stream, size]() -> int {
        const auto contents = AVDexedSysex::scan(stream, size_t(juce::jmin(size, 65535)));
        if (contents.banks.empty())
            return -1;
        const auto& bank = contents.banks.front();
        memcpy(voiceData + 6, bank.voices.data(), AVDexedSysex::bankBytes);
        hasData = true;
        if (bank.hasAmem) {
            memcpy(amemData, bank.amem.data(), sizeof(amemData));
            hasAmem = true;
        }
        return bank.checksumOk ? 0 : 1;
    };

    if ( size < 4096 ) {
        memcpy(voiceData+6, pos, size);
        hasData = true;
        DXLOG("ERROR: Cartridge too small sysex rc=2");
        return 2;
    }

    if ( pos[0] != 0xF0 ) {
        if (const int rc = firstScannedBank(); rc >= 0)
            return rc;
        // it is not, just copy the first 4096 bytes
        memcpy(voiceData + 6, pos, 4096);
        hasData = true;
        DXLOG("ERROR: Cartridge stream is not a sysex rc=2");
        return 2;
    }

    // Banks can be wrapped by performance or other complete SysEx messages.
    // Advance past F7 (not onto it) and never inspect beyond the available data.
    size = juce::jmin(size, 65535);
    int offset = 0;
    while (offset < size) {
        if (stream[offset] != 0xf0) break;
        int end = offset + 1;
        while (end < size && stream[end] != 0xf7) ++end;
        if (end >= size) break;
        const int messageSize = end - offset + 1;
        const auto* message = stream + offset;
        if (messageSize == SYSEX_SIZE && message[1] == 0x43
            && (message[2] & 0xf0) == 0 && message[3] == 9
            && message[4] == 0x20 && message[5] == 0) {
            memcpy(voiceData, message, SYSEX_SIZE);
            hasData = true;
            attachAmem(stream, size);
            return sysexChecksum(voiceData + 6, 4096) == message[4102] ? 0 : 1;
        }
        offset = end + 1;
    }

    if (const int rc = firstScannedBank(); rc >= 0)
        return rc;

    // it is a sysex, but doesn't seems to be related to any DX series ...
    memcpy(voiceData + 6, stream, 4096);
    hasData = true;
    DXLOG("ERROR: Nothing in the sysex stream was DX related rc=2");
    return 2;
}

int Cartridge::saveVoice(juce::File f) {
    if (!hasData)
        return -1;

    setHeader();
    // Every write goes through a temporary file that is swapped in only once
    // it is complete: a full disk never leaves the bank empty or cut short.
    const juce::MemoryBlock bank(voiceData, SYSEX_SIZE);

    if ( ! f.existsAsFile() ) {
        // file doesn't exists, create it
        return AVDexedLibrary::writeAtomically(f, bank) ? 1 : -1;
    }

    // Banks are read from the first 64 KB of a file only. A larger file (an
    // archive of many concatenated banks) rewritten from that part would lose
    // everything after it, so it is never rewritten.
    const auto fileSize = f.getSize();
    if (fileSize >= maxRewritableFileSize)
        return -1;
    juce::MemoryBlock original;
    if (! f.loadFileAsData(original) || static_cast<juce::int64>(original.getSize()) != fileSize)
        return -1;
    auto* buffer = static_cast<uint8_t*>(original.getData());
    const int sz = static_cast<int>(original.getSize());

    // if the file is smaller than 4104, it probably needs to be overridden.
    if ( sz <= 4104 ) {
        return AVDexedLibrary::writeAtomically(f, bank) ? 1 : -1;
    }

    // To avoid to erase the performance data, we skip the sysex stream until
    // we see the header 0xF0, 0x43, 0x00, 0x09, 0x20, 0x00

    int pos = 0;
    bool found = false;
    const uint8_t header[] = SYSEX_HEADER;
    while (pos < sz) {
        if (buffer[pos] != 0xF0)
            return -1;

        int end = pos + 1;
        while (end < sz && buffer[end] != 0xF7)
            ++end;
        if (end >= sz)
            return -1;

        const int messageSize = end - pos + 1;
        if (messageSize == SYSEX_SIZE && buffer[pos + 1] == header[1]
            && (buffer[pos + 2] & 0xf0) == 0
            && memcmp(buffer + pos + 3, header + 3, sizeof(header) - 3) == 0) {
            memcpy(buffer + pos, voiceData, SYSEX_SIZE);
            found = true;
            break;
        }
        pos = end + 1;
    }

    if (! found)
        return -1;

    // The whole file goes back: every other message (performance data, the
    // embedded sound settings) stays as it was.
    return AVDexedLibrary::writeAtomically(f, original) ? 1 : -1;
}

Program Cartridge::unpackProgram(int idx) {
    if (idx < 0 || idx >= 32 || !hasData)
        return Program::initialProgram();
    // As Dexed 1.1.0 (eebb748) unpacks a voice, field for field, with two
    // differences: bit 7 of the operator bytes is masked (a MIDI data byte never
    // has it, and the DX7 ignores it), and the LFO speed and delay are limited
    // to 99 (see avLfoTime). tests/AVDexedVoiceTests.cpp compares both.
    uint8_t ret[Program::PROGRAM_SIZE];
    uint8_t *bulk = static_cast<uint8_t *>(voiceData) + 6 + (idx * 128);
    for (int op = 0; op < 6; op++) {
        // eg rate and level, brk pt, depth, scaling: as stored (Dexed copies them unchanged)
        for(int i=0; i<11; i++)
            ret[op * 21 + i] = bulk[op * 17 + i] & 0x7F;

        char leftrightcurves = bulk[op * 17 + 11]&0xF; // bits 4-7 don't care per sysex spec
        ret[op * 21 + 11] = leftrightcurves & 3;
        ret[op * 21 + 12] = (leftrightcurves >> 2) & 3;
        char detune_rs = bulk[op * 17 + 12]&0x7F;
        ret[op * 21 + 13] = detune_rs & 7;
        char kvs_ams = bulk[op * 17 + 13]&0x1F; // bits 5-7 don't care per sysex spec
        ret[op * 21 + 14] = kvs_ams & 3;
        ret[op * 21 + 15] = (kvs_ams >> 2) & 7;
        ret[op * 21 + 16] = bulk[op * 17 + 14]&0x7F;  // output level
        char fcoarse_mode = bulk[op * 17 + 15]&0x3F; //bits 6-7 don't care per sysex spec
        ret[op * 21 + 17] = fcoarse_mode & 1;
        ret[op * 21 + 18] = (fcoarse_mode >> 1)&0x1F;
        ret[op * 21 + 19] = bulk[op * 17 + 16]&0x7F;  // fine freq
        ret[op * 21 + 20] = (detune_rs >> 3) &0x7F;
    }

    for (int i=0; i<8; i++)  {
        uint8_t currparm = bulk[102 + i] & 0x7F; // mask BIT7 (don't care per sysex spec)
        ret[126+i] = normparm(currparm, 99, 126+i);
    }
    ret[134] = normparm(bulk[110]&0x1F, 31, 134); // bits 5-7 are don't care per sysex spec

    char oks_fb = bulk[111]&0xF;//bits 4-7 are don't care per spec
    ret[135] = oks_fb & 7;
    ret[136] = oks_fb >> 3;
    ret[137] = avLfoTime(bulk[112] & 0x7F); // lfs
    ret[138] = avLfoTime(bulk[113] & 0x7F); // lfd
    ret[139] = bulk[114] & 0x7F; // lpmd
    ret[140] = bulk[115] & 0x7F; // lamd
    char lpms_lfw_lks = bulk[116] & 0x7F;
    ret[141] = lpms_lfw_lks & 1;
    ret[142] = (lpms_lfw_lks >> 1) & 7;
    ret[143] = lpms_lfw_lks >> 4;
    ret[144] = bulk[117] & 0x7F;
    for (int name_idx = 0; name_idx < 10; name_idx++) {
        ret[145 + name_idx] = bulk[118 + name_idx] & 0x7F;
    }

    return { ret };
}

void Cartridge::applyBankRules(Program& voice) {
    // The same values unpackProgram() gives when this voice is packed into a
    // bank and read back: each field within the bits a bank stores it in, the
    // pitch envelope and algorithm normalised as Dexed does, the LFO speed and
    // delay limited to 99. The name and the operator switches are kept.
    for (int op = 0; op < 6; op++) {
        const int base = op * 21;
        for (int i = 0; i < 11; i++) voice[base + i] &= 0x7F;
        voice[base + 11] &= 3;
        voice[base + 12] &= 3;
        voice[base + 13] &= 7;
        voice[base + 14] &= 3;
        voice[base + 15] &= 7;
        voice[base + 16] &= 0x7F;
        voice[base + 17] &= 1;
        voice[base + 18] &= 0x1F;
        voice[base + 19] &= 0x7F;
        voice[base + 20] &= 0x0F;
    }
    for (int i = 126; i < 134; i++) voice[i] = normparm(voice[i] & 0x7F, 99, i);
    voice[134] &= 0x1F;
    voice[135] &= 7;
    voice[136] &= 1;
    voice[137] = avLfoTime(voice[137] & 0x7F);
    voice[138] = avLfoTime(voice[138] & 0x7F);
    voice[139] &= 0x7F;
    voice[140] &= 0x7F;
    voice[141] &= 1;
    voice[142] &= 7;
    voice[143] &= 7;
    voice[144] &= 0x7F;
    for (int i = 145; i < 155; i++) voice[i] &= 0x7F;
}

/**
 * Pack a program into a 32 packed sysex
 */
bool Cartridge::packProgram(const uint8_t *src, int idx, const juce::String& name) {
    if (src == nullptr || idx < 0 || idx >= 32)
        return false;

    uint8_t *bulk = voiceData + 6 + (idx * 128);
    uint8_t before[128];
    memcpy(before, bulk, 128);

    for(int op = 0; op < 6; op++) {
        // eg rate and level, brk pt, depth, scaling
        memcpy(bulk + op * 17, src + op * 21, 11);
        int pp = op*17;
        int up = op*21;

        // left curves
        bulk[pp+11] = (src[up+11]&0x03) | ((src[up+12]&0x03) << 2);
        bulk[pp+12] = (src[up+13]&0x07) | ((src[up+20]&0x0f) << 3);
        // kvs_ams
        bulk[pp+13] = (src[up+14]&0x03) | ((src[up+15]&0x07) << 2);
        // output lvl
        if ((src[155] & (1U << (5 - op))) == 0)
            bulk[pp+14] = 0;
        else
            bulk[pp+14] = src[up+16];
        // fcoarse_mode
        bulk[pp+15] = (src[up+17]&0x01) | ((src[up+18]&0x1f) << 1);
        // fine freq
        bulk[pp+16] = src[up+19];
    }
    memcpy(bulk + 102, src + 126, 9);      // pitch env, algo
    bulk[111] = (src[135]&0x07) | ((src[136]&0x01) << 3);
    memcpy(bulk + 112, src + 137, 4);      // lfo
    bulk[116] = (src[141]&0x01) | (((src[142]&0x07) << 1) | ((src[143]&0x07) << 4));
    bulk[117] = src[144];

    bool eos = false;

    for(int i=0; i < 10; i++) {
        juce::juce_wchar wc = i < name.length() ? name[i] : 0;
        char c = wc <= 127 ? static_cast<char>(wc) : ' ';
        if (c == 0)
            eos = true;
        if ( eos ) {
            bulk[118+i] = ' ';
            continue;
        }
        c = c < 32 ? ' ' : c;
        c = c > 127 ? ' ' : c;
        bulk[118+i] = c;
    }
    // AV Dexed: another voice in the slot drops the DX7II additional data
    // (unison, mono, random pitch...) that belonged to the one before.
    if (hasAmem && memcmp(before, bulk, 128) != 0)
        memset(amemData + idx * 35, 0, 35);
    hasData = true;
    return true;
}
