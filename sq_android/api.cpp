// sqterm — its API for the sqterm-* commands (see api.hpp).
//
// This file is YOURS (ownership class: seeded), like entry.cpp.

#include "api.hpp"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

import sq.data;

namespace sq_android {

namespace {

constexpr jint kFrame = 32;
constexpr jint kFlagActivityNewTask = 0x10000000;  // Intent.FLAG_ACTIVITY_NEW_TASK
constexpr std::size_t kRequestLimit = 64 * 1024;
constexpr int kRequestTimeoutMs = 5000;

void say(const char* what, const std::string& detail = {}) noexcept {
    __android_log_print(ANDROID_LOG_INFO, "sqterm", "api: %s%s%s", what, detail.empty() ? "" : " ", detail.c_str());
}

using sq::data::JsonValue;

std::string response(JsonValue value) {
    auto text = sq::data::write_json(value);
    return text ? text.text + "\n" : std::string("{\"sq\": 1, \"ok\": false}\n");
}

std::string ok(JsonValue result) {
    return response(JsonValue(JsonValue::Object{{"sq", JsonValue(std::int64_t{1})}, {"ok", JsonValue(true)}, {"result", std::move(result)}}));
}

std::string fail(std::string_view code, std::string message) {
    return response(JsonValue(JsonValue::Object{
        {"sq", JsonValue(std::int64_t{1})},
        {"ok", JsonValue(false)},
        {"error", JsonValue(JsonValue::Object{{"code", JsonValue(code)}, {"message", JsonValue(std::move(message))}})}}));
}

/// A package name as Android has them: letters, digits, _ and dots.
bool good_package(std::string_view name) {
    if (name.empty() || name.size() > 255 || name.front() == '.' || name.back() == '.') return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

/// The pending Java exception's toString, cleared; "" when there is none.
std::string take_exception(JNIEnv* env) {
    jthrowable thrown = env->ExceptionOccurred();
    if (thrown == nullptr) return {};
    env->ExceptionClear();
    std::string text = "a Java exception";
    jclass type = env->GetObjectClass(thrown);
    jmethodID to_string = type != nullptr ? env->GetMethodID(type, "toString", "()Ljava/lang/String;") : nullptr;
    if (to_string != nullptr) {
        auto s = static_cast<jstring>(env->CallObjectMethod(thrown, to_string));
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (s != nullptr) {
            const char* chars = env->GetStringUTFChars(s, nullptr);
            if (chars != nullptr) {
                text = chars;
                env->ReleaseStringUTFChars(s, chars);
            }
        }
    }
    return text;
}

bool make_directories(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

}  // namespace

Api& api() noexcept {
    static Api one;
    return one;
}

bool Api::start(ANativeActivity* activity) noexcept {
    if (vm_ != nullptr) return listen_fd_ >= 0;  // once a process
    vm_ = activity->vm;
    api_level_ = activity->sdkVersion;
    JNIEnv* env = activity->env;
    if (env->PushLocalFrame(kFrame) != JNI_OK) return false;
    jclass activity_class = env->GetObjectClass(activity->clazz);
    jmethodID get_application = env->GetMethodID(activity_class, "getApplicationContext", "()Landroid/content/Context;");
    jobject application = get_application != nullptr ? env->CallObjectMethod(activity->clazz, get_application) : nullptr;
    const std::string thrown = take_exception(env);
    if (application != nullptr && thrown.empty()) context_ = env->NewGlobalRef(application);
    env->PopLocalFrame(nullptr);
    if (context_ == nullptr) {
        say("no application context", thrown);
        return false;
    }

    // The prefix's var/run/: files/usr, as the shell's $PREFIX (prefix.cpp).
    if (activity->internalDataPath == nullptr) return false;
    const std::string run = std::string(activity->internalDataPath) + "/usr/var/run";
    path_ = run + "/sqterm.sock";
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(address.sun_path) || !make_directories(run)) {
        say("no socket path", path_);
        return false;
    }
    std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    ::unlink(path_.c_str());  // one left by a process that has gone
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::chmod(path_.c_str(), 0600) != 0 ||
        ::listen(fd, 4) != 0) {
        say("cannot serve", path_ + ": " + std::strerror(errno));
        ::close(fd);
        return false;
    }
    listen_fd_ = fd;
    thread_ = std::jthread([this](std::stop_token stop) { serve(stop); });
    say("serving", path_);
    return true;
}

void Api::serve(std::stop_token stop) noexcept {
    JNIEnv* env = nullptr;
    JavaVMAttachArgs args{JNI_VERSION_1_6, "sqterm api", nullptr};
    if (vm_->AttachCurrentThread(&env, &args) != JNI_OK || env == nullptr) {
        say("cannot attach a thread to the VM");
        return;
    }
    while (!stop.stop_requested()) {
        pollfd p{listen_fd_, POLLIN, 0};
        const int ready = ::poll(&p, 1, 1000);
        if (ready <= 0) continue;
        const int connection = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (connection < 0) continue;
        answer(connection, env);
        ::close(connection);
    }
    vm_->DetachCurrentThread();
}

void Api::answer(int connection, JNIEnv* env) noexcept {
    // Only sqterm's own user (the socket's mode says so too).
    ucred peer{};
    socklen_t size = sizeof(peer);
    if (::getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::getuid()) return;
    // The request: everything until the client ends its side, or a newline.
    std::string request;
    std::array<char, 4096> buffer{};
    for (;;) {
        pollfd p{connection, POLLIN, 0};
        if (::poll(&p, 1, kRequestTimeoutMs) <= 0) return;
        const ssize_t n = ::read(connection, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        request.append(buffer.data(), static_cast<std::size_t>(n));
        if (request.size() > kRequestLimit) return;
        if (request.find('\n') != std::string::npos) break;
    }
    const std::string reply = handle(request, env);
    std::size_t sent = 0;
    while (sent < reply.size()) {
        const ssize_t n = ::write(connection, reply.data() + sent, reply.size() - sent);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        sent += static_cast<std::size_t>(n);
    }
}

std::string Api::handle(std::string_view request, JNIEnv* env) noexcept {
    auto parsed = sq::data::parse_json(request);
    if (!parsed) return fail("bad_request", "not JSON");
    const JsonValue& r = parsed.value;
    // sq.data keeps a number that fits as unsigned when it is not negative:
    // 1 is either kind.
    const JsonValue* sq = r.find("sq");
    const bool one = sq != nullptr && ((sq->unsigned_integer_if() != nullptr && *sq->unsigned_integer_if() == 1) ||
                                       (sq->signed_integer_if() != nullptr && *sq->signed_integer_if() == 1));
    if (!one) return fail("bad_request", "not an SDK request (sq: 1)");
    const JsonValue* command = r.find("command");
    const std::string* name = command != nullptr ? command->string_if() : nullptr;
    if (name == nullptr) return fail("bad_request", "no command");
    const JsonValue* args = r.find("args");

    if (*name == "status") {
        JsonValue::Object result{{"commands", JsonValue(JsonValue::Array{JsonValue("status"), JsonValue("launch")})},
                                 {"api_level", JsonValue(std::int64_t{api_level_})}};
        // The installed APK: `apkpack verify` on it says which key signed it.
        if (const std::string apk = apk_path(env); !apk.empty()) result.emplace("apk", JsonValue(apk));
        return ok(JsonValue(std::move(result)));
    }
    if (*name == "launch") {
        const JsonValue* package = args != nullptr ? args->find("package") : nullptr;
        const std::string* p = package != nullptr ? package->string_if() : nullptr;
        if (p == nullptr || !good_package(*p)) return fail("bad_request", "launch: args.package, a package name");
        const std::string why = launch(env, *p);
        if (!why.empty()) return fail("not_launched", why);
        return ok(JsonValue(JsonValue::Object{{"package", JsonValue(*p)}}));
    }
    return fail("unknown_command", *name + ": sqterm's API answers status and launch");
}

std::string Api::apk_path(JNIEnv* env) noexcept {
    if (env->PushLocalFrame(kFrame) != JNI_OK) return {};
    std::string path;
    jclass context_class = env->GetObjectClass(context_);
    jmethodID code_path = env->GetMethodID(context_class, "getPackageCodePath", "()Ljava/lang/String;");
    auto s = code_path != nullptr ? static_cast<jstring>(env->CallObjectMethod(context_, code_path)) : nullptr;
    if (take_exception(env).empty() && s != nullptr) {
        if (const char* chars = env->GetStringUTFChars(s, nullptr); chars != nullptr) {
            path = chars;
            env->ReleaseStringUTFChars(s, chars);
        }
    }
    env->PopLocalFrame(nullptr);
    return path;
}

std::string Api::launch(JNIEnv* env, const std::string& package) noexcept {
    if (env->PushLocalFrame(kFrame) != JNI_OK) return "out of memory";
    std::string why;
    jclass context_class = env->GetObjectClass(context_);
    jmethodID get_manager = env->GetMethodID(context_class, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    jobject manager = get_manager != nullptr ? env->CallObjectMethod(context_, get_manager) : nullptr;
    jclass manager_class = manager != nullptr ? env->GetObjectClass(manager) : nullptr;
    jmethodID get_launch =
        manager_class != nullptr
            ? env->GetMethodID(manager_class, "getLaunchIntentForPackage", "(Ljava/lang/String;)Landroid/content/Intent;")
            : nullptr;
    jobject intent = get_launch != nullptr ? env->CallObjectMethod(manager, get_launch, env->NewStringUTF(package.c_str())) : nullptr;
    why = take_exception(env);
    if (why.empty() && intent == nullptr) why = package + " is not installed, or has no launcher activity";
    if (why.empty()) {
        jclass intent_class = env->GetObjectClass(intent);
        jmethodID add_flags = env->GetMethodID(intent_class, "addFlags", "(I)Landroid/content/Intent;");
        if (add_flags != nullptr) env->CallObjectMethod(intent, add_flags, kFlagActivityNewTask);
        jmethodID start = env->GetMethodID(context_class, "startActivity", "(Landroid/content/Intent;)V");
        if (start != nullptr) env->CallVoidMethod(context_, start, intent);
        why = take_exception(env);
    }
    env->PopLocalFrame(nullptr);
    say(why.empty() ? "launched" : "not launched", why.empty() ? package : package + ": " + why);
    return why;
}

}  // namespace sq_android
