#pragma once

#include "dd/pipeline.hpp"
#include "dd/store.hpp"

#include <atomic>
#include <string>
#include <thread>

namespace dd::server {

struct Options {
    std::string host = "127.0.0.1";
    int port = 8080;          // 0 asks the OS for an ephemeral port
    std::string web_root = "web";
};

// Minimal HTTP/1.1 server over POSIX sockets: the JSON API plus the static
// UI. One thread accepts, one thread per connection handles it; every
// response closes the connection. Built for a local operator console, not
// for the open internet.
class Server {
public:
    Server(store::Store& store, pipeline::Pipeline& pipeline, Options options);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and starts accepting. Throws dd::Error when the port is taken.
    void start();
    void stop();

    // The bound port; useful when options.port was 0.
    int port() const noexcept { return bound_port_; }

private:
    void accept_loop();
    void handle_connection(int client_fd);

    store::Store& store_;
    pipeline::Pipeline& pipeline_;
    Options options_;
    int listen_fd_ = -1;
    int bound_port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex run_mutex_; // one pipeline run at a time
};

} // namespace dd::server
