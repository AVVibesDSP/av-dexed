// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// MIDI program changes and the learned arrows walk the folder of the sound
// that plays, in the browser's order. Its sounds are read beforehand on a
// thread of their own, so render plays one (a converted bank voice: its voice
// and sound settings) at the message's place in the block, as a bank voice
// always was played: an offline bounce matches playing live.

#include "PluginProcessor.h"
#include "parameter/Model.h"
#include "core/AVDexedLibrary.h"
#include <map>

namespace {
/** Every instance on the same folder shares one reading of it. */
juce::CriticalSection& folderCacheLock() {
    static juce::CriticalSection lock;
    return lock;
}
}

std::shared_ptr<const DexedAudioProcessor::AVFolderSounds> DexedAudioProcessor::readAVFolderSounds(const juce::File& folder,
                                                                                                  juce::int64 stamp) const {
    static std::map<juce::String, std::weak_ptr<const AVFolderSounds>> cache;
    {
        const juce::ScopedLock lock(folderCacheLock());
        if (const auto found = cache[folder.getFullPathName()].lock(); found != nullptr && found->stamp == stamp)
            return found;
    }
    auto read = std::make_shared<AVFolderSounds>();
    read->folder = folder;
    read->stamp = stamp;
    std::vector<std::pair<int, AVFolderSound>> found;   // its order in the folder (-1: none), the sound
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findFiles)) {
        const auto file = entry.getFile();
        juce::MemoryBlock data;
        if (! AVDexedLibrary::isPresetFile(file) || AVDexedLibrary::isIgnoredName(file.getFileName())
            || file.getSize() > 32 * 1024 * 1024 || ! file.loadFileAsData(data))
            continue;
        const auto xml = getXmlFromBinary(data.getData(), int(data.getSize()));
        const auto* session = xml != nullptr ? xml->getChildByName("session") : nullptr;
        juce::MemoryBlock voice;
        if (session == nullptr || ! voice.fromBase64Encoding(session->getStringAttribute("voice"))
            || voice.getSize() != size_t(Program::PROGRAM_SIZE + 1))
            continue;
        AVFolderSound sound;
        sound.file = file;
        sound.id = xml->getStringAttribute("libraryPresetId").trim();
        sound.name = session->getStringAttribute("avPresetName").trim();
        if (sound.name.isEmpty()) sound.name = file.getFileNameWithoutExtension();
        // A converted bank voice is its voice and sound settings (loading the
        // file keeps the instrument's own); anything else is read whole.
        sound.direct = session->hasAttribute("avSoundKey") && xml->getChildByName("avLayers") == nullptr;
        const auto* bytes = static_cast<const uint8_t*>(voice.getData());
        sound.voice = Program(bytes);
        sound.voice.setOperatorSwitches(bytes[Program::PROGRAM_SIZE]);
        Cartridge::applyBankRules(sound.voice);
        std::map<juce::String, float> stored;
        if (const auto* values = xml->getChildByName(IDs::parameters.toString()))
            for (const auto* value : values->getChildIterator())
                stored[value->getStringAttribute("id")] = float(value->getDoubleAttribute("value"));
        for (size_t i = 0; i < avSoundParameters.size(); ++i)
            if (const auto* parameter = avSoundParameters[i]) {
                const auto own = stored.find(parameter->paramID);
                sound.values[i] = own != stored.end() ? parameter->convertTo0to1(own->second) : parameter->getDefaultValue();
            }
        juce::MemoryBlock amem;
        sound.hasAmem = amem.fromBase64Encoding(session->getStringAttribute("voiceAmem")) && amem.getSize() == sound.amem.size();
        if (sound.hasAmem) std::memcpy(sound.amem.data(), amem.getData(), sound.amem.size());
        found.emplace_back(session->getIntAttribute("avLibraryOrder", -1), std::move(sound));
    }
    // The browser's order: converted bank voices as their banks had them,
    // then the other sounds by name.
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        if ((a.first >= 0) != (b.first >= 0)) return a.first >= 0;
        if (a.first >= 0) return a.first < b.first;
        return a.second.file.getFileName().compareNatural(b.second.file.getFileName(), false) < 0;
    });
    for (auto& [order, sound] : found) read->sounds.push_back(std::move(sound));
    const juce::ScopedLock lock(folderCacheLock());
    cache[folder.getFullPathName()] = read;
    return read;
}

void DexedAudioProcessor::syncAVFolderSounds() {
    // What render just played: its name and file ("Guardar" writes there).
    if (const int played = avFolderPendingContext.exchange(-1, std::memory_order_acq_rel); played >= 0) {
        AVFolderSound sound;
        {
            const juce::ScopedLock lock(getCallbackLock());
            if (avFolderSounds != nullptr && juce::isPositiveAndBelow(played, int(avFolderSounds->sounds.size())))
                sound = avFolderSounds->sounds[size_t(played)];
        }
        if (sound.file.existsAsFile()) {
            setAVPresetContext(sound.name, sound.id, sound.file);
            captureAVPresetBaseline();
            avFolderPlaying = sound.file;
            updateHostDisplay(ChangeDetails().withProgramChanged(true));
        }
    }
    // A sound of the folder that is more than a voice (layers, a sound saved
    // with its own instrument settings): read whole, here.
    if (const int whole = avFolderPendingLoad.exchange(-1, std::memory_order_acq_rel); whole >= 0) {
        juce::File file;
        juce::String id;
        {
            const juce::ScopedLock lock(getCallbackLock());
            if (avFolderSounds != nullptr && juce::isPositiveAndBelow(whole, int(avFolderSounds->sounds.size()))) {
                file = avFolderSounds->sounds[size_t(whole)].file;
                id = avFolderSounds->sounds[size_t(whole)].id;
            }
        }
        juce::MemoryBlock data;
        if (file.existsAsFile() && file.loadFileAsData(data) && loadAVNativePreset(data.getData(), int(data.getSize()))) {
            setAVPresetContext(getAVPresetName(), id, file);
            captureAVPresetBaseline();
            avFolderPosition.store(whole, std::memory_order_release);
            avFolderPlaying = file;
            updateHostDisplay(ChangeDetails().withProgramChanged(true));
        }
    }
    if (avFolderPendingContext.load(std::memory_order_acquire) >= 0) return;   // render played another meanwhile

    const auto root = AVDexedLibrary::rootFor(dexedAppDir);
    const auto playing = getAVPresetPath();
    if (playing == juce::File() || ! root.isDirectory() || ! playing.isAChildOf(root) || ! AVDexedLibrary::isPresetFile(playing)) {
        // A bank voice, or nothing of the library: program changes go the bank's way.
        avFolderPosition.store(-1, std::memory_order_release);
        avRestoredHostProgram.store(-1, std::memory_order_release);
        avFolderPlaying = juce::File();
        return;
    }
    const auto folder = playing.getParentDirectory();
    const auto stamp = folder.getLastModificationTime().toMilliseconds();
    bool current = false;
    {
        const juce::ScopedLock lock(getCallbackLock());
        current = avFolderSounds != nullptr && avFolderSounds->folder == folder && avFolderSounds->stamp == stamp;
        if (current && playing != avFolderPlaying) {
            int position = -1;
            for (size_t i = 0; i < avFolderSounds->sounds.size() && position < 0; ++i)
                if (avFolderSounds->sounds[i].file == playing) position = int(i);
            avFolderPosition.store(position, std::memory_order_release);
        }
    }
    if (current) {
        avFolderPlaying = playing;
        avRestoredHostProgram.store(-1, std::memory_order_release);   // its place is known
        return;
    }
    // Read once per folder and time; meanwhile program changes go the bank's way.
    const auto request = folder.getFullPathName() + "|" + juce::String(stamp);
    if (request == avFolderRequested) return;
    avFolderRequested = request;
    avFolderPosition.store(-1, std::memory_order_release);
    avFolderPlaying = juce::File();
    avFolderReader.addJob([this, folder, stamp, playing] {
        auto sounds = readAVFolderSounds(folder, stamp);
        int position = -1;
        for (size_t i = 0; i < sounds->sounds.size() && position < 0; ++i)
            if (sounds->sounds[i].file == playing) position = int(i);
        const juce::ScopedLock lock(getCallbackLock());
        avFolderSounds = std::move(sounds);
        avFolderPosition.store(position, std::memory_order_release);
        avRestoredHostProgram.store(-1, std::memory_order_release);
    });
}

bool DexedAudioProcessor::playAVFolderSound(int index) {
    // Render (the callback lock held): the sound playing is one of a folder's.
    if (avFolderSounds == nullptr || avFolderPosition.load(std::memory_order_acquire) < 0) return false;
    if (! juce::isPositiveAndBelow(index, int(avFolderSounds->sounds.size()))) return true;   // beyond the folder: nothing changes
    // Another sound: a restore's guard is consumed.
    avRestoreGuardProgram.store(-1, std::memory_order_release);
    avRestoreGuardHostProgram.store(-1, std::memory_order_release);
    avRestoredHostProgram.store(-1, std::memory_order_release);
    const auto& sound = avFolderSounds->sounds[size_t(index)];
    if (! sound.direct) {
        avFolderPendingLoad.store(index, std::memory_order_release);
        return true;
    }
    // As setAVBankProgram plays a bank voice: the voice, then its settings.
    applyProgram(sound.voice);
    if (isAVLayerModeOn() && sound.hasAmem)
        setAVLayerVoiceExtras(avSelectedLayer.load(std::memory_order_acquire), sound.amem.data());
    avKnownPresetOrigin.store(true, std::memory_order_relaxed);
    avSoundFromBankSlot.store(false, std::memory_order_relaxed);
    for (size_t i = 0; i < avSoundParameters.size(); ++i)
        if (auto* parameter = avSoundParameters[i]; parameter != nullptr && std::abs(parameter->getValue() - sound.values[i]) > 1.0e-6f)
            parameter->setValueNotifyingHost(sound.values[i]);
    captureAVPresetBaseline();
    avFolderPosition.store(index, std::memory_order_release);
    avFolderPendingContext.store(index, std::memory_order_release);
    return true;
}

bool DexedAudioProcessor::stepAVFolderSound(int steps) {
    const int position = avFolderPosition.load(std::memory_order_acquire);
    if (avFolderSounds == nullptr || position < 0 || avFolderSounds->sounds.empty()) return false;
    const int count = int(avFolderSounds->sounds.size());
    return playAVFolderSound(((position + steps) % count + count) % count);
}
