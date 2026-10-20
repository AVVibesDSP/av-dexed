// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedSysex.h"

namespace AVDexedSysex {
namespace {
bool allDataBytes(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i)
        if (data[i] & 0x80) return false;
    return true;
}

bool isLM(const uint8_t* data, size_t size, size_t at) {
    return at + 4 <= size && data[at] == 'L' && data[at + 1] == 'M' && data[at + 2] == ' ' && data[at + 3] == ' ';
}

size_t nextStatusByte(const uint8_t* data, size_t size, size_t from) {
    while (from < size && (data[from] & 0x80) == 0) ++from;
    return from;
}

/** "MCRYM" followed by a binary 0 or 1 reads "MCRYM0"/"MCRYM1"; trailing spaces go. */
juce::String readId(const uint8_t* data) {
    juce::String id;
    for (int i = 0; i < 6; ++i) {
        const auto c = data[i];
        id += c < 10 ? juce::String(int(c)) : juce::String::charToString(juce::juce_wchar(c < 32 || c > 126 ? '?' : c));
    }
    return id.trimEnd();
}

/** One universal message from its F0: its entries, and where it ends. */
size_t parseUniversal(const uint8_t* data, size_t size, size_t start, std::vector<UniversalEntry>& out) {
    size_t p = start + 4;   // F0 43 0n 7E
    while (p + 12 <= size) {
        const int count = (int(data[p]) << 7) | int(data[p + 1]);
        if (! isLM(data, size, p + 2) || (data[p] & 0x80) || (data[p + 1] & 0x80))
            break;
        const size_t dataStart = p + 12;
        // The DX7II counts the ten ASCII bytes, the TX802 performances do not:
        // the right length is the one followed by F7 or by the next entry.
        bool found = false;
        for (const int length : { count - 10, count }) {
            if (length < 0) continue;
            const size_t checksumAt = dataStart + size_t(length);
            const size_t next = checksumAt + 1;
            if (next > size || ! allDataBytes(data + dataStart, size_t(length) + 1))
                continue;
            const bool endsHere = next == size || data[next] == 0xF7;
            const bool nextEntry = next + 6 <= size && isLM(data, size, next + 2) && (data[next] & 0x80) == 0;
            if (! endsHere && ! nextEntry)
                continue;
            UniversalEntry entry;
            entry.id = readId(data + p + 6);
            entry.data.assign(data + dataStart, data + checksumAt);
            unsigned sum = 0;
            for (size_t i = p + 2; i <= checksumAt; ++i) sum += data[i];
            entry.checksumOk = (sum & 0x7F) == 0;
            out.push_back(std::move(entry));
            p = next;
            found = true;
            break;
        }
        if (! found)
            break;
        if (p >= size || data[p] == 0xF7)
            return p < size ? p + 1 : p;
    }
    return nextStatusByte(data, size, start + 1);
}
}

std::vector<const UniversalEntry*> Contents::universalNamed(const juce::String& id) const {
    std::vector<const UniversalEntry*> found;
    for (const auto& entry : universal)
        if (entry.id == id) found.push_back(&entry);
    return found;
}

uint8_t checksum(const uint8_t* data, size_t size) {
    unsigned sum = 0;
    for (size_t i = 0; i < size; ++i) sum += data[i];
    return uint8_t((0x80 - (sum & 0x7F)) & 0x7F);
}

bool looksLikeRawCartridge(const uint8_t* data, size_t size) {
    if (size != size_t(bankBytes) || data == nullptr || ! allDataBytes(data, size))
        return false;
    int readable = 0;
    for (int voice = 0; voice < 32; ++voice) {
        bool ok = true;
        for (int i = 118; i < 128; ++i) {
            const auto c = data[voice * 128 + i];
            ok &= c >= 32 && c < 127;
        }
        readable += ok ? 1 : 0;
    }
    return readable >= 28;
}

Contents scan(const uint8_t* data, size_t size) {
    Contents found;
    if (data == nullptr || size == 0)
        return found;
    if (looksLikeRawCartridge(data, size)) {
        Bank bank;
        std::copy(data, data + bankBytes, bank.voices.begin());
        bank.fromRawCartridge = true;
        found.banks.push_back(bank);
        return found;
    }

    std::vector<std::pair<size_t, std::array<uint8_t, amemBytes>>> amems;   // position in file order
    std::vector<size_t> bankPositions;
    bool acedPending = false;
    std::array<uint8_t, acedBytes> pendingAced {};
    size_t pos = 0;
    while (pos < size) {
        if (data[pos] != 0xF0) { ++pos; continue; }
        const size_t start = pos;
        if (start + 6 > size) break;
        if (data[start + 1] != 0x43 || (data[start + 2] >> 4) != 0) {   // not a Yamaha bulk dump
            pos = nextStatusByte(data, size, start + 1);
            continue;
        }
        const int format = data[start + 3];
        if (format == 0x7E) {
            pos = parseUniversal(data, size, start, found.universal);
            continue;
        }
        const int count = (int(data[start + 4]) << 7) | int(data[start + 5]);
        const size_t payload = start + 6;
        const size_t checksumAt = payload + size_t(count);
        if ((data[start + 4] & 0x80) || (data[start + 5] & 0x80) || checksumAt >= size
            || ! allDataBytes(data + payload, size_t(count) + 1)) {
            pos = nextStatusByte(data, size, start + 1);   // cut short: what follows is another message
            continue;
        }
        const bool checksumOk = checksum(data + payload, size_t(count)) == data[checksumAt];
        pos = checksumAt + 1;
        if (pos < size && data[pos] == 0xF7) ++pos;   // a missing last F7 is tolerated

        if (format == 9 && count == bankBytes) {
            Bank bank;
            std::copy(data + payload, data + payload + bankBytes, bank.voices.begin());
            bank.checksumOk = checksumOk;
            found.banks.push_back(bank);
            bankPositions.push_back(start);
        } else if (format == 0 && count == voiceBytes) {
            std::array<uint8_t, voiceBytes> voice {};
            std::copy(data + payload, data + payload + voiceBytes, voice.begin());
            found.voices.push_back(voice);
            found.acedForVoices.push_back(pendingAced);
            found.voiceHasAced.push_back(acedPending);
            acedPending = false;
        } else if (format == 5 && count == acedBytes) {
            std::copy(data + payload, data + payload + acedBytes, pendingAced.begin());
            acedPending = true;
        } else if (format == 6 && count == amemBytes) {
            std::array<uint8_t, amemBytes> amem {};
            std::copy(data + payload, data + payload + amemBytes, amem.begin());
            amems.emplace_back(start, amem);
        } else if (format == 2 && count == tx7PerformanceBytes) {
            std::array<uint8_t, tx7PerformanceBytes> performances {};
            std::copy(data + payload, data + payload + tx7PerformanceBytes, performances.begin());
            found.tx7Performances.push_back(performances);
        }
    }

    // The additional data of a bank: a TX802 or a DX7II bank dump sends each
    // AMEM right before its VMEM; a full DX7II dump sends the two banks, then
    // the two AMEM. As many AMEM as banks pair in order; otherwise each AMEM
    // goes with the next bank after it that has none.
    if (! amems.empty() && amems.size() == found.banks.size()) {
        for (size_t i = 0; i < amems.size(); ++i) {
            found.banks[i].hasAmem = true;
            found.banks[i].amem = amems[i].second;
        }
    } else {
        for (const auto& [position, amem] : amems) {
            bool paired = false;
            for (size_t b = 0; b < found.banks.size() && ! paired; ++b)
                if (bankPositions[b] > position && ! found.banks[b].hasAmem) {
                    found.banks[b].hasAmem = true;
                    found.banks[b].amem = amem;
                    paired = true;
                }
            if (! paired) found.unpairedAmem.push_back(amem);
        }
    }
    return found;
}

Contents scan(const juce::MemoryBlock& data) {
    return scan(static_cast<const uint8_t*>(data.getData()), data.getSize());
}

std::array<uint8_t, bankBytes + 8> bankMessage(const uint8_t* voices) {
    std::array<uint8_t, bankBytes + 8> message {};
    const uint8_t header[] = { 0xF0, 0x43, 0x00, 0x09, 0x20, 0x00 };
    std::copy(header, header + 6, message.begin());
    std::copy(voices, voices + bankBytes, message.begin() + 6);
    message[6 + bankBytes] = checksum(voices, bankBytes);
    message[7 + bankBytes] = 0xF7;
    return message;
}

std::array<uint8_t, amemBytes + 8> amemMessage(const uint8_t* amem) {
    std::array<uint8_t, amemBytes + 8> message {};
    const uint8_t header[] = { 0xF0, 0x43, 0x00, 0x06, 0x08, 0x60 };
    std::copy(header, header + 6, message.begin());
    std::copy(amem, amem + amemBytes, message.begin() + 6);
    message[6 + amemBytes] = checksum(amem, amemBytes);
    message[7 + amemBytes] = 0xF7;
    return message;
}

std::array<uint8_t, 128> packVoice(const uint8_t* src) {
    std::array<uint8_t, 128> bulk {};
    for (int op = 0; op < 6; op++) {
        const int pp = op * 17, up = op * 21;
        for (int i = 0; i < 11; ++i) bulk[pp + i] = src[up + i] & 0x7F;
        bulk[pp + 11] = (src[up + 11] & 0x03) | ((src[up + 12] & 0x03) << 2);
        bulk[pp + 12] = (src[up + 13] & 0x07) | ((src[up + 20] & 0x0f) << 3);
        bulk[pp + 13] = (src[up + 14] & 0x03) | ((src[up + 15] & 0x07) << 2);
        bulk[pp + 14] = src[up + 16] & 0x7F;
        bulk[pp + 15] = (src[up + 17] & 0x01) | ((src[up + 18] & 0x1f) << 1);
        bulk[pp + 16] = src[up + 19] & 0x7F;
    }
    for (int i = 0; i < 9; ++i) bulk[102 + i] = src[126 + i] & 0x7F;   // pitch EG, algorithm
    bulk[111] = (src[135] & 0x07) | ((src[136] & 0x01) << 3);
    for (int i = 0; i < 4; ++i) bulk[112 + i] = src[137 + i] & 0x7F;   // LFO
    bulk[116] = (src[141] & 0x01) | ((src[142] & 0x07) << 1) | ((src[143] & 0x07) << 4);
    bulk[117] = src[144] & 0x7F;
    for (int i = 0; i < 10; ++i) {
        const auto c = src[145 + i] & 0x7F;
        bulk[118 + i] = c < 32 || c == 127 ? ' ' : c;
    }
    return bulk;
}

std::vector<uint8_t> decodeAsciiNibbles(const std::vector<uint8_t>& text, bool highFirst) {
    const auto value = [](uint8_t c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    };
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        const int first = value(text[i]), second = value(text[i + 1]);
        bytes.push_back(uint8_t(highFirst ? (first << 4) | second : (second << 4) | first));
    }
    return bytes;
}
}
