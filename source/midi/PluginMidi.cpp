#include "PluginProcessor.h"
#include "MidiCCHandler.h"

void DexedAudioProcessor::processMidiMessages(MidiBufferIterator &cur, MidiBufferIterator &end, int samplePos) {
    for(;cur != end;++cur) {
        juce::MidiMessageMetadata meta = *cur;
        if ( meta.samplePosition > samplePos )
            return;
        if (meta.numBytes > 0 && meta.data[0] == 0xf0) {
            enqueueAVSysEx(meta.data, meta.numBytes);
            continue;
        }
        const MidiMessage message = meta.getMessage();
        processMidiMessage(&message);
    }
}

void DexedAudioProcessor::processMidiMessage(const MidiMessage *msg) {
    if ( msg->isSysEx() ) {
        handleIncomingMidiMessage(NULL, *msg);
        return;
    }

    const uint8 *buf  = msg->getRawData();
    uint8_t cmd = buf[0];
    uint8_t cf0 = cmd & 0xf0;
    auto channel = msg->getChannel();

    if( controllers.mpeEnabled && channel != 1 &&
        (
            (cf0 == 0xb0 && buf[1] == 74 ) || //timbre
            (cf0 == 0xd0 ) || // aftertouch
            (cf0 == 0xe0 ) // pb
            )
        ) {
        // OK so find my voice index
        int voiceIndex = -1;
        for( int i=0; i<MAX_ACTIVE_NOTES; ++i ) {
            if( voices[i].keydown && voices[i].channel == channel ) {
                voiceIndex = i;
                break;
            }
        }
        if( voiceIndex >= 0 ) {
            int i = voiceIndex;
            switch(cf0) {
            // THIS IS COMMENTED SINCE mepTimbre and mpePressure is not used
            // case 0xb0:
            //     voices[i].mpeTimbre = (int)buf[2];
            //     voices[i].dx7_note->mpeTimbre = (int)buf[2];
            //     break;
            // case 0xd0:
            //     voices[i].mpePressure = (int)buf[1];
            //     voices[i].dx7_note->mpePressure = (int)buf[1];
            //     break;
            case 0xe0:
                voices[i].mpePitchBend = (int)( buf[1] | (buf[2] << 7) );
                voices[i].dx7_note->mpePitchBend = (int)( buf[1] | ( buf[2] << 7 ) );
                break;
            }
        }
    }
    else {
        switch(cmd & 0xf0) {
        case 0x80 :
            keyup(channel, buf[1], buf[2]);
        return;

        case 0x90 :
            if (!synthTuningState->is_standard_tuning() || !buf[2] ||
                !MTS_HasMaster(mtsClient) || !MTS_ShouldFilterNote(mtsClient, buf[1], channel - 1))
                keydown(channel, buf[1], buf[2]);
        return;

        case 0xb0 : {
            int ctrl = buf[1];
            int value = buf[2];
            midiCCMapper->processControlChange(ctrl, value);
                switch(ctrl) {
                case 1:
                    controllers.modwheel_cc = value;
                    avKeyboardMod.store(value, std::memory_order_relaxed);
                    controllers.refresh();
                    break;
                case 2:
                    controllers.breath_cc = value;
                    controllers.refresh();
                    break;
                case 4:
                    controllers.foot_cc = value;
                    controllers.refresh();
                    break;
                case 5:
                    controllers.portamento_cc = value;
                    break;
                case 64:
                    sustain = value > 63;
                    if (!sustain) {
                        for (int note = 0; note < MAX_ACTIVE_NOTES; note++) {
                            if (voices[note].sustained && !voices[note].keydown) {
                                voices[note].dx7_note->keyup();
                                voices[note].sustained = false;
                            }
                        }
                    }
                    break;
                case 65:
                    controllers.portamento_enable_cc = value >= 64;
                    break;
                case 120:
                    panic();
                    break;
                case 123:
                    for (int note = 0; note < MAX_ACTIVE_NOTES; note++) {
                        if (voices[note].keydown)
                            keyup(channel, voices[note].midi_note, 0);
                    }
                    break;
                default:
                    //TRACE("handle channel %d CC %d = %d", channel, ctrl, value);
                    break;
                }
            }
            return;

        case 0xc0 :
            setCurrentProgram(buf[1]);
            return;

        case 0xd0 :
            controllers.aftertouch_cc = buf[1];
            controllers.refresh();
            return;

		case 0xe0 :
			controllers.pitch_cc = buf[1] | (buf[2] << 7);
            avKeyboardPitch.store(controllers.pitch_cc, std::memory_order_relaxed);
            return;
        }
    }
}

void DexedAudioProcessor::handleIncomingMidiMessage(MidiInput* source, const MidiMessage& message) {
    // A bounded MPMC mailbox accepts both hardware callbacks and host MIDI.
    // No command allocation or raw voice/APVTS mutation occurs on either path.
    juce::ignoreUnused(source);
    if (message.isSysEx())
        enqueueAVSysEx(message.getRawData(), message.getRawDataSize());
}

bool DexedAudioProcessor::enqueueAVSysEx(const uint8_t* data, int size) noexcept {
    if (data == nullptr || size <= 0 || size > AVSysExPacketSize) {
        avDroppedSysEx.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    size_t position = avSysExWrite.load(std::memory_order_relaxed);
    AVSysExPacket* selected = nullptr;
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto& packet = avSysExPackets[position % AVSysExQueueSize];
        const auto difference = static_cast<intptr_t>(packet.sequence.load(std::memory_order_acquire))
            - static_cast<intptr_t>(position);
        if (difference == 0) {
            if (avSysExWrite.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                selected = &packet;
                break;
            }
        } else if (difference < 0) break;
        else position = avSysExWrite.load(std::memory_order_relaxed);
    }
    if (selected == nullptr) {
        avDroppedSysEx.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::memcpy(selected->bytes.data(), data, static_cast<size_t>(size));
    selected->size = size;
    selected->sequence.store(position + 1, std::memory_order_release);
    return true;
}

void DexedAudioProcessor::timerCallback() { flushAVPendingSysEx(); }

void DexedAudioProcessor::flushAVPendingSysEx() {
    for (int count = 0; count < AVSysExQueueSize; ++count) {
        auto& packet = avSysExPackets[avSysExRead % AVSysExQueueSize];
        if (packet.sequence.load(std::memory_order_acquire) != avSysExRead + 1) break;
        handleAVSysExNow(packet.bytes.data(), packet.size);
        packet.sequence.store(avSysExRead + AVSysExQueueSize, std::memory_order_release);
        ++avSysExRead;
    }
}

void DexedAudioProcessor::handleAVSysExNow(const uint8_t* buf, int sz) {
    const juce::ScopedLock avSysexStateLock(getCallbackLock());

#ifdef IMPLEMENT_MidiMonitor
    sysexComm.inActivity = true; // indicate to MidiMonitor that a MIDI messages (other than Active Sense) is received
#endif //IMPLEMENT_MidiMonitor


    //TRACE("%X %X %X %X %X %X", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

    if (sz < 5)
        return;
    juce::ScopedValueSetter<bool> avDisableIncomingSysexEcho(sendSysexChange, false);

    // test if it is a Yamaha Sysex
    if ( buf[1] != 0x43 ) {
        DXLOG("Sysex message is not a yamaha sysex %d", buf[1]);
        return;
    }

    int substatus = buf[2] >> 4;
    switch(substatus) {
        case 0 : {
            // single voice dump
            if ( buf[3] == 0 ) {
                if ( sz != 163 || buf[4] != 0x01 || buf[5] != 0x1b || buf[162] != 0xf7
                    || sysexChecksum(buf + 6, 155) != buf[161] ) {
                    DXLOG("wrong single voice datasize %d", sz);
                    return;
                }
                if ( updateProgramFromSysex(buf+6) )
                    DXLOG("bad checksum when updating program from sysex message");
            }

            // 32 voice dump
            if ( buf[3] == 9 ) {
                if ( sz < 4104 ) {
                    DXLOG("wrong 32 voice dump data size %d", sz);
                    return;
                }

                Cartridge received;
                if ( received.load(buf, sz) == 0 ) {
                    loadCartridge(received);
                    setCurrentProgram(0);
                }
            }
        }
        break;
        case 1 : {
            // parameter change
            if ( sz < 7 ) {
            DXLOG("wrong single voice parameter datasize %d", sz);
            return;
            }

            const int offset = (buf[3] << 7) + buf[4];
            uint8 value = buf[5];

            DXLOG("Parameter change message offset:%d value:%d", offset, value);

            if ( offset > 155 ) {
                DXLOG("parameter wrong offset size");
                return;
            }

            setDxParameter(offset, value);
        }
        break;
        case 2: {
            if ( buf[3] == 0 ) {
                // single voice request
                sendCurrentSysexProgram();
            } else if ( buf[3] == 9 ) {
                // cart request
                sendCurrentSysexCartridge();
            } else {
                TRACE("Unknown voice request: %d", buf[3]);
            }
        }
        return;

        default: {
            TRACE("unknown sysex substatus: %d", substatus);
        }
        return;
    }

    // Voice/cartridge dumps refresh the UI via applyProgram/loadCartridge
    // (programChanged), and parameter changes via setDxParameter -> the bound
    // control follows automatically. Nothing else to refresh here.
}
