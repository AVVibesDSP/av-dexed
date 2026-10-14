// Modified by AV Vibes, 2026 (AV Dexed): tuning errors are returned to the
// caller instead of opening alert windows from the audio or host thread.

#ifndef __SYNTH_TUNING_H
#define __SYNTH_TUNING_H

#include "synth.h"
#include <memory>
#include <string>
#include "Tunings.h"

class TuningState {
public:
    virtual ~TuningState() { }

    virtual int32_t midinote_to_logfreq(int midinote) = 0;
    virtual bool is_standard_tuning() { return true; }
    virtual int scale_length() { return 12; }
    virtual std::string display_tuning_str() { return "Standard Tuning"; }

    virtual Tunings::Tuning &getTuning() {
        static Tunings::Tuning t;
        return t;
    }
};

std::shared_ptr<TuningState> createStandardTuning();

// On failure these return nullptr and, when `error` is given, the parser's
// technical detail; the caller decides how to present it.
std::shared_ptr<TuningState> createTuningFromSCLData( const std::string &sclData, std::string* error = nullptr );
std::shared_ptr<TuningState> createTuningFromKBMData( const std::string &kbmData, std::string* error = nullptr );
std::shared_ptr<TuningState> createTuningFromSCLAndKBMData( const std::string &sclData, const std::string &kbmData, std::string* error = nullptr );

#endif
