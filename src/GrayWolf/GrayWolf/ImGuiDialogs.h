#pragma once

// Small helpers the shell's windows share: ImGui text-buffer chores (the file dialogs are UiAsync's).

#include <string>

// Writes Value into a fixed ImGui text buffer, truncating rather than overflowing.
void SetBuf(char* Buf, size_t Size, const std::string& Value);

// A folder as the user should see it: a relative one is resolved from the exe's folder, which is
// where the app writes it (not the working directory, which depends on how it was launched).
std::string AbsoluteFolder(const std::string& Folder);

// A path as typed or pasted: without the spaces around it and the quotes "Copy as path" adds.
std::string TrimPath(const std::string& Typed);

// Case-insensitive substring search, for the name-filter boxes.
bool ContainsNoCase(const std::string& Haystack, const char* Needle);
