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
    reverb.setSampleRate(sr); reverb.reset();
    delayBuffer.setSize(2, static_cast<int>(sr * 0.7) + 2); delayBuffer.clear();
    wetBuffer.setSize(2, maxBlock); wetBuffer.clear();
    delayWrite = 0; envelope = 0;
}
void VocalAtmosphere::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals guard;
    const int n = buffer.getNumSamples(), channels = buffer.getNumChannels();
    auto val = [&](const char* id) { return params.getRawParameterValue(id)->load(); };
    const float inGain = juce::Decibels::decibelsToGain(val("input"));
    const float outGain = juce::Decibels::decibelsToGain(val("output"));
    const float comp = val("compress") * 0.01f, drive = 1.0f + val("warmth") * 0.025f;
    const float dmix = val("delay") * 0.01f, rmix = val("reverb") * 0.01f;
    const int delayLength = juce::jlimit(1, delayBuffer.getNumSamples() - 1, static_cast<int>(sampleRateHz * val("time") * 0.001));
    const float attack = std::exp(-1.0f / static_cast<float>(sampleRateHz * 0.008));
    const float release = std::exp(-1.0f / static_cast<float>(sampleRateHz * 0.12));
    juce::Reverb::Parameters rp; rp.roomSize = 0.65f; rp.damping = 0.55f; rp.wetLevel = 1.0f; rp.dryLevel = 0.0f; rp.width = 1.0f; rp.freezeMode = 0.0f;
    reverb.setParameters(rp);
    // Resize only if the host supplies a block larger than its advertised maximum.
    if (wetBuffer.getNumSamples() < n) wetBuffer.setSize(2, n, false, false, true);
    for (int i = 0; i < n; ++i) {
        float peak = 0;
        for (int c = 0; c < channels; ++c) {
            float x = highpass.processSample(c, buffer.getSample(c, i) * inGain);
            buffer.setSample(c, i, x); peak = juce::jmax(peak, std::abs(x));
        }
        float coeff = peak > envelope ? attack : release;
        envelope = coeff * envelope + (1.0f - coeff) * peak;
        float levelDb = juce::Decibels::gainToDecibels(envelope, -100.0f);
        float reduction = juce::jmax(0.0f, levelDb + 20.0f) * (0.75f * comp);
        float gain = juce::Decibels::decibelsToGain(-reduction + comp * 2.0f);
        int read = (delayWrite - delayLength + delayBuffer.getNumSamples()) % delayBuffer.getNumSamples();
        for (int c = 0; c < channels; ++c) {
            float x = buffer.getSample(c, i) * gain;
            x = std::tanh(x * drive) / drive;
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
        g.setColour(juce::Colour(0xff29344b)); g.strokePath(track, juce::PathStrokeType(4.0f));
        juce::Path active; active.addCentredArc(centre.x, centre.y, radius, radius, 0, start, start + pos * (end-start), true);
        g.setColour(juce::Colour(0xff84e4d7)); g.strokePath(active, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff354055), centre.x, bounds.getY(), juce::Colour(0xff171e2e), centre.x, bounds.getBottom(), false));
        g.fillEllipse(bounds.reduced(7.0f));
        float angle = start + pos * (end - start);
        juce::Path pointer; pointer.addRoundedRectangle(-2.0f, -radius + 15.0f, 4.0f, radius * 0.36f, 2.0f);
        g.setColour(juce::Colour(0xffd9fff9)); g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre.x, centre.y));
    }
};

class AtmosphereEditor : public juce::AudioProcessorEditor {
public:
    explicit AtmosphereEditor(VocalAtmosphere& p) : AudioProcessorEditor(&p), processor(p) {
        setLookAndFeel(&look);
        const char* ids[] = {"input", "compress", "warmth", "delay", "time", "reverb", "output"};
        const char* names[] = {"INPUT", "COMPRESS", "WARMTH", "DELAY", "TIME", "REVERB", "OUTPUT"};
        const char* suffixes[] = {" dB", " %", " %", " %", " ms", " %", " dB"};
        for (int i = 0; i < 7; ++i) {
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
        setSize(820, 390);
    }
    ~AtmosphereEditor() override { setLookAndFeel(nullptr); }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff1e2c3c), 0, 0, juce::Colour(0xff0b101d), r.getRight(), r.getBottom(), false));
        g.fillAll();
        g.setColour(juce::Colour(0xff263c4a)); g.fillRoundedRectangle(24, 104, 772, 238, 14);
        g.setColour(juce::Colour(0xff111b2b)); g.fillRoundedRectangle(25, 105, 770, 236, 13);
        g.setColour(juce::Colour(0xff81e4d7)); g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText("VOCAL CHAIN  /  VST3", 30, 23, 300, 20, juce::Justification::left);
        g.setColour(juce::Colour(0xfff1f8f7)); g.setFont(juce::FontOptions(27.0f, juce::Font::bold));
        g.drawText("VOCAL ATMOSPHERE", 29, 48, 490, 36, juce::Justification::left);
        g.setColour(juce::Colour(0xff91aaaF)); g.setFont(juce::FontOptions(13.0f));
        g.drawText("AIR  /  SPACE  /  WARMTH", 531, 56, 260, 22, juce::Justification::right);
        g.setColour(juce::Colour(0xff8295a7)); g.setFont(juce::FontOptions(12.0f));
        g.drawText("DRAG TO ADJUST   /   DOUBLE-CLICK TO RESET   /   CLICK VALUE TO TYPE", 30, 352, 750, 20, juce::Justification::left);
    }
    void resized() override {
        for (int i = 0; i < 7; ++i) {
            const int x = 37 + i * 108;
            labels[(size_t)i].setBounds(x, 133, 100, 22);
            sliders[(size_t)i].setBounds(x, 164, 100, 135);
        }
    }
private:
    VocalAtmosphere& processor;
    AtmosphereLook look;
    std::array<juce::Slider, 7> sliders;
    std::array<juce::Label, 7> labels;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, 7> attachments;
};
juce::AudioProcessorEditor* VocalAtmosphere::createEditor() { return new AtmosphereEditor(*this); }
void VocalAtmosphere::getStateInformation(juce::MemoryBlock& dest) { if (auto xml = params.copyState().createXml()) copyXmlToBinary(*xml, dest); }
void VocalAtmosphere::setStateInformation(const void* data, int size) { if (auto xml = getXmlFromBinary(data, size)) if (xml->hasTagName(params.state.getType())) params.replaceState(juce::ValueTree::fromXml(*xml)); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VocalAtmosphere(); }
