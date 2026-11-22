// sqterm — its API for the sqterm-* commands (docs/install-api.md).
//
// This file is YOURS (ownership class: seeded), like entry.cpp.
//
// A Unix socket in the prefix, $PREFIX/var/run/sqterm.sock, which only
// sqterm's own Linux user can reach: the user every shell and program in
// sqterm runs as, and nothing else on the phone. One JSON request and one
// JSON response a connection, as the SDK's tools speak (squared's
// docs/developer/sdk-tools.md):
//
//   {"sq": 1, "command": "status", "args": {}}
//   {"sq": 1, "command": "launch", "args": {"package": "dev.me.game"}}
//
//   {"sq": 1, "ok": true, "result": {...}}
//   {"sq": 1, "ok": false, "error": {"code": "...", "message": "..."}}
//
// Served on a thread of its own, attached to the VM; every Java call opens a
// local frame and closes it. Android is reached through the application's
// context, which lives as long as the process.

#pragma once

#include <android/native_activity.h>
#include <jni.h>

import std;

namespace sq_android {

class Api final {
public:
    /// Once a process, from the activity's onCreate (Android's UI thread):
    /// keeps the application's context and starts serving. False when the
    /// socket could not be made (said in the log; sqterm runs on without it).
    bool start(ANativeActivity* activity) noexcept;

    /// The socket's path: the prefix's var/run/sqterm.sock.
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    Api() = default;
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;

private:
    void serve(std::stop_token stop) noexcept;
    void answer(int connection, JNIEnv* env) noexcept;
    std::string handle(std::string_view request, JNIEnv* env) noexcept;
    /// Starts the package's launcher activity: "" when it did, else why not.
    std::string launch(JNIEnv* env, const std::string& package) noexcept;

    JavaVM* vm_ = nullptr;
    jobject context_ = nullptr;  // the application's: a global reference
    int api_level_ = 0;
    std::string path_;
    int listen_fd_ = -1;
    std::jthread thread_;
};

/// The process's one Api.
Api& api() noexcept;

}  // namespace sq_android
