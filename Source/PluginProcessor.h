#pragma once
#include <JuceHeader.h>
#include <array>

class VocalAtmosphere : public juce::AudioProcessor {
public:
    VocalAtmosphere();
    void prepareToPlay(double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Vocal Atmosphere"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
public:
    juce::AudioProcessorValueTreeState& getParameters() { return params; }
private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeParameters();
    juce::AudioProcessorValueTreeState params;
    juce::dsp::StateVariableTPTFilter<float> highpass;
    juce::Reverb reverb;
    juce::AudioBuffer<float> delayBuffer, wetBuffer;
    int delayWrite = 0;
    double sampleRateHz = 44100.0;
    float envelope = 0.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocalAtmosphere)
};
