// sqterm: unpacking a package (unpack.cpp). Shared by the app and pkg.
//
// This file is YOURS (ownership class: seeded, sqterm's own).
//
// No standard type in the interface: the app's files import std, pkg's
// include its headers, and both include this.

#pragma once

namespace sqterm {

/// Unpack `archive` (ustar, compressed with zstd or plain) into `prefix`.
/// Entries under SQ-INF/ go to the directory `meta` instead, when it is not
/// null or empty. 0 on success; otherwise what went wrong is written to
/// `error` (cut to `error_size`) and the result is not 0.
int unpack_archive(const char* archive, const char* prefix, const char* meta, char* error,
                   unsigned long error_size);

/// Unpack a package that is a squared cartridge, `archive` (a .sq), into
/// `prefix` (unpack_cartridge.cpp): its files, with mode 0755 where its
/// flags have x, and the symbolic links its manifest names; the manifest
/// to `meta`, as manifest.json. The same result and error as unpack_archive.
int unpack_cartridge(const char* archive, const char* prefix, const char* meta, char* error,
                     unsigned long error_size);

/// Where packages are built to live (packages/build.sh's SQ_PREFIX): sqterm's
/// folder, which some programs (zsh, git, nvim, tic) and scripts (#! lines)
/// keep in them. Unpacked into another app's folder (sqIDE's), both
/// unpackers rewrite it in each file to that app's (relocate_file).
inline constexpr char built_prefix[] = "/data/data/dev.squared.sqterm/files/usr";

/// What to write over built_prefix in the files unpacked into `prefix`, into
/// `out` (cut to `out_size`): 1 when `prefix` is another folder, 0 when it is
/// built_prefix (sqterm's own; /data/user/0/... names the same folder).
/// /data/user/0/<app>/... is given as /data/data/<app>/..., which is shorter.
int relocation_of(const char* prefix, char* out, unsigned long out_size);

/// In the file at `path`, built_prefix rewritten to `to`. A text file's every
/// one, as long as `to` is; a binary's (one with a NUL in its first 8 KB) in
/// place: the rest of the C string it is in moved up and ended with NULs, so
/// nothing after moves, and only where `to` is no longer than built_prefix.
/// 0 on success (or nothing to rewrite); otherwise why, into `error`.
int relocate_file(const char* path, const char* to, char* error, unsigned long error_size);

}  // namespace sqterm
