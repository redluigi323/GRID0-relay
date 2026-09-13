// One authorized launch, one child. No daemon, password handling or shell commands.
// The GUI owns the control socket. Disconnecting stops and reaps our own child.
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static volatile sig_atomic_t interrupted = 0;
static void stop(int) { interrupted = 1; }
int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "Usage: supervisor socket-path gui-uid relay [args]\n"); return 2; }
    char *end = nullptr; auto uid = strtoul(argv[2], &end, 10);
    if (!end || *end || strlen(argv[1]) >= sizeof(sockaddr_un::sun_path)) return 2;
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, stop); signal(SIGINT, stop);
    int connection = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    strcpy(address.sun_path, argv[1]);
    if (connection < 0 || connect(connection, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
        perror("Cannot connect to desktop app"); return 2;
    }
    uid_t peer; gid_t group;
    if (getpeereid(connection, &peer, &group) || peer != uid) return 2;
    fcntl(connection, F_SETFD, FD_CLOEXEC);
    int logs[2]; if (pipe(logs)) return 2;
    pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
        close(connection); close(logs[0]);
        dup2(logs[1], STDOUT_FILENO); dup2(logs[1], STDERR_FILENO); close(logs[1]);
        int null = open("/dev/null", O_RDONLY); dup2(null, STDIN_FILENO); close(null);
        umask(0022); // Captures are readable within the GUI's private report directory.
        execv(argv[3], argv + 3); perror("Cannot execute relay"); _exit(127);
    }
    close(logs[1]);
    fcntl(connection, F_SETFL, O_NONBLOCK);
    fcntl(logs[0], F_SETFL, O_NONBLOCK);
    bool stopping = false, connected = true, logOpen = true;
    std::string output, commands;
    auto deadline = std::chrono::steady_clock::time_point::max();
    int status = 0;
    for (;;) {
        if (interrupted && !stopping) {
            kill(child, SIGINT); stopping = true;
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        if (stopping && std::chrono::steady_clock::now() > deadline) kill(child, SIGKILL);
        pollfd fds[2]{{connected ? connection : -1, short(POLLIN | (output.empty() ? 0 : POLLOUT)), 0},
                      {logOpen ? logs[0] : -1, POLLIN, 0}};
        poll(fds, 2, 50);
        char buffer[16384];
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(connection, buffer, sizeof(buffer));
            if (n > 0) {
                commands.append(buffer, n);
                if (commands.find("STOP\n") != std::string::npos || commands.size() > 64) interrupted = 1;
            } else if (!n || (errno != EAGAIN && errno != EINTR)) { connected = false; interrupted = 1; }
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n;
            while ((n = read(logs[0], buffer, sizeof(buffer))) > 0) {
                if (connected) output.append(buffer, n);
                if (output.size() > 4 * 1024 * 1024) { connected = false; interrupted = 1; output.clear(); }
            }
            if (n == 0) logOpen = false;
        }
        if (connected && !output.empty()) {
            ssize_t n = send(connection, output.data(), output.size(), 0);
            if (n > 0) output.erase(0, n);
            else if (n < 0 && errno != EAGAIN && errno != EINTR) { connected = false; interrupted = 1; }
        }
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            // Child has exited: drain its remaining output, bounded by the socket buffer.
            ssize_t n;
            while ((n = read(logs[0], buffer, sizeof(buffer))) > 0) output.append(buffer, n);
            for (int tries = 0; connected && !output.empty() && tries < 20; ++tries) {
                n = send(connection, output.data(), output.size(), 0);
                if (n > 0) output.erase(0, n); else { pollfd p{connection, POLLOUT, 0}; poll(&p, 1, 50); }
            }
            break;
        }
        if (result < 0 && errno != EINTR) break;
    }
    close(logs[0]); close(connection);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
