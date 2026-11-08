// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Test of vncviewer/UiText (see run.sh).

#include "UiText.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>

HINSTANCE m_hInstResDLL = NULL; // the viewer's resource module; here: the exe

static int g_failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			             #cond);                                                 \
			++g_failures;                                                        \
		}                                                                        \
	} while (0)

int main()
{
	SetEnvironmentVariableW(L"UVNC_UI_LANG", L"en");
	CHECK(wcscmp(Tr(L"Upload  \u2192"), L"Upload  \u2192") == 0);
	CHECK(TrF(L"%u folders, %u files", 3u, 4u) == L"3 folders, 4 files");

	SetEnvironmentVariableW(L"UVNC_UI_LANG", L"zh");
	CHECK(std::string(UiLanguage()) == "zh");
	CHECK(wcscmp(Tr(L"Upload  \u2192"), L"\u4E0A\u4F20  \u2192") == 0);              // 上传  →
	CHECK(TrF(L"%u folders, %u files", 3u, 4u) == L"3 \u4E2A\u6587\u4EF6\u5939\uFF0C4 \u4E2A\u6587\u4EF6"); // 3 个文件夹，4 个文件
	CHECK(TrF(L"Cannot open %ls\n\n%ls", L"C:\\x", L"why") == L"\u65E0\u6CD5\u6253\u5F00 C:\\x\n\nwhy");
	CHECK(wcscmp(Tr(L"access denied"), L"\u62D2\u7EDD\u8BBF\u95EE") == 0);         // 拒绝访问
	CHECK(wcscmp(Tr(L"&Open\tEnter"), L"\u6253\u5F00(&O)\tEnter") == 0);
	CHECK(wcscmp(Tr(L"no such text"), L"no such text") == 0);                     // unknown: English

	SetEnvironmentVariableW(L"UVNC_UI_LANG", L"zhtw");
	CHECK(std::string(UiLanguage()) == "zhtw");
	CHECK(wcscmp(Tr(L"New folder"), L"\u65B0\u8CC7\u6599\u593E") == 0);            // 新資料夾
	CHECK(TrF(L"Delete \u201C%ls\u201D?", L"a.txt") == L"\u522A\u9664\u300Ca.txt\u300D\uFF1F"); // 刪除「a.txt」？

	// Without the override: the exe is the resource module (built-in
	// English), so the Windows display language decides.
	SetEnvironmentVariableW(L"UVNC_UI_LANG", NULL);
	std::printf("language with the Windows display language %04x: %s\n", GetUserDefaultUILanguage(), UiLanguage());

	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all uitext tests passed\n");
	return 0;
}
