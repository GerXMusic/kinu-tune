// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
// File opening (dialog, drop target, recent files), extracted from
// MainComponent.cpp without changing its callbacks or behaviour.

#include "FileDropController.h"

#include "../Components/StyledMessageBox.h"
#include "../MainComponent.h"
#include "../../Utils/Localization.h"

bool FileDropController::isInterestedInFileDrag(
    const juce::StringArray &files) const
{
  if (owner.isPluginMode())
    return false;

  for (const auto &file : files)
  {
    if (file.endsWithIgnoreCase(".pitchnet") || file.endsWithIgnoreCase(".wav") ||
        file.endsWithIgnoreCase(".mp3") ||
        file.endsWithIgnoreCase(".flac") || file.endsWithIgnoreCase(".aiff") ||
        file.endsWithIgnoreCase(".ogg") || file.endsWithIgnoreCase(".m4a"))
      return true;
  }
  return false;
}

void FileDropController::filesDropped(const juce::StringArray &files)
{
  if (files.isEmpty())
    return;

  openFileFromPath(juce::File(files[0]));
}

void FileDropController::openFileFromPath(const juce::File &file)
{
  if (owner.isPluginMode())
    return;

  if (!file.existsAsFile())
    return;

  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(&owner);
    juce::MessageManager::callAsync([safeThis, file]()
                                    {
      if (safeThis != nullptr)
        safeThis->openFileFromPath(file); });
    return;
  }

  if (file.hasFileExtension("pitchnet") || file.hasFileExtension(".pitchnet"))
    owner.openProjectFile(file);
  else
    owner.loadAudioFile(file);
}

void FileDropController::openFile()
{
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(&owner);
    juce::MessageManager::callAsync([safeThis]()
                                    {
      if (safeThis != nullptr)
        safeThis->openFile(); });
    return;
  }

  // Prevent re-triggering while dialog is open
  if (owner.fileChooser != nullptr)
    return;

  owner.fileChooser = std::make_unique<juce::FileChooser>(
      TR("dialog.select_audio"), juce::File{},
      "*.pitchnet;*.wav;*.mp3;*.flac;*.aiff");

  auto chooserFlags = juce::FileBrowserComponent::openMode |
                      juce::FileBrowserComponent::canSelectFiles;

  juce::Component::SafePointer<MainComponent> safeThis(&owner);
  owner.fileChooser->launchAsync(
      chooserFlags, [safeThis](const juce::FileChooser &fc)
      {
        if (safeThis == nullptr)
          return;

        auto file = fc.getResult();
        safeThis->fileChooser.reset(); // Allow next dialog to open
        if (file.existsAsFile()) {
          safeThis->addRecentFile(file);
          if (file.hasFileExtension("pitchnet") || file.hasFileExtension(".pitchnet"))
            safeThis->openProjectFile(file);
          else
            safeThis->loadAudioFile(file);
        } });
}

void FileDropController::refreshRecentFilesMenu()
{
  if (owner.menuHandler)
    owner.menuHandler->setRecentFiles(recentFiles);
  if (owner.menuHandler)
    owner.menuHandler->menuItemsChanged();
}

void FileDropController::addRecentFile(const juce::File &file)
{
  if (!file.existsAsFile())
    return;

  const auto fullPath = file.getFullPathName();
  recentFiles.removeString(fullPath);
  recentFiles.insert(0, fullPath);

  constexpr int kMaxRecentFiles = 10;
  while (recentFiles.size() > kMaxRecentFiles)
    recentFiles.remove(recentFiles.size() - 1);

  if (owner.settingsManager)
  {
    owner.settingsManager->setLastFilePath(file);
    owner.settingsManager->setRecentFiles(recentFiles);
    owner.settingsManager->saveConfig();
  }
  refreshRecentFilesMenu();
}

void FileDropController::openRecentFile(const juce::File &file)
{
  if (!file.existsAsFile())
  {
    StyledMessageBox::show(&owner, TR("dialog.recent_missing"),
                           TR("dialog.file_not_found") + "\n" +
                               file.getFullPathName(),
                           StyledMessageBox::WarningIcon);
    recentFiles.removeString(file.getFullPathName());
    if (owner.settingsManager)
    {
      owner.settingsManager->setRecentFiles(recentFiles);
      owner.settingsManager->saveConfig();
    }
    refreshRecentFilesMenu();
    return;
  }

  if (file.hasFileExtension("pitchnet") || file.hasFileExtension(".pitchnet"))
    owner.openProjectFile(file);
  else
    owner.loadAudioFile(file);
}
