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

}  // namespace sqterm
