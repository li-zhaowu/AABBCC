// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "VideoCursor.h"

#include <algorithm>
#include <cstring>
#include <vector>

HCURSOR CreateCursorFromShape(const video::Cursor& c)
{
	HCURSOR cursor = NULL;
	if (c.type == video::kCursorMonochrome) {
		// AND mask over XOR mask, as CreateIconIndirect wants it: a 1 bpp
		// bitmap of twice the height, rows aligned to 16 bits.
		const int rowBytes = ((c.width + 15) / 16) * 2;
		std::vector<uint8_t> bits(size_t(rowBytes) * c.height * 2, 0);
		const size_t copy = (std::min)(size_t(rowBytes), size_t(c.pitch));
		for (int y = 0; y < c.height * 2; ++y)
			memcpy(&bits[size_t(y) * rowBytes], &c.data[size_t(y) * c.pitch], copy);
		HBITMAP mask = CreateBitmap(c.width, c.height * 2, 1, 1, bits.data());
		ICONINFO ii = { FALSE, c.hotX, c.hotY, mask, NULL };
		cursor = mask ? CreateIconIndirect(&ii) : NULL;
		if (mask)
			DeleteObject(mask);
	} else if (c.type == video::kCursorColor) {
		BITMAPV5HEADER bh = {};
		bh.bV5Size = sizeof(bh);
		bh.bV5Width = c.width;
		bh.bV5Height = -static_cast<LONG>(c.height); // top-down
		bh.bV5Planes = 1;
		bh.bV5BitCount = 32;
		bh.bV5Compression = BI_BITFIELDS;
		bh.bV5RedMask = 0x00FF0000;
		bh.bV5GreenMask = 0x0000FF00;
		bh.bV5BlueMask = 0x000000FF;
		bh.bV5AlphaMask = 0xFF000000;
		void* pixels = nullptr;
		HDC dc = GetDC(NULL);
		HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &pixels, NULL, 0);
		ReleaseDC(NULL, dc);
		if (color && pixels) {
			for (int y = 0; y < c.height; ++y)
				memcpy(static_cast<uint8_t*>(pixels) + size_t(y) * c.width * 4, &c.data[size_t(y) * c.pitch],
					size_t(c.width) * 4);
			// With an alpha channel the mask is ignored, but it must exist.
			std::vector<uint8_t> zeros(size_t(((c.width + 15) / 16) * 2) * c.height, 0);
			HBITMAP mask = CreateBitmap(c.width, c.height, 1, 1, zeros.data());
			ICONINFO ii = { FALSE, c.hotX, c.hotY, mask, color };
			cursor = mask ? CreateIconIndirect(&ii) : NULL;
			if (mask)
				DeleteObject(mask);
		}
		if (color)
			DeleteObject(color);
	}
	return cursor;
}
