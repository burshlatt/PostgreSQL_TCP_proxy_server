#include <iostream>
#include <vector>
#include <fstream>
#include <unistd.h>
#include <algorithm>
#include <sys/socket.h>

#include "server.hpp"

Server::Server(unsigned port) :
    port_(port),
    log_file_("requests.log", std::ios::app);
{
    s_addr_.sin_family = AF_INET;
    s_addr_.sin_addr.s_addr = INADDR_ANY;
    s_addr_.sin_port = htons(port_);
}

Server::~Server() {
    close(s_socket_);
    close(epoll_fd_);

    for (auto& [client_fd, pgsql_fd] : pgsql_sockets_) {
        close(pgsql_fd);
    }
}

int Server::ConnectToPGSQL() {
    int pgsql_socket{socket(AF_INET, SOCK_STREAM, 0)};

    struct sockaddr_in server_addr;

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5432);

    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        close(pgsql_socket);
        throw std::runtime_error("Error connecting to postgresql srver!");
    }

    auto casted_addr{reinterpret_cast<struct sockaddr*>(&server_addr)};

    if (connect(pgsql_socket, casted_addr, sizeof(server_addr))) {
        close(pgsql_socket);
        throw std::runtime_error("Error connecting to postgresql srver!");
    }

    return pgsql_socket;
}

void Server::SetupSocket() {
    s_socket_ = socket(AF_INET, SOCK_STREAM, 0);

    if (s_socket_ == -1) {
        throw std::runtime_error("Error: socket()");
    }

    int opt{1};

    if (setsockopt(s_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        throw std::runtime_error("Error: setsockopt()");
    }
}

void Server::SetupEpoll() {
    epoll_fd_ = epoll_create1(0);
    
    if (epoll_fd_ == -1) {
        throw std::runtime_error("Error: epoll_create1()");
    }

    struct epoll_event event;

    event.events = EPOLLIN;
    event.data.fd = s_socket_;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, s_socket_, &event) == -1) {
        throw std::runtime_error("Error: epoll_ctl()");
    }
}

bool Server::AcceptNewConnection(epoll_event& event) {
    struct sockaddr_in c_addr;
    socklen_t c_addr_len{sizeof(c_addr)};
    auto casted_c_addr{reinterpret_cast<struct sockaddr*>(&c_addr)};
    int c_socket{accept(s_socket_, casted_c_addr, &c_addr_len)};

    if (c_socket == -1) {
        std::cerr << "Error: Failed to accept connection\n";
    } else {
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = c_socket;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, c_socket, &event) == -1) {
            std::cerr << "Error: Failed to add client socket to epoll\n";
            close(c_socket);
            return false;
        }
    }

    return true;
}

bool Server::IsSQLRequest(std::string_view request) {
    static const std::vector<std::string> tokens{
        "BEGIN", "COMMIT", "INSERT",
        "SELECT", "UPDATE", "DELETE"
    };

    for (const auto& token : tokens) {
        if (request.find(token) != std::string::npos) {
            return true;
        }
    }

    return false;
}

std::string Server::GetSQLRequest(std::string_view request) {
    static const std::vector<std::string> tokens{
        "BEGIN", "COMMIT", "INSERT",
        "SELECT", "UPDATE", "DELETE"
    };

    std::vector<int> positions(tokens.size());

    for (const auto& token : tokens) {
        if (auto pos{request.find(token)}; pos != std::string::npos) {
            positions.push_back(pos);
        }
    }

    request.remove_prefix(*std::min_element(positions.begin(), positions.end()));

    return std::string(request).substr(0, request.find('\0'));
}

void Server::SaveLogs(std::string_view request) {
    if (!IsSQLRequest(request)) {
        return;
    }

    std::string sql_req(std::move(GetSQLRequest(request)));

    std::lock_guard<std::mutex> lock(mutex_);

    log_file_ << sql_req << '\n';
}

ssize_t Server::SendAll(int fd, const char* buf, size_t len) {
    size_t total{};
    ssize_t bytes{};

    while (total < len) {
        bytes = send(fd, buf + total, len - total, 0);

        if (bytes == -1) {
            break;
        }

        total += bytes;
    }

    return (bytes == -1 ? -1 : total);
}

void Server::HandleClientEvent(epoll_event& event) {
    char buffer[max_buffer_size_];
    ssize_t bytes_read{recv(event.data.fd, buffer, max_buffer_size_, 0)};

    if (bytes_read <= 0) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, event.data.fd, NULL);
        close(event.data.fd);
        close(pgsql_sockets_[event.data.fd]);
        pgsql_sockets_.erase(event.data.fd);
    } else {
        SaveLogs(std::string(buffer, bytes_read));

        if (SendAll(pgsql_sockets_[event.data.fd], buffer, bytes_read) < 0) {
            throw std::runtime_error("Error: send()");
        }

        memset(buffer, 0, sizeof(buffer));

        bytes_read = recv(pgsql_sockets_[event.data.fd], buffer, max_buffer_size_, 0);

        if (bytes_read <= 0) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, event.data.fd, NULL);
            close(event.data.fd);
            close(pgsql_sockets_[event.data.fd]);
            pgsql_sockets_.erase(event.data.fd);
        } else {           
            SaveLogs(std::string(buffer, bytes_read));

            if (SendAll(event.data.fd, buffer, bytes_read) < 0) {
                throw std::runtime_error("Error: send()");
            }
        }
    }
}

bool Server::IsSSLRequest(char* buffer) {
    static constexpr int ssl_request_code{0x04d2162f};

    char tmp_buffer[8]{
        buffer[0], buffer[1], buffer[2], buffer[3],
        buffer[4], buffer[5], buffer[6], buffer[7],
    };

    uint32_t msg_len{ntohl(*reinterpret_cast<int*>(tmp_buffer))};
    uint32_t ssl_code{ntohl(*reinterpret_cast<int*>(tmp_buffer + 4))};

    if (msg_len == 8 && ssl_code == ssl_request_code) {
        return true;
    }

    return false;
}

void Server::DisableSSL(epoll_event& event) {
    char buffer[max_buffer_size_];
    ssize_t bytes_read{recv(event.data.fd, buffer, max_buffer_size_, 0)};

    if (bytes_read > 0 && IsSSLRequest(buffer)) {
        if (send(event.data.fd, "N", 1, 0) < 0) {
            throw std::runtime_error("Error: send()");
        }
    }
}

void Server::EventLoop() {
    std::vector<struct epoll_event> events(max_events_);

    while (true) {
        int num_events{epoll_wait(epoll_fd_, events.data(), max_events_, -1)};

        for (int i{}; i < num_events; ++i) {
            if (events[i].data.fd == s_socket_) {
                if (AcceptNewConnection(events[i])) {
                    DisableSSL(events[i]);
                    pgsql_sockets_[events[i].data.fd] = ConnectToPGSQL();
                }
            } else {
                HandleClientEvent(events[i]);
            }
        }
    }
}

void Server::Start() {
    SetupSocket();

    auto casted_s_addr{reinterpret_cast<struct sockaddr*>(&s_addr_)};

    if (bind(s_socket_, casted_s_addr, sizeof(s_addr_)) == -1) {
        throw std::runtime_error("Error: bind()");
    }

    if (listen(s_socket_, SOMAXCONN) == -1) {
        throw std::runtime_error("Error: listen()");
    }

    SetupEpoll();
    EventLoop();
}
