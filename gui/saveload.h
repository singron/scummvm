/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef GUI_SAVELOAD_DIALOG_H
#define GUI_SAVELOAD_DIALOG_H

#include "gui/dialog.h"
#include "engines/metaengine.h"

namespace GUI {

class ListWidget;
class GraphicsWidget;
class ButtonWidget;
class CommandSender;
class ContainerWidget;
class StaticTextWidget;

class SaveLoadChooser : GUI::Dialog {
	typedef Common::String String;
	typedef Common::Array<Common::String> StringArray;
protected:
	GUI::ListWidget		*_list;
	GUI::ButtonWidget	*_chooseButton;
	GUI::ButtonWidget	*_deleteButton;
	GUI::GraphicsWidget	*_gfxWidget;
	GUI::ContainerWidget	*_container;
	GUI::StaticTextWidget	*_date;
	GUI::StaticTextWidget	*_time;
	GUI::StaticTextWidget	*_playtime;

	const EnginePlugin		*_plugin;
	bool					_delSupport;
	bool					_metaInfoSupport;
	bool					_thumbnailSupport;
	bool					_saveDateSupport;
	bool					_playTimeSupport;
	String					_target;
	SaveStateList			_saveList;
	String					_resultString;

	uint8 _fillR, _fillG, _fillB;

	void updateSaveList();
	void updateSelection(bool redraw);
public:
	SaveLoadChooser(const String &title, const String &buttonLabel, bool saveMode);
	~SaveLoadChooser();

	virtual void handleCommand(GUI::CommandSender *sender, uint32 cmd, uint32 data);

	/**
	 * Runs the save/load chooser with the currently active config manager
	 * domain as target.
	 *
	 * @return The selcted save slot. -1 in case none is selected.
	 */
	int runModalWithCurrentTarget();
	int runModalWithPluginAndTarget(const EnginePlugin *plugin, const String &target);
	void open();

	const Common::String &getResultString() const;

	/**
	 * Creates a default save description for the specified slot. Depending
	 * on the ScummVM configuration this might be a simple "Slot #" description
	 * or the current date and time.
	 *
	 * TODO: This might not be the best place to put this, since engines not
	 * using this class might want to mimic the same behavior. Check whether
	 * moving this to a better place makes sense and find what this place would
	 * be.
	 *
	 * @param slot The slot number (must be >= 0).
	 * @return The slot description.
	 */
	Common::String createDefaultSaveDescription(const int slot) const;

	virtual void reflowLayout();

	virtual void close();
};

} // End of namespace GUI

#endif
