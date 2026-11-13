#pragma once

// File access for presets: the one bounded reader every preset file read goes through, and the
// rules for the bank reference stored in a host session (untrusted: a session can come from
// anywhere). Message thread only (never the audio thread, never from setStateInformation).

#include <juce_core/juce_core.h>

#include <string>

namespace vs::presets
{
/** Reads a preset file into `out`. Only a regular file is read (not a FIFO, device, socket or
    directory), its size must be > 0 and <= maxBytes, and at most maxBytes + 1 bytes are ever read
    through a stream (a file that grows while being read is rejected, nothing is read "to the end").
    On POSIX the file is opened without blocking (a FIFO cannot stall the open) and checked on the
    open descriptor (no gap between the check and the read). With followSymlinks false a symbolic
    link is refused. Returns an empty string on success, else a message (and `out` is empty). */
juce::String readPresetFile (const juce::File& file, size_t maxBytes, bool followSymlinks, std::string& out);

/** Whether `name` may be used as the file name of a user bank stored in a session: a plain file
    name with the given extension, nothing that can leave the banks folder or name something other
    than an ordinary file there. Refused: empty or longer than 128 characters, absolute paths,
    "." or "..", anything containing "..", '/', '\\' or ':' (drives, UNC and URL-like prefixes),
    control characters (C0, DEL, C1), Unicode format characters (bidirectional controls and the
    like), a leading or trailing space or dot, a leading '-', the Windows reserved device names
    (CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or without an extension) and characters Windows does
    not allow in file names (<>"|?*). */
bool isSafeBankFileName (const juce::String& name, const juce::String& extension);

/** The file `name` names inside `banksFolder`, or juce::File() when the name is not safe
    (isSafeBankFileName) or the result is not a direct child of the folder, or is a symbolic link. */
juce::File resolveBankFile (const juce::File& banksFolder, const juce::String& name, const juce::String& extension);
} // namespace vs::presets
