#include "MenuHandler.h"

MenuHandler::MenuHandler() = default;

juce::StringArray MenuHandler::getMenuBarNames() {
#if JUCE_MAC
    if (pluginMode)
        return {TR("menu.edit"), TR("menu.options")};
    return {TR("menu.file"), TR("menu.edit"), TR("menu.options")};
#else
    if (pluginMode)
        return {TR("menu.edit"), TR("menu.options"), TR("menu.settings")};
    return {TR("menu.file"), TR("menu.edit"), TR("menu.options")};
#endif
}

juce::PopupMenu MenuHandler::getMenuForIndex(int menuIndex, const juce::String& /*menuName*/) {
    juce::PopupMenu menu;

    const auto addEditMenu = [this, &menu]()
    {
        if (!commandManager)
            return;

        menu.addCommandItem(commandManager, CommandIDs::undo);
        menu.addCommandItem(commandManager, CommandIDs::redo);
        menu.addSeparator();
        // Melodyne's clipboard commands are visible and inactive until the
        // audio clipboard exists; a disabled item is clearer than hiding them.
        menu.addCommandItem(commandManager, CommandIDs::cutNotes);
        menu.addCommandItem(commandManager, CommandIDs::copyNotes);
        menu.addCommandItem(commandManager, CommandIDs::pasteNotes);
        menu.addSeparator();
        menu.addCommandItem(commandManager, CommandIDs::deleteNotes);
        menu.addCommandItem(commandManager, CommandIDs::selectAll);
        menu.addSeparator();
        menu.addCommandItem(commandManager, CommandIDs::muteNotes);
        menu.addCommandItem(commandManager, CommandIDs::unmuteNotes);
        menu.addSeparator();
        menu.addCommandItem(commandManager, CommandIDs::correctPitch);
        menu.addCommandItem(commandManager, CommandIDs::quantizeTime);
        menu.addCommandItem(commandManager, CommandIDs::leveling);
        menu.addSeparator();

        juce::PopupMenu resetMenu;
        resetMenu.addCommandItem(commandManager, CommandIDs::resetPitch);
        resetMenu.addCommandItem(commandManager, CommandIDs::resetFormant);
        resetMenu.addCommandItem(commandManager, CommandIDs::resetAmplitude);
        resetMenu.addCommandItem(commandManager, CommandIDs::resetTiming);
        resetMenu.addSeparator();
        resetMenu.addCommandItem(commandManager, CommandIDs::resetAllEdits);
        menu.addSubMenu(TR("menu.reset"), resetMenu);

        menu.addSeparator();
        menu.addCommandItem(commandManager, CommandIDs::mergeNotes);
        menu.addSeparator();

        juce::PopupMenu toolsMenu;
        toolsMenu.addCommandItem(commandManager, CommandIDs::activateMainTool);
        toolsMenu.addCommandItem(commandManager, CommandIDs::activatePitchTool);
        toolsMenu.addCommandItem(commandManager, CommandIDs::activateFormantTool);
        toolsMenu.addCommandItem(commandManager, CommandIDs::activateAmplitudeTool);
        toolsMenu.addCommandItem(commandManager, CommandIDs::activateTimingTool);
        toolsMenu.addCommandItem(commandManager, CommandIDs::activateSeparationTool);
        menu.addSubMenu(TR("menu.tools"), toolsMenu);
    };

    const auto addOptionsMenu = [this, &menu]()
    {
        if (!commandManager)
            return;
        menu.addCommandItem(commandManager, CommandIDs::showDeltaPitch);
        menu.addCommandItem(commandManager, CommandIDs::showBasePitch);
        menu.addSeparator();
        menu.addCommandItem(commandManager, CommandIDs::zoomFull);
    };

    if (pluginMode) {
        if (menuIndex == 0) {
            addEditMenu();
        } else if (menuIndex == 1) {
            addOptionsMenu();
        }
#if !JUCE_MAC
        else if (menuIndex == 2) {
            if (commandManager)
                menu.addCommandItem(commandManager, CommandIDs::showSettings);
        }
#endif
    } else {
        if (menuIndex == 0) {
            // File menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::openFile);
                juce::PopupMenu recentMenu;
                if (recentFiles.isEmpty()) {
                    recentMenu.addItem(1, TR("menu.no_recent_files"), false, false);
                } else {
                    const int count = juce::jmin(kMaxRecentMenuItems, recentFiles.size());
                    for (int i = 0; i < count; ++i) {
                        juce::File file(recentFiles[i]);
                        const juce::String label =
                            juce::String(i + 1) + "  " + file.getFullPathName();
                        recentMenu.addItem(kRecentFileMenuBaseId + i, label);
                    }
                }
                menu.addSubMenu(TR("menu.recent_files"), recentMenu);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::saveProject);
                menu.addCommandItem(commandManager, CommandIDs::saveProjectAs);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::exportAudio);
                menu.addCommandItem(commandManager, CommandIDs::exportMidi);
#if !JUCE_MAC
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::showSettings);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::showAbout);
#endif
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::quit);
            }
        } else if (menuIndex == 1) {
            addEditMenu();
        } else if (menuIndex == 2) {
            addOptionsMenu();
        }
    }

    return menu;
}

#if JUCE_MAC
juce::PopupMenu MenuHandler::getMacExtraAppleMenu() const {
    juce::PopupMenu menu;

    if (commandManager) {
        menu.addCommandItem(commandManager, CommandIDs::showAbout);
        menu.addSeparator();
        menu.addCommandItem(commandManager, CommandIDs::showSettings);
    }

    return menu;
}
#endif

void MenuHandler::menuItemSelected(int menuItemID, int /*topLevelMenuIndex*/) {
    // Command items are handled automatically by ApplicationCommandManager
    if (menuItemID >= kRecentFileMenuBaseId &&
        menuItemID < kRecentFileMenuBaseId + kMaxRecentMenuItems) {
        const int idx = menuItemID - kRecentFileMenuBaseId;
        if (idx >= 0 && idx < recentFiles.size() && onRecentFileSelected) {
            onRecentFileSelected(juce::File(recentFiles[idx]));
        }
        return;
    }
}
