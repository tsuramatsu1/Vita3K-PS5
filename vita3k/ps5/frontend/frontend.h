// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct EmuEnvState;

namespace ps5::frontend {

// What an installation reports as it goes, from the thread it runs on
class InstallProgress {
public:
    virtual ~InstallProgress() = default;
    // Item index of count starts: a firmware package or an app archive
    virtual void begin(const std::string &name, int index, int count) = 0;
    virtual void progress(float fraction) = 0;
    // The file being written now and how much of it is done, for the list of what has gone by
    virtual void detail(const std::string &item, float fraction) = 0;
    virtual void finished(const std::string &message, bool succeeded) = 0;
    // Whether the player has asked for this installation to stop, polled as it runs
    virtual bool cancelled() const = 0;
};

// One line of the installer's file browser: a folder to open, or a file
struct Entry {
    std::string name;
    std::string path;
    bool directory = false;
    // A firmware package or an app the installer can take (.pup, .vpk, .zip)
    bool installable = false;
    std::uint64_t bytes = 0;
};

// Somewhere a game can be installed to: the console's own storage, or a USB drive
struct Destination {
    std::string name;
    std::string path;
};

using Installer = std::function<void(const Entry &file, const Destination &destination, InstallProgress &progress)>;
using DestinationLister = std::function<std::vector<Destination>()>;
// What a folder holds, folders first. An empty path asks for the places to start from: the install folder and the
// USB drives
using DirectoryLister = std::function<std::vector<Entry>(const std::string &path)>;
// Remembers a folder to look in for games, as well as the ones always looked in
using GamesFolderSetter = std::function<void(const std::string &path)>;
// Removes an installed game's files. False when they could not all be removed
using GameRemover = std::function<bool(const std::string &title_id)>;

// One setting the player can move through a fixed set of choices, since a console has no text entry
struct Setting {
    std::string name;
    std::string explanation;
    std::vector<std::string> choices;
    int choice = 0;
};

struct SettingsAccess {
    std::function<std::vector<Setting>()> read;
    std::function<void(const Setting &setting)> write;
};

struct Game {
    std::string title;
    std::string title_id;
    std::string icon_path;
    // Its files are still encrypted, so it cannot be read or run until it is installed with its licence
    bool sealed = false;
};

// The front end: the library of installed games, from which the player picks one to play or opens the installer.
// Returns the title ID picked, or nothing when the player quits
std::string run(const Installer &install, const DirectoryLister &list_directory, const DestinationLister &list_destinations,
    const GamesFolderSetter &set_games_folder, const GameRemover &remove_game, const SettingsAccess &settings,
    bool show_splash, const std::function<std::vector<Game>()> &list_games);

} // namespace ps5::frontend
