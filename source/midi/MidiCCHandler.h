#pragma once

#include "parameter/DexedApvts.h"
#include "parameter/Model.h"
#include <array>
#include <atomic>

/** Musical CC mappings. The render thread reads atomics and publishes a learned
    CC; all ValueTree writes and completion callbacks run on the message thread. */
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
    juce::RangedAudioParameter* AVMappedParameter(int cc) const noexcept {
        const auto completed = avLearnCompletion.load(std::memory_order_acquire);
        if (completed != 0 && (completed >> 24) == avLearnGeneration.load(std::memory_order_acquire)
            && int(completed & 255) == cc) {
            const int index = int((completed >> 8) & 65535) - 1;
            if (juce::isPositiveAndBelow(index, int(avParameters.size()))) return avParameters[size_t(index)];
        }
        return avMap[size_t(cc)].load(std::memory_order_relaxed);
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
        vtConfig.addListener(this);
        for (auto child : vtConfig) valueTreeChildAdded(vtConfig, child);
        startTimerHz(30);
    }

    ~MidiCCHandler() override {
        stopTimer();
        vtConfig.removeListener(this);
    }

    void startMapping(juce::RangedAudioParameter* parameter, std::function<void()> completion) {
        cancelMapping();
        if (parameter == nullptr) return;
        avLearningParameter = parameter->getParameterID();
        callback = std::move(completion);
        const auto generation = avLearnGeneration.load(std::memory_order_relaxed);
        avLearnRequest.store((generation << 16) | uint64_t(parameter->getParameterIndex() + 1), std::memory_order_release);
    }

    void cancelMapping() {
        avLearnGeneration.fetch_add(1, std::memory_order_acq_rel);
        avLearnRequest.store(0, std::memory_order_release);
        avLearnCompletion.store(0, std::memory_order_release);
        avLearningParameter.clear();
        callback = {};
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
        direction. Refused when the target CC already drives something else. */
    bool moveAVMapping(int fromCC, int toCC) {
        if (fromCC == toCC || !juce::isPositiveAndBelow(toCC, 128) || AVMappedParameter(toCC) != nullptr) return false;
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

    juce::ValueTree createAVMappingsSnapshot() const {
        juce::ValueTree result(IDs::midiCCMappings);
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
        if (auto* parameter = AVMappedParameter(cc)) {
            const float normalised = static_cast<float>(juce::jlimit(0, 127, value)) / 127.0f;
            parameter->setValueNotifyingHost(avInvert[size_t(cc)].load(std::memory_order_relaxed) ? 1.0f - normalised : normalised);
        }
    }

    // Message-thread operation, also useful before an explicit UI save.
    void commitAVPendingMapping() {
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
    void timerCallback() override { commitAVPendingMapping(); }
};
