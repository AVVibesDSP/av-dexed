// Copyright (C) 2026 AV Vibes. Part of AV Dexed.
// SPDX-License-Identifier: MIT (full text in doc/THIRD_PARTY_NOTICES.md)
// Generated from faust/AVDexedDelay.dsp with the command documented there.

/* ------------------------------------------------------------
author: "AV Vibes"
license: "MIT"
name: "AVDexedDelay"
version: "2.0"
Code generated with Faust 2.85.9 (https://faust.grame.fr)
Compilation options: -lang cpp -inpl -fpga-mem-th 4 -ct 1 -cn AVDexedDelayFaust -scn AVDexedFaustDspBase -es 1 -mcd 16 -mdd 1024 -mdy 33 -single -ftz 0
------------------------------------------------------------ */

#ifndef  __AVDexedDelayFaust_H__
#define  __AVDexedDelayFaust_H__

#ifndef FAUSTFLOAT
#define FAUSTFLOAT float
#endif 

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <math.h>

#ifndef FAUSTCLASS 
#define FAUSTCLASS AVDexedDelayFaust
#endif

#ifdef __APPLE__ 
#define exp10f __exp10f
#define exp10 __exp10
#endif

#if defined(_WIN32)
#define RESTRICT __restrict
#else
#define RESTRICT __restrict__
#endif


class AVDexedDelayFaust : public AVDexedFaustDspBase {
	
 private:
	
	FAUSTFLOAT fHslider0;
	FAUSTFLOAT fHslider1;
	int fSampleRate;
	float fConst0;
	float fConst1;
	FAUSTFLOAT fHslider2;
	float fConst2;
	float fRec3[2];
	FAUSTFLOAT fHslider3;
	float fRec4[2];
	float fRec2[2];
	int IOTA0;
	float fVec0[1048576];
	float fConst3;
	FAUSTFLOAT fHslider4;
	float fRec5[2];
	float fRec6[2];
	float fRec7[2];
	float fRec8[2];
	float fRec0[2];
	float fRec9[1048576];
	FAUSTFLOAT fHslider5;
	float fRec10[2];
	float fRec11[2];
	float fRec12[2];
	float fRec13[2];
	float fRec1[2];
	
 public:
	AVDexedDelayFaust() {
	}
	
	AVDexedDelayFaust(const AVDexedDelayFaust&) = default;
	
	virtual ~AVDexedDelayFaust() = default;
	
	AVDexedDelayFaust& operator=(const AVDexedDelayFaust&) = default;
	
	void metadata(Meta* m) { 
		m->declare("author", "AV Vibes");
		m->declare("basics.lib/name", "Faust Basic Element Library");
		m->declare("basics.lib/version", "1.22.0");
		m->declare("compile_options", "-lang cpp -inpl -fpga-mem-th 4 -ct 1 -cn AVDexedDelayFaust -scn AVDexedFaustDspBase -es 1 -mcd 16 -mdd 1024 -mdy 33 -single -ftz 0");
		m->declare("delays.lib/name", "Faust Delay Library");
		m->declare("delays.lib/version", "1.2.0");
		m->declare("filename", "AVDexedDelay.dsp");
		m->declare("license", "MIT");
		m->declare("maths.lib/author", "GRAME");
		m->declare("maths.lib/copyright", "GRAME");
		m->declare("maths.lib/license", "LGPL with exception");
		m->declare("maths.lib/name", "Faust Math Library");
		m->declare("maths.lib/version", "2.9.0");
		m->declare("name", "AVDexedDelay");
		m->declare("platform.lib/name", "Generic Platform Library");
		m->declare("platform.lib/version", "1.3.0");
		m->declare("routes.lib/name", "Faust Signal Routing Library");
		m->declare("routes.lib/version", "1.3.0");
		m->declare("signals.lib/name", "Faust Routing Library");
		m->declare("signals.lib/version", "1.6.0");
		m->declare("version", "2.0");
	}

	virtual int getNumInputs() {
		return 2;
	}
	virtual int getNumOutputs() {
		return 2;
	}
	
	static void classInit(int sample_rate) {
	}
	
	virtual void instanceConstants(int sample_rate) {
		fSampleRate = sample_rate;
		fConst0 = std::min<float>(1.92e+05f, std::max<float>(1.0f, static_cast<float>(fSampleRate)));
		fConst1 = 44.1f / fConst0;
		fConst2 = 1.0f - fConst1;
		fConst3 = 0.001f * fConst0;
	}
	
	virtual void instanceResetUserInterface() {
		fHslider0 = static_cast<FAUSTFLOAT>(1.0f);
		fHslider1 = static_cast<FAUSTFLOAT>(0.0f);
		fHslider2 = static_cast<FAUSTFLOAT>(0.3f);
		fHslider3 = static_cast<FAUSTFLOAT>(0.35f);
		fHslider4 = static_cast<FAUSTFLOAT>(5e+02f);
		fHslider5 = static_cast<FAUSTFLOAT>(5e+02f);
	}
	
	virtual void instanceClear() {
		for (int l0 = 0; l0 < 2; l0 = l0 + 1) {
			fRec3[l0] = 0.0f;
		}
		for (int l1 = 0; l1 < 2; l1 = l1 + 1) {
			fRec4[l1] = 0.0f;
		}
		for (int l2 = 0; l2 < 2; l2 = l2 + 1) {
			fRec2[l2] = 0.0f;
		}
		IOTA0 = 0;
		for (int l3 = 0; l3 < 1048576; l3 = l3 + 1) {
			fVec0[l3] = 0.0f;
		}
		for (int l4 = 0; l4 < 2; l4 = l4 + 1) {
			fRec5[l4] = 0.0f;
		}
		for (int l5 = 0; l5 < 2; l5 = l5 + 1) {
			fRec6[l5] = 0.0f;
		}
		for (int l6 = 0; l6 < 2; l6 = l6 + 1) {
			fRec7[l6] = 0.0f;
		}
		for (int l7 = 0; l7 < 2; l7 = l7 + 1) {
			fRec8[l7] = 0.0f;
		}
		for (int l8 = 0; l8 < 2; l8 = l8 + 1) {
			fRec0[l8] = 0.0f;
		}
		for (int l9 = 0; l9 < 1048576; l9 = l9 + 1) {
			fRec9[l9] = 0.0f;
		}
		for (int l10 = 0; l10 < 2; l10 = l10 + 1) {
			fRec10[l10] = 0.0f;
		}
		for (int l11 = 0; l11 < 2; l11 = l11 + 1) {
			fRec11[l11] = 0.0f;
		}
		for (int l12 = 0; l12 < 2; l12 = l12 + 1) {
			fRec12[l12] = 0.0f;
		}
		for (int l13 = 0; l13 < 2; l13 = l13 + 1) {
			fRec13[l13] = 0.0f;
		}
		for (int l14 = 0; l14 < 2; l14 = l14 + 1) {
			fRec1[l14] = 0.0f;
		}
	}
	
	virtual void init(int sample_rate) {
		classInit(sample_rate);
		instanceInit(sample_rate);
	}
	
	virtual void instanceInit(int sample_rate) {
		instanceConstants(sample_rate);
		instanceResetUserInterface();
		instanceClear();
	}
	
	virtual AVDexedDelayFaust* clone() {
		return new AVDexedDelayFaust(*this);
	}
	
	virtual int getSampleRate() {
		return fSampleRate;
	}
	
	virtual void buildUserInterface(UI* ui_interface) {
		ui_interface->openVerticalBox("AVDexedDelay");
		ui_interface->addHorizontalSlider("damp", &fHslider2, FAUSTFLOAT(0.3f), FAUSTFLOAT(0.0f), FAUSTFLOAT(1.0f), FAUSTFLOAT(0.001f));
		ui_interface->addHorizontalSlider("dry", &fHslider0, FAUSTFLOAT(1.0f), FAUSTFLOAT(0.0f), FAUSTFLOAT(1.0f), FAUSTFLOAT(0.001f));
		ui_interface->addHorizontalSlider("feedback", &fHslider3, FAUSTFLOAT(0.35f), FAUSTFLOAT(0.0f), FAUSTFLOAT(0.9f), FAUSTFLOAT(0.001f));
		ui_interface->declare(&fHslider4, "unit", "ms");
		ui_interface->addHorizontalSlider("timeMsL", &fHslider4, FAUSTFLOAT(5e+02f), FAUSTFLOAT(2e+01f), FAUSTFLOAT(1.5e+03f), FAUSTFLOAT(0.1f));
		ui_interface->declare(&fHslider5, "unit", "ms");
		ui_interface->addHorizontalSlider("timeMsR", &fHslider5, FAUSTFLOAT(5e+02f), FAUSTFLOAT(2e+01f), FAUSTFLOAT(1.5e+03f), FAUSTFLOAT(0.1f));
		ui_interface->addHorizontalSlider("wet", &fHslider1, FAUSTFLOAT(0.0f), FAUSTFLOAT(0.0f), FAUSTFLOAT(1.0f), FAUSTFLOAT(0.001f));
		ui_interface->closeBox();
	}
	
	virtual void compute(int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs) {
		FAUSTFLOAT* input0 = inputs[0];
		FAUSTFLOAT* input1 = inputs[1];
		FAUSTFLOAT* output0 = outputs[0];
		FAUSTFLOAT* output1 = outputs[1];
		float fSlow0 = static_cast<float>(fHslider0);
		float fSlow1 = static_cast<float>(fHslider1);
		float fSlow2 = fConst1 * static_cast<float>(fHslider2);
		float fSlow3 = fConst1 * static_cast<float>(fHslider3);
		float fSlow4 = std::min<float>(524287.0f, std::max<float>(1.0f, fConst3 * static_cast<float>(fHslider4)));
		float fSlow5 = std::min<float>(524287.0f, std::max<float>(1.0f, fConst3 * static_cast<float>(fHslider5)));
		for (int i0 = 0; i0 < count; i0 = i0 + 1) {
			float fTemp0 = static_cast<float>(input0[i0]);
			float fTemp1 = static_cast<float>(input1[i0]);
			fRec3[0] = fSlow2 + fConst2 * fRec3[1];
			fRec4[0] = fSlow3 + fConst2 * fRec4[1];
			float fTemp2 = 1.0f - 0.85f * fRec3[0];
			fRec2[0] = 0.85f * fRec3[0] * fRec2[1] + fRec4[0] * fRec1[1] * fTemp2;
			float fTemp3 = fRec2[0] + 0.5f * (fTemp0 + fTemp1);
			fVec0[IOTA0 & 1048575] = fTemp3;
			float fTemp4 = ((fRec5[1] != 0.0f) ? (((fRec6[1] > 0.0f) & (fRec6[1] < 1.0f)) ? fRec5[1] : 0.0f) : (((fRec6[1] == 0.0f) & (fSlow4 != fRec7[1])) ? 0.0009765625f : (((fRec6[1] == 1.0f) & (fSlow4 != fRec8[1])) ? -0.0009765625f : 0.0f)));
			fRec5[0] = fTemp4;
			fRec6[0] = std::max<float>(0.0f, std::min<float>(1.0f, fRec6[1] + fTemp4));
			fRec7[0] = (((fRec6[1] >= 1.0f) & (fRec8[1] != fSlow4)) ? fSlow4 : fRec7[1]);
			fRec8[0] = (((fRec6[1] <= 0.0f) & (fRec7[1] != fSlow4)) ? fSlow4 : fRec8[1]);
			float fTemp5 = fVec0[(IOTA0 - static_cast<int>(std::min<float>(524288.0f, std::max<float>(0.0f, fRec7[0])))) & 1048575];
			fRec0[0] = fTemp5 + fRec6[0] * (fVec0[(IOTA0 - static_cast<int>(std::min<float>(524288.0f, std::max<float>(0.0f, fRec8[0])))) & 1048575] - fTemp5);
			fRec9[IOTA0 & 1048575] = 0.85f * fRec3[0] * fRec9[(IOTA0 - 1) & 1048575] + fRec4[0] * fTemp2 * fRec0[1];
			float fTemp6 = ((fRec10[1] != 0.0f) ? (((fRec11[1] > 0.0f) & (fRec11[1] < 1.0f)) ? fRec10[1] : 0.0f) : (((fRec11[1] == 0.0f) & (fSlow5 != fRec12[1])) ? 0.0009765625f : (((fRec11[1] == 1.0f) & (fSlow5 != fRec13[1])) ? -0.0009765625f : 0.0f)));
			fRec10[0] = fTemp6;
			fRec11[0] = std::max<float>(0.0f, std::min<float>(1.0f, fRec11[1] + fTemp6));
			fRec12[0] = (((fRec11[1] >= 1.0f) & (fRec13[1] != fSlow5)) ? fSlow5 : fRec12[1]);
			fRec13[0] = (((fRec11[1] <= 0.0f) & (fRec12[1] != fSlow5)) ? fSlow5 : fRec13[1]);
			float fTemp7 = fRec9[(IOTA0 - static_cast<int>(std::min<float>(524288.0f, std::max<float>(0.0f, fRec12[0])))) & 1048575];
			fRec1[0] = fTemp7 + fRec11[0] * (fRec9[(IOTA0 - static_cast<int>(std::min<float>(524288.0f, std::max<float>(0.0f, fRec13[0])))) & 1048575] - fTemp7);
			output0[i0] = static_cast<FAUSTFLOAT>(fSlow0 * fTemp0 + fSlow1 * fRec0[0]);
			output1[i0] = static_cast<FAUSTFLOAT>(fSlow0 * fTemp1 + fSlow1 * fRec1[0]);
			fRec3[1] = fRec3[0];
			fRec4[1] = fRec4[0];
			fRec2[1] = fRec2[0];
			IOTA0 = IOTA0 + 1;
			fRec5[1] = fRec5[0];
			fRec6[1] = fRec6[0];
			fRec7[1] = fRec7[0];
			fRec8[1] = fRec8[0];
			fRec0[1] = fRec0[0];
			fRec10[1] = fRec10[0];
			fRec11[1] = fRec11[0];
			fRec12[1] = fRec12[0];
			fRec13[1] = fRec13[0];
			fRec1[1] = fRec1[0];
		}
	}

};

#endif
