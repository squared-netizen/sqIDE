// sqIDE: the Runner (runner.cppm says what it is).
//
// This file is sqIDE's own (sqterm has none of it).

module;

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../sqterm.hpp"

module sqide;

namespace sqide {

namespace {

/// What output keeps: its last 16 KiB, cut back to 12 at a line's start.
constexpr std::size_t output_most = 16U * 1024U;
constexpr std::size_t output_kept = 12U * 1024U;

}  // namespace

std::vector<std::string> environment(const sqterm::Places& places) {
    const std::string bin = places.prefix.empty() ? std::string() : places.prefix + "/bin:";
    // As the Shell's (shell.cpp), but no terminal: TERM=dumb, so a tool
    // that colours for a terminal does not.
    const char* certificates = ::access("/apex/com.android.conscrypt/cacerts", R_OK) == 0
                                   ? "/apex/com.android.conscrypt/cacerts"
                                   : "/system/etc/security/cacerts";
    return {
        "TERM=dumb",
        "LANG=en_US.UTF-8",
        "HOME=" + places.home,
        "TMPDIR=" + places.tmp,
        "PREFIX=" + places.prefix,
        "PATH=" + bin + "/system/bin:/system/xbin",
        "SQTERM=1",
        "ANDROID_DATA=/data",
        "ANDROID_ROOT=/system",
        std::string("SSL_CERT_DIR=") + certificates,
    };
}

Runner::Runner(sqterm::Places places) : places_(std::move(places)) {}

Runner::~Runner() {
    if (child_ > 0) {
        ::kill(-child_, SIGTERM);
        ::waitpid(child_, nullptr, 0);
    }
    if (out_ >= 0) {
        ::close(out_);
    }
}

void Runner::receive(sq::messaging::Telegraph& t, const sq::messaging::Received& received) {
    const sq::messaging::Name to = received.telegram.address().name;
    if (to == addresses::run.name()) {
        start(t, {reinterpret_cast<const char*>(received.data.data()), received.data.size()});
    } else if (to == addresses::stop.name() && child_ > 0) {
        stopped_ = true;
        ::kill(-child_, SIGTERM);  // its group: sqmake's compilers too
    }
}

void Runner::start(sq::messaging::Telegraph& t, std::string_view request) {
    if (child_ > 0) {
        return;  // one at a time: the UI offers Stop meanwhile
    }
    const auto parsed = sq::data::parse_json(request);
    const sq::data::JsonValue* list = parsed ? parsed.value.find("argv") : nullptr;
    std::vector<std::string> argv;
    if (list != nullptr && list->array_if() != nullptr) {
        for (const auto& a : *list->array_if()) {
            argv.push_back(text_of(&a));
        }
    }
    ++id_;
    title_ = parsed ? text_of(parsed.value.find("title")) : std::string();
    cwd_ = parsed ? text_of(parsed.value.find("cwd")) : std::string();
    command_.clear();
    for (const std::string& a : argv) {
        command_ += (command_.empty() ? "" : " ") + a;
    }
    output_.clear();
    escape_.clear();
    cr_ = false;
    stopped_ = false;
    status_ = 0;
    if (argv.empty() || argv.front().empty()) {
        state_ = "failed";
        output_ = "nothing to run\n";
        publish(t);
        return;
    }
    // Everything the child needs, made before fork: after it, only calls
    // that are safe in a child of a threaded process.
    const std::string program = argv.front().find('/') == std::string::npos
                                    ? places_.prefix + "/bin/" + argv.front()
                                    : argv.front();
    std::vector<char*> args;
    for (std::string& a : argv) {
        args.push_back(a.data());
    }
    args.push_back(nullptr);
    std::vector<std::string> env_text = environment(places_);
    std::vector<char*> env;
    for (std::string& e : env_text) {
        env.push_back(e.data());
    }
    env.push_back(nullptr);
    const std::string failed = "sqIDE: cannot run " + program + "\n";

    std::array<int, 2> pipe_ends{-1, -1};
    if (::pipe2(pipe_ends.data(), O_CLOEXEC) != 0) {
        state_ = "failed";
        output_ = "sqIDE: no pipe for its output\n";
        publish(t);
        return;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setpgid(0, 0);  // its own group, so Stop reaches what it starts
        const int nothing = ::open("/dev/null", O_RDONLY);
        if (nothing >= 0) {
            ::dup2(nothing, 0);
        }
        ::dup2(pipe_ends[1], 1);
        ::dup2(pipe_ends[1], 2);
        if (!cwd_.empty() && ::chdir(cwd_.c_str()) != 0) {
            static_cast<void>(::write(2, failed.data(), failed.size()));
            ::_exit(126);
        }
        ::execve(program.c_str(), args.data(), env.data());
        static_cast<void>(::write(2, failed.data(), failed.size()));
        ::_exit(127);
    }
    ::close(pipe_ends[1]);
    if (pid < 0) {
        ::close(pipe_ends[0]);
        state_ = "failed";
        output_ = "sqIDE: cannot start a process\n";
        publish(t);
        return;
    }
    ::setpgid(pid, pid);  // as the child does: whichever comes first
    child_ = pid;
    out_ = pipe_ends[0];
    ::fcntl(out_, F_SETFL, ::fcntl(out_, F_GETFL) | O_NONBLOCK);
    if (!t.watch(out_)) {
        ::kill(-child_, SIGTERM);
        take("sqIDE: cannot watch its output\n");
        finish(t);
        return;
    }
    state_ = "running";
    publish(t);
}

void Runner::readable(sq::messaging::Telegraph& t, int fd) {
    if (fd != out_) {
        return;
    }
    std::array<char, 8192> buffer{};
    for (;;) {
        const ssize_t n = ::read(out_, buffer.data(), buffer.size());
        if (n > 0) {
            take({buffer.data(), static_cast<std::size_t>(n)});
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;  // all of it, for now
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        finish(t);  // the end of its output: it has ended
        return;
    }
    publish(t);
}

void Runner::finish(sq::messaging::Telegraph& t) {
    if (out_ >= 0) {
        t.unwatch(out_);
        ::close(out_);
        out_ = -1;
    }
    if (child_ > 0) {
        int raw = 0;
        while (::waitpid(child_, &raw, 0) < 0 && errno == EINTR) {
        }
        status_ = WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw);
        child_ = -1;
    }
    state_ = stopped_ ? "stopped" : status_ == 0 ? "done" : "failed";
    publish(t);
}

void Runner::take(std::string_view bytes) {
    for (const char c : bytes) {
        // Escape sequences (colours, cursor moves) are a terminal's: left out.
        if (!escape_.empty()) {
            if (escape_ == "\x1b") {
                escape_ = c == '[' || c == ']' ? std::string(1, c) : std::string();
            } else if (escape_ == "[") {
                if (c >= 0x40 && c <= 0x7e) {
                    escape_.clear();  // the final byte
                }
            } else if (c == '\a' || c == '\x1b') {
                escape_.clear();  // an OSC's end
            }
            continue;
        }
        if (c == '\x1b') {
            escape_ = "\x1b";
            continue;
        }
        if (cr_) {
            cr_ = false;
            output_ += '\n';  // \r\n, or a \r alone (a progress line): one line end
            if (c == '\n') {
                continue;
            }
        }
        if (c == '\r') {
            cr_ = true;
            continue;
        }
        output_ += c;
    }
    if (output_.size() > output_most) {
        std::size_t from = output_.size() - output_kept;
        if (const std::size_t nl = output_.find('\n', from); nl != std::string::npos) {
            from = nl + 1;
        }
        output_.erase(0, from);
    }
}

void Runner::publish(sq::messaging::Telegraph& t) {
    const std::string text =
        "{\"id\": " + std::to_string(id_) + ", \"title\": " + json_string(title_) +
        ", \"command\": " + json_string(command_) + ", \"cwd\": " + json_string(cwd_) +
        ", \"state\": " + json_string(state_) + ", \"status\": " + std::to_string(status_) +
        ", \"output\": " + json_string(output_) + "}";
    const std::span<std::byte> room = t.snapshot(addresses::output, text.size());
    if (room.size() < text.size()) {
        return;
    }
    std::memcpy(room.data(), text.data(), text.size());
    static_cast<void>(t.publish(addresses::output, text.size()));
}

}  // namespace sqide
