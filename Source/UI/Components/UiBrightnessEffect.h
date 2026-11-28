// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../../JuceHeader.h"

// Scales the whole editor image when the Settings "UI brightness" value is not
// 100 %. Extracted from MainComponent.cpp without changing the filter.
class UiBrightnessEffect final : public juce::ImageEffectFilter
{
public:
  void setBrightnessPercent(double percent)
  {
    brightnessFactor = static_cast<float>(
        juce::jlimit(75.0, 200.0, percent) / 100.0);
  }

  void applyEffect(juce::Image &sourceImage, juce::Graphics &destContext,
                   float scaleFactor, float alpha) override
  {
    juce::ignoreUnused(scaleFactor);
    juce::Image::BitmapData pixels(sourceImage,
                                   juce::Image::BitmapData::readWrite);

    const auto adjustedChannel = [this](juce::uint8 channel,
                                        int maximum = 255)
    {
      return static_cast<juce::uint8>(juce::jlimit(
          0, maximum,
          static_cast<int>(std::round(static_cast<float>(channel) *
                                      brightnessFactor))));
    };

    for (int y = 0; y < pixels.height; ++y)
    {
      auto *line = pixels.getLinePointer(y);
      for (int x = 0; x < pixels.width; ++x)
      {
        auto *pixelData = line + x * pixels.pixelStride;
        if (pixels.pixelFormat == juce::Image::RGB)
        {
          auto &pixel = *reinterpret_cast<juce::PixelRGB *>(pixelData);
          pixel.setARGB(255, adjustedChannel(pixel.getRed()),
                        adjustedChannel(pixel.getGreen()),
                        adjustedChannel(pixel.getBlue()));
        }
        else if (pixels.pixelFormat == juce::Image::ARGB)
        {
          auto &pixel = *reinterpret_cast<juce::PixelARGB *>(pixelData);
          const auto pixelAlpha = pixel.getAlpha();
          pixel.setARGB(pixelAlpha,
                        adjustedChannel(pixel.getRed(), pixelAlpha),
                        adjustedChannel(pixel.getGreen(), pixelAlpha),
                        adjustedChannel(pixel.getBlue(), pixelAlpha));
        }
      }
    }

    destContext.setOpacity(alpha);
    destContext.drawImageAt(sourceImage, 0, 0, false);
  }

private:
  float brightnessFactor = 1.0f;
};
