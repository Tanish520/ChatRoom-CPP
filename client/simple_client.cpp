#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <iostream>
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

    const std::string prompt = "chat> ";
    std::mutex output_mutex;

    {
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << "\n=== ChatRoom-CPP Client ===\n";
        std::cout << "Connected to " << server_ip << ':' << server_port << '\n';
        std::cout << "Commands:\n";
        std::cout << "  REGISTER <name> <password>\n";
        std::cout << "  LOGIN <name> <password>\n";
        std::cout << "  CREATE_ROOM <room> <password_or_dash>\n";
        std::cout << "  JOIN_ROOM <room> <password_or_dash>\n";
        std::cout << "  ROOM_USERS\n";
        std::cout << "  SET_ROOM_PASS <room> <new_password_or_dash>\n";
        std::cout << "  DELETE_ROOM <room>\n";
        std::cout << "  MSG <text>\n";
        std::cout << "  PM <user> <text>\n";
        std::cout << "  USERS\n";
        std::cout << "  ROOMS\n";
        std::cout << "  QUIT\n\n";
    }

    std::atomic<bool> running{true};

    // Reader thread:
    // keeps receiving and printing server messages while the main thread
    // continues to read user input.
    std::thread reader_thread([&running, &output_mutex, &prompt, client_fd]() {
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
            std::cout << "\r[server] " << buffer;
            if (buffer[bytes_received - 1] != '\n') {
                std::cout << '\n';
            }
            std::cout << '\n';
            std::cout << prompt << std::flush;
        }
    });

    while (running.load()) {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cout << prompt << std::flush;
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
