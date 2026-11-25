// sqIDE: the Runner, the model's part that runs a long command (a build, a
// run, a test) without holding the model up: the child's output is a pipe
// the model's loop watches (Telegraph::watch), as the Shell's pty is, so
// the terminal keeps going while sqmake builds.
//
// This file is sqIDE's own (sqterm has none of it).
//
//   run.start  {"cwd": PATH, "argv": ["sqmake", "test"], "title": "Test"}:
//              one at a time; argv[0] from the prefix's bin/ unless a path
//   run.stop   its process group, SIGTERM
//   run.output {"id", "title", "command", "cwd", "state": "running" | "done"
//              | "failed" | "stopped", "status", "output": its last 16 KiB,
//              escapes taken out}
//
// The child gets the environment the Shell gives zsh (environment()), so
// sqmake finds the prefix's toolchain as it does in the terminal.

module;

#include "../sqterm.hpp"

export module sqide:runner;

export namespace sqide {

namespace addresses {
/// Run a command: {"cwd", "argv", "title"}.
inline constexpr sq::messaging::Key<std::string_view, sq::messaging::do_> run{"run.start"};
/// Stop the one running.
inline constexpr sq::messaging::Key<void, sq::messaging::do_> stop{"run.stop"};
/// What it is doing, and what it wrote.
inline constexpr sq::messaging::SnapshotKey output{"run.output"};
}  // namespace addresses

/// The environment a command from the prefix needs, "NAME=value" each: the
/// one the Shell gives zsh (HOME, PREFIX, PATH, TMPDIR, ...).
[[nodiscard]] std::vector<std::string> environment(const sqterm::Places& places);

/// The model's part that runs one long command at a time.
class Runner final {
public:
    static constexpr std::array takes{addresses::run.name(), addresses::stop.name()};

    explicit Runner(sqterm::Places places);
    ~Runner();
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;
    Runner(Runner&&) = delete;
    Runner& operator=(Runner&&) = delete;

    void receive(sq::messaging::Telegraph& t, const sq::messaging::Received& received);
    void readable(sq::messaging::Telegraph& t, int fd);
    [[nodiscard]] bool watches(int fd) const noexcept { return fd >= 0 && fd == out_; }

private:
    void start(sq::messaging::Telegraph& t, std::string_view request);
    void finish(sq::messaging::Telegraph& t);
    void publish(sq::messaging::Telegraph& t);
    void take(std::string_view bytes);

    sqterm::Places places_;
    int child_ = -1;
    int out_ = -1;
    std::uint64_t id_ = 0;
    std::string title_;
    std::string command_;
    std::string cwd_;
    std::string state_ = "idle";
    int status_ = 0;
    std::string output_;
    std::string escape_;  ///< an escape sequence cut by a read
    bool cr_ = false;     ///< a \r last: \r\n is one line end, a lone \r one too
    bool stopped_ = false;
};

}  // namespace sqide
