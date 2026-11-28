// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../../JuceHeader.h"

class MainComponent;

// Opening a project/audio file: the Open dialog, the drop target and the recent
// files list. Moved out of MainComponent.cpp without changing its callbacks or
// what a drop does.
class FileDropController
{
public:
  explicit FileDropController(MainComponent &owner) : owner(owner) {}

  bool isInterestedInFileDrag(const juce::StringArray &files) const;
  void filesDropped(const juce::StringArray &files);
  void openFile();
  void openFileFromPath(const juce::File &file);
  void openRecentFile(const juce::File &file);
  void addRecentFile(const juce::File &file);
  void refreshRecentFilesMenu();
  void setRecentFiles(const juce::StringArray &files) { recentFiles = files; }

private:
  MainComponent &owner;
  juce::StringArray recentFiles;

  JUCE_DECLARE_NON_COPYABLE(FileDropController)
};
