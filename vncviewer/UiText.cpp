// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "UiText.h"

#include <windows.h>

#include <cstdarg>
#include <cwchar>
#include <unordered_map>

extern HINSTANCE m_hInstResDLL; // the viewer's resource module (language DLL or the exe)

namespace {

enum Lang { kEnglish, kSimplified, kTraditional };

struct Entry {
	const wchar_t* en;
	const wchar_t* zh;
	const wchar_t* zhtw;
};

// English, Simplified Chinese, Traditional Chinese.
const Entry kTexts[] = {
#include "UiTextTable.inc"
};

Lang WindowsLanguage()
{
	const LANGID id = GetUserDefaultUILanguage();
	if (PRIMARYLANGID(id) != LANG_CHINESE)
		return kEnglish;
	switch (SUBLANGID(id)) {
	case SUBLANG_CHINESE_TRADITIONAL:
	case SUBLANG_CHINESE_HONGKONG:
	case SUBLANG_CHINESE_MACAU:
		return kTraditional;
	default:
		return kSimplified;
	}
}

Lang CurrentLanguage()
{
	// Test hook: UVNC_UI_LANG=en|zh|zhtw.
	wchar_t forced[16] = {};
	if (GetEnvironmentVariableW(L"UVNC_UI_LANG", forced, 16)) {
		if (_wcsicmp(forced, L"zh") == 0)
			return kSimplified;
		if (_wcsicmp(forced, L"zhtw") == 0)
			return kTraditional;
		return kEnglish;
	}
	// The language DLL the viewer loaded (it can change at run time).
	static HINSTANCE cachedModule = reinterpret_cast<HINSTANCE>(-1);
	static Lang cached = kEnglish;
	HINSTANCE module = m_hInstResDLL;
	if (module == cachedModule)
		return cached;
	cachedModule = module;
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(module, path, MAX_PATH);
	const wchar_t* file = wcsrchr(path, L'\\');
	file = file ? file + 1 : path;
	if (_wcsnicmp(file, L"vnclang_zhtw", 12) == 0)
		cached = kTraditional;
	else if (_wcsnicmp(file, L"vnclang_zh", 10) == 0)
		cached = kSimplified;
	else if (_wcsnicmp(file, L"vnclang_", 8) == 0)
		cached = kEnglish; // another language: no translation of our texts
	else
		cached = WindowsLanguage(); // built-in English
	return cached;
}

} // namespace

const wchar_t* Tr(const wchar_t* english)
{
	const Lang lang = CurrentLanguage();
	if (lang == kEnglish || !english)
		return english;
	static const std::unordered_map<std::wstring, const Entry*> index = [] {
		std::unordered_map<std::wstring, const Entry*> m;
		for (const Entry& e : kTexts)
			m.emplace(e.en, &e);
		return m;
	}();
	auto it = index.find(english);
	if (it == index.end())
		return english;
	const wchar_t* t = lang == kTraditional ? it->second->zhtw : it->second->zh;
	return t && *t ? t : english;
}

std::wstring TrF(const wchar_t* englishFormat, ...)
{
	wchar_t buf[2048];
	va_list args;
	va_start(args, englishFormat);
	vswprintf(buf, 2048, Tr(englishFormat), args);
	va_end(args);
	return buf;
}

const char* UiLanguage()
{
	switch (CurrentLanguage()) {
	case kSimplified: return "zh";
	case kTraditional: return "zhtw";
	default: return "en";
	}
}
