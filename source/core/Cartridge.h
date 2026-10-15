#pragma once

#include <juce_core/juce_core.h>
#include "Program.h"
#include "../debugger/Logger.h"

#define SYSEX_HEADER { 0xF0, 0x43, 0x00, 0x09, 0x20, 0x00 }
#define SYSEX_SIZE 4104

void exportSysexPgm(uint8_t *dest, const uint8_t *src);

class Cartridge {
    uint8_t voiceData[SYSEX_SIZE];
    uint8_t perfData[SYSEX_SIZE];
    bool hasData = false;

    void setHeader() {
        uint8_t voiceHeader[] = SYSEX_HEADER;
        memcpy(voiceData, voiceHeader, 6);
        voiceData[4102] = sysexChecksum(voiceData+6, 4096);
        voiceData[4103] = 0xF7;
    }

public:
    Cartridge() {
        memset(voiceData, 0, sizeof(voiceData));
        memset(perfData, 0, sizeof(perfData));
    }

    Cartridge(const Cartridge &cpy) {
        memcpy(voiceData, cpy.voiceData, SYSEX_SIZE);
        memcpy(perfData, cpy.perfData, SYSEX_SIZE);
        hasData = cpy.hasData;
    }

    int load(juce::File f) {
        std::unique_ptr<juce::FileInputStream> fis = f.createInputStream();
        if ( fis == NULL )
            return -1;
        int rc = load(*fis);
        return rc;
    }

    /**
     * Loads sysex stream
     * Returns 0 if it was parsed successfully
     * Returns -1 if it cannot open the stream
     */
    int load(juce::InputStream &fis) {
        uint8_t buffer[65535];
        int sz = fis.read(buffer, 65535);
        if ( sz == 0 )
            return -1;
        return load(buffer, sz);
    }

    /**
     * Loads sysex buffer
     * Returns 0 if it was parsed successfully
     * Returns 1 if sysex checksum didn't match
     * Returns 2 if no sysex data found, probably random data
     */
    int load(const uint8_t *stream, int size);

    /** Files of this size or more are never rewritten by saveVoice(): only
        their first 64 KB are ever read as a bank (load()). */
    static constexpr juce::int64 maxRewritableFileSize = 65535;
    /**
     * Writes this bank into `f`: a new file, or only the bank message of an
     * existing one (its other SysEx messages stay), through an atomic write.
     * Returns 1 when written, -1 otherwise (no data, a file of
     * maxRewritableFileSize or more, a file without a DX7 bank to replace,
     * or a write that did not complete; the file is then left as it was).
     */
    int saveVoice(juce::File f);
    bool containsData() const { return hasData; }

    void saveVoice(uint8_t *sysex) {
        setHeader();
        memcpy(sysex, voiceData, SYSEX_SIZE);
    }

    char *getRawVoice() {
        return (char *) voiceData + 6;
    }

    // Convert this to JUCE blob
    char *getVoiceSysex() {
        setHeader();
        return (char *) voiceData;
    }

    const char *getVoiceSysex() const {
        return (const char *) voiceData;
    }

    void getProgramNames(juce::StringArray &dest) {
        dest.clear();
        for (int i = 0; i < 32; i++)
            dest.add( Program::normalizePgmName(getRawVoice() + ((i * 128) + 118)) );
    }

    juce::String getProgramName(int idx) {
        jassert(idx >= 0 && idx < 32);
        return Program::normalizePgmName(getRawVoice() + ((idx * 128) + 118));
    }

    Cartridge operator =(const Cartridge other) {
        memcpy(voiceData, other.voiceData, SYSEX_SIZE);
        memcpy(perfData, other.perfData, SYSEX_SIZE);
        hasData = other.hasData;
        return *this;
    }

    void setPackedProgram(int idx, char *src) {
        if (idx < 0 || idx >= 32 || src == nullptr)
            return;
        memcpy(getRawVoice() + (idx * 128), src, 128);
        hasData = true;
    }

    bool packProgram(const uint8_t *src, int idx, const juce::String& name);

    Program unpackProgram(int idx);
    /** Applies to a voice that arrives unpacked (a single-voice SysEx dump, a
        sound file, a DAW session) the rule unpackProgram() applies to a bank
        voice, so every way in loads a voice the same way. */
    static void applyBankRules(Program& voice);
};
