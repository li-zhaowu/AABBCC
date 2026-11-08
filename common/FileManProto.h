// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Wire format of the file manager channel (extension channel 5), used by
// the viewer's file transfer window and the file-only connection mode. See
// docs/design.md section 6. Platform independent (tests/fileman_proto).
//
// The viewer sends requests, the server answers. Every request except
// Write carries a request id that comes back in the Reply.
//
//   Roots      drives and the user's well-known folders
//   List       directory listing (may come in several Reply pages)
//   Stat       one path: exists? attributes, size, time
//   MkDir, Rename, Delete (directories recursively)
//   OpenRead   -> handle, size, time
//   Read       stream [offset, offset+length) of a handle as Data messages;
//              a Reply only comes on error
//   Close      close a read handle
//   OpenWrite  -> handle (+ resume offset); the server writes to a
//              temporary file next to the target. With kWriteResume the
//              temporary file is named after the target, size and time, is
//              kept when the connection drops, and a later OpenWrite of the
//              same file continues it (the reply says from where)
//   Write      bytes at an offset (in order); acknowledged by WriteAck
//   CloseWrite commit (rename the temporary file into place, set the time)
//              or discard
//
// Priorities: Read data and Write go at kPrioBulk, everything else at
// kPrioControl. CloseWrite must therefore only be sent once every Write is
// acknowledged (control messages overtake bulk ones).
//
// Paths are absolute Windows paths in UTF-8 ("C:\dir\file", "\\server\share\x").

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fileman {

enum Subtype : uint8_t {
	// viewer -> server
	kRoots      = 1,
	kList       = 2,
	kStat       = 3,
	kMkDir      = 4,
	kRename     = 5,
	kDelete     = 6,
	kOpenRead   = 7,
	kRead       = 8,
	kClose      = 9,
	kOpenWrite  = 10,
	kWrite      = 11,
	kCloseWrite = 12,
	// server -> viewer
	kReply      = 32,
	kData       = 33,
	kWriteAck   = 34,
};

enum Status : uint32_t {
	kOk             = 0,
	kErrNotFound    = 1,
	kErrAccess      = 2,
	kErrExists      = 3,
	kErrBadPath     = 4,
	kErrIo          = 5,
	kErrBusy        = 6,  // too many open handles
	kErrBadHandle   = 7,
	kErrDiskFull    = 8,
	kErrUnsupported = 9,
	kErrBadRequest  = 10,
	// Local only (never on the wire).
	kErrDisconnected = 100,
	kErrCancelled    = 101,
	kErrTimeout      = 102,
};
const char* StatusName(uint32_t status);

// Windows FILE_ATTRIBUTE_* bits that are carried across.
constexpr uint32_t kAttrReadOnly  = 0x01;
constexpr uint32_t kAttrHidden    = 0x02;
constexpr uint32_t kAttrSystem    = 0x04;
constexpr uint32_t kAttrDirectory = 0x10;
constexpr uint32_t kAttrArchive   = 0x20;
constexpr uint32_t kAttrMask = kAttrReadOnly | kAttrHidden | kAttrSystem | kAttrDirectory | kAttrArchive;

// Limits.
constexpr size_t   kMaxPathBytes   = 4096;           // UTF-8
constexpr size_t   kMaxPathChars   = 32000;          // UTF-16
constexpr size_t   kMaxMessageText = 1024;
constexpr uint32_t kMaxReadLength  = 16 * 1024 * 1024;
constexpr size_t   kDataPieceBytes = 256 * 1024;     // Data / Write payload
constexpr uint32_t kListPageEntries = 2000;
constexpr uint32_t kMaxOpenHandles = 16;

enum RootType : uint8_t {
	kRootUnknown   = 0,
	kRootFixed     = 1,
	kRootRemovable = 2,
	kRootNetwork   = 3,
	kRootCdrom     = 4,
	kRootRam       = 5,
	kRootFolder    = 10, // a well-known folder (Desktop, Documents, ...)
};

struct Root {
	std::string path;     // "C:\" or a folder
	std::string label;    // volume label or folder name
	uint8_t type = kRootUnknown;
	uint64_t totalBytes = 0;
	uint64_t freeBytes = 0;
};

struct Entry {
	std::string name;     // file name only
	uint32_t attributes = 0;
	uint64_t size = 0;
	uint64_t mtime = 0;   // FILETIME as 64 bit, 0 = unknown
	bool IsDirectory() const { return (attributes & kAttrDirectory) != 0; }
};

// --- requests -------------------------------------------------------------

// Roots, List, Stat, MkDir, Delete, OpenRead: reqId + one path.
std::vector<uint8_t> EncodePathRequest(uint32_t reqId, const std::string& path);
bool DecodePathRequest(const uint8_t* p, size_t len, uint32_t& reqId, std::string& path);
// Roots: reqId only.
std::vector<uint8_t> EncodeRootsRequest(uint32_t reqId);
bool DecodeRootsRequest(const uint8_t* p, size_t len, uint32_t& reqId);

struct RenameRequest {
	uint32_t reqId = 0;
	std::string from, to;
};
std::vector<uint8_t> EncodeRename(const RenameRequest& r);
bool DecodeRename(const uint8_t* p, size_t len, RenameRequest& out);

struct ReadRequest {
	uint32_t reqId = 0;
	uint32_t handle = 0;
	uint64_t offset = 0;
	uint32_t length = 0;
};
std::vector<uint8_t> EncodeRead(const ReadRequest& r);
bool DecodeRead(const uint8_t* p, size_t len, ReadRequest& out);

// Close: reqId + handle.
std::vector<uint8_t> EncodeHandleRequest(uint32_t reqId, uint32_t handle);
bool DecodeHandleRequest(const uint8_t* p, size_t len, uint32_t& reqId, uint32_t& handle);

enum OpenWriteFlags : uint8_t {
	kWriteOverwrite = 0x01, // replace an existing file (else kErrExists)
	kWriteResume    = 0x02, // keep a partial upload across disconnects; continue one
};
struct OpenWriteRequest {
	uint32_t reqId = 0;
	std::string path;
	uint64_t size = 0;    // expected size (disk space check, resume identity)
	uint8_t flags = 0;
	uint64_t mtime = 0;   // source time (resume identity); optional on the wire
};
std::vector<uint8_t> EncodeOpenWrite(const OpenWriteRequest& r);
bool DecodeOpenWrite(const uint8_t* p, size_t len, OpenWriteRequest& out);

struct WriteHeader {
	uint32_t handle = 0;
	uint64_t offset = 0;
};
constexpr size_t kWriteHeaderSize = 12;
std::vector<uint8_t> EncodeWrite(const WriteHeader& h, const uint8_t* bytes, size_t n);
bool DecodeWriteHeader(const uint8_t* p, size_t len, WriteHeader& out);

struct CloseWriteRequest {
	uint32_t reqId = 0;
	uint32_t handle = 0;
	uint64_t mtime = 0;   // 0 = leave as is
	bool commit = false;  // false: discard the temporary file
};
std::vector<uint8_t> EncodeCloseWrite(const CloseWriteRequest& r);
bool DecodeCloseWrite(const uint8_t* p, size_t len, CloseWriteRequest& out);

// --- replies ---------------------------------------------------------------

enum ReplyFlags : uint8_t {
	kReplyMore = 0x01, // List: more pages follow with the same reqId
};

struct ReplyHeader {
	uint32_t reqId = 0;
	uint32_t status = kOk;
	uint8_t flags = 0;
	std::string message;  // UTF-8, for the user (may be empty)
};
// Reply = header + body (depends on the request; empty on error).
std::vector<uint8_t> EncodeReply(const ReplyHeader& h, const std::vector<uint8_t>& body);
// bodyOffset: where the body starts in p.
bool DecodeReplyHeader(const uint8_t* p, size_t len, ReplyHeader& out, size_t& bodyOffset);

std::vector<uint8_t> EncodeRoots(const std::vector<Root>& roots);
bool DecodeRoots(const uint8_t* p, size_t len, std::vector<Root>& out);

std::vector<uint8_t> EncodeEntries(const std::vector<Entry>& entries);
bool DecodeEntries(const uint8_t* p, size_t len, std::vector<Entry>& out);

// Stat body.
std::vector<uint8_t> EncodeStat(const Entry& e);
bool DecodeStat(const uint8_t* p, size_t len, Entry& out);

struct OpenReadReply {
	uint32_t handle = 0;
	uint64_t size = 0;
	uint64_t mtime = 0;
};
std::vector<uint8_t> EncodeOpenReadReply(const OpenReadReply& r);
bool DecodeOpenReadReply(const uint8_t* p, size_t len, OpenReadReply& out);

// OpenWrite body: the handle, and where to continue (0 = from the start;
// optional on the wire).
struct OpenWriteReply {
	uint32_t handle = 0;
	uint64_t offset = 0;
};
std::vector<uint8_t> EncodeOpenWriteReply(const OpenWriteReply& r);
bool DecodeOpenWriteReply(const uint8_t* p, size_t len, OpenWriteReply& out);

// Name of the temporary file of a resumable transfer of `targetPath` with
// the given source size and time: "~uvnc-<16 hex digits>.part", the same
// on both sides (stable across restarts, different for another version of
// the file).
std::string ResumeTempName(const std::string& targetPath, uint64_t size, uint64_t mtime);

enum DataFlags : uint8_t {
	kDataEof = 0x01, // the file ends with this piece
};
struct DataHeader {
	uint32_t handle = 0;
	uint64_t offset = 0;
	uint8_t flags = 0;
};
constexpr size_t kDataHeaderSize = 13;
std::vector<uint8_t> EncodeData(const DataHeader& h, const uint8_t* bytes, size_t n);
bool DecodeDataHeader(const uint8_t* p, size_t len, DataHeader& out);

struct WriteAck {
	uint32_t handle = 0;
	uint32_t status = kOk;
	uint64_t written = 0;  // bytes written so far (contiguous from 0)
};
std::vector<uint8_t> EncodeWriteAck(const WriteAck& a);
bool DecodeWriteAck(const uint8_t* p, size_t len, WriteAck& out);

// --- paths -----------------------------------------------------------------

// An absolute path the server may hand to the file system: "X:\..." or
// "\\server\share\...", '\' separated, no "." / ".." components, no empty
// components (except the root "X:\"), no characters Windows forbids, no
// reserved device names, no trailing dot or space in a component, no
// "\\?\" or "\\.\" prefixes, valid UTF-8 and not too long.
bool IsAcceptablePath(const std::string& utf8);
// True for "X:\" and "\\server\share" (nothing above them).
bool IsRootPath(const std::string& utf8);
// A single file name typed by the user (new folder, rename).
bool IsValidName(const std::string& utf8);

// dir + '\' + name (no doubled separator after "X:\").
std::string JoinPath(const std::string& dir, const std::string& name);
// Parent directory, or empty for a root.
std::string ParentPath(const std::string& path);
// Last component ("C:\a\b" -> "b"; "C:\" -> "C:\").
std::string FileNamePart(const std::string& path);

// UTF-16 length of valid UTF-8, or SIZE_MAX.
size_t Utf16Length(const std::string& utf8);

} // namespace fileman
