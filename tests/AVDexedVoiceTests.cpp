// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// Bank voices load as in Dexed 1.1.0 (commit eebb748), field for field, and
// they sound. Two command-line tools check real banks without opening the
// plugin:
//   --render-voice <bank.syx> <slot 1-32>   level of one note of one voice
//   --scan-voices <folder>                   every voice of every bank, against Dexed 1.1.0

#include "../source/PluginProcessor.h"
#include "../source/parameter/Model.h"
#include <cmath>
#include <iostream>

namespace {
bool expect(bool condition, const juce::String& message) {
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

//==============================================================================
// Dexed 1.1.0 (eebb748) unpacks a bank voice like this, verbatim: the reference.
uint8_t referenceNormparm(uint8_t value, char max) {
    if ( value <= max )
        return value;
    return ((float)value)/255 * max;
}

void referenceUnpack(const uint8_t* bulk, uint8_t* ret) {
    for (int op = 0; op < 6; op++) {
        for(int i=0; i<11; i++) {
            uint8_t currparm = bulk[op * 17 + i] & 0x7F;
            ret[op * 21 + i] = referenceNormparm(currparm, 99);
        }
        memcpy(ret + op * 21, bulk + op * 17, 11);
        char leftrightcurves = bulk[op * 17 + 11]&0xF;
        ret[op * 21 + 11] = leftrightcurves & 3;
        ret[op * 21 + 12] = (leftrightcurves >> 2) & 3;
        char detune_rs = bulk[op * 17 + 12]&0x7F;
        ret[op * 21 + 13] = detune_rs & 7;
        char kvs_ams = bulk[op * 17 + 13]&0x1F;
        ret[op * 21 + 14] = kvs_ams & 3;
        ret[op * 21 + 15] = (kvs_ams >> 2) & 7;
        ret[op * 21 + 16] = bulk[op * 17 + 14]&0x7F;
        char fcoarse_mode = bulk[op * 17 + 15]&0x3F;
        ret[op * 21 + 17] = fcoarse_mode & 1;
        ret[op * 21 + 18] = (fcoarse_mode >> 1)&0x1F;
        ret[op * 21 + 19] = bulk[op * 17 + 16]&0x7F;
        ret[op * 21 + 20] = (detune_rs >> 3) &0x7F;
    }
    for (int i=0; i<8; i++)  {
        uint8_t currparm = bulk[102 + i] & 0x7F;
        ret[126+i] = referenceNormparm(currparm, 99);
    }
    ret[134] = referenceNormparm(bulk[110]&0x1F, 31);
    char oks_fb = bulk[111]&0xF;
    ret[135] = oks_fb & 7;
    ret[136] = oks_fb >> 3;
    ret[137] = bulk[112] & 0x7F;
    ret[138] = bulk[113] & 0x7F;
    ret[139] = bulk[114] & 0x7F;
    ret[140] = bulk[115] & 0x7F;
    char lpms_lfw_lks = bulk[116] & 0x7F;
    ret[141] = lpms_lfw_lks & 1;
    ret[142] = (lpms_lfw_lks >> 1) & 7;
    ret[143] = lpms_lfw_lks >> 4;
    ret[144] = bulk[117] & 0x7F;
    for (int name_idx = 0; name_idx < 10; name_idx++)
        ret[145 + name_idx] = bulk[118 + name_idx] & 0x7F;
}

/** The only fields where AV Dexed differs from the reference on purpose: the
    LFO speed and delay, which Dexed 1.1.0 reads past its tables above 99. */
bool isDocumentedDifference(int offset) { return offset == 137 || offset == 138; }

/** The reference voice made safe to play (the two documented fields limited). */
Program playableReference(const uint8_t* bulk) {
    uint8_t ret[Program::PROGRAM_SIZE];
    referenceUnpack(bulk, ret);
    ret[137] = std::min<uint8_t>(ret[137], 99);
    ret[138] = std::min<uint8_t>(ret[138], 99);
    return Program(ret);
}

/** Peak of one note (velocity 100): held, then released. */
float renderPeak(DexedAudioProcessor& processor, double seconds = 1.0, int note = 60, int velocity = 100) {
    constexpr int blockSize = 512;
    constexpr double sampleRate = 48000.0;
    juce::AudioBuffer<float> buffer(2, blockSize);
    float peak = 0.0f;
    const int blocks = int((seconds + 0.5) * sampleRate / blockSize);
    for (int block = 0; block < blocks; ++block) {
        juce::MidiBuffer midi;
        if (block == 0) midi.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) velocity), 0);
        if (block == int(seconds * sampleRate / blockSize)) midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
        buffer.clear();
        processor.processBlock(buffer, midi);
        peak = std::max(peak, buffer.getMagnitude(0, blockSize));
    }
    return peak;
}

void prepare(DexedAudioProcessor& processor) {
    processor.setRateAndBufferSizeDetails(48000.0, 512);
    processor.prepareToPlay(48000.0, 512);
}

/** Algorithm 32 (all carriers), sine voices; then the out-of-range values
    real banks hold, all in slot 3. */
Cartridge outOfRangeBank() {
    Cartridge bank;
    auto voice = Program::initialProgram();
    for (int slot = 0; slot < 32; ++slot)
        bank.packProgram(voice.getRawData(), slot, "OUTOFRANGE");
    auto* bulk = reinterpret_cast<uint8_t*>(bank.getRawVoice()) + 3 * 128;
    bulk[5 * 17 + 14] = 102;            // OP1 output level 102 (documented maximum 99)
    bulk[4 * 17 + 0] = 127;             // OP2 EG rate 1: 127
    bulk[3 * 17 + 4] = 120;             // OP3 EG level 1: 120
    bulk[2 * 17 + 8] = 120;             // OP4 break point 120
    bulk[2 * 17 + 9] = 110;             // OP4 left depth 110
    bulk[1 * 17 + 16] = 120;            // OP5 fine 120
    bulk[1 * 17 + 12] = (15 << 3) | 2;  // OP5 detune 15 (maximum 14), rate scaling 2
    bulk[102] = 120;                    // pitch EG rate 1: 120
    bulk[106] = 127;                    // pitch EG level 1: 127
    bulk[110] = 31;                     // algorithm 32
    bulk[112] = 127;                    // LFO speed 127
    bulk[113] = 106;                    // LFO delay 106
    bulk[114] = 127;                    // LFO PMD 127
    bulk[115] = 112;                    // LFO AMD 112
    bulk[116] = (7 << 4) | (6 << 1) | 1;// PMS 7, wave 6 (maximum 5), key sync
    bulk[117] = 60;                     // transpose 60 (maximum 48)
    bank.getVoiceSysex();               // header and checksum
    return bank;
}

//==============================================================================
bool testBankVoiceMatchesDexed110() {
    auto bank = outOfRangeBank();
    const auto* bulk = reinterpret_cast<const uint8_t*>(bank.getRawVoice()) + 3 * 128;
    uint8_t reference[Program::PROGRAM_SIZE];
    referenceUnpack(bulk, reference);
    const auto loaded = bank.unpackProgram(3);
    bool passed = true;
    for (int offset = 0; offset < Program::PROGRAM_SIZE; ++offset) {
        const int expected = isDocumentedDifference(offset) ? std::min<int>(reference[offset], 99) : reference[offset];
        passed &= expect(loaded.getRawData()[offset] == expected,
                         "voice byte " + juce::String(offset) + ": AV Dexed " + juce::String(loaded.getRawData()[offset])
                             + ", Dexed 1.1.0 " + juce::String(reference[offset]));
    }
    passed &= expect(loaded.getRawData()[5 * 21 + 16] == 102 && loaded.getRawData()[144] == 60
                         && loaded.getRawData()[142] == 6 && loaded.getRawData()[4 * 21 + 0] == 127,
                     "output level 102, transpose 60, wave 6 and EG rate 127 load as the bank holds them");
    passed &= expect(loaded.getRawData()[130] == reference[130] && reference[130] < 99,
                     "the pitch envelope keeps Dexed's normalisation (127 -> 49), not a clamp to 99");
    // A voice that arrives unpacked (SysEx, sound file) gets the same values.
    Program unpacked(reference);
    Cartridge::applyBankRules(unpacked);
    for (int offset = 0; offset < Program::PROGRAM_SIZE; ++offset)
        passed &= expect(unpacked.getRawData()[offset] == loaded.getRawData()[offset],
                         "unpacked voice byte " + juce::String(offset) + " follows the bank rule");
    return passed;
}

bool testOutOfRangeVoiceSounds() {
    auto bank = outOfRangeBank();
    const auto* bulk = reinterpret_cast<const uint8_t*>(bank.getRawVoice()) + 3 * 128;
    DexedAudioProcessor loaded(DexedAudioProcessor::TestMode{});
    prepare(loaded);
    loaded.setActiveBank(bank, juce::File());
    loaded.setAVBankProgram(3);
    const float peak = renderPeak(loaded);
    DexedAudioProcessor original(DexedAudioProcessor::TestMode{});
    prepare(original);
    original.applyProgram(playableReference(bulk));
    const float referencePeak = renderPeak(original);
    bool passed = expect(peak > 0.01f, "a voice with an output level of 102 sounds (peak "
                                           + juce::String(juce::Decibels::gainToDecibels(peak), 1) + " dBFS)");
    passed &= expect(std::abs(juce::Decibels::gainToDecibels(peak) - juce::Decibels::gainToDecibels(referencePeak)) < 0.1f,
                     "it sounds as Dexed 1.1.0 plays it");
    return passed;
}

bool testSingleVoiceSysExFollowsTheBankRule() {
    auto bank = outOfRangeBank();
    const auto* bulk = reinterpret_cast<const uint8_t*>(bank.getRawVoice()) + 3 * 128;
    uint8_t voice[Program::PROGRAM_SIZE];
    referenceUnpack(bulk, voice);
    voice[134] = 40;    // algorithm out of the 32 the engine has
    voice[126] = 127;   // pitch EG rate 1 beyond 99
    uint8_t message[163] = { 0xf0, 0x43, 0x00, 0x00, 0x01, 0x1b };
    memcpy(message + 6, voice, Program::PROGRAM_SIZE);
    message[161] = static_cast<uint8_t>(sysexChecksum(voice, Program::PROGRAM_SIZE) ^ 1);   // a wrong checksum, as Dexed tolerates
    message[162] = 0xf7;
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    prepare(processor);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer events;
    events.addEvent(juce::MidiMessage(message, sizeof(message)), 0);
    processor.processBlock(audio, events);
    processor.flushAVPendingSysExForTesting();
    const auto* loaded = processor.activeProgram.getRawData();
    bool passed = expect(loaded[5 * 21 + 16] == 102 && loaded[144] == 60, "a SysEx voice keeps an output level of 102 and transpose 60");
    passed &= expect(loaded[134] == (40 & 31) && loaded[126] == referenceNormparm(127, 99) && loaded[137] == 99,
                     "a SysEx voice gets the bank rule (algorithm in range, pitch EG normalised, LFO speed 99)");
    passed &= expect(processor.activeProgram.getName().startsWith("OUTOFRANGE"), "and loads even with a wrong checksum, as in Dexed 1.1.0");
    return passed;
}

bool testSessionKeepsBankValues() {
    auto bank = outOfRangeBank();
    juce::MemoryBlock state;
    {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        processor.setActiveBank(bank, juce::File());
        processor.setAVBankProgram(3);
        processor.getStateInformation(state);
    }
    DexedAudioProcessor restored(DexedAudioProcessor::TestMode{});
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    const auto* voice = restored.activeProgram.getRawData();
    bool passed = expect(voice[5 * 21 + 16] == 102 && voice[144] == 60 && voice[1 * 21 + 20] == 15,
                         "a reopened project keeps the bank's out-of-range values (output 102, transpose 60, detune 15)");
    // Moving the control writes what it shows.
    if (auto* output = restored.parameters.getParameter(IDs::outputLevel.op(0).name)) {   // OP1
        output->setValueNotifyingHost(output->convertTo0to1(50.0f));
        passed &= expect(restored.activeProgram.getRawData()[5 * 21 + 16] == 50, "an edit of that control writes its own value");
    }
    return passed;
}

//==============================================================================
/** Every voice of every bank under `folder`: AV Dexed against Dexed 1.1.0. */
bool scanVoices(const juce::File& folder) {
    juce::Array<juce::File> files;
    folder.findChildFiles(files, juce::File::findFiles, true, "*.syx;*.SYX");
    files.sort();
    int banks = 0, voices = 0, differing = 0, silent = 0, silentOnlyHere = 0;
    float worstDifference = 0.0f;
    juce::String worstVoice;
    DexedAudioProcessor here(DexedAudioProcessor::TestMode{}), original(DexedAudioProcessor::TestMode{});
    prepare(here);
    prepare(original);
    for (const auto& file : files) {
        Cartridge bank;
        const int rc = bank.load(file);
        if (rc != 0 && rc != 1) continue;
        ++banks;
        here.setActiveBank(bank, juce::File());
        const auto* data = reinterpret_cast<const uint8_t*>(bank.getRawVoice());
        for (int slot = 0; slot < 32; ++slot) {
            ++voices;
            here.setAVBankProgram(slot);
            const auto ours = bank.unpackProgram(slot);
            const auto reference = playableReference(data + slot * 128);
            const bool same = memcmp(ours.getRawData(), reference.getRawData(), Program::PROGRAM_SIZE) == 0;
            float peak = renderPeak(here, 0.4);
            if (peak < 0.001f) {
                // Slow attacks and voices for other octaves: three notes, longer.
                for (int note : { 36, 60, 84 })
                    peak = std::max(peak, renderPeak(here, 3.0, note));
                if (peak < 0.001f) {
                    ++silent;
                    const auto* voice = ours.getRawData();
                    juce::String outputs;
                    for (int op = 5; op >= 0; --op) outputs << int(voice[op * 21 + 16]) << (op > 0 ? "/" : "");
                    std::cout << "silent: " << file.getFileName() << " voice " << slot + 1 << " \""
                              << here.getProgramName(slot) << "\" algorithm " << int(voice[134]) + 1
                              << " output levels OP1-6 " << outputs << " peak " << juce::Decibels::gainToDecibels(peak, -120.0f) << " dBFS\n";
                }
            }
            if (same) continue;
            ++differing;
            original.applyProgram(reference);
            const float referencePeak = renderPeak(original, 0.4);
            if (peak < 0.001f && referencePeak >= 0.001f) {
                ++silentOnlyHere;
                std::cout << "SILENT ONLY IN AV DEXED: " << file.getFileName() << " voice " << slot + 1 << '\n';
            }
            const float difference = std::abs(juce::Decibels::gainToDecibels(peak, -120.0f) - juce::Decibels::gainToDecibels(referencePeak, -120.0f));
            if (difference > worstDifference) {
                worstDifference = difference;
                worstVoice = file.getFileName() + " voice " + juce::String(slot + 1);
            }
        }
    }
    std::cout << "Banks " << banks << ", voices " << voices << ", silent (also in Dexed 1.1.0 unless listed) " << silent
              << ", voices whose bytes differ from Dexed 1.1.0 only in the documented LFO fields " << differing
              << ", silent only in AV Dexed " << silentOnlyHere << ", largest level difference " << worstDifference
              << " dB (" << worstVoice << ")\n";
    return silentOnlyHere == 0;
}
}

bool AVRenderBankVoice(const juce::File& bankFile, int slot) {
    Cartridge bank;
    if (bank.load(bankFile) < 0 || ! juce::isPositiveAndBelow(slot - 1, 32)) {
        std::cerr << "Cannot read " << bankFile.getFullPathName() << " or slot out of 1-32\n";
        return false;
    }
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    prepare(processor);
    processor.setActiveBank(bank, juce::File());
    processor.setAVBankProgram(slot - 1);
    auto program = bank.unpackProgram(slot - 1);
    std::cout << "Voice " << slot << " \"" << processor.programNames[slot - 1] << "\" algorithm "
              << int(program.getRawData()[134]) + 1 << '\n';
    const float peak = renderPeak(processor);
    std::cout << "Peak " << juce::Decibels::gainToDecibels(peak, -120.0f) << " dBFS (C4, velocity 100)\n";
    // The loudest it gets: velocity 127 on notes 24-108.
    float loudest = 0.0f;
    int loudestNote = 60;
    for (int note = 24; note <= 108; note += 3)
        if (const float p = renderPeak(processor, 2.0, note, 127); p > loudest) { loudest = p; loudestNote = note; }
    std::cout << "Loudest " << juce::Decibels::gainToDecibels(loudest, -120.0f) << " dBFS (note " << loudestNote << ", velocity 127)\n";
    return peak > 0.001f;
}

bool AVScanBankVoices(const juce::File& folder) { return scanVoices(folder); }

/** For organising a collection: every voice of every bank under `folder`, one
    TSV line each: file (relative), slot 0-31, name (hex), the voice as AV Dexed
    plays it (145 unpacked bytes, hex, without the name) and its loudest peak at
    velocity 127. A voice quiet on C2/C4/C6 is swept over notes 24-108 (3 s
    each) and then held 8 s, so slow attacks and voices for other octaves are
    not mistaken for silent ones. */
bool AVAuditVoices(const juce::File& folder, const juce::File& output) {
    juce::Array<juce::File> files;
    folder.findChildFiles(files, juce::File::findFiles, true, "*.syx;*.SYX");
    files.sort();
    juce::FileOutputStream out(output);
    if (! out.openedOk()) return false;
    out.setPosition(0);
    out.truncate();
    out << "file\tslot\tname_hex\tvoice_hex\tloudest_dbfs\tloudest_note\tload_rc\n";
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    prepare(processor);
    constexpr float audible = 0.01f;   // -40 dBFS
    int count = 0;
    for (const auto& file : files) {
        Cartridge bank;
        const int rc = bank.load(file);
        if (rc != 0 && rc != 1) {
            out << file.getRelativePathFrom(folder) << "\t-1\t\t\t\t\t" << rc << "\n";
            continue;
        }
        processor.setActiveBank(bank, juce::File());
        const auto* raw = reinterpret_cast<const uint8_t*>(bank.getRawVoice());
        for (int slot = 0; slot < 32; ++slot) {
            processor.setAVBankProgram(slot);
            const auto voice = bank.unpackProgram(slot);
            float loudest = 0.0f;
            int loudestNote = 60;
            const auto listen = [&](int note, double seconds) {
                if (const float p = renderPeak(processor, seconds, note, 127); p > loudest) { loudest = p; loudestNote = note; }
            };
            for (int note : { 36, 60, 84 }) listen(note, 1.5);
            if (loudest < audible)
                for (int note = 24; note <= 108 && loudest < audible; note += 3) listen(note, 3.0);
            if (loudest < audible)
                for (int note : { 36, 48, 60, 72, 84 }) listen(note, 8.0);
            out << file.getRelativePathFrom(folder) << "\t" << slot << "\t"
                << juce::String::toHexString(raw + slot * 128 + 118, 10, 0) << "\t"
                << juce::String::toHexString(voice.getRawData(), 145, 0) << "\t"
                << juce::String(juce::Decibels::gainToDecibels(loudest, -120.0f), 2) << "\t" << loudestNote << "\t" << rc << "\n";
            if (++count % 2000 == 0) std::cout << count << " voices\n" << std::flush;
        }
    }
    out.flush();
    std::cout << "Audited " << count << " voices into " << output.getFullPathName() << '\n';
    return out.getStatus().wasOk();
}

bool testWheelIsPlayedNotSaved() {
    // CC1 drives the Cutoff by default (inverted). Playing the wheel is not an
    // edit of the sound; moving the Cutoff knob is.
    auto bank = outOfRangeBank();
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    prepare(processor);
    processor.setActiveBank(bank, juce::File());
    processor.setAVBankProgram(0);
    auto* cutoff = processor.parameters.getParameter(IDs::cutoff.name);
    const float soundCutoff = cutoff->getValue();
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer wheel;
    wheel.addEvent(juce::MidiMessage::controllerEvent(1, 1, 100), 0);
    processor.processBlock(audio, wheel);
    bool passed = expect(std::abs(cutoff->getValue() - soundCutoff) > 0.1f,
                         juce::String::fromUTF8("the mod wheel closes the filter (CC1 → Cutoff, inverted)"));
    passed &= expect(! processor.isAVPresetModified(), "playing the wheel does not mark the sound modified");
    passed &= expect(processor.captureAVSoundSettings().values.count(IDs::cutoff.name) == 0,
                     "saving keeps the sound's own cutoff, not where the wheel left it");
    cutoff->setValueNotifyingHost(0.3f);   // the knob, the host
    passed &= expect(processor.isAVPresetModified(), "moving the Cutoff knob does mark it modified");
    passed &= expect(processor.captureAVSoundSettings().values.count(IDs::cutoff.name) == 1, "and saving stores that cutoff");
    return passed;
}

bool testTransposeBeyondRangeStaysInTune() {
    // Transpose 60 (+36 semitones) on a high note: the note is kept within the
    // 128-note table instead of reading past it.
    auto bank = outOfRangeBank();
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    prepare(processor);
    processor.setActiveBank(bank, juce::File());
    processor.setAVBankProgram(3);
    const float peak = renderPeak(processor, 0.3, 120, 127);
    return expect(std::isfinite(peak) && peak > 0.0f && peak < 8.0f, "a voice transposed past the top of the keyboard still plays sanely");
}

bool testVoiceLoadIsNotAGesture() {
    // Loading a voice is a sound change, not the user's hand: a host in Touch
    // or Latch recorded every voice parameter while each came as a gesture.
    // Only what differs reaches the host, and the parameters show the voice.
    struct Host final : juce::AudioProcessorListener {
        int gestures = 0, values = 0;
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override { ++values; }
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
        void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int) override { ++gestures; }
    } host;
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    const Program first = Program::initialProgram();
    uint8_t bytes[Program::PROGRAM_SIZE];
    std::memcpy(bytes, first.getRawData(), sizeof(bytes));
    bytes[IDs::algorithm.pos] = 7;
    bytes[IDs::feedback.pos] = 5;
    const Program second(bytes);
    processor.applyProgram(first);
    processor.addListener(&host);
    processor.applyProgram(first);
    const int sameVoice = host.values;
    processor.applyProgram(second);
    processor.removeListener(&host);
    bool shown = true;
    for (const auto& meta : dxParameters)
        if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(processor.parameters.getParameter(meta.name)))
            shown &= juce::roundToInt(parameter->convertFrom0to1(parameter->getValue())) + meta.displayOffset
                  == processor.activeProgram.getRawData()[meta.pos];
    return expect(host.gestures == 0, "loading a voice sends no change gestures to the host")
        && expect(sameVoice == 0 && host.values == 2, "only the parameters that differ reach the host")
        && expect(shown, "and the parameters show the voice loaded");
}

bool runAVDexedVoiceTests() {
    bool passed = testBankVoiceMatchesDexed110();
    passed &= testVoiceLoadIsNotAGesture();
    passed &= testWheelIsPlayedNotSaved();
    passed &= testTransposeBeyondRangeStaysInTune();
    passed &= testOutOfRangeVoiceSounds();
    passed &= testSingleVoiceSysExFollowsTheBankRule();
    passed &= testSessionKeepsBankValues();
    return passed;
}
