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
constexpr int kFlagUpdateCurrent = 0x08000000;  // PendingIntent.FLAG_UPDATE_CURRENT
constexpr int kFlagMutable = 0x02000000;        // PendingIntent.FLAG_MUTABLE (API 31): Android fills in the status
constexpr int kStatusPendingUserAction = -1;    // PackageInstaller.STATUS_PENDING_USER_ACTION
constexpr int kStatusSuccess = 0;               // PackageInstaller.STATUS_SUCCESS
constexpr const char* kReceiver = "dev.squared.ResultReceiver";
constexpr const char* kRequestExtra = "dev.squared.sqterm.REQUEST";  // which request a result answers
constexpr auto kAnswerTimeout = std::chrono::minutes(10);  // a person is deciding

/// PackageInstaller.STATUS_* by name, for the error's code.
std::string status_name(int status) {
    switch (status) {
        case 0: return "STATUS_SUCCESS";
        case 1: return "STATUS_FAILURE";
        case 2: return "STATUS_FAILURE_BLOCKED";
        case 3: return "STATUS_FAILURE_ABORTED";
        case 4: return "STATUS_FAILURE_INVALID";
        case 5: return "STATUS_FAILURE_CONFLICT";
        case 6: return "STATUS_FAILURE_STORAGE";
        case 7: return "STATUS_FAILURE_INCOMPATIBLE";
        case 8: return "STATUS_FAILURE_TIMEOUT";
        default: return "STATUS_" + std::to_string(status);
    }
}

/// What each status means to the person at the keyboard.
std::string status_hint(int status) {
    switch (status) {
        case 3: return "cancelled";
        case 5: return "an app with this package is installed, signed with another key: uninstall it first";
        case 4: return "not a valid APK";
        case 6: return "not enough storage";
        case 7: return "not for this phone (its ABI or Android version)";
        case 2: return "blocked: allow sqterm in Settings, Install unknown apps";
        default: return {};
    }
}

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

/// A Java string's UTF-8; "" for null.
std::string string_of(JNIEnv* env, jobject value) {
    if (value == nullptr) return {};
    auto s = static_cast<jstring>(value);
    const char* chars = env->GetStringUTFChars(s, nullptr);
    if (chars == nullptr) return {};
    std::string text = chars;
    env->ReleaseStringUTFChars(s, chars);
    return text;
}

/// True (and why, if nothing else said it) when a Java call threw.
bool thrown(JNIEnv* env, std::string& why) {
    std::string e = take_exception(env);
    if (e.empty()) return false;
    if (why.empty()) why = std::move(e);
    return true;
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
    receiver_ = register_receiver(activity);

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
        JsonValue::Array commands{JsonValue("status"), JsonValue("launch")};
        if (receiver_) {
            commands.emplace_back("install");
            commands.emplace_back("uninstall");
        }
        JsonValue::Object result{{"commands", JsonValue(std::move(commands))},
                                 {"api_level", JsonValue(std::int64_t{api_level_})},
                                 {"can_install", JsonValue(can_install(env))}};
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
    if (*name == "install" || *name == "uninstall") {
        if (!receiver_) return fail("unavailable", "this sqterm has no dev.squared.ResultReceiver: install a newer sqterm");
        const bool installing = *name == "install";
        const JsonValue* what = args != nullptr ? args->find(installing ? "apk" : "package") : nullptr;
        const std::string* w = what != nullptr ? what->string_if() : nullptr;
        if (installing && (w == nullptr || w->empty() || w->front() != '/'))
            return fail("bad_request", "install: args.apk, the APK's absolute path");
        if (!installing && (w == nullptr || !good_package(*w)))
            return fail("bad_request", "uninstall: args.package, a package name");
        std::string why;
        const auto outcome = installing ? install(env, *w, why) : uninstall(env, *w, why);
        if (!outcome) return fail(installing ? "not_installed" : "not_uninstalled", why);
        if (outcome->status != kStatusSuccess) {
            std::string message = outcome->message;
            if (const std::string hint = status_hint(outcome->status); !hint.empty())
                message = message.empty() ? hint : hint + " (" + message + ")";
            return fail(status_name(outcome->status), message.empty() ? "Android said no" : message);
        }
        JsonValue::Object result;
        const std::string package = outcome->package.empty() && !installing ? *w : outcome->package;
        if (!package.empty()) result.emplace("package", JsonValue(package));
        if (installing && !package.empty()) {
            if (const auto code = version_code(env, package)) result.emplace("version_code", JsonValue(*code));
        }
        return ok(JsonValue(std::move(result)));
    }
    return fail("unknown_command", *name + ": sqterm's API answers status, launch, install and uninstall");
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

bool Api::register_receiver(ANativeActivity* activity) noexcept {
    JNIEnv* env = activity->env;
    if (env->PushLocalFrame(kFrame) != JNI_OK) return false;
    bool done = false;
    std::string why;
    // The app's class loader: FindClass would use NativeActivity's (the
    // system's), which has never heard of the shim (as background.cpp).
    jclass activity_class = env->GetObjectClass(activity->clazz);
    jmethodID get_loader = env->GetMethodID(activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = get_loader != nullptr ? env->CallObjectMethod(activity->clazz, get_loader) : nullptr;
    jclass loader_class = env->FindClass("java/lang/ClassLoader");
    jmethodID load = loader_class != nullptr
                         ? env->GetMethodID(loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;")
                         : nullptr;
    if (!thrown(env, why) && loader != nullptr && load != nullptr) {
        auto shim = static_cast<jclass>(env->CallObjectMethod(loader, load, env->NewStringUTF(kReceiver)));
        if (!thrown(env, why) && shim != nullptr) {
            const JNINativeMethod native{"onReceive", "(Landroid/content/Context;Landroid/content/Intent;)V",
                                         reinterpret_cast<void*>(&Api::on_receive)};
            done = env->RegisterNatives(shim, &native, 1) == JNI_OK;
            thrown(env, why);
        }
    }
    env->PopLocalFrame(nullptr);
    say(done ? "dev.squared.ResultReceiver registered: install and uninstall"
             : "no dev.squared.ResultReceiver: install and uninstall unavailable",
        why);
    return done;
}

void Api::on_receive(JNIEnv* env, jobject /*receiver*/, jobject /*context*/, jobject intent) {
    Api& self = api();
    if (intent == nullptr || env->PushLocalFrame(kFrame) != JNI_OK) return;
    std::string why;
    jclass intent_class = env->GetObjectClass(intent);
    jmethodID get_int = env->GetMethodID(intent_class, "getIntExtra", "(Ljava/lang/String;I)I");
    jmethodID get_string = env->GetMethodID(intent_class, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
    const int id = env->CallIntMethod(intent, get_int, env->NewStringUTF(kRequestExtra), 0);
    Outcome outcome;
    outcome.status = env->CallIntMethod(intent, get_int, env->NewStringUTF("android.content.pm.extra.STATUS"), 1);
    outcome.message =
        string_of(env, env->CallObjectMethod(intent, get_string, env->NewStringUTF("android.content.pm.extra.STATUS_MESSAGE")));
    outcome.package =
        string_of(env, env->CallObjectMethod(intent, get_string, env->NewStringUTF("android.content.pm.extra.PACKAGE_NAME")));
    thrown(env, why);
    if (outcome.status == kStatusPendingUserAction) {
        // Android's own screen, "Do you want to install this app?": start it
        // and wait for the final status, which comes here again.
        jmethodID get_parcelable =
            env->GetMethodID(intent_class, "getParcelableExtra", "(Ljava/lang/String;)Landroid/os/Parcelable;");
        jobject confirm = env->CallObjectMethod(intent, get_parcelable, env->NewStringUTF("android.intent.extra.INTENT"));
        if (!thrown(env, why) && confirm != nullptr) {
            jclass confirm_class = env->GetObjectClass(confirm);
            jmethodID add_flags = env->GetMethodID(confirm_class, "addFlags", "(I)Landroid/content/Intent;");
            env->CallObjectMethod(confirm, add_flags, kFlagActivityNewTask);
            jclass context_class = env->GetObjectClass(self.context_);
            jmethodID start = env->GetMethodID(context_class, "startActivity", "(Landroid/content/Intent;)V");
            env->CallVoidMethod(self.context_, start, confirm);
            if (!thrown(env, why)) {
                say("asking the person", std::to_string(id));
                env->PopLocalFrame(nullptr);
                return;
            }
        }
        outcome.status = 1;
        outcome.message = why.empty() ? "Android's confirmation could not be shown" : why;
    }
    env->PopLocalFrame(nullptr);
    say("result", std::to_string(id) + " " + status_name(outcome.status) + " " + outcome.message);
    {
        const std::lock_guard lock(self.results_mutex_);
        self.results_[id] = std::move(outcome);
    }
    self.results_changed_.notify_all();
}

std::optional<Api::Outcome> Api::wait_for(int id) noexcept {
    std::unique_lock lock(results_mutex_);
    if (!results_changed_.wait_for(lock, kAnswerTimeout, [&] { return results_.contains(id); })) return std::nullopt;
    Outcome outcome = std::move(results_.at(id));
    results_.erase(id);
    return outcome;
}

jobject Api::result_sender(JNIEnv* env, int id) noexcept {
    jclass intent_class = env->FindClass("android/content/Intent");
    jobject intent = env->NewObject(intent_class, env->GetMethodID(intent_class, "<init>", "()V"));
    jclass context_class = env->GetObjectClass(context_);
    jobject package = env->CallObjectMethod(context_, env->GetMethodID(context_class, "getPackageName", "()Ljava/lang/String;"));
    env->CallObjectMethod(intent,
                          env->GetMethodID(intent_class, "setClassName",
                                           "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;"),
                          package, env->NewStringUTF(kReceiver));
    env->CallObjectMethod(intent, env->GetMethodID(intent_class, "putExtra", "(Ljava/lang/String;I)Landroid/content/Intent;"),
                          env->NewStringUTF(kRequestExtra), id);
    jclass pending_class = env->FindClass("android/app/PendingIntent");
    jmethodID get_broadcast = env->GetStaticMethodID(
        pending_class, "getBroadcast", "(Landroid/content/Context;ILandroid/content/Intent;I)Landroid/app/PendingIntent;");
    const jint flags = kFlagUpdateCurrent | (api_level_ >= 31 ? kFlagMutable : 0);
    jobject pending = env->CallStaticObjectMethod(pending_class, get_broadcast, context_, id, intent, flags);
    if (env->ExceptionCheck() || pending == nullptr) return nullptr;
    return env->CallObjectMethod(pending, env->GetMethodID(pending_class, "getIntentSender", "()Landroid/content/IntentSender;"));
}

jobject Api::package_installer(JNIEnv* env) noexcept {
    jclass context_class = env->GetObjectClass(context_);
    jobject manager = env->CallObjectMethod(
        context_, env->GetMethodID(context_class, "getPackageManager", "()Landroid/content/pm/PackageManager;"));
    if (env->ExceptionCheck() || manager == nullptr) return nullptr;
    jclass manager_class = env->GetObjectClass(manager);
    return env->CallObjectMethod(
        manager, env->GetMethodID(manager_class, "getPackageInstaller", "()Landroid/content/pm/PackageInstaller;"));
}

bool Api::can_install(JNIEnv* env) noexcept {
    if (env->PushLocalFrame(kFrame) != JNI_OK) return false;
    std::string why;
    jclass context_class = env->GetObjectClass(context_);
    jobject manager = env->CallObjectMethod(
        context_, env->GetMethodID(context_class, "getPackageManager", "()Landroid/content/pm/PackageManager;"));
    bool can = false;
    if (!thrown(env, why) && manager != nullptr) {
        jmethodID m = env->GetMethodID(env->GetObjectClass(manager), "canRequestPackageInstalls", "()Z");
        can = m != nullptr && env->CallBooleanMethod(manager, m) == JNI_TRUE;
        if (thrown(env, why)) can = false;
    }
    env->PopLocalFrame(nullptr);
    return can;
}

std::optional<std::int64_t> Api::version_code(JNIEnv* env, const std::string& package) noexcept {
    if (env->PushLocalFrame(kFrame) != JNI_OK) return std::nullopt;
    std::string why;
    std::optional<std::int64_t> code;
    jclass context_class = env->GetObjectClass(context_);
    jobject manager = env->CallObjectMethod(
        context_, env->GetMethodID(context_class, "getPackageManager", "()Landroid/content/pm/PackageManager;"));
    if (!thrown(env, why) && manager != nullptr) {
        jmethodID get_info = env->GetMethodID(env->GetObjectClass(manager), "getPackageInfo",
                                              "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
        jobject info = env->CallObjectMethod(manager, get_info, env->NewStringUTF(package.c_str()), 0);
        if (!thrown(env, why) && info != nullptr) {
            jmethodID long_code = env->GetMethodID(env->GetObjectClass(info), "getLongVersionCode", "()J");
            const jlong value = env->CallLongMethod(info, long_code);
            if (!thrown(env, why)) code = value;
        }
    }
    env->PopLocalFrame(nullptr);
    return code;
}

std::optional<Api::Outcome> Api::install(JNIEnv* env, const std::string& apk, std::string& why) noexcept {
    const int fd = ::open(apk.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        why = apk + ": " + std::strerror(errno);
        return std::nullopt;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        ::close(fd);
        why = apk + ": not a file";
        return std::nullopt;
    }
    if (env->PushLocalFrame(kFrame) != JNI_OK) {
        ::close(fd);
        why = "out of memory";
        return std::nullopt;
    }
    int id = 0;
    bool committed = false;
    jobject installer = package_installer(env);
    jobject session = nullptr;
    jclass session_class = nullptr;
    if (!thrown(env, why) && installer != nullptr) {
        jclass installer_class = env->GetObjectClass(installer);
        jclass params_class = env->FindClass("android/content/pm/PackageInstaller$SessionParams");
        jobject params = params_class != nullptr
                             ? env->NewObject(params_class, env->GetMethodID(params_class, "<init>", "(I)V"), 1)  // MODE_FULL_INSTALL
                             : nullptr;
        const jint session_id =
            params != nullptr
                ? env->CallIntMethod(installer,
                                     env->GetMethodID(installer_class, "createSession",
                                                      "(Landroid/content/pm/PackageInstaller$SessionParams;)I"),
                                     params)
                : 0;
        if (!thrown(env, why) && params != nullptr)
            session = env->CallObjectMethod(
                installer, env->GetMethodID(installer_class, "openSession", "(I)Landroid/content/pm/PackageInstaller$Session;"),
                session_id);
    }
    if (!thrown(env, why) && session != nullptr) {
        session_class = env->GetObjectClass(session);
        // The APK's bytes, straight into the session: no file is shared.
        jobject out = env->CallObjectMethod(
            session, env->GetMethodID(session_class, "openWrite", "(Ljava/lang/String;JJ)Ljava/io/OutputStream;"),
            env->NewStringUTF("base.apk"), jlong{0}, static_cast<jlong>(st.st_size));
        bool written = !thrown(env, why) && out != nullptr;
        if (written) {
            jclass out_class = env->GetObjectClass(out);
            jmethodID write = env->GetMethodID(out_class, "write", "([BII)V");
            constexpr jsize kChunk = 64 * 1024;
            jbyteArray chunk = env->NewByteArray(kChunk);
            std::array<char, kChunk> buffer{};
            while (written && chunk != nullptr) {
                const ssize_t n = ::read(fd, buffer.data(), buffer.size());
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) {
                    why = apk + ": " + std::strerror(errno);
                    written = false;
                }
                if (n <= 0) break;
                env->SetByteArrayRegion(chunk, 0, static_cast<jsize>(n), reinterpret_cast<const jbyte*>(buffer.data()));
                env->CallVoidMethod(out, write, chunk, 0, static_cast<jint>(n));
                if (thrown(env, why)) written = false;
            }
            if (chunk == nullptr) written = false;
            if (written) {
                env->CallVoidMethod(session, env->GetMethodID(session_class, "fsync", "(Ljava/io/OutputStream;)V"), out);
                if (thrown(env, why)) written = false;
            }
            env->CallVoidMethod(out, env->GetMethodID(out_class, "close", "()V"));
            if (thrown(env, why)) written = false;
        }
        if (written) {
            {
                const std::lock_guard lock(results_mutex_);
                id = next_request_++;
            }
            jobject sender = result_sender(env, id);
            if (!thrown(env, why) && sender != nullptr) {
                env->CallVoidMethod(session,
                                    env->GetMethodID(session_class, "commit", "(Landroid/content/IntentSender;)V"), sender);
                committed = !thrown(env, why);
            }
        }
        if (!committed) {
            env->CallVoidMethod(session, env->GetMethodID(session_class, "abandon", "()V"));
            thrown(env, why);
        } else {
            env->CallVoidMethod(session, env->GetMethodID(session_class, "close", "()V"));
            thrown(env, why);
        }
    }
    env->PopLocalFrame(nullptr);
    ::close(fd);
    if (!committed) {
        if (why.empty()) why = "Android's package installer could not be reached";
        return std::nullopt;
    }
    say("installing", apk);
    auto outcome = wait_for(id);
    if (!outcome) why = "no answer from Android in 10 minutes";
    return outcome;
}

std::optional<Api::Outcome> Api::uninstall(JNIEnv* env, const std::string& package, std::string& why) noexcept {
    if (env->PushLocalFrame(kFrame) != JNI_OK) {
        why = "out of memory";
        return std::nullopt;
    }
    int id = 0;
    bool asked = false;
    jobject installer = package_installer(env);
    if (!thrown(env, why) && installer != nullptr) {
        {
            const std::lock_guard lock(results_mutex_);
            id = next_request_++;
        }
        jobject sender = result_sender(env, id);
        if (!thrown(env, why) && sender != nullptr) {
            env->CallVoidMethod(installer,
                                env->GetMethodID(env->GetObjectClass(installer), "uninstall",
                                                 "(Ljava/lang/String;Landroid/content/IntentSender;)V"),
                                env->NewStringUTF(package.c_str()), sender);
            asked = !thrown(env, why);
        }
    }
    env->PopLocalFrame(nullptr);
    if (!asked) {
        if (why.empty()) why = "Android's package installer could not be reached";
        return std::nullopt;
    }
    say("uninstalling", package);
    auto outcome = wait_for(id);
    if (!outcome) why = "no answer from Android in 10 minutes";
    return outcome;
}

}  // namespace sq_android
