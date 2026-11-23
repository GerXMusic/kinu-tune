#include "MelodyneIcons.h"

namespace
{
juce::Path arrowCursorPath()
{
    juce::Path p;
    p.startNewSubPath(0.0f, 0.0f);
    p.lineTo(0.0f, 0.72f);
    p.lineTo(0.18f, 0.56f);
    p.lineTo(0.33f, 0.92f);
    p.lineTo(0.47f, 0.86f);
    p.lineTo(0.32f, 0.51f);
    p.lineTo(0.52f, 0.50f);
    p.closeSubPath();
    return p;
}

juce::Path gearPath()
{
    juce::Path p;
    const float cx = 0.5f, cy = 0.5f;
    const float outer = 0.42f, inner = 0.30f;
    constexpr int teeth = 8;
    for (int i = 0; i < teeth * 2; ++i)
    {
        const float angle = juce::MathConstants<float>::twoPi *
                            static_cast<float>(i) / static_cast<float>(teeth * 2);
        const float radius = (i % 2 == 0) ? outer : inner;
        const float x = cx + std::cos(angle) * radius;
        const float y = cy + std::sin(angle) * radius;
        if (i == 0)
            p.startNewSubPath(x, y);
        else
            p.lineTo(x, y);
    }
    p.closeSubPath();
    p.addEllipse(cx - 0.13f, cy - 0.13f, 0.26f, 0.26f);
    return p;
}
} // namespace

void drawGuiIcon(juce::Graphics& g, GuiIcon icon, juce::Rectangle<float> bounds,
                 juce::Colour colour, float strokeWidth)
{
    const auto box = bounds.reduced(0.5f);
    const float w = box.getWidth();
    const float h = box.getHeight();
    const auto at = [&](float x, float y)
    { return juce::Point<float>(box.getX() + x * w, box.getY() + y * h); };

    g.setColour(colour);
    juce::PathStrokeType stroke(strokeWidth, juce::PathStrokeType::curved,
                                juce::PathStrokeType::rounded);

    switch (icon)
    {
    case GuiIcon::Notes:
    {
        // Three sliders: the note inspector.
        for (int i = 0; i < 3; ++i)
        {
            const float y = 0.24f + static_cast<float>(i) * 0.26f;
            g.drawLine({ at(0.12f, y), at(0.88f, y) }, strokeWidth);
            g.fillEllipse(juce::Rectangle<float>(0.0f, 0.0f, 0.15f, 0.15f)
                              .withCentre(at(i == 1 ? 0.62f : 0.34f, y)));
        }
        break;
    }
    case GuiIcon::Engine:
    {
        auto chip = juce::Rectangle<float>(0.26f, 0.24f, 0.48f, 0.52f);
        chip = { box.getX() + chip.getX() * w, box.getY() + chip.getY() * h,
                 chip.getWidth() * w, chip.getHeight() * h };
        g.drawRoundedRectangle(chip, 2.0f, strokeWidth);
        for (int i = 0; i < 3; ++i)
        {
            const float y = 0.34f + static_cast<float>(i) * 0.16f;
            g.drawLine({ at(0.12f, y), at(0.26f, y) }, strokeWidth);
            g.drawLine({ at(0.74f, y), at(0.88f, y) }, strokeWidth);
        }
        g.fillEllipse(juce::Rectangle<float>(0.0f, 0.0f, 0.10f, 0.10f)
                          .withCentre(at(0.5f, 0.5f)));
        break;
    }
    case GuiIcon::Tracks:
    {
        for (int i = 0; i < 3; ++i)
        {
            const float y = 0.20f + static_cast<float>(i) * 0.26f;
            juce::Path layer;
            layer.startNewSubPath(at(0.14f, y + 0.10f));
            layer.lineTo(at(0.50f, y - 0.04f));
            layer.lineTo(at(0.86f, y + 0.10f));
            layer.lineTo(at(0.50f, y + 0.24f));
            layer.closeSubPath();
            g.strokePath(layer, stroke);
        }
        break;
    }
    case GuiIcon::Harmony:
    {
        // Quarter-note glyph centred in the icon box. The old path started at
        // x = 0.46 and ran to 0.88, so the whole icon read shifted right.
        g.drawLine({ at(0.52f, 0.12f), at(0.52f, 0.72f) }, strokeWidth * 1.4f);
        g.drawLine({ at(0.52f, 0.12f), at(0.76f, 0.18f) }, strokeWidth * 1.4f);
        g.fillEllipse(juce::Rectangle<float>(0.0f, 0.0f, 0.24f * w, 0.18f * h)
                          .withCentre(at(0.40f, 0.76f)));
        g.fillEllipse(juce::Rectangle<float>(0.0f, 0.0f, 0.15f * w, 0.15f * h)
                          .withCentre(at(0.66f, 0.70f)));
        break;
    }
    case GuiIcon::MidiGuide:
    {
        juce::Path doc;
        doc.startNewSubPath(at(0.24f, 0.10f));
        doc.lineTo(at(0.66f, 0.10f));
        doc.lineTo(at(0.80f, 0.24f));
        doc.lineTo(at(0.80f, 0.90f));
        doc.lineTo(at(0.24f, 0.90f));
        doc.closeSubPath();
        g.strokePath(doc, stroke);
        g.drawLine({ at(0.34f, 0.42f), at(0.70f, 0.42f) }, strokeWidth);
        g.drawLine({ at(0.34f, 0.56f), at(0.70f, 0.56f) }, strokeWidth);
        g.drawLine({ at(0.34f, 0.70f), at(0.58f, 0.70f) }, strokeWidth);
        break;
    }
    case GuiIcon::Settings:
    {
        auto gear = gearPath();
        gear.applyTransform(juce::AffineTransform::scale(w, h)
                                .translated(box.getX(), box.getY()));
        g.strokePath(gear, stroke);
        break;
    }
    case GuiIcon::MainTool:
    {
        auto p = arrowCursorPath();
        p.applyTransform(juce::AffineTransform::scale(w * 1.25f, h * 1.10f)
                             .translated(box.getX() + w * 0.12f, box.getY() + h * 0.05f));
        g.fillPath(p);
        break;
    }
    case GuiIcon::PitchTool:
    {
        juce::Path p;
        p.startNewSubPath(at(0.12f, 0.70f));
        p.cubicTo(at(0.30f, 0.70f), at(0.36f, 0.30f), at(0.52f, 0.30f));
        p.cubicTo(at(0.68f, 0.30f), at(0.70f, 0.60f), at(0.88f, 0.58f));
        g.strokePath(p, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
        g.fillEllipse(juce::Rectangle<float>(0.0f, 0.0f, 0.17f * w, 0.17f * w)
                          .withCentre(at(0.12f, 0.70f)));
        g.fillEllipse(juce::Rectangle<float>(0.0f, 0.0f, 0.17f * w, 0.17f * w)
                          .withCentre(at(0.88f, 0.58f)));
        break;
    }
    case GuiIcon::FormantTool:
    {
        const float heights[] = {0.52f, 0.86f, 0.42f};
        for (int i = 0; i < 3; ++i)
        {
            const float x = 0.24f + static_cast<float>(i) * 0.24f;
            const float hh = heights[i];
            juce::Rectangle<float> bar(box.getX() + x * w - 1.6f,
                                       box.getY() + (0.5f - hh * 0.5f) * h,
                                       3.2f, hh * h);
            g.fillRoundedRectangle(bar, 1.6f);
        }
        g.fillEllipse(juce::Rectangle<float>(0.0f, 0.0f, 0.12f * w, 0.12f * w)
                          .withCentre(at(0.5f, 0.90f)));
        break;
    }
    case GuiIcon::AmplitudeTool:
    {
        const float heights[] = {0.30f, 0.52f, 0.78f, 0.42f, 0.64f};
        for (int i = 0; i < 5; ++i)
        {
            const float x = 0.18f + static_cast<float>(i) * 0.16f;
            const float hh = heights[i];
            g.fillRoundedRectangle(juce::Rectangle<float>(
                                       box.getX() + x * w - 1.8f,
                                       box.getY() + (0.5f - hh * 0.5f) * h,
                                       3.6f, hh * h),
                                   1.8f);
        }
        break;
    }
    case GuiIcon::SeparationTool:
    {
        g.drawLine({ at(0.20f, 0.16f), at(0.78f, 0.78f) }, strokeWidth);
        g.drawLine({ at(0.80f, 0.16f), at(0.22f, 0.78f) }, strokeWidth);
        g.drawEllipse(juce::Rectangle<float>(0.0f, 0.0f, 0.16f, 0.16f)
                          .withCentre(at(0.18f, 0.82f)),
                      strokeWidth);
        g.drawEllipse(juce::Rectangle<float>(0.0f, 0.0f, 0.16f, 0.16f)
                          .withCentre(at(0.82f, 0.82f)),
                      strokeWidth);
        break;
    }
    case GuiIcon::TimingTool:
    {
        g.drawLine({ at(0.22f, 0.14f), at(0.22f, 0.86f) }, strokeWidth);
        g.drawLine({ at(0.78f, 0.14f), at(0.78f, 0.86f) }, strokeWidth);
        g.drawLine({ at(0.22f, 0.50f), at(0.48f, 0.50f) }, strokeWidth);
        g.drawLine({ at(0.52f, 0.50f), at(0.78f, 0.50f) }, strokeWidth);
        g.fillEllipse(juce::Rectangle<float>(0.0f, 0.0f, 0.14f, 0.14f)
                          .withCentre(at(0.50f, 0.50f)));
        break;
    }
    case GuiIcon::Menu:
    {
        for (int i = 0; i < 3; ++i)
            g.fillEllipse(juce::Rectangle<float>(0.0f, 0.0f, 0.11f * w, 0.11f * w)
                              .withCentre(at(0.5f, 0.30f + 0.20f * static_cast<float>(i))));
        break;
    }
    case GuiIcon::Close:
        g.drawLine({ at(0.24f, 0.24f), at(0.76f, 0.76f) }, strokeWidth);
        g.drawLine({ at(0.76f, 0.24f), at(0.24f, 0.76f) }, strokeWidth);
        break;
    case GuiIcon::ChevronDown:
    {
        juce::Path p;
        p.startNewSubPath(at(0.28f, 0.40f));
        p.lineTo(at(0.50f, 0.62f));
        p.lineTo(at(0.72f, 0.40f));
        g.strokePath(p, stroke);
        break;
    }
    case GuiIcon::ChevronLeft:
    {
        juce::Path p;
        p.startNewSubPath(at(0.60f, 0.24f));
        p.lineTo(at(0.38f, 0.50f));
        p.lineTo(at(0.60f, 0.76f));
        g.strokePath(p, stroke);
        break;
    }
    case GuiIcon::ChevronRight:
    {
        juce::Path p;
        p.startNewSubPath(at(0.40f, 0.24f));
        p.lineTo(at(0.62f, 0.50f));
        p.lineTo(at(0.40f, 0.76f));
        g.strokePath(p, stroke);
        break;
    }
    case GuiIcon::Play:
    {
        juce::Path p;
        p.startNewSubPath(at(0.34f, 0.20f));
        p.lineTo(at(0.78f, 0.50f));
        p.lineTo(at(0.34f, 0.80f));
        p.closeSubPath();
        g.fillPath(p);
        break;
    }
    case GuiIcon::Stop:
        g.fillRect(juce::Rectangle<float>(box.getX() + 0.30f * w,
                                          box.getY() + 0.30f * h,
                                          0.40f * w, 0.40f * h));
        break;
    case GuiIcon::Loop:
    {
        juce::Path p;
        p.addRoundedRectangle(box.getX() + 0.18f * w, box.getY() + 0.24f * h,
                              0.64f * w, 0.52f * h, 0.12f * w);
        g.strokePath(p, stroke);
        juce::Path arrow;
        arrow.startNewSubPath(at(0.62f, 0.16f));
        arrow.lineTo(at(0.76f, 0.28f));
        arrow.lineTo(at(0.62f, 0.40f));
        g.strokePath(arrow, stroke);
        break;
    }
    case GuiIcon::Undo:
    {
        juce::Path p;
        p.startNewSubPath(at(0.30f, 0.38f));
        p.lineTo(at(0.18f, 0.50f));
        p.lineTo(at(0.30f, 0.62f));
        g.strokePath(p, stroke);
        juce::Path curve;
        curve.startNewSubPath(at(0.20f, 0.50f));
        curve.cubicTo(at(0.50f, 0.50f), at(0.72f, 0.42f), at(0.80f, 0.70f));
        g.strokePath(curve, stroke);
        break;
    }
    case GuiIcon::Redo:
    {
        juce::Path p;
        p.startNewSubPath(at(0.70f, 0.38f));
        p.lineTo(at(0.82f, 0.50f));
        p.lineTo(at(0.70f, 0.62f));
        g.strokePath(p, stroke);
        juce::Path curve;
        curve.startNewSubPath(at(0.80f, 0.50f));
        curve.cubicTo(at(0.50f, 0.50f), at(0.28f, 0.42f), at(0.20f, 0.70f));
        g.strokePath(curve, stroke);
        break;
    }
    case GuiIcon::Record:
        g.fillEllipse(juce::Rectangle<float>(0.0f, 0.0f, 0.46f * w, 0.46f * w)
                          .withCentre(at(0.5f, 0.5f)));
        break;
    case GuiIcon::Audition:
    {
        juce::Path p;
        p.startNewSubPath(at(0.16f, 0.50f));
        p.lineTo(at(0.30f, 0.50f));
        p.lineTo(at(0.38f, 0.22f));
        p.lineTo(at(0.48f, 0.78f));
        p.lineTo(at(0.58f, 0.34f));
        p.lineTo(at(0.66f, 0.62f));
        p.lineTo(at(0.76f, 0.50f));
        p.lineTo(at(0.86f, 0.50f));
        g.strokePath(p, stroke);
        break;
    }
    }
}
