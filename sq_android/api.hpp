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
//   {"sq": 1, "command": "install", "args": {"apk": "/abs/path/game.apk"}}
//   {"sq": 1, "command": "uninstall", "args": {"package": "dev.me.game"}}
//
//   {"sq": 1, "ok": true, "result": {...}}
//   {"sq": 1, "ok": false, "error": {"code": "...", "message": "..."}}
//
// Served on a thread of its own, attached to the VM; every Java call opens a
// local frame and closes it. Android is reached through the application's
// context, which lives as long as the process.
//
// install and uninstall go through PackageInstaller, which answers through
// an IntentSender: a PendingIntent to squared's shim receiver,
// dev.squared.ResultReceiver, whose onReceive is native (on_receive, on the
// main thread). Android's confirmation screen arrives that way first
// (STATUS_PENDING_USER_ACTION) and is started there; the final status then
// wakes the request waiting on the API thread. One request is served at a
// time, so a status asked for while an install waits on the person is
// answered after it.

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
    /// The installed APK's path (Context.getPackageCodePath), or "".
    std::string apk_path(JNIEnv* env) noexcept;
    /// Starts the package's launcher activity: "" when it did, else why not.
    std::string launch(JNIEnv* env, const std::string& package) noexcept;
    /// An installed package's versionCode (PackageInfo.getLongVersionCode).
    std::optional<std::int64_t> version_code(JNIEnv* env, const std::string& package) noexcept;
    /// sqterm's PackageInstaller; a local reference (in the caller's frame).
    jobject package_installer(JNIEnv* env) noexcept;
    /// Whether Android lets sqterm install (Settings, "Install unknown apps").
    bool can_install(JNIEnv* env) noexcept;

    /// What PackageInstaller said in the end.
    struct Outcome {
        int status = 1;           // PackageInstaller.STATUS_*; 1, STATUS_FAILURE, when unknown
        std::string message;      // its EXTRA_STATUS_MESSAGE
        std::string package;      // its EXTRA_PACKAGE_NAME
    };
    /// Installs the APK and waits for Android's answer; std::nullopt (why in
    /// `why`) when it could not even be asked.
    std::optional<Outcome> install(JNIEnv* env, const std::string& apk, std::string& why) noexcept;
    std::optional<Outcome> uninstall(JNIEnv* env, const std::string& package, std::string& why) noexcept;
    /// An IntentSender that brings request `id`'s result to on_receive.
    jobject result_sender(JNIEnv* env, int id) noexcept;
    /// Waits for request `id`'s final status (10 minutes at most: a person
    /// is answering).
    std::optional<Outcome> wait_for(int id) noexcept;
    /// dev.squared.ResultReceiver.onReceive(Context, Intent), on the main thread.
    static void on_receive(JNIEnv* env, jobject receiver, jobject context, jobject intent);
    bool register_receiver(ANativeActivity* activity) noexcept;

    JavaVM* vm_ = nullptr;
    jobject context_ = nullptr;  // the application's: a global reference
    int api_level_ = 0;
    std::string path_;
    int listen_fd_ = -1;
    bool receiver_ = false;  // the shim's onReceive registered

    std::mutex results_mutex_;
    std::condition_variable results_changed_;
    std::map<int, Outcome> results_;  // final statuses, by request
    int next_request_ = 1;

    std::jthread thread_;  // last: stopped before the rest goes
};

/// The process's one Api.
Api& api() noexcept;

}  // namespace sq_android
