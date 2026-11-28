// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
// Audio and MIDI export, extracted from MainComponent.cpp without changing the
// dialogs, the settings or the output.

#include "ExportController.h"

#include "../MainComponent.h"
#include "../Components/StyledMessageBox.h"
#include "ExportHelper.h"
#include "../../Audio/IO/MidiExporter.h"
#include "../../Utils/Localization.h"

#include <thread>

void ExportController::exportFile()
{
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(&owner);
    juce::MessageManager::callAsync([safeThis]()
                                    {
      if (safeThis != nullptr)
        safeThis->exportFile(); });
    return;
  }

  auto *project = owner.getProject();
  if (!project)
    return;

  // Prevent re-triggering while dialog is open
  if (owner.fileChooser != nullptr)
    return;

  const int inputSampleRate = project->getAudioData().sampleRate;
  juce::Component::SafePointer<MainComponent> safeThis(&owner);
  ExportHelper::showExportSettingsDialogAsync(
      &owner, inputSampleRate, owner.settingsManager->getExportSettings(),
      [safeThis](std::optional<ExportHelper::ExportSettings> exportSettings)
      {
        if (safeThis == nullptr || !exportSettings.has_value())
          return;

        safeThis->settingsManager->setExportSettings(*exportSettings);
        safeThis->settingsManager->saveConfig();

        auto performExport = [safeThis, exportSettings](const juce::File &targetFile)
        {
          if (safeThis == nullptr)
            return;

          auto *activeProject = safeThis->getProject();
          if (!activeProject)
            return;

          auto file = targetFile;
          const auto extension = ExportHelper::getFormatExtension(exportSettings->format);
          if (file.getFileExtension().isEmpty())
            file = file.withFileExtension(extension);

          auto &audioData = activeProject->getAudioData();
          juce::AudioBuffer<float> sourceBuffer;
          sourceBuffer.makeCopyOf(audioData.waveform);
          const int sourceRate = audioData.sampleRate;

          safeThis->toolbar.showProgress(TR("progress.exporting_audio"));
          safeThis->toolbar.setProgress(-1.0f);
          safeThis->toolbar.setEnabled(false);

          std::thread([safeThis, settings = *exportSettings, file = std::move(file),
                       sourceBuffer = std::move(sourceBuffer), sourceRate]() mutable
                      {
            juce::String error;
            bool success = false;

            do {
              juce::AudioBuffer<float> exportBuffer =
                  ExportHelper::convertChannels(sourceBuffer, settings.channels);
              exportBuffer = ExportHelper::resampleAudio(exportBuffer, sourceRate, settings.sampleRate);

              if (file.existsAsFile() && !file.deleteFile()) {
                error = TR("dialog.failed_delete") + "\n" + file.getFullPathName();
                break;
              }

              auto fileStream = std::make_unique<juce::FileOutputStream>(file);
              if (!fileStream || !fileStream->openedOk()) {
                error = TR("dialog.failed_open") + "\n" + file.getFullPathName();
                break;
              }
              std::unique_ptr<juce::OutputStream> outputStream = std::move(fileStream);

              juce::AudioFormatManager formatManager;
              formatManager.registerBasicFormats();
              auto *format = ExportHelper::findFormatForExtension(
                  formatManager, ExportHelper::getFormatExtension(settings.format));
              if (!format) {
                error = TR("error.no_exporter") + " " +
                        ExportHelper::getFormatDisplayName(settings.format);
                break;
              }

              auto writerOptions = juce::AudioFormatWriterOptions{}
                                       .withSampleRate(settings.sampleRate)
                                       .withNumChannels(exportBuffer.getNumChannels())
                                       .withBitsPerSample(settings.bitsPerSample);
              constexpr int ogg192KbpsQualityOption = 6;
              if (settings.format == ExportHelper::ExportFormat::ogg)
                writerOptions = writerOptions.withQualityOptionIndex(
                    ogg192KbpsQualityOption);

              auto writer = format->createWriterFor(outputStream, writerOptions);
              if (!writer) {
                error = TR("dialog.failed_create_writer") + "\n" +
                        file.getFullPathName();
                break;
              }

              success = writer->writeFromAudioSampleBuffer(
                  exportBuffer, 0, exportBuffer.getNumSamples());
              writer->flush();
              writer.reset();
              if (!success) {
                error = TR("dialog.failed_write") + "\n" + file.getFullPathName();
                break;
              }
            } while (false);

            juce::MessageManager::callAsync([safeThis, success, error]() {
              if (safeThis == nullptr)
                return;
              safeThis->toolbar.setEnabled(true);
              safeThis->toolbar.hideProgress();
              if (!success) {
                StyledMessageBox::show(
                    safeThis.getComponent(), TR("dialog.export_failed"), error,
                    StyledMessageBox::WarningIcon);
              }
            }); })
              .detach();
        };

        if (safeThis->fileChooser != nullptr)
          return;

        juce::File defaultFile;
        if (!safeThis->isPluginMode())
        {
          if (auto *activeProject = safeThis->getProject())
          {
            if (activeProject->getFilePath().existsAsFile())
              defaultFile = activeProject->getFilePath();
            else if (activeProject->getProjectFilePath().existsAsFile())
              defaultFile = activeProject->getProjectFilePath();

            // Save dialogs need a full file path to initialise both the folder
            // and filename consistently across native platform backends.
            if (defaultFile != juce::File{})
              defaultFile = defaultFile.withFileExtension(
                  ExportHelper::getFormatExtension(exportSettings->format));
          }
        }

        safeThis->fileChooser = std::make_unique<juce::FileChooser>(
            TR("dialog.save_audio"), defaultFile,
            ExportHelper::getFormatWildcard(exportSettings->format));

        auto chooserFlags = juce::FileBrowserComponent::saveMode |
                            juce::FileBrowserComponent::canSelectFiles |
                            juce::FileBrowserComponent::warnAboutOverwriting;

        safeThis->fileChooser->launchAsync(
            chooserFlags, [safeThis, performExport](const juce::FileChooser &fc)
            {
              if (safeThis == nullptr)
                return;
              const auto file = fc.getResult();
              safeThis->fileChooser.reset();
              if (file == juce::File{})
                return;
              performExport(file); });
      });
}

void ExportController::exportMidiFile()
{
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(&owner);
    juce::MessageManager::callAsync([safeThis]()
                                    {
      if (safeThis != nullptr)
        safeThis->exportMidiFile(); });
    return;
  }

  auto *project = owner.getProject();
  if (!project)
    return;

  // Prevent re-triggering while dialog is open
  if (owner.fileChooser != nullptr)
    return;

  const auto &notes = project->getNotes();
  if (notes.empty())
  {
    StyledMessageBox::show(&owner, TR("dialog.export_failed"),
                           TR("dialog.no_notes_to_export"),
                           StyledMessageBox::WarningIcon);
    return;
  }

  // Suggest filename based on project or audio file
  juce::File defaultFile;
  if (project->getFilePath().existsAsFile())
  {
    defaultFile = project->getFilePath().withFileExtension("mid");
  }
  else if (project->getProjectFilePath().existsAsFile())
  {
    defaultFile = project->getProjectFilePath().withFileExtension("mid");
  }

#if JUCE_WINDOWS && JUCE_MODAL_LOOPS_PERMITTED
  juce::FileChooser chooser(TR("dialog.export_midi"), defaultFile,
                            "*.mid;*.midi", true, false, &owner);
  if (!chooser.browseForFileToSave(true))
    return;

  auto file = chooser.getResult();
  if (file == juce::File{})
    return;

  // Ensure .mid extension
  if (file.getFileExtension().isEmpty())
    file = file.withFileExtension("mid");

  // Show progress
  owner.toolbar.showProgress(TR("progress.exporting_midi"));
  owner.toolbar.setProgress(0.3f);

  // Configure export options
  MidiExporter::ExportOptions options;
  options.tempo = static_cast<float>(project->getTimelineTempoBpm());
  options.ticksPerQuarterNote = 480;
  options.velocity = 100;
  options.quantizePitch = true; // Round to nearest semitone

  owner.toolbar.setProgress(0.6f);

  // Export using adjusted pitch data (includes user edits)
  bool success = MidiExporter::exportToFile(project->getNotes(), file, options);

  owner.toolbar.setProgress(1.0f);
  owner.toolbar.hideProgress();

  if (!success)
  {
    StyledMessageBox::show(
        &owner, TR("dialog.export_failed"),
        TR("dialog.failed_write_midi") + "\n" + file.getFullPathName(),
        StyledMessageBox::WarningIcon);
  }
#else
  owner.fileChooser = std::make_unique<juce::FileChooser>(
      TR("dialog.export_midi"), defaultFile, "*.mid;*.midi");

  auto chooserFlags = juce::FileBrowserComponent::saveMode |
                      juce::FileBrowserComponent::canSelectFiles |
                      juce::FileBrowserComponent::warnAboutOverwriting;

  juce::Component::SafePointer<MainComponent> safeThis(&owner);
  owner.fileChooser->launchAsync(
      chooserFlags, [safeThis](const juce::FileChooser &fc)
      {
        if (safeThis == nullptr)
          return;

        auto file = fc.getResult();
        safeThis->fileChooser
            .reset(); // Allow next dialog to open (must be after getResult)

        if (file == juce::File{})
          return;

        // Ensure .mid extension
        if (file.getFileExtension().isEmpty())
          file = file.withFileExtension("mid");

        // Show progress
        safeThis->toolbar.showProgress(TR("progress.exporting_midi"));
        safeThis->toolbar.setProgress(0.3f);

        auto *project = safeThis ? safeThis->getProject() : nullptr;
        if (!project)
          return;

        // Configure export options
        MidiExporter::ExportOptions options;
        options.tempo = static_cast<float>(project->getTimelineTempoBpm());
        options.ticksPerQuarterNote = 480;
        options.velocity = 100;
        options.quantizePitch = true; // Round to nearest semitone

        safeThis->toolbar.setProgress(0.6f);

        // Export using adjusted pitch data (includes user edits)
        bool success =
            MidiExporter::exportToFile(project->getNotes(), file, options);

        safeThis->toolbar.setProgress(1.0f);
        safeThis->toolbar.hideProgress();

        if (!success) {
          StyledMessageBox::show(
              safeThis.getComponent(), TR("dialog.export_failed"),
              TR("dialog.failed_write_midi") + "\n" + file.getFullPathName(),
              StyledMessageBox::WarningIcon);
        } });
#endif
}
