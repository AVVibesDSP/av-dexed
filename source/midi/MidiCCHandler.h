#pragma once

#include "parameter/DexedApvts.h"
#include "parameter/Model.h"
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>

/** Musical CC mappings. The render thread reads atomics and publishes a learned
    CC; all ValueTree writes and completion callbacks run on the message thread.
    It also holds the CCs learned for the previous / next sound arrows: one CC
    drives one thing, so an arrow and a parameter never share a CC. */
class MidiCCHandler : public juce::ValueTree::Listener, private juce::Timer {
    DexedApvts& apvts;
    std::array<std::atomic<juce::RangedAudioParameter*>, 128> avMap;
    std::array<std::atomic<bool>, 128> avInvert;  // per CC: 127 → 0 when set
    // Tagged requests/completions prevent a late audio event completing a newer
    // Learn or reviving a cancelled one. Pending mappings are visible to state
    // snapshots, but only the message thread commits them to the permanent map.
    std::atomic<uint64_t> avLearnGeneration { 0 }, avLearnRequest { 0 }, avLearnCompletion { 0 };
    std::atomic<int> avLastLearnedCC { -1 };
    std::vector<juce::RangedAudioParameter*> avParameters;

    // The sound arrows (0 previous, 1 next): their CC (-1 none) and, on the
    // render thread, the last value each one saw (a press is a rise from below
    // 64 to 64 or above). They are not part of a sound, so they stay out of the
    // mappings fingerprint. Their Learn is tagged like the parameter Learn:
    // request = generation << 8 | step + 1, completion = generation << 16 |
    // (step + 1) << 8 | cc. Starting either Learn cancels the other.
    std::array<std::atomic<int>, 2> avStepCC;
    std::array<std::atomic<int>, 2> avStepLastValue;
    std::atomic<int> avStepPending { 0 };   // net steps (+1 next, -1 previous) not yet consumed
    std::atomic<int> avStepLearned { -1 };  // the arrow whose Learn just committed
    std::atomic<uint64_t> avStepLearnGeneration { 0 }, avStepLearnRequest { 0 }, avStepLearnCompletion { 0 };

    // A mod wheel gesture is played, not part of the sound. Per parameter
    // index: the value CC1 last wrote (NaN: none since the last sound load) and
    // the value the parameter had before the wheel took it over. While the
    // parameter still holds what the wheel wrote, the sound's value is the
    // earlier one; a knob, another CC, the host or a loaded sound moving it
    // makes it the sound's.
    std::unique_ptr<std::atomic<float>[]> avWrittenByCC, avBeforeCC;

    int AVPendingParameterCC() const noexcept {
        const auto completed = avLearnCompletion.load(std::memory_order_acquire);
        return completed != 0 && (completed >> 24) == avLearnGeneration.load(std::memory_order_acquire)
            ? int(completed & 255) : -1;
    }
    /** The arrow Learn that render finished and the message thread has not
        committed yet: { step, cc }, or { -1, -1 }. */
    std::pair<int, int> AVPendingStep() const noexcept {
        const auto completed = avStepLearnCompletion.load(std::memory_order_acquire);
        if (completed == 0 || (completed >> 16) != avStepLearnGeneration.load(std::memory_order_acquire))
            return { -1, -1 };
        const int step = int((completed >> 8) & 255) - 1;
        return juce::isPositiveAndBelow(step, 2) ? std::pair { step, int(completed & 255) } : std::pair { -1, -1 };
    }
    juce::RangedAudioParameter* AVMappedParameter(int cc) const noexcept {
        const auto completed = avLearnCompletion.load(std::memory_order_acquire);
        if (completed != 0 && (completed >> 24) == avLearnGeneration.load(std::memory_order_acquire)
            && int(completed & 255) == cc) {
            const int index = int((completed >> 8) & 65535) - 1;
            if (juce::isPositiveAndBelow(index, int(avParameters.size()))) return avParameters[size_t(index)];
        }
        // An arrow just learned this CC: its mapping goes when that Learn commits.
        if (AVPendingStep().second == cc) return nullptr;
        return avMap[size_t(cc)].load(std::memory_order_relaxed);
    }
    /** The arrow `step` answers to, counting a Learn not committed yet. */
    int AVEffectiveStepCC(int step) const noexcept {
        const auto [pendingStep, pendingCC] = AVPendingStep();
        if (pendingStep == step) return pendingCC;
        const int cc = avStepCC[size_t(step)].load(std::memory_order_relaxed);
        return cc < 0 || cc == pendingCC || cc == AVPendingParameterCC() ? -1 : cc;
    }
    /** A small whole number a stored list keeps as a property (an arrow's CC,
        the version), or -1: absent or not a number. An unusable one is no
        reason to refuse a session; the caller drops it. */
    static int AVStoredNumber(const juce::ValueTree& tree, const juce::Identifier& id) {
        const auto text = tree.getProperty(id).toString().trim();
        if (text.isEmpty() || text.length() > 3 || !text.containsOnly("0123456789"))
            return -1;
        return text.getIntValue();
    }
    /** Message thread: `step` answers to `cc` (-1 none). The CC leaves the other
        arrow and its parameter mapping, after the arrow has it (render checks
        the arrows first, so the CC never drives the old parameter meanwhile). */
    void AVTakeStepCC(int step, int cc) {
        auto& other = avStepCC[size_t(1 - step)];
        if (cc >= 0 && other.load(std::memory_order_relaxed) == cc) other.store(-1, std::memory_order_relaxed);
        avStepCC[size_t(step)].store(cc, std::memory_order_relaxed);
        if (cc >= 0) removeMapping(cc);
    }
    juce::ValueTree vtConfig;
    juce::String avLearningParameter;
    std::function<void()> callback;

public:
    explicit MidiCCHandler(DexedApvts& state) : apvts(state) {
        for (auto* parameter : apvts.processor.getParameters())
            avParameters.push_back(dynamic_cast<juce::RangedAudioParameter*>(parameter));
        vtConfig = apvts.rootVt.getChildWithName(IDs::profile).getChildWithName(IDs::midiCCMappings);
        jassert(vtConfig.isValid());
        for (auto& parameter : avMap) parameter.store(nullptr, std::memory_order_relaxed);
        for (auto& inverted : avInvert) inverted.store(false, std::memory_order_relaxed);
        for (auto& cc : avStepCC) cc.store(-1, std::memory_order_relaxed);
        for (auto& value : avStepLastValue) value.store(0, std::memory_order_relaxed);
        avWrittenByCC = std::make_unique<std::atomic<float>[]>(avParameters.size());
        avBeforeCC = std::make_unique<std::atomic<float>[]>(avParameters.size());
        forgetAVCCWrites();
        // A new instance starts with AV Dexed's own assignments; a session or a
        // sound file restored later brings its own list (see restoreAVMappings).
        if (vtConfig.getNumChildren() == 0) addAVDefaultMappings(vtConfig);
        vtConfig.addListener(this);
        for (auto child : vtConfig) valueTreeChildAdded(vtConfig, child);
        startTimerHz(30);
    }

    ~MidiCCHandler() override {
        stopTimer();
        vtConfig.removeListener(this);
    }

    /** Version of the assignments a stored list was saved with (a property of
        the list). 1: AV Dexed has default assignments. A list without it was
        saved before they existed, so its emptiness was never a choice. */
    static constexpr int AVMappingsVersion = 1;

    /** AV Dexed's default assignments: CC1 (the mod wheel) closes the filter as
        it rises (Cutoff, inverted: wheel down = filter open, the default). The
        wheel keeps driving the DX7 modulation too. A normal mapping: Settings
        lists it and it can be moved, flipped or removed. Not added to a list
        that already uses CC1 or the Cutoff (one CC drives one thing). */
    static void addAVDefaultMappings(juce::ValueTree& mappings) {
        for (const auto& mapping : mappings)
            if (int(mapping.getProperty(IDs::midiCC, -1)) == 1
                || mapping.getProperty(IDs::midiParameterName).toString() == IDs::cutoff.name)
                return;
        juce::ValueTree wheel(IDs::midiCCMapping);
        wheel.setProperty(IDs::midiCC, 1, nullptr);
        wheel.setProperty(IDs::midiParameterName, IDs::cutoff.name, nullptr);
        wheel.setProperty(IDs::midiInvert, true, nullptr);
        mappings.addChild(wheel, -1, nullptr);
    }

    /** Message thread: back to a new instance's assignments (the default list,
        no arrow CCs). Any running Learn is cancelled. */
    void restoreAVDefaultMappings() {
        cancelMapping();
        vtConfig.removeAllChildren(nullptr);
        addAVDefaultMappings(vtConfig);
        restoreAVPresetSteps(-1, -1);
    }

    /** Message thread, render locked out: the list a session or a sound file
        stored (already validated) replaces the live one, even an empty one. A
        list saved before the default assignments existed gets them where it
        leaves their CC and parameter free: an old session or sound file would
        otherwise take CC1 → Cutoff away from every instance it is loaded into.
        A session brings its arrow CCs too. A sound file never carries them:
        loading one keeps the arrows exactly as they are (CC and last value, so
        a pedal still held down after the step that loaded it is not pressed
        again), and a mapping of the file on an arrow's CC gives way. */
    void restoreAVMappings(const juce::ValueTree& stored, bool keepPresetSteps) {
        vtConfig.copyPropertiesAndChildrenFrom(stored, nullptr);
        // The live list keeps no stale copy of these: the snapshot writes them.
        vtConfig.removeAllProperties(nullptr);
        // Only a sound file gets the defaults added: an older DAW project keeps
        // its list as it was (CC1 recorded there for the DX7 vibrato must not
        // start closing the filter), and a new instance already has them.
        if (keepPresetSteps && AVStoredNumber(stored, IDs::avMappingsVersion) < AVMappingsVersion)
            addAVDefaultMappings(vtConfig);
        if (keepPresetSteps) {
            for (int step = 0; step < 2; ++step)
                if (const int cc = getAVPresetStepCC(step); cc >= 0) removeMapping(cc);
            return;
        }
        restoreAVPresetSteps(AVStoredNumber(stored, IDs::avPresetPreviousCC),
                             AVStoredNumber(stored, IDs::avPresetNextCC));
    }

    void startMapping(juce::RangedAudioParameter* parameter, std::function<void()> completion) {
        cancelMapping();
        if (parameter == nullptr) return;
        avLearningParameter = parameter->getParameterID();
        callback = std::move(completion);
        const auto generation = avLearnGeneration.load(std::memory_order_relaxed);
        avLearnRequest.store((generation << 16) | uint64_t(parameter->getParameterIndex() + 1), std::memory_order_release);
    }

    /** Cancels every Learn: a parameter's and an arrow's. */
    void cancelMapping() {
        avLearnGeneration.fetch_add(1, std::memory_order_acq_rel);
        avLearnRequest.store(0, std::memory_order_release);
        avLearnCompletion.store(0, std::memory_order_release);
        avLearningParameter.clear();
        callback = {};
        cancelAVPresetStepLearn();
    }

    void removeMapping(int cc) {
        if (!juce::isPositiveAndBelow(cc, static_cast<int>(avMap.size()))) return;
        avMap[static_cast<size_t>(cc)].store(nullptr, std::memory_order_relaxed);
        for (int index = vtConfig.getNumChildren(); --index >= 0;)
            if (static_cast<int>(vtConfig.getChild(index).getProperty(IDs::midiCC)) == cc)
                vtConfig.removeChild(index, nullptr);
    }

    int getMapping(const juce::String& parameterName) const {
        for (int cc = 0; cc < static_cast<int>(avMap.size()); ++cc)
            if (auto* parameter = AVMappedParameter(cc))
                if (parameter->getParameterID() == parameterName) return cc;
        return -1;
    }

    juce::String getAVMappingParameterId(int cc) const {
        if (!juce::isPositiveAndBelow(cc, static_cast<int>(avMap.size()))) return {};
        auto* parameter = AVMappedParameter(cc);
        return parameter != nullptr ? parameter->getParameterID() : juce::String();
    }
    juce::String getAVLearningParameterId() const { return avLearningParameter; }
    /** The parameter `cc` drives now (counting a Learn not committed yet), or
        nullptr. Lock-free and allocation-free: the processor's musical
        fingerprint reads it, also on the audio thread. */
    juce::RangedAudioParameter* getAVMappedParameter(int cc) const noexcept {
        return juce::isPositiveAndBelow(cc, static_cast<int>(avMap.size())) ? AVMappedParameter(cc) : nullptr;
    }

    /** The value `parameter` has as part of the sound (normalised): the value it
        had before a mapped CC moved it, while it still holds what the CC wrote;
        otherwise its current value. Lock-free, for the audio thread too. */
    float getAVSoundValue(const juce::AudioProcessorParameter* parameter) const noexcept {
        const float now = parameter->getValue();
        const int index = parameter->getParameterIndex();
        if (!juce::isPositiveAndBelow(index, static_cast<int>(avParameters.size()))) return now;
        const float written = avWrittenByCC[size_t(index)].load(std::memory_order_relaxed);
        return !std::isnan(written) && std::abs(now - written) <= 1.0e-6f
            ? avBeforeCC[size_t(index)].load(std::memory_order_relaxed) : now;
    }
    /** A sound was loaded: no CC has moved anything of it yet. Lock-free. */
    void forgetAVCCWrites() noexcept {
        for (size_t i = 0; i < avParameters.size(); ++i)
            avWrittenByCC[i].store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
    }
    /** What the CCs hold moved, for a session: each parameter with the value
        it has in the sound and the one the CC wrote. */
    juce::ValueTree createAVCCWritesSnapshot() const {
        juce::ValueTree result("avCCWrites");
        for (size_t i = 0; i < avParameters.size(); ++i) {
            auto* parameter = avParameters[i];
            const float written = avWrittenByCC[i].load(std::memory_order_relaxed);
            if (parameter == nullptr || std::isnan(written) || std::abs(parameter->getValue() - written) > 1.0e-6f) continue;
            juce::ValueTree write("write");
            write.setProperty(IDs::id, parameter->getParameterID(), nullptr);
            write.setProperty("sound", avBeforeCC[i].load(std::memory_order_relaxed), nullptr);
            write.setProperty("written", written, nullptr);
            result.addChild(write, -1, nullptr);
        }
        return result;
    }
    /** A session again: a parameter that still holds what a CC wrote there
        keeps the sound's own value under it (the wheel did not edit the sound). */
    void restoreAVCCWrites(const juce::ValueTree& writes) {
        for (const auto& write : writes) {
            const auto id = write.getProperty(IDs::id).toString();
            for (size_t i = 0; i < avParameters.size(); ++i) {
                auto* parameter = avParameters[i];
                if (parameter == nullptr || parameter->getParameterID() != id) continue;
                const float written = float(write.getProperty("written", -1.0));
                const float sound = float(write.getProperty("sound", -1.0));
                if (std::abs(parameter->getValue() - written) <= 1.0e-6f && sound >= 0.0f && sound <= 1.0f) {
                    avBeforeCC[i].store(sound, std::memory_order_relaxed);
                    avWrittenByCC[i].store(parameter->getValue(), std::memory_order_relaxed);
                }
            }
        }
    }

    bool isAVInverted(int cc) const noexcept {
        return juce::isPositiveAndBelow(cc, 128) && avInvert[size_t(cc)].load(std::memory_order_relaxed);
    }
    /** Message thread: flips a mapping's direction; stored with the mapping. */
    void setAVInverted(int cc, bool inverted) {
        for (auto child : vtConfig)
            if (int(child.getProperty(IDs::midiCC, -1)) == cc) {
                if (inverted) child.setProperty(IDs::midiInvert, true, nullptr);
                else child.removeProperty(IDs::midiInvert, nullptr);
            }
    }
    /** Message thread: moves a mapping to another CC, keeping its parameter and
        direction. Refused when the target CC already drives something else
        (another parameter or a sound arrow). */
    bool moveAVMapping(int fromCC, int toCC) {
        if (fromCC == toCC || !juce::isPositiveAndBelow(toCC, 128) || AVMappedParameter(toCC) != nullptr
            || getAVPresetStepCC(0) == toCC || getAVPresetStepCC(1) == toCC) return false;
        for (auto child : vtConfig)
            if (int(child.getProperty(IDs::midiCC, -1)) == fromCC) {
                child.setProperty(IDs::midiCC, toCC, nullptr);
                return true;
            }
        return false;
    }
    int getAVLastLearnedCC() const noexcept { return avLastLearnedCC.load(std::memory_order_relaxed); }
    uint64_t getAVMappingsFingerprint() const noexcept {
        uint64_t hash = 14695981039346656037ULL;
        for (int cc = 0; cc < 128; ++cc) {
            const auto* parameter = AVMappedParameter(cc);
            const auto index = parameter != nullptr ? parameter->getParameterIndex() + 1 : 0;
            hash = (hash ^ static_cast<uint64_t>(index)) * 1099511628211ULL;
            if (parameter != nullptr && avInvert[size_t(cc)].load(std::memory_order_relaxed))
                hash = (hash ^ 0x9e3779b97f4a7c15ULL) * 1099511628211ULL;
        }
        return hash;
    }

    /** The mappings as a session stores them, with the version and the arrow
        CCs as properties of the list (not entries: older versions refuse
        unknown entries, and ignore unknown properties). */
    juce::ValueTree createAVMappingsSnapshot() const {
        juce::ValueTree result(IDs::midiCCMappings);
        result.setProperty(IDs::avMappingsVersion, AVMappingsVersion, nullptr);
        for (int cc = 0; cc < static_cast<int>(avMap.size()); ++cc) {
            auto* parameter = AVMappedParameter(cc);
            if (parameter == nullptr) continue;
            juce::ValueTree config(IDs::midiCCMapping);
            config.setProperty(IDs::midiCC, cc, nullptr);
            config.setProperty(IDs::midiParameterName, parameter->getParameterID(), nullptr);
            if (avInvert[size_t(cc)].load(std::memory_order_relaxed))
                config.setProperty(IDs::midiInvert, true, nullptr);
            result.addChild(config, -1, nullptr);
        }
        if (const int cc = getAVPresetStepCC(0); cc >= 0) result.setProperty(IDs::avPresetPreviousCC, cc, nullptr);
        if (const int cc = getAVPresetStepCC(1); cc >= 0) result.setProperty(IDs::avPresetNextCC, cc, nullptr);
        return result;
    }

    void processControlChange(int cc, int value) noexcept {
        if (!juce::isPositiveAndBelow(cc, static_cast<int>(avMap.size()))) return;
        const auto request = avLearnRequest.exchange(0, std::memory_order_acq_rel);
        if (request != 0) {
            const auto generation = request >> 16;
            if (generation == avLearnGeneration.load(std::memory_order_acquire)) {
                avLastLearnedCC.store(cc, std::memory_order_relaxed);
                avLearnCompletion.store((generation << 24) | ((request & 65535) << 8) | uint64_t(cc), std::memory_order_release);
            }
            return;
        }
        const auto stepRequest = avStepLearnRequest.exchange(0, std::memory_order_acq_rel);
        if (stepRequest != 0) {
            const auto generation = stepRequest >> 8;
            const int step = int(stepRequest & 255) - 1;
            if (generation == avStepLearnGeneration.load(std::memory_order_acquire) && juce::isPositiveAndBelow(step, 2)) {
                // The press that teaches the CC is not a step; the next one is.
                avStepLastValue[size_t(step)].store(value, std::memory_order_relaxed);
                avStepLearnCompletion.store((generation << 16) | ((stepRequest & 255) << 8) | uint64_t(cc), std::memory_order_release);
            }
            return;
        }
        for (int step = 0; step < 2; ++step)
            if (AVEffectiveStepCC(step) == cc) {
                const int previous = avStepLastValue[size_t(step)].exchange(value, std::memory_order_relaxed);
                if (previous < 64 && value >= 64)
                    avStepPending.fetch_add(step == 0 ? -1 : 1, std::memory_order_relaxed);
                return;
            }
        if (auto* parameter = AVMappedParameter(cc)) {
            const float normalised = static_cast<float>(juce::jlimit(0, 127, value)) / 127.0f;
            // Only the mod wheel (CC1) is played; a knob of the controller on
            // another CC edits the sound like the mouse does.
            const int index = parameter->getParameterIndex();
            const bool tracked = cc == 1 && juce::isPositiveAndBelow(index, static_cast<int>(avParameters.size()));
            if (tracked) {
                // The first write of a gesture (or after anything else moved the
                // parameter) remembers the value the sound had.
                const float written = avWrittenByCC[size_t(index)].load(std::memory_order_relaxed);
                const float now = parameter->getValue();
                if (std::isnan(written) || std::abs(now - written) > 1.0e-6f)
                    avBeforeCC[size_t(index)].store(now, std::memory_order_relaxed);
            }
            parameter->setValueNotifyingHost(avInvert[size_t(cc)].load(std::memory_order_relaxed) ? 1.0f - normalised : normalised);
            if (tracked)
                avWrittenByCC[size_t(index)].store(parameter->getValue(), std::memory_order_relaxed);
        }
    }

    // Message-thread operation, also useful before an explicit UI save.
    void commitAVPendingMapping() {
        // The parameter takes the CC from a sound arrow too: first, so render
        // never sees the arrow answer to it again once the completion is gone.
        if (const int pendingCC = AVPendingParameterCC(); pendingCC >= 0)
            for (auto& stepCC : avStepCC)
                if (stepCC.load(std::memory_order_relaxed) == pendingCC) stepCC.store(-1, std::memory_order_relaxed);
        const auto completed = avLearnCompletion.exchange(0, std::memory_order_acq_rel);
        if (completed == 0 || (completed >> 24) != avLearnGeneration.load(std::memory_order_acquire)) return;
        const int cc = int(completed & 255);
        const int index = int((completed >> 8) & 65535) - 1;
        if (!juce::isPositiveAndBelow(index, int(avParameters.size()))) return;
        auto* parameter = avParameters[size_t(index)];
        if (parameter == nullptr) return;
        removeMapping(cc);
        juce::ValueTree config(IDs::midiCCMapping);
        config.setProperty(IDs::midiCC, cc, nullptr);
        config.setProperty(IDs::midiParameterName, parameter->getParameterID(), nullptr);
        vtConfig.addChild(config, -1, nullptr);
        avLearningParameter.clear();
        auto completion = std::move(callback);
        callback = {};
        if (completion) completion();
    }

    //==========================================================================
    // Sound arrows (DexedAudioProcessor::AVPresetStep: 0 previous, 1 next).

    /** Message thread: the next CC that arrives is learned for `step`. Cancels
        a parameter Learn (and a parameter Learn started later cancels this). */
    void startAVPresetStepLearn(int step) {
        cancelMapping();
        if (!juce::isPositiveAndBelow(step, 2)) return;
        const auto generation = avStepLearnGeneration.load(std::memory_order_relaxed);
        avStepLearnRequest.store((generation << 8) | uint64_t(step + 1), std::memory_order_release);
    }
    /** Message thread. A CC that render took but the message thread has not
        committed yet is dropped too: the arrow keeps what it had. */
    void cancelAVPresetStepLearn() {
        avStepLearnGeneration.fetch_add(1, std::memory_order_acq_rel);
        avStepLearnRequest.store(0, std::memory_order_release);
        avStepLearnCompletion.store(0, std::memory_order_release);
    }
    bool isAVPresetStepLearning() const noexcept {
        return avStepLearnRequest.load(std::memory_order_acquire) != 0 || AVPendingStep().first >= 0;
    }
    /** The CC of `step` (-1 none), counting a Learn not committed yet. */
    int getAVPresetStepCC(int step) const noexcept {
        return juce::isPositiveAndBelow(step, 2) ? AVEffectiveStepCC(step) : -1;
    }
    /** Message thread: `step` gets `cc` (-1 none). It takes the CC from a
        parameter mapping or from the other arrow. An arrow Learn still running
        wins when it finishes (it is the newer choice). */
    void assignAVPresetStepCC(int step, int cc) {
        if (!juce::isPositiveAndBelow(step, 2)) return;
        cc = juce::isPositiveAndBelow(cc, 128) ? cc : -1;
        // A new CC starts released (its first press counts); the same CC keeps
        // what render saw, so a control held down is not pressed again.
        if (avStepCC[size_t(step)].load(std::memory_order_relaxed) != cc)
            avStepLastValue[size_t(step)].store(0, std::memory_order_relaxed);
        AVTakeStepCC(step, cc);
    }
    /** Message thread: `step` has no CC any more; a Learn running for it stops. */
    void clearAVPresetStepCC(int step) {
        if (!juce::isPositiveAndBelow(step, 2)) return;
        const int requested = int(avStepLearnRequest.load(std::memory_order_acquire) & 255) - 1;
        if (requested == step || AVPendingStep().first == step) cancelAVPresetStepLearn();
        assignAVPresetStepCC(step, -1);
    }
    /** Message thread, after a session's mappings are in place: the arrow CCs
        it stored. One that is not a CC or that a mapping of the session uses
        is dropped (the session's mapping keeps it), as is a second arrow on
        the CC of the first. */
    void restoreAVPresetSteps(int previous, int next) {
        const std::array<int, 2> stored { previous, next };
        for (size_t step = 0; step < 2; ++step) {
            int cc = stored[step];
            if (!juce::isPositiveAndBelow(cc, 128) || avMap[size_t(cc)].load(std::memory_order_relaxed) != nullptr
                || (step == 1 && avStepCC[0].load(std::memory_order_relaxed) == cc))
                cc = -1;
            avStepLastValue[step].store(0, std::memory_order_relaxed);
            avStepCC[step].store(cc, std::memory_order_relaxed);
        }
    }
    /** Message thread: commits an arrow Learn that render finished (the
        handler's timer does it at 30 Hz; tests call it directly). */
    void commitAVPresetStepLearn() {
        auto completed = avStepLearnCompletion.load(std::memory_order_acquire);
        if (completed == 0) return;
        const int step = int((completed >> 8) & 255) - 1;
        const bool current = (completed >> 16) == avStepLearnGeneration.load(std::memory_order_acquire)
            && juce::isPositiveAndBelow(step, 2);
        // The CC is the arrow's before the pending completion goes, so render
        // never sees it belong to nothing (or to its old parameter) meanwhile.
        // Its last value stays the one of the press that taught it.
        if (current) AVTakeStepCC(step, int(completed & 255));
        avStepLearnCompletion.compare_exchange_strong(completed, 0, std::memory_order_acq_rel);
        if (current) avStepLearned.store(step, std::memory_order_release);
    }
    /** The arrow whose Learn committed since the last call (0 / 1), or -1. */
    int consumeAVPresetStepLearned() noexcept { return avStepLearned.exchange(-1, std::memory_order_acq_rel); }
    /** Net steps the arrow CCs asked for since the last call. */
    int consumeAVPresetSteps() noexcept { return avStepPending.exchange(0, std::memory_order_acq_rel); }

    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree& child) override {
        const int cc = child.getProperty(IDs::midiCC, -1);
        if (!juce::isPositiveAndBelow(cc, static_cast<int>(avMap.size()))) return;
        auto* parameter = apvts.getParameter(child.getProperty(IDs::midiParameterName).toString());
        avMap[static_cast<size_t>(cc)].store(parameter, std::memory_order_relaxed);
        avInvert[static_cast<size_t>(cc)].store(bool(child.getProperty(IDs::midiInvert, false)), std::memory_order_relaxed);
    }

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&) override {
        // A mapping changed CC or direction: rebuild the lock-free tables.
        if (tree.getParent() != vtConfig) return;
        for (int cc = 0; cc < 128; ++cc) {
            avMap[size_t(cc)].store(nullptr, std::memory_order_relaxed);
            avInvert[size_t(cc)].store(false, std::memory_order_relaxed);
        }
        for (auto child : vtConfig) valueTreeChildAdded(vtConfig, child);
    }

    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree& child, int) override {
        const int cc = child.getProperty(IDs::midiCC, -1);
        if (juce::isPositiveAndBelow(cc, static_cast<int>(avMap.size()))) {
            avMap[static_cast<size_t>(cc)].store(nullptr, std::memory_order_relaxed);
            avInvert[static_cast<size_t>(cc)].store(false, std::memory_order_relaxed);
        }
    }

private:
    void timerCallback() override {
        commitAVPendingMapping();
        commitAVPresetStepLearn();
    }
};
