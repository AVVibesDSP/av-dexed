/**
 *
 * Copyright (c) 2014 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#ifndef SYSEXCOMM_H_INCLUDED
#define SYSEXCOMM_H_INCLUDED

#include "../JuceLibraryCode/JuceHeader.h"
#include "../util/CommandFifo.h"

class DexedAudioProcessor;

 // If the macro ''IMPLEMENT_MidiMonitor'' is defined,
 // then the class ''MidiMonitor'' and its related variables 
 // and invocations from other parts are implemented.
 // If the macro is not defined the related source snippets are excluded.
 // WARNING: this class and related variables and invocations 
 // are very likely candidates for deprecation / obsolescence,
 // so it is NOT RECOMMENDED to define this macro!
 //#define IMPLEMENT_MidiMonitor

class SysexComm : private juce::Timer {
    std::unique_ptr<MidiInput> input;
    std::unique_ptr<MidiOutput> output;
    String inputName;
    String outputName;
    std::atomic<int> sysexChl { 0 };
    
    bool inputOutput;

    MidiBuffer noteOutput;

    CommandFifo<DexedAudioProcessor> *commandFifo;
    static constexpr int AVSendQueueSize = 256, AVSendPacketBytes = 4104;
    struct AVSendPacket {
        std::array<uint8_t, AVSendPacketBytes> bytes {};
        int size = 0;
        std::atomic<size_t> sequence { 0 };
    };
    std::unique_ptr<AVSendPacket[]> avSendPackets;
    std::atomic<size_t> avSendWrite { 0 };
    size_t avSendRead = 0;
    std::atomic<int> avDroppedOutput { 0 };
    juce::MidiBuffer avSendBatch;
    DexedAudioProcessor* avProcessor = nullptr;
    void timerCallback() override;
public :
    MidiInputCallback *listener;
#ifdef IMPLEMENT_MidiMonitor
    bool inActivity;
    bool outActivity;
#endif //IMPLEMENT_MidiMonitor    
    SysexComm();
    ~SysexComm() override { stopTimer(); if (input != nullptr) input->stop(); }
    void setAVProcessor(DexedAudioProcessor* processor) noexcept { avProcessor = processor; }
    int getAVDroppedOutputCount() const noexcept { return avDroppedOutput.load(std::memory_order_relaxed); }
#if defined(AVDEXED_TESTING)
    void flushAVPendingOutputForTesting() { timerCallback(); }
#endif
    
    bool setInput(String name);
    bool setOutput(String name);
    void setChl(int chl);
    
    String getInput();
    String getOutput();
    int getChl();
    
    bool isInputActive();
    bool isOutputActive();
    inline bool isInputOutputActive() {
        return inputOutput;
    }
    
    void setCommandFifo(CommandFifo<DexedAudioProcessor> *fifo);
    int send(const MidiMessage& message);
    void playBuffer(MidiBuffer &keyboardEvents, int numSamples);
};

#endif  // SYSEXCOMM_H_INCLUDED
