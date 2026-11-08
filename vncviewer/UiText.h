// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Translations of the texts added with the custom features (file transfer
// window, video / window / audio menus and messages, media settings). The
// English text is the key: Tr(L"Upload") returns the text in the user's
// language, or the English text if there is no translation.
//
// Language: the viewer's language (its vnclang_*.dll); when the viewer runs
// in its built-in English, the Windows display language. Simplified and
// Traditional Chinese are translated; other languages show English.

#pragma once

#include <string>

const wchar_t* Tr(const wchar_t* english);

// printf-style formatting of a translated text: TrF(L"%d items", n).
std::wstring TrF(const wchar_t* englishFormat, ...);

// "en", "zh" or "zhtw" (for tests and logs).
const char* UiLanguage();
