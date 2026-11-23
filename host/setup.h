// Setup for the Asura's Wrath recomp: finds the player's own copies of the game, checks each against the known SHA-256, and asks for what is missing.
//
// The files (all from title 43430817, see kSetupFiles in setup.cpp):
//   disc image (ISO)            required   the game's data and its executable (default.xex)
//   title update (TU 1)         required   default.xexp: the recompiled code was made from the executable with this update applied
//   Episode Pack: Part IV       strongly recommended: episodes 19-22, the game's real ending
//   Episode 11.5, Episode 15.5, Lost Episode 1, Lost Episode 2: optional
//
// What it found is written to game\sources.ini (path, size, time and SHA-256 of each verified file), which also saves hashing the 8.7 GB disc image
// again on the next run. Verified DLC is unpacked into game\dlc\<content id>\ (+ <content id>.name), where the game finds it. Unpacking the disc
// and the update into game\ and private\ is the next step and is not done here yet (NOTES.md, "Setup").
//
//   asura_setup.exe                     the interactive setup (dialogs). Once the disc and the update are verified (game\sources.ini), running it
//                                       again opens the DLC screen instead: add package files (several at once, any names) or search a folder
//   asura_setup.exe --full              the whole setup again
//   asura_setup.exe --add-dlc           straight to the DLC screen
//   asura_setup.exe --add <file>...     no dialogs: identify each file by size and SHA-256, unpack verified DLC into game\dlc, print the result
//                                       (exit code 1 if a file was not recognised)
//   asura_setup.exe --check [--search <folder>]... [--drives] [--report <file>] [--root <folder>]
//                                       no dialogs: look in the usual places (and the given folders; --drives: every fixed drive, 6 levels deep),
//                                       verify, print what was found; exit code 0 when
//                                       every required file is there and verified, 1 otherwise
//   asura_setup.exe --hash <file>       print a file's SHA-256 and size (for the table)
#pragma once
