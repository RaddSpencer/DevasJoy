// Setup for the Asura's Wrath recomp: finds the player's own copies of the game, checks each against the known SHA-256, and asks for what is missing.
//
// The files (all from title 43430817, see kSetupFiles in setup.cpp):
//   disc image (ISO)            required   the game's data and its executable (default.xex)
//   title update (TU 1)         required   default.xexp: the recompiled code was made from the executable with this update applied
//   Episode Pack: Part IV       strongly recommended: episodes 19-22, the game's real ending
//   Episode 11.5, Episode 15.5, Lost Episode 1, Lost Episode 2: optional
//
// What it found is written to game\sources.ini (path, size, time and SHA-256 of each verified file), which also saves hashing the 8.7 GB disc image
// again on the next run. Then it unpacks (setup.cpp unpack_game), skipping anything already in place and correct:
//   game\disc\ (the disc's files without $SystemUpdate\ and default.xex), game\update\ (empty), private\default.xex, private\default.xexp,
//   private\default_patched.xex (the update applied with XenonRecomp's patcher, thirdparty\xexpatch) and private\image.bin,
// each private file checked against the hash of the one the recompiled code was made from; and every verified DLC into game\dlc\<content id>\
// (+ <content id>.name).
//
//   asura_setup.exe                     the setup window: a row per file (path box, Browse..., drag and drop, a green/red status), OK unpacks.
//                                       OK stays greyed out until the disc image and the title update are verified or already installed.
//                                       Run it again at any time to add DLC: what is installed shows green.
//   asura_setup.exe --unpack            no dialogs: find and verify (as --check), then unpack the game and the verified DLC
//   asura_setup.exe --add <file>...     no dialogs: identify each file by size and SHA-256, unpack verified DLC into game\dlc, print the result
//                                       (exit code 1 if a file was not recognised)
//   asura_setup.exe --check [--search <folder>]... [--drives] [--report <file>] [--root <folder>]
//                                       no dialogs: look in the usual places (and the given folders; --drives: every fixed drive, 6 levels deep),
//                                       verify, print what was found; exit code 0 when
//                                       every required file is there and verified, 1 otherwise
//   asura_setup.exe --hash <file>       print a file's SHA-256 and size (for the table)
#pragma once
