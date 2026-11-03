// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Wire format of the file clipboard channel (extension channel 1).
// See docs/design.md section 2. Platform independent so it can be unit
// tested (tests/fileclip_proto).
//
// Roles: the "source" holds the files (its user pressed Ctrl+C), the
// "target" put a virtual file list on its clipboard and pulls file contents
// when its user pastes. Both the viewer and the server can play either role.
//
//   Offer  source -> target  new file list; replaces any previous offer
//   Revoke source -> target  the source clipboard no longer holds that offer
//   Read   target -> source  please stream [offset, offset+length) of a file
//   Data   source -> target  a piece of a file (in order per stream)
//   Cancel target -> source  stop streaming this stream, close the file
//   Error  source -> target  the stream failed
//
// A "stream" is one IStream handed to the target's Explorer. The target picks
// the stream id; the source opens the file on the first Read of a stream id
// and closes it on Cancel, on end of file or when the stream fails.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fileclip {

enum Subtype : uint8_t {
	kOffer  = 1,
	kRevoke = 2,
	kRead   = 3,
	kData   = 4,
	kCancel = 5,
	kError  = 6,
};

enum ErrorCode : uint32_t {
	kErrStaleOffer = 1, // the offer was replaced or revoked
	kErrBadFile    = 2, // unknown file id or not a regular file
	kErrOpen       = 3, // the file could not be opened
	kErrRead       = 4, // reading failed
	kErrBusy       = 5, // too many open streams
};

enum DataFlags : uint8_t {
	kDataEof = 0x01, // no data follows this piece; the file ends here
};

// Windows FILE_ATTRIBUTE_* bits that are carried across.
constexpr uint32_t kAttrReadOnly  = 0x01;
constexpr uint32_t kAttrHidden    = 0x02;
constexpr uint32_t kAttrDirectory = 0x10;
constexpr uint32_t kAttrArchive   = 0x20;
constexpr uint32_t kAttrMask = kAttrReadOnly | kAttrHidden | kAttrDirectory | kAttrArchive;

// Limits.
constexpr uint32_t kMaxEntries     = 100000;
constexpr size_t   kMaxPathBytes   = 1024;  // UTF-8, on the wire
constexpr size_t   kMaxPathChars   = 259;   // UTF-16, FILEDESCRIPTORW.cFileName - 1
constexpr uint32_t kMaxReadLength  = 16 * 1024 * 1024;
constexpr size_t   kDataPieceBytes = 256 * 1024;

struct Entry {
	std::string path;      // relative, '/' separated, UTF-8
	uint32_t attributes = 0;
	uint64_t size = 0;     // 0 for directories
	uint64_t mtime = 0;    // Windows FILETIME as a 64-bit value, 0 = unknown
	bool IsDirectory() const { return (attributes & kAttrDirectory) != 0; }
};

struct Offer {
	uint32_t seq = 0;
	std::vector<Entry> entries; // the index is the file id
};

struct Read {
	uint32_t seq = 0;
	uint32_t streamId = 0;
	uint32_t fileId = 0;
	uint64_t offset = 0;
	uint32_t length = 0;
};

struct DataHeader {
	uint32_t streamId = 0;
	uint64_t offset = 0;
	uint8_t flags = 0;
};
constexpr size_t kDataHeaderSize = 13;

struct Error {
	uint32_t streamId = 0;
	uint32_t code = 0;
};

std::vector<uint8_t> EncodeOffer(const Offer& o);
bool DecodeOffer(const uint8_t* p, size_t len, Offer& out);

std::vector<uint8_t> EncodeRevoke(uint32_t seq);
bool DecodeRevoke(const uint8_t* p, size_t len, uint32_t& seq);

std::vector<uint8_t> EncodeRead(const Read& r);
bool DecodeRead(const uint8_t* p, size_t len, Read& out);

// Data = header followed by the file bytes.
std::vector<uint8_t> EncodeData(const DataHeader& h, const uint8_t* bytes, size_t n);
bool DecodeDataHeader(const uint8_t* p, size_t len, DataHeader& out);

std::vector<uint8_t> EncodeCancel(uint32_t streamId);
bool DecodeCancel(const uint8_t* p, size_t len, uint32_t& streamId);

std::vector<uint8_t> EncodeError(const Error& e);
bool DecodeError(const uint8_t* p, size_t len, Error& out);

// True if a path received from the peer is safe to hand to the local file
// system as a path relative to the paste folder: no absolute paths, drive
// letters, UNC or device prefixes, no "." / ".." components, no characters
// Windows forbids, no reserved device names, no trailing dot or space,
// valid UTF-8, and short enough for FILEDESCRIPTORW.
bool IsSafeRelativePath(const std::string& utf8);

// Number of UTF-16 code units the UTF-8 string needs, or SIZE_MAX if it is
// not valid UTF-8.
size_t Utf16Length(const std::string& utf8);

// Drops entries whose path is not safe (an entry below an unsafe directory
// is itself unsafe, since its path contains the bad component). Returns the
// number of entries removed. idMap receives, for each remaining entry, its
// original file id (index in the offer as received).
size_t FilterUnsafeEntries(Offer& offer, std::vector<uint32_t>& idMap);

} // namespace fileclip
