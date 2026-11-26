#include "PluginProcessor.h"
#include <cmath>

juce::AudioProcessorValueTreeState::ParameterLayout VocalAtmosphere::makeParameters() {
    using P = juce::AudioParameterFloat;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    auto add = [&](const char* id, const char* name, float lo, float hi, float def) {
        p.push_back(std::make_unique<P>(juce::ParameterID{id, 1}, name, juce::NormalisableRange<float>{lo, hi}, def));
    };
    add("input", "Input (dB)", -18, 18, 0);
    add("compress", "Compression", 0, 100, 50);
    add("warmth", "Warmth", 0, 100, 20);
    add("delay", "Delay mix", 0, 50, 13);
    add("time", "Delay time (ms)", 100, 650, 340);
    add("reverb", "Reverb mix", 0, 60, 20);
    add("output", "Output (dB)", -18, 12, 0);
    add("deess", "De-esser", 0, 100, 25);
    add("distort", "Distortion", 0, 100, 8);
    add("tone", "Brightness", -100, 100, 0);
    add("tune", "Pitch correction", 0, 100, 0);
    p.push_back(std::make_unique<P>(juce::ParameterID{"key", 1}, "Key (0=C ... 11=B)", juce::NormalisableRange<float>{0, 11, 1}, 0));
    p.push_back(std::make_unique<P>(juce::ParameterID{"scale", 1}, "Scale (0=chromatic 1=major 2=minor)", juce::NormalisableRange<float>{0, 2, 1}, 0));
    return {p.begin(), p.end()};
}
VocalAtmosphere::VocalAtmosphere()
 : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                   .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
   params(*this, nullptr, "Parameters", makeParameters()) {}

bool VocalAtmosphere::isBusesLayoutSupported(const BusesLayout& layouts) const {
    auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
        && layouts.getMainInputChannelSet() == out;
}
void VocalAtmosphere::prepareToPlay(double sr, int maxBlock) {
    sampleRateHz = sr;
    highpass.prepare({sr, static_cast<juce::uint32>(maxBlock), static_cast<juce::uint32>(getTotalNumOutputChannels())});
    highpass.setType(juce::dsp::StateVariableTPTFilterType::highpass);
    highpass.setCutoffFrequency(90.0f);
    highpass.reset();
    sibilanceFilter.prepare({sr, static_cast<juce::uint32>(maxBlock), static_cast<juce::uint32>(getTotalNumOutputChannels())});
    sibilanceFilter.setType(juce::dsp::StateVariableTPTFilterType::highpass);
    sibilanceFilter.setCutoffFrequency(6000.0f); sibilanceFilter.reset();
    reverb.setSampleRate(sr); reverb.reset();
    delayBuffer.setSize(2, static_cast<int>(sr * 0.7) + 2); delayBuffer.clear();
    wetBuffer.setSize(2, maxBlock); wetBuffer.clear();
    delayWrite = 0; envelope = 0; sibilanceEnvelope = 0; toneLow = {0.0f, 0.0f};
    for (auto& channel : tuneRing) channel.fill(0.0f);
    pitchHistory.fill(0.0f); tuneWrite = pitchWrite = pitchCounter = 0;
    tunePhase = 0.0f; targetRatio = currentRatio = 1.0f;
    setLatencySamples(1024);
}

float VocalAtmosphere::readTuneRing(int channel, float delay) const {
    float position = static_cast<float>(tuneWrite) - delay;
    while (position < 0.0f) position += static_cast<float>(tuneRingSize);
    int i0 = static_cast<int>(position) % tuneRingSize;
    int i1 = (i0 + 1) % tuneRingSize;
    float f = position - std::floor(position);
    return tuneRing[(size_t)channel][(size_t)i0] * (1.0f - f) + tuneRing[(size_t)channel][(size_t)i1] * f;
}
void VocalAtmosphere::updatePitch(float strength, int key, int scale) {
    if (strength < 0.001f) { targetRatio = 1.0f; return; }
    // Normalized autocorrelation. Designed for a single, reasonably clean vocal.
    float best = 0.0f; int bestLag = 0;
    int minLag = juce::jmax(24, (int)(sampleRateHz / 950.0));
    int maxLag = juce::jmin(900, (int)(sampleRateHz / 80.0));
    for (int lag = minLag; lag <= maxLag; lag += 2) {
        double cross = 0, energyA = 0, energyB = 0;
        for (int n = 0; n < 1024; n += 4) {
            float a = pitchHistory[(size_t)((pitchWrite - 1 - n + 4096) % 2048)];
            float b = pitchHistory[(size_t)((pitchWrite - 1 - n - lag + 4096) % 2048)];
            cross += a * b; energyA += a * a; energyB += b * b;
        }
        if (energyA < 0.001 || energyB < 0.001) continue;
        float score = (float)(cross / std::sqrt(energyA * energyB + 1.0e-12));
        // Prefer a smaller lag when correlations are comparably strong.
        if (score > best + 0.015f) { best = score; bestLag = lag; }
    }
    if (best < 0.68f || bestLag == 0) { targetRatio = 1.0f; return; }
    float hz = (float)sampleRateHz / (float)bestLag;
    float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
    static constexpr int major[] = {0,2,4,5,7,9,11};
    static constexpr int minor[] = {0,2,3,5,7,8,10};
    int closest = (int)std::round(midi); float distance = 100.0f;
    for (int note = (int)std::floor(midi) - 6; note <= (int)std::ceil(midi) + 6; ++note) {
        int pc = ((note - key) % 12 + 12) % 12;
        bool allowed = scale == 0;
        for (int j = 0; j < 7 && !allowed; ++j) allowed = pc == (scale == 1 ? major[j] : minor[j]);
        float d = std::abs((float)note - midi);
        if (allowed && d < distance) { closest = note; distance = d; }
    }
    targetRatio = juce::jlimit(0.75f, 1.3333f, std::pow(2.0f, (((float)closest - midi) * strength) / 12.0f));
}

void VocalAtmosphere::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals guard;
    const int n = buffer.getNumSamples(), channels = buffer.getNumChannels();
    auto val = [&](const char* id) { return params.getRawParameterValue(id)->load(); };
    const float inGain = juce::Decibels::decibelsToGain(val("input"));
    const float outGain = juce::Decibels::decibelsToGain(val("output"));
    const float comp = val("compress") * 0.01f, drive = 1.0f + val("warmth") * 0.025f;
    const float dmix = val("delay") * 0.01f, rmix = val("reverb") * 0.01f, deess = val("deess") * 0.01f;
    const float distortion = val("distort") * 0.01f, tone = val("tone") * 0.005f;
    const float tuneStrength = val("tune") * 0.01f;
    const int key = juce::jlimit(0, 11, (int)std::round(val("key")));
    const int scale = juce::jlimit(0, 2, (int)std::round(val("scale")));
    const float toneCoeff = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * 2800.0f / static_cast<float>(sampleRateHz));
    const int delayLength = juce::jlimit(1, delayBuffer.getNumSamples() - 1, static_cast<int>(sampleRateHz * val("time") * 0.001));
    const float attack = std::exp(-1.0f / static_cast<float>(sampleRateHz * 0.008));
    const float release = std::exp(-1.0f / static_cast<float>(sampleRateHz * 0.12));
    juce::Reverb::Parameters rp; rp.roomSize = 0.65f; rp.damping = 0.55f; rp.wetLevel = 1.0f; rp.dryLevel = 0.0f; rp.width = 1.0f; rp.freezeMode = 0.0f;
    reverb.setParameters(rp);
    // Resize only if the host supplies a block larger than its advertised maximum.
    if (wetBuffer.getNumSamples() < n) wetBuffer.setSize(2, n, false, false, true);
    for (int i = 0; i < n; ++i) {
        float mono = 0.0f;
        for (int c = 0; c < channels; ++c) {
            float input = buffer.getSample(c, i);
            tuneRing[(size_t)c][(size_t)tuneWrite] = input;
            mono += input / (float)channels;
        }
        pitchHistory[(size_t)pitchWrite] = mono;
        pitchWrite = (pitchWrite + 1) % 2048;
        if (++pitchCounter >= 512) { pitchCounter = 0; updatePitch(tuneStrength, key, scale); }
        currentRatio += 0.002f * (targetRatio - currentRatio);
        tunePhase += (1.0f - currentRatio) / 1024.0f;
        tunePhase -= std::floor(tunePhase);
        const float phase2 = std::fmod(tunePhase + 0.5f, 1.0f);
        const float weight1 = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * tunePhase);
        const float weight2 = 1.0f - weight1;
        float peak = 0;
        for (int c = 0; c < channels; ++c) {
            float tuned = readTuneRing(c, 512.0f + 1024.0f * tunePhase) * weight1
                        + readTuneRing(c, 512.0f + 1024.0f * phase2) * weight2;
            float x = highpass.processSample(c, tuned * inGain);
            buffer.setSample(c, i, x); peak = juce::jmax(peak, std::abs(x));
        }
        tuneWrite = (tuneWrite + 1) % tuneRingSize;
        float coeff = peak > envelope ? attack : release;
        envelope = coeff * envelope + (1.0f - coeff) * peak;
        float levelDb = juce::Decibels::gainToDecibels(envelope, -100.0f);
        float reduction = juce::jmax(0.0f, levelDb + 20.0f) * (0.75f * comp);
        float gain = juce::Decibels::decibelsToGain(-reduction + comp * 2.0f);
        int read = (delayWrite - delayLength + delayBuffer.getNumSamples()) % delayBuffer.getNumSamples();
        float highs[2] = {0, 0};
        float highPeak = 0.0f;
        for (int c = 0; c < channels; ++c) {
            highs[c] = sibilanceFilter.processSample(c, buffer.getSample(c, i) * gain);
            highPeak = juce::jmax(highPeak, std::abs(highs[c]));
        }
        const float sCoeff = highPeak > sibilanceEnvelope ? attack : release;
        sibilanceEnvelope = sCoeff * sibilanceEnvelope + (1.0f - sCoeff) * highPeak;
        float sReduction = deess * juce::jlimit(0.0f, 0.75f, (sibilanceEnvelope - 0.015f) * 4.0f);
        for (int c = 0; c < channels; ++c) {
            float x = buffer.getSample(c, i) * gain - highs[c] * sReduction;
            x = std::tanh(x * drive) / drive;
            toneLow[(size_t)c] += toneCoeff * (x - toneLow[(size_t)c]);
            x += (x - toneLow[(size_t)c]) * tone;
            const float dirty = std::tanh(x * (1.0f + 8.0f * distortion));
            x = x * (1.0f - distortion * 0.5f) + dirty * (distortion * 0.25f);
            float delayed = delayBuffer.getSample(c, read);
            delayBuffer.setSample(c, delayWrite, x + delayed * 0.28f);
            float combined = x + delayed * dmix;
            buffer.setSample(c, i, combined);
            wetBuffer.setSample(c, i, combined);
        }
        delayWrite = (delayWrite + 1) % delayBuffer.getNumSamples();
    }
    if (channels == 1) reverb.processMono(wetBuffer.getWritePointer(0), n);
    else reverb.processStereo(wetBuffer.getWritePointer(0), wetBuffer.getWritePointer(1), n);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < n; ++i)
            buffer.setSample(c, i, (buffer.getSample(c, i) + wetBuffer.getSample(c, i) * rmix) * outGain);
}
class AtmosphereLook : public juce::LookAndFeel_V4 {
public:
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float pos, float start, float end, juce::Slider&) override {
        auto area = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height).reduced(9.0f);
        auto radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f - 7.0f;
        auto centre = area.getCentre();
        auto bounds = juce::Rectangle<float>(radius * 2, radius * 2).withCentre(centre);
        juce::Path track; track.addCentredArc(centre.x, centre.y, radius, radius, 0, start, end, true);
        g.setColour(juce::Colour(0xff403052)); g.strokePath(track, juce::PathStrokeType(4.0f));
        juce::Path active; active.addCentredArc(centre.x, centre.y, radius, radius, 0, start, start + pos * (end-start), true);
        g.setColour(juce::Colour(0xffbf8cff)); g.strokePath(active, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff463451), centre.x, bounds.getY(), juce::Colour(0xff17101e), centre.x, bounds.getBottom(), false));
        g.fillEllipse(bounds.reduced(7.0f));
        float angle = start + pos * (end - start);
        juce::Path pointer; pointer.addRoundedRectangle(-2.0f, -radius + 15.0f, 4.0f, radius * 0.36f, 2.0f);
        g.setColour(juce::Colour(0xffffd9f7)); g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre.x, centre.y));
    }
};

class AtmosphereEditor : public juce::AudioProcessorEditor {
public:
    explicit AtmosphereEditor(VocalAtmosphere& p) : AudioProcessorEditor(&p), processor(p) {
        setLookAndFeel(&look);
        const char* ids[] = {"input", "compress", "warmth", "delay", "time", "reverb", "output", "deess", "distort", "tone", "tune", "key", "scale"};
        const char* names[] = {"INPUT", "COMPRESS", "WARMTH", "DELAY", "TIME", "REVERB", "OUTPUT", "DE-ESS", "DISTORT", "BRIGHT", "PITCH", "KEY", "SCALE"};
        const char* suffixes[] = {" dB", " %", " %", " %", " ms", " %", " dB", " %", " %", " %", " %", "", ""};
        for (int i = 0; i < 13; ++i) {
            auto& slider = sliders[(size_t)i];
            slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 88, 24);
            slider.setTextValueSuffix(suffixes[i]);
            slider.setDoubleClickReturnValue(true, processor.getParameters().getParameter(ids[i])->convertFrom0to1(processor.getParameters().getParameter(ids[i])->getDefaultValue()));
            slider.setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xffe4f5f4));
            slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            slider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            addAndMakeVisible(slider);
            auto& label = labels[(size_t)i]; label.setText(names[i], juce::dontSendNotification);
            label.setJustificationType(juce::Justification::centred); label.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            label.setColour(juce::Label::textColourId, juce::Colour(0xffa6bdca)); addAndMakeVisible(label);
            attachments[(size_t)i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.getParameters(), ids[i], slider);
        }
        const char* presetNames[] = {"URAL GAISIN", "MADK1D", "VILLIAN", "TEMNY PRINCE", "TRAVIS SCOTT", "SPACE RAP", "DARK RAP", "CLEAN RAP"};
        for (int i = 0; i < 8; ++i) {
            auto& button = presetButtons[(size_t)i];
            button.setButtonText(presetNames[i]);
            button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff382548));
            button.setColour(juce::TextButton::textColourOffId, juce::Colour(0xfff4e8ff));
            button.onClick = [this, i] { applyPreset(i); };
            addAndMakeVisible(button);
        }
        setSize(820, 825);
    }
    ~AtmosphereEditor() override { setLookAndFeel(nullptr); }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff28172e), 0, 0, juce::Colour(0xff0c0913), r.getRight(), r.getBottom(), false));
        g.fillAll();
        g.setColour(juce::Colour(0xff563862)); g.fillRoundedRectangle(24, 219, 772, 559, 14);
        g.setColour(juce::Colour(0xff16101f)); g.fillRoundedRectangle(25, 220, 770, 559, 13);
        g.setColour(juce::Colour(0xffd8a5ff)); g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText("VOCAL CHAIN  /  VST3", 30, 23, 300, 20, juce::Justification::left);
        g.setColour(juce::Colour(0xfff1f8f7)); g.setFont(juce::FontOptions(27.0f, juce::Font::bold));
        g.drawText("VOCAL ATMOSPHERE", 29, 48, 490, 36, juce::Justification::left);
        g.setColour(juce::Colour(0xffba9abf)); g.setFont(juce::FontOptions(13.0f));
        g.drawText("ARTIST-INSPIRED  /  VOCAL FX", 531, 56, 260, 22, juce::Justification::right);
        g.setColour(juce::Colour(0xff8295a7)); g.setFont(juce::FontOptions(12.0f));
        g.drawText("ARTIST-INSPIRED PRESETS   /   EXPERIMENTAL STARTING POINTS", 30, 100, 750, 20, juce::Justification::left);
        g.drawText("DRAG TO ADJUST   /   DOUBLE-CLICK TO RESET   /   CLICK VALUE TO TYPE", 30, 792, 750, 20, juce::Justification::left);
    }
    void resized() override {
        for (int i = 0; i < 8; ++i)
            presetButtons[(size_t)i].setBounds(30 + (i % 4) * 193, 128 + (i / 4) * 38, 183, 31);
        for (int i = 0; i < 13; ++i) {
            const int x = 72 + (i % 5) * 145;
            const int y = 232 + (i / 5) * 168;
            labels[(size_t)i].setBounds(x, y, 100, 22);
            sliders[(size_t)i].setBounds(x, y + 24, 100, 130);
        }
    }
private:
    void applyPreset(int index) {
        static const char* ids[] = {"input", "compress", "warmth", "delay", "time", "reverb", "output", "deess", "distort", "tone", "tune", "key", "scale"};
        static const float values[8][10] = {
            {0, 50, 20, 13, 340, 20, 0, 25, 8, 0},
            {0, 65, 27, 16, 280, 18, -1, 30, 15, 5},
            {0, 72, 34, 10, 240, 15, -1, 35, 20, 0},
            {0, 60, 37, 14, 430, 23, -2, 30, 15, -15},
            {0, 68, 32, 22, 380, 30, -2, 30, 18, -10},
            {0, 60, 27, 18, 390, 25, -1, 25, 8, 0},
            {0, 75, 45, 12, 300, 15, -2, 35, 25, -10},
            {0, 55, 14, 4, 180, 6, 0, 25, 0, 5}
        };
        for (int i = 0; i < 10; ++i) {
            auto* param = processor.getParameters().getParameter(ids[i]);
            param->beginChangeGesture();
            param->setValueNotifyingHost(param->convertTo0to1(values[index][i]));
            param->endChangeGesture();
        }
    }
    VocalAtmosphere& processor;
    AtmosphereLook look;
    std::array<juce::TextButton, 8> presetButtons;
    std::array<juce::Slider, 13> sliders;
    std::array<juce::Label, 13> labels;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, 13> attachments;
};
juce::AudioProcessorEditor* VocalAtmosphere::createEditor() { return new AtmosphereEditor(*this); }
void VocalAtmosphere::getStateInformation(juce::MemoryBlock& dest) { if (auto xml = params.copyState().createXml()) copyXmlToBinary(*xml, dest); }
void VocalAtmosphere::setStateInformation(const void* data, int size) { if (auto xml = getXmlFromBinary(data, size)) if (xml->hasTagName(params.state.getType())) params.replaceState(juce::ValueTree::fromXml(*xml)); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VocalAtmosphere(); }
