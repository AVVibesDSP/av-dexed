#include "Cartridge.h"

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
 */
uint8_t normparm(uint8_t value, char max, int id) {
    if ( value <= max )
        return value;
    // if this is beyond the max, we expect a 0-255 range, normalize this
    // to the expected return value; and this value as a random data.
    return ((float)value)/255 * max;
}


int Cartridge::load(const uint8_t *stream, int size) {
    hasData = false;
    memset(voiceData, 0, sizeof(voiceData));
    memset(perfData, 0, sizeof(perfData));

    if (stream == nullptr || size <= 0)
        return -1;

    const uint8_t *pos = stream;

    if ( size < 4096 ) {
        memcpy(voiceData+6, pos, size);
        hasData = true;
        DXLOG("ERROR: Cartridge too small sysex rc=2");
        return 2;
    }

    if ( pos[0] != 0xF0 ) {
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
            return sysexChecksum(voiceData + 6, 4096) == message[4102] ? 0 : 1;
        }
        offset = end + 1;
    }

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

    if ( ! f.existsAsFile() ) {
        // file doesn't exists, create it
        return f.replaceWithData(voiceData, SYSEX_SIZE);
    }

    std::unique_ptr<juce::FileInputStream> fis = f.createInputStream();
    if ( fis == NULL )
        return -1;

    uint8_t buffer[65535];
    int sz = fis->read(buffer, 65535);

    // if the file is smaller than 4104, it probably needs to be overridden.
    if ( sz <= 4104 ) {
        return f.replaceWithData(voiceData, SYSEX_SIZE);
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

    return f.replaceWithData(buffer, sz);
}

Program Cartridge::unpackProgram(int idx) {
    if (idx < 0 || idx >= 32 || !hasData)
        return Program::initialProgram();
    uint8_t ret[Program::PROGRAM_SIZE];
    uint8_t *bulk = static_cast<uint8_t *>(voiceData) + 6 + (idx * 128);
    for (int op = 0; op < 6; op++) {
        // eg rate and level, brk pt, depth, scaling

        for(int i=0; i<11; i++) {
            uint8_t currparm = bulk[op * 17 + i] & 0x7F; // mask BIT7 (don't care per sysex spec)
            ret[op * 21 + i] = normparm(currparm, 99, i);
        }

        char leftrightcurves = bulk[op * 17 + 11]&0xF; // bits 4-7 don't care per sysex spec
        ret[op * 21 + 11] = leftrightcurves & 3;
        ret[op * 21 + 12] = (leftrightcurves >> 2) & 3;
        char detune_rs = bulk[op * 17 + 12]&0x7F;
        ret[op * 21 + 13] = detune_rs & 7;
        char kvs_ams = bulk[op * 17 + 13]&0x1F; // bits 5-7 don't care per sysex spec
        ret[op * 21 + 14] = kvs_ams & 3;
        ret[op * 21 + 15] = (kvs_ams >> 2) & 7;
        ret[op * 21 + 16] = normparm(bulk[op * 17 + 14]&0x7F, 99, op * 21 + 16);
        char fcoarse_mode = bulk[op * 17 + 15]&0x3F; //bits 6-7 don't care per sysex spec
        ret[op * 21 + 17] = fcoarse_mode & 1;
        ret[op * 21 + 18] = (fcoarse_mode >> 1)&0x1F;
        ret[op * 21 + 19] = normparm(bulk[op * 17 + 16]&0x7F, 99, op * 21 + 19);
        ret[op * 21 + 20] = normparm((detune_rs >> 3) &0x7F, 14, op * 21 + 20);
    }

    for (int i=0; i<8; i++)  {
        uint8_t currparm = bulk[102 + i] & 0x7F; // mask BIT7 (don't care per sysex spec)
        ret[126+i] = normparm(currparm, 99, 126+i);
    }
    ret[134] = normparm(bulk[110]&0x1F, 31, 134); // bits 5-7 are don't care per sysex spec

    char oks_fb = bulk[111]&0xF;//bits 4-7 are don't care per spec
    ret[135] = oks_fb & 7;
    ret[136] = oks_fb >> 3;
    ret[137] = normparm(bulk[112] & 0x7F, 99, 137);
    ret[138] = normparm(bulk[113] & 0x7F, 99, 138);
    ret[139] = normparm(bulk[114] & 0x7F, 99, 139);
    ret[140] = normparm(bulk[115] & 0x7F, 99, 140);
    char lpms_lfw_lks = bulk[116] & 0x7F;
    ret[141] = lpms_lfw_lks & 1;
    ret[142] = normparm((lpms_lfw_lks >> 1) & 7, 5, 142);
    ret[143] = lpms_lfw_lks >> 4;
    ret[144] = normparm(bulk[117] & 0x7F, 48, 144);
    for (int name_idx = 0; name_idx < 10; name_idx++) {
        ret[145 + name_idx] = bulk[118 + name_idx] & 0x7F;
    }

    return { ret };
}

/**
 * Pack a program into a 32 packed sysex
 */
bool Cartridge::packProgram(const uint8_t *src, int idx, const juce::String& name) {
    if (src == nullptr || idx < 0 || idx >= 32)
        return false;

    uint8_t *bulk = voiceData + 6 + (idx * 128);

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
    hasData = true;
    return true;
}
