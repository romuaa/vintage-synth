#include "common/presets/PresetFiles.h"

#include "common/presets/PresetModel.h"

#if JUCE_WINDOWS
 #include <filesystem>
 #include <system_error>
#else
 #include <cerrno>
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace vs::presets
{
namespace
{
juce::String errorFor (const juce::File& f, const juce::String& what) { return what + " (" + f.getFileName() + ")"; }
} // namespace

juce::String readPresetFile (const juce::File& file, size_t maxBytes, bool followSymlinks, std::string& out)
{
    out.clear();
    if (file == juce::File())
        return "No file was given.";

#if JUCE_WINDOWS
    // Windows: no FIFOs on the file system, but devices ("\\.\..."), UNC paths to other machines
    // and reparse points exist. The status is checked first, then the file is read through a
    // stream that never reads more than maxBytes + 1 bytes.
    std::error_code ec;
    const std::filesystem::path path (file.getFullPathName().toWideCharPointer());
    const auto linkStatus = std::filesystem::symlink_status (path, ec);
    if (ec || ! std::filesystem::exists (linkStatus))
        return errorFor (file, "The file does not exist");
    if (! followSymlinks && std::filesystem::is_symlink (linkStatus))
        return errorFor (file, "The file is a link: not read");
    const auto status = std::filesystem::status (path, ec);
    if (ec || ! std::filesystem::is_regular_file (status))
        return errorFor (file, "Not an ordinary file: not read");
    const auto size = std::filesystem::file_size (path, ec);
    if (ec)
        return errorFor (file, "The file could not be read");
    if (size == 0)
        return errorFor (file, "The file is empty");
    if (size > maxBytes)
        return errorFor (file, "The file is too large to be a preset");
    juce::FileInputStream in (file);
    if (! in.openedOk())
        return errorFor (file, "The file could not be read");
    // The buffer starts a little above the file's size and grows (at most to maxBytes + 1), so a small
    // file costs a small buffer whatever the cap is, and a file that grows while it is read still
    // ends at maxBytes + 1 bytes read.
    out.resize (std::min<size_t> (maxBytes + 1, static_cast<size_t> (size) + 4096u));
    size_t total = 0;
    while (total < maxBytes + 1)
    {
        if (total == out.size())
            out.resize (std::min<size_t> (maxBytes + 1, out.size() * 2u));
        const auto chunk = static_cast<int> (std::min<size_t> (out.size() - total, 65536u));
        const int got = in.read (out.data() + total, chunk);
        if (got <= 0)
            break;
        total += static_cast<size_t> (got);
    }
#else
    int flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC; // O_NONBLOCK: opening a FIFO without a writer returns at once
    if (! followSymlinks)
        flags |= O_NOFOLLOW;
    const int fd = ::open (file.getFullPathName().toRawUTF8(), flags);
    if (fd < 0)
        return errorFor (file, errno == ENOENT ? "The file does not exist"
                                               : (errno == ELOOP ? "The file is a link: not read" : "The file could not be opened"));
    struct Closer
    {
        int fd;
        ~Closer() { ::close (fd); }
    } closer { fd };

    struct stat st {};
    if (::fstat (fd, &st) != 0)
        return errorFor (file, "The file could not be read");
    if (! S_ISREG (st.st_mode))
        return errorFor (file, "Not an ordinary file: not read");
    if (st.st_size <= 0)
        return errorFor (file, "The file is empty");
    if (static_cast<unsigned long long> (st.st_size) > static_cast<unsigned long long> (maxBytes))
        return errorFor (file, "The file is too large to be a preset");
    // A regular file: reads do not block on O_NONBLOCK, and none reads more than is left of maxBytes + 1.
    // The buffer starts a little above the file's size and grows (at most to maxBytes + 1), so a small
    // file costs a small buffer whatever the cap is, and a file that grows while it is read still
    // ends at maxBytes + 1 bytes read.
    out.resize (std::min<size_t> (maxBytes + 1, static_cast<size_t> (st.st_size) + 4096u));
    size_t total = 0;
    while (total < maxBytes + 1)
    {
        if (total == out.size())
            out.resize (std::min<size_t> (maxBytes + 1, out.size() * 2u));
        const auto r = ::read (fd, out.data() + total, std::min<size_t> (out.size() - total, 65536u));
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
        {
            out.clear();
            return errorFor (file, "The file could not be read");
        }
        if (r == 0)
            break;
        total += static_cast<size_t> (r);
    }
#endif
    if (total > maxBytes)
    {
        out.clear();
        return errorFor (file, "The file is too large to be a preset");
    }
    if (total == 0)
    {
        out.clear();
        return errorFor (file, "The file is empty");
    }
    out.resize (total);
    return {};
}

bool isSafeBankFileName (const juce::String& name, const juce::String& extension)
{
    if (name.isEmpty() || name.length() > 128 || extension.isEmpty())
        return false;
    if (! name.endsWithIgnoreCase (extension) || name.length() <= extension.length())
        return false;
    if (name.contains ("..") || name.containsAnyOf ("/\\:<>\"|?*"))
        return false;
    for (auto c : name)
        if (isHiddenOrControlCharacter (c))
            return false;
    if (name.startsWithChar (' ') || name.endsWithChar (' ') || name.startsWithChar ('.') || name.endsWithChar ('.')
        || name.startsWithChar ('-'))
        return false;
    if (juce::File::isAbsolutePath (name))
        return false;
    // Windows device names, whatever follows the first dot ("NUL.vsbank" opens the null device).
    const auto stem = name.upToFirstOccurrenceOf (".", false, false).trimEnd().toUpperCase();
    static const char* const reserved[] = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };
    for (const auto* r : reserved)
        if (stem == r)
            return false;
    if (stem.length() == 4 && (stem.startsWith ("COM") || stem.startsWith ("LPT")) && juce::CharacterFunctions::isDigit (stem[3]))
        return false;
    return true;
}

juce::File resolveBankFile (const juce::File& banksFolder, const juce::String& name, const juce::String& extension)
{
    if (banksFolder == juce::File() || ! isSafeBankFileName (name, extension))
        return {};
    const auto f = banksFolder.getChildFile (name);
    if (f.getParentDirectory() != banksFolder || f.getFileName() != name)
        return {};
    if (f.isSymbolicLink())
        return {};
    return f;
}
} // namespace vs::presets
