// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Test of vncviewer/VideoCursor on Windows or Wine (see run.sh).

#include "../../vncviewer/VideoCursor.h"

#include <cstdio>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			             #cond);                                                 \
			++g_failures;                                                        \
		}                                                                        \
	} while (0)

// Draw the cursor on a white 64x64 DIB and return the pixels (BGRA).
static std::vector<uint32_t> Render(HCURSOR cur)
{
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = 64;
	bi.bmiHeader.biHeight = -64;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	void* bits = nullptr;
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	HGDIOBJ old = SelectObject(dc, bmp);
	for (int i = 0; i < 64 * 64; ++i)
		static_cast<uint32_t*>(bits)[i] = 0x00FFFFFF;
	DrawIconEx(dc, 0, 0, cur, 0, 0, 0, NULL, DI_NORMAL);
	GdiFlush();
	std::vector<uint32_t> px(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + 64 * 64);
	SelectObject(dc, old);
	DeleteObject(bmp);
	DeleteDC(dc);
	return px;
}

int main()
{
	// Hidden pointer: no cursor.
	video::Cursor hidden;
	CHECK(CreateCursorFromShape(hidden) == NULL);

	// Colour: 24x20, left half opaque red, right half transparent.
	video::Cursor c;
	c.type = video::kCursorColor;
	c.width = 24;
	c.height = 20;
	c.hotX = 3;
	c.hotY = 4;
	c.pitch = 24 * 4 + 8; // padded rows must be handled
	c.data.assign(size_t(c.pitch) * c.height, 0);
	for (int y = 0; y < c.height; ++y)
		for (int x = 0; x < 12; ++x) {
			uint8_t* p = &c.data[size_t(y) * c.pitch + x * 4];
			p[0] = 0; p[1] = 0; p[2] = 0xFF; p[3] = 0xFF; // B G R A
		}
	HCURSOR cc = CreateCursorFromShape(c);
	CHECK(cc != NULL);
	if (cc) {
		ICONINFO ii = {};
		CHECK(GetIconInfo(cc, &ii));
		CHECK(!ii.fIcon && ii.xHotspot == 3 && ii.yHotspot == 4);
		CHECK(ii.hbmColor != NULL);
		BITMAP b = {};
		if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(b), &b))
			CHECK(b.bmWidth == 24 && b.bmHeight == 20);
		if (ii.hbmMask) DeleteObject(ii.hbmMask);
		if (ii.hbmColor) DeleteObject(ii.hbmColor);
		auto px = Render(cc);
		std::printf("colour cursor: (5,5)=%08x (20,5)=%08x\n", px[5 * 64 + 5], px[5 * 64 + 20]);
		CHECK((px[5 * 64 + 5] & 0xFFFFFF) == 0xFF0000);  // red where opaque
		CHECK((px[5 * 64 + 20] & 0xFFFFFF) == 0xFFFFFF); // background where transparent
		DestroyCursor(cc);
	}

	// Monochrome 32x32: AND all 1 (transparent) except a black 8x8 square
	// (AND 0, XOR 0) at the top left, and an inverting 8x8 square (AND 1,
	// XOR 1) next to it.
	video::Cursor m;
	m.type = video::kCursorMonochrome;
	m.width = 32;
	m.height = 32;
	m.hotX = 16;
	m.hotY = 16;
	m.pitch = 4;
	m.data.assign(4 * 64, 0);
	for (int y = 0; y < 32; ++y) {
		uint8_t* andRow = &m.data[y * 4];
		uint8_t* xorRow = &m.data[(32 + y) * 4];
		andRow[0] = andRow[1] = andRow[2] = andRow[3] = 0xFF;
		if (y < 8) {
			andRow[0] = 0x00;  // x 0..7: black
			xorRow[1] = 0xFF;  // x 8..15: AND 1, XOR 1 -> inverted
		}
	}
	HCURSOR mc = CreateCursorFromShape(m);
	CHECK(mc != NULL);
	if (mc) {
		ICONINFO ii = {};
		CHECK(GetIconInfo(mc, &ii));
		CHECK(ii.xHotspot == 16 && ii.yHotspot == 16 && ii.hbmColor == NULL);
		BITMAP b = {};
		if (ii.hbmMask && GetObject(ii.hbmMask, sizeof(b), &b))
			CHECK(b.bmWidth == 32 && b.bmHeight == 64);
		if (ii.hbmMask) DeleteObject(ii.hbmMask);
		auto px = Render(mc);
		std::printf("mono cursor: (2,2)=%08x (10,2)=%08x (20,20)=%08x\n", px[2 * 64 + 2], px[2 * 64 + 10],
			px[20 * 64 + 20]);
		CHECK((px[2 * 64 + 2] & 0xFFFFFF) == 0x000000);   // black
		CHECK((px[2 * 64 + 10] & 0xFFFFFF) == 0x000000);  // white inverted -> black
		CHECK((px[20 * 64 + 20] & 0xFFFFFF) == 0xFFFFFF); // transparent
		DestroyCursor(mc);
	}

	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all video_cursor tests passed\n");
	return 0;
}
