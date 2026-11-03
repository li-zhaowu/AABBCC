// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Remote mouse pointer in video mode: builds a Windows cursor from the shape
// the server took from DXGI Desktop Duplication.

#pragma once

#include <windows.h>

#include "common/VideoProto.h"

// Returns NULL for kCursorHidden or on failure. The caller destroys it.
HCURSOR CreateCursorFromShape(const video::Cursor& c);
