#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <mutex>

/*
Version 12 client: interactive terminal client.

This client is intentionally still simple, but it now behaves much more like
a real chat terminal:
- one thread receives messages from the server
- the main thread keeps reading user input
- you can type protocol commands interactively
- each command is sent as one newline-terminated line

Supported commands from the user:
- REGISTER <name> <password>
- LOGIN <name> <password>
- CREATE_ROOM <room> <password_or_dash>
- JOIN_ROOM <room> <password_or_dash>
- ROOM_USERS
- SET_ROOM_PASS <room> <new_password_or_dash>
- DELETE_ROOM <room>
- MSG <text>
- PM <user> <text>
- USERS
- ROOMS
- QUIT

Optional command-line usage:
- ./simple_client
- ./simple_client <server_ip>
- ./simple_client <server_ip> <port>
*/

namespace {

struct ClientViewState {
    std::string username = "guest";
    std::string room;
};

std::string current_prompt(const ClientViewState& state) {
    std::string prompt = "[";
    prompt += state.username;
    if (!state.room.empty()) {
        prompt += " @ ";
        prompt += state.room;
    }
    prompt += "] > ";
    return prompt;
}

void print_banner(const std::string& server_ip, int server_port) {
    std::cout << "========================================\n";
    std::cout << "           ChatRoom-CPP Client          \n";
    std::cout << "========================================\n";
    std::cout << "Connected to: " << server_ip << ':' << server_port << "\n\n";
    std::cout << "Account commands\n";
    std::cout << "  REGISTER <name> <password>\n";
    std::cout << "  LOGIN <name> <password>\n";
    std::cout << "  USERS\n\n";
    std::cout << "Room commands\n";
    std::cout << "  CREATE_ROOM <room> <password_or_dash>\n";
    std::cout << "  JOIN_ROOM <room> <password_or_dash>\n";
    std::cout << "  ROOM_USERS\n";
    std::cout << "  ROOMS\n";
    std::cout << "  SET_ROOM_PASS <room> <new_password_or_dash>\n";
    std::cout << "  DELETE_ROOM <room>\n\n";
    std::cout << "Messaging commands\n";
    std::cout << "  MSG <text>\n";
    std::cout << "  PM <user> <text>\n";
    std::cout << "  QUIT\n";
    std::cout << "========================================\n";
}

std::string trim(const std::string& text) {
    std::size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }

    std::size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }

    return text.substr(start, end - start);
}

std::string format_server_line(const std::string& line, ClientViewState& state) {
    std::istringstream input(line);
    std::string command;
    input >> command;

    if (command == "OK") {
        std::string action;
        input >> action;

        if (action == "REGISTER") {
            std::string username;
            input >> username;
            return "[ok] Registered account: " + username;
        }

        if (action == "LOGIN") {
            std::string username;
            input >> username;
            state.username = username;
            state.room.clear();
            return "[ok] Logged in as " + username;
        }

        if (action == "CREATE_ROOM") {
            std::string room;
            input >> room;
            state.room = room;
            return "[ok] Created and joined room: " + room;
        }

        if (action == "JOIN_ROOM") {
            std::string room;
            input >> room;
            state.room = room;
            return "[ok] Joined room: " + room;
        }

        if (action == "SET_ROOM_PASS") {
            std::string room;
            input >> room;
            return "[ok] Updated room password: " + room;
        }

        if (action == "DELETE_ROOM") {
            std::string room;
            input >> room;
            if (state.room == room) {
                state.room.clear();
            }
            return "[ok] Deleted room: " + room;
        }

        if (action == "PM") {
            std::string username;
            input >> username;
            return "[ok] Private message sent to " + username;
        }
    }

    if (command == "ERR") {
        std::string code;
        input >> code;
        return "[error] " + code;
    }

    if (command == "WELCOME") {
        const std::string details = trim(line.substr(command.size()));
        return "[server] " + details;
    }

    if (command == "BYE") {
        state.username = "guest";
        state.room.clear();
        return "[server] Session closed.";
    }

    if (command == "USERS") {
        std::string entry;
        std::string formatted = "[users]";
        bool has_entries = false;
        while (input >> entry) {
            if (!has_entries) {
                formatted += " ";
            } else {
                formatted += ", ";
            }
            formatted += entry;
            has_entries = true;
        }
        if (!has_entries) {
            formatted += " none";
        }
        return formatted;
    }

    if (command == "ROOMS") {
        std::string entry;
        std::string formatted = "[rooms]";
        bool has_entries = false;
        while (input >> entry) {
            if (!has_entries) {
                formatted += " ";
            } else {
                formatted += ", ";
            }
            formatted += entry;
            has_entries = true;
        }
        if (!has_entries) {
            formatted += " none";
        }
        return formatted;
    }

    if (command == "ROOM_USERS") {
        std::string room;
        input >> room;
        std::string user;
        std::string formatted = "[room " + room + "] ";
        bool has_users = false;
        while (input >> user) {
            if (has_users) {
                formatted += ", ";
            }
            formatted += user;
            has_users = true;
        }
        if (!has_users) {
            formatted += "no users";
        }
        return formatted;
    }

    if (command == "MSG") {
        std::string room;
        std::string sender;
        input >> room;
        input >> sender;

        std::string text;
        std::getline(input, text);
        return "[" + room + "] " + sender + ": " + trim(text);
    }

    if (command == "PM") {
        std::string sender;
        input >> sender;

        std::string text;
        std::getline(input, text);
        return "[pm from " + sender + "] " + trim(text);
    }

    return "[server] " + line;
}

void print_prompt(const ClientViewState& state) {
    std::cout << current_prompt(state) << std::flush;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string server_ip = (argc >= 2) ? argv[1] : "127.0.0.1";
    const int server_port = (argc >= 3) ? std::stoi(argv[2]) : 5555;

    const int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd == -1) {
        std::cerr << "socket() failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip.c_str(), &server_address.sin_addr) != 1) {
        std::cerr << "inet_pton() failed: " << std::strerror(errno) << '\n';
        close(client_fd);
        return 1;
    }

    if (connect(client_fd,
                reinterpret_cast<sockaddr*>(&server_address),
                sizeof(server_address)) == -1) {
        std::cerr << "connect() failed: " << std::strerror(errno) << '\n';
        close(client_fd);
        return 1;
    }

    std::mutex output_mutex;
    ClientViewState view_state;

    {
        std::lock_guard<std::mutex> lock(output_mutex);
        print_banner(server_ip, server_port);
    }

    std::atomic<bool> running{true};

    // Reader thread:
    // keeps receiving and printing server messages while the main thread
    // continues to read user input.
    std::thread reader_thread([&running, &output_mutex, &view_state, client_fd]() {
        while (running.load()) {
            char buffer[1024] = {};
            const ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

            if (bytes_received == -1) {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::cerr << "\n[error] recv() failed: " << std::strerror(errno) << '\n';
                running.store(false);
                return;
            }

            if (bytes_received == 0) {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::cout << "\n[server] Connection closed.\n";
                running.store(false);
                return;
            }

            buffer[bytes_received] = '\0';
            std::lock_guard<std::mutex> lock(output_mutex);
            std::istringstream lines(std::string(buffer, static_cast<std::size_t>(bytes_received)));
            std::string line;
            while (std::getline(lines, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                if (line.empty()) {
                    continue;
                }

                std::cout << "\r" << format_server_line(line, view_state) << '\n';
            }
            print_prompt(view_state);
        }
    });

    while (running.load()) {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(output_mutex);
            print_prompt(view_state);
        }
        std::getline(std::cin, message);

        if (!std::cin) {
            break;
        }

        message += '\n';

        const ssize_t bytes_sent = send(client_fd, message.c_str(), message.size(), 0);
        if (bytes_sent == -1) {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cerr << "\n[error] send() failed: " << std::strerror(errno) << '\n';
            running.store(false);
            break;
        }

        if (message == "QUIT\n") {
            break;
        }
    }

    shutdown(client_fd, SHUT_RDWR);
    running.store(false);

    if (reader_thread.joinable()) {
        reader_thread.join();
    }

    close(client_fd);
    return 0;
}
