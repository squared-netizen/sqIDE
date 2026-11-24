// The sqterm-* commands (docs/install-api.md): one program, named for what
// it does (argv[0]), each a client of sqterm's API, the Unix socket the app
// serves at $PREFIX/var/run/sqterm.sock (sq_android/api.cpp).
//
//   sqterm-install APK       install an APK (Android asks the person first),
//                            and wait for Android's answer
//   sqterm-uninstall PACKAGE uninstall an app (Android asks)
//   sqterm-launch PACKAGE    start an installed app (its launcher activity)
//   sqterm-api JSON          one request as given, its response printed
//                            (what the commands are made of; for testing)
//
// Each sends one JSON request (the SDK's tool protocol: squared's
// docs/developer/sdk-tools.md), prints what came back, and exits 0 when the
// app said ok, 1 when it said no, 2 when sqterm's API could not be reached
// (not in sqterm, or an older sqterm).

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "yyjson.h"

namespace {

std::string program = "sqterm";

int usage() {
    std::fprintf(stderr,
                 "usage: sqterm-install APK       install an APK (Android asks first)\n"
                 "       sqterm-uninstall PACKAGE uninstall an app (Android asks)\n"
                 "       sqterm-launch PACKAGE    start an installed app\n"
                 "       sqterm-api JSON          one request to sqterm's API, its response\n");
    return 2;
}

std::string socket_path() {
    const char* prefix = std::getenv("PREFIX");
    std::string p = prefix != nullptr && *prefix != '\0' ? prefix : "/data/data/dev.squared.sqide/files/usr";
    return p + "/var/run/sqterm.sock";
}

/// The request sent, the response read whole: false (said) when the API
/// cannot be reached.
bool ask(const std::string& request, std::string& response) {
    const std::string path = socket_path();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        std::fprintf(stderr, "%s: the socket's path is too long: %s\n", program.c_str(), path.c_str());
        return false;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr,
                     "%s: sqterm's API is not there (%s: %s).\n"
                     "  It runs inside sqterm, from the sqterm that serves it; outside sqterm, use adb.\n",
                     program.c_str(), path.c_str(), std::strerror(errno));
        if (fd >= 0) ::close(fd);
        return false;
    }
    const std::string line = request + "\n";
    std::size_t sent = 0;
    while (sent < line.size()) {
        const ssize_t n = ::write(fd, line.data() + sent, line.size() - sent);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
    ::shutdown(fd, SHUT_WR);
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        response.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return true;
}

/// A request: {"sq": 1, "command": COMMAND, "args": {KEY: VALUE}}, written by
/// yyjson so any text is quoted right.
std::string request_of(const char* command, const char* key, const char* value) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "sq", 1);
    yyjson_mut_obj_add_str(doc, root, "command", command);
    yyjson_mut_val* args = yyjson_mut_obj(doc);
    if (key != nullptr) yyjson_mut_obj_add_str(doc, args, key, value);
    yyjson_mut_obj_add_val(doc, root, "args", args);
    char* text = yyjson_mut_write(doc, 0, nullptr);
    std::string out = text != nullptr ? text : "";
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

/// Exits as the response says: 0 ok, 1 an error (its message said). On ok,
/// `done` is printed, with the result's package and version_code after it
/// when `with_result`.
int outcome(const std::string& response, const char* done, bool with_result = false) {
    yyjson_doc* doc = yyjson_read(response.data(), response.size(), 0);
    yyjson_val* root = doc != nullptr ? yyjson_doc_get_root(doc) : nullptr;
    if (root == nullptr) {
        std::fprintf(stderr, "%s: sqterm's API answered what is not JSON: %s\n", program.c_str(), response.c_str());
        if (doc != nullptr) yyjson_doc_free(doc);
        return 1;
    }
    const bool ok = yyjson_get_bool(yyjson_obj_get(root, "ok"));
    if (ok) {
        if (done != nullptr) {
            yyjson_val* result = yyjson_obj_get(root, "result");
            const char* package = with_result ? yyjson_get_str(yyjson_obj_get(result, "package")) : nullptr;
            yyjson_val* code = with_result ? yyjson_obj_get(result, "version_code") : nullptr;
            std::printf("%s: %s", program.c_str(), done);
            if (package != nullptr) std::printf(" %s", package);
            if (code != nullptr && yyjson_is_int(code))
                std::printf(" (version %lld)", static_cast<long long>(yyjson_get_sint(code)));
            std::printf("\n");
        }
    } else {
        yyjson_val* error = yyjson_obj_get(root, "error");
        const char* message = yyjson_get_str(yyjson_obj_get(error, "message"));
        const char* code = yyjson_get_str(yyjson_obj_get(error, "code"));
        std::fprintf(stderr, "%s: %s%s%s%s\n", program.c_str(), message != nullptr ? message : "failed",
                     code != nullptr ? " (" : "", code != nullptr ? code : "", code != nullptr ? ")" : "");
    }
    yyjson_doc_free(doc);
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string_view name = argc > 0 ? argv[0] : "sqterm";
    if (const auto slash = name.rfind('/'); slash != std::string_view::npos) name.remove_prefix(slash + 1);
    program = std::string(name);
    if (argc >= 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        usage();
        return 0;
    }
    std::string response;
    if (name == "sqterm-launch") {
        if (argc != 2) return usage();
        if (!ask(request_of("launch", "package", argv[1]), response)) return 2;
        const std::string done = std::string("started ") + argv[1];
        return outcome(response, done.c_str());
    }
    if (name == "sqterm-install") {
        if (argc != 2) return usage();
        // The app reads the file itself: an absolute path, whatever the
        // shell's directory.
        char path[PATH_MAX];
        if (::realpath(argv[1], path) == nullptr) {
            std::fprintf(stderr, "%s: %s: %s\n", program.c_str(), argv[1], std::strerror(errno));
            return 1;
        }
        std::fprintf(stderr, "%s: asking Android to install %s (confirm on the screen)\n", program.c_str(), path);
        if (!ask(request_of("install", "apk", path), response)) return 2;
        return outcome(response, "installed", true);
    }
    if (name == "sqterm-uninstall") {
        if (argc != 2) return usage();
        std::fprintf(stderr, "%s: asking Android to uninstall %s (confirm on the screen)\n", program.c_str(), argv[1]);
        if (!ask(request_of("uninstall", "package", argv[1]), response)) return 2;
        return outcome(response, "uninstalled", true);
    }
    if (name == "sqterm-api") {
        if (argc != 2) return usage();
        if (!ask(argv[1], response)) return 2;
        std::fputs(response.c_str(), stdout);
        return outcome(response, nullptr);
    }
    std::fprintf(stderr, "%s: not one of the sqterm-* commands (sqterm-install, sqterm-uninstall, sqterm-launch, sqterm-api)\n", program.c_str());
    return 2;
}
