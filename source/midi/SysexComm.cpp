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

#include "SysexComm.h"
#include "../PluginProcessor.h"

SysexComm::SysexComm() {
    sysexChl = 0;
    listener = NULL;    // this will get injected later
    
    inputName = "";
    outputName = "";
    
    input = NULL;
    output = NULL;
    inputOutput = false;
    commandFifo = NULL;
    avSendPackets = std::make_unique<AVSendPacket[]>(AVSendQueueSize);
    for (size_t index = 0; index < AVSendQueueSize; ++index)
        avSendPackets[index].sequence.store(index, std::memory_order_relaxed);
    avSendBatch.ensureSize(65536);
    startTimerHz(30);

#ifdef IMPLEMENT_MidiMonitor
    inActivity = false;
    outActivity = false;
#endif //IMPLEMENT_MidiMonitor
}

String SysexComm::getInput() {
    return inputName;
}

bool SysexComm::setInput(String target) {
    if (target == inputName && input != nullptr)
        return true;

    if ( input != NULL ) {
        input->stop();
        input = NULL;
    }
    inputOutput = false;
    inputName.clear();
    
    if ( listener == NULL )
        return target.isEmpty() || target == "None";

    if ( target == "None" || target == "" ) {
        return true;
    }


    for (const auto& device : MidiInput::getAvailableDevices()) {
        if (device.name != target && device.identifier != target) continue;
        input = MidiInput::openDevice(device.identifier, listener);
        if (input == nullptr) return false;
        inputName = device.name;
        input->start();
        inputOutput = output != nullptr;
        return true;
    }
    return false;
}

String SysexComm::getOutput() {
    return outputName;
}

bool SysexComm::setOutput(String target) {
    if (target == outputName && output != nullptr)
        return true;
    if ( output != NULL ) {
        output = NULL;
    }
    inputOutput = false;
    outputName.clear();

    if ( target == "None" || target == "" ) {
        return true;
    }

    for (const auto& device : MidiOutput::getAvailableDevices()) {
        if (device.name != target && device.identifier != target) continue;
        output = MidiOutput::openDevice(device.identifier);
        if (output == nullptr) return false;
        outputName = device.name;
        inputOutput = input != nullptr;
        return true;
    }
    return false;
}

bool SysexComm::isInputActive() {
    return commandFifo != nullptr;
}

bool SysexComm::isOutputActive() {
    return avProcessor != nullptr;
}

int SysexComm::getChl() {
    return sysexChl;
}

void SysexComm::setChl(int chl) {
    sysexChl = juce::jlimit(0, 15, chl);
}

void SysexComm::setCommandFifo(CommandFifo<DexedAudioProcessor> *fifo) {
    commandFifo = fifo;
}

int SysexComm::send(const MidiMessage& message) {
    const int size = message.getRawDataSize();
    if (size <= 0 || size > AVSendPacketBytes) return -1;
    size_t position = avSendWrite.load(std::memory_order_relaxed);
    AVSendPacket* selected = nullptr;
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto& packet = avSendPackets[position % AVSendQueueSize];
        const auto sequence = packet.sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position);
        if (difference == 0) {
            if (avSendWrite.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                selected = &packet;
                break;
            }
        } else if (difference < 0) {
            break;
        } else {
            position = avSendWrite.load(std::memory_order_relaxed);
        }
    }
    if (selected == nullptr) {
        avDroppedOutput.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    std::memcpy(selected->bytes.data(), message.getRawData(), static_cast<size_t>(size));
    selected->size = size;
    selected->sequence.store(position + 1, std::memory_order_release);
    return 0;
}

void SysexComm::timerCallback() {
    avSendBatch.clear();
    int count = 0;
    while (count < AVSendQueueSize) {
        auto& packet = avSendPackets[avSendRead % AVSendQueueSize];
        if (packet.sequence.load(std::memory_order_acquire) != avSendRead + 1) break;
        if (avSendBatch.data.size() + packet.size + 8 > 65536) {
            if (avProcessor != nullptr) avProcessor->queueAVMidiOutputBatch(avSendBatch);
            avSendBatch.clear();
        }
        const MidiMessage message(packet.bytes.data(), packet.size);
        if (output != nullptr) output->sendMessageNow(message);
        avSendBatch.addEvent(message, 0);
        packet.sequence.store(avSendRead + AVSendQueueSize, std::memory_order_release);
        ++avSendRead;
        ++count;
    }
    if (!avSendBatch.isEmpty() && avProcessor != nullptr)
        avProcessor->queueAVMidiOutputBatch(avSendBatch);
}

void SysexComm::playBuffer(MidiBuffer &keyboardEvents, int numSamples ) {
    noteOutput.addEvents(keyboardEvents, 0, numSamples, 0);
}
