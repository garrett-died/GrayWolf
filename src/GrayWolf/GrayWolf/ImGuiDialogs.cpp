#include "stdafx.h"

#include "ImGuiDialogs.h"
#include "FileSystems.h"
#include "Strings.h"

#include <algorithm>
#include <cstring>
#include <filesystem>


void SetBuf(char* Buf, size_t Size, const std::string& Value)
{
    strncpy_s(Buf, Size, Value.c_str(), _TRUNCATE);
}

std::string AbsoluteFolder(const std::string& Folder)
{
    std::filesystem::path P(Folder);
    if (!Folder.empty() && P.is_relative()) P = std::filesystem::path(FileSystems::GetApplicationPath()) / P;
    return P.lexically_normal().string();
}

std::string TrimPath(const std::string& Typed)
{
    const auto Blank = [](char C) { return C == ' ' || C == '\t' || C == '\r' || C == '\n'; };
    std::string S = Typed;
    for (;;)
    {
        const size_t Before = S.size();
        while (!S.empty() && Blank(S.back())) S.pop_back();
        size_t From = 0;
        while (From < S.size() && Blank(S[From])) From++;
        S.erase(0, From);
        if (S.size() >= 2 && S.front() == '"' && S.back() == '"') { S.erase(S.size() - 1); S.erase(0, 1); }
        else if (!S.empty() && S.front() == '"' && S.find('"', 1) == std::string::npos) S.erase(0, 1);   // a lone quote from a half paste
        else if (!S.empty() && S.back() == '"' && S.find('"') == S.size() - 1) S.pop_back();
        if (S.size() == Before) return S;
    }
}

bool ContainsNoCase(const std::string& Haystack, const char* Needle)
{
    if (Needle == nullptr || *Needle == 0) return true;
    const auto It = std::search(Haystack.begin(), Haystack.end(), Needle, Needle + strlen(Needle),
        [](char A, char B) { return tolower((unsigned char)A) == tolower((unsigned char)B); });
    return It != Haystack.end();
}
