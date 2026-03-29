#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

#ifdef __linux__

#include <algorithm>
#include <condition_variable>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <queue>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
Version 12: owner-controlled room management.

Version 6 gave us the architectural core:
- epoll for readiness
- thread pool for bounded parallel processing

Version 12 keeps the Version 11 authentication model and adds:
- REGISTER <username> <password>
- LOGIN <username> <password>
- CREATE_ROOM <room> <password_or_dash>
- JOIN_ROOM <room> <password_or_dash>
- ROOM_USERS
- SET_ROOM_PASS <room> <new_password_or_dash>
- DELETE_ROOM <room>
- MSG <message>
- PM <username> <message>
- USERS
- ROOMS
- QUIT

This version upgrades authorization semantics:
- rooms now have real owner-only controls
- owners can change room protection
- owners can delete their rooms
*/

namespace {

constexpr int kPort = 5555;
constexpr int kBacklog = 16;
constexpr int kMaxEvents = 16;
constexpr std::size_t kBufferSize = 1024;
constexpr std::size_t kWorkerCount = 4;

struct ClientSession {
    std::string username;
    std::string room;
    std::string input_buffer;
};

struct AccountInfo {
    std::string salt;
    std::size_t password_hash = 0;
};

struct RoomInfo {
    std::string owner_username;
    bool is_protected = false;
    std::string salt;
    std::size_t password_hash = 0;
};

void print_errno(const std::string& context) {
    std::cerr << context << ": " << std::strerror(errno) << '\n';
}

bool set_non_blocking(int fd) {
    const int current_flags = fcntl(fd, F_GETFL, 0);
    if (current_flags == -1) {
        print_errno("fcntl(F_GETFL) failed");
        return false;
    }

    if (fcntl(fd, F_SETFL, current_flags | O_NONBLOCK) == -1) {
        print_errno("fcntl(F_SETFL, O_NONBLOCK) failed");
        return false;
    }

    return true;
}

bool add_to_epoll(int epoll_fd, int target_fd, uint32_t events) {
    epoll_event event{};
    event.events = events;
    event.data.fd = target_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, target_fd, &event) == -1) {
        print_errno("epoll_ctl(ADD) failed");
        return false;
    }

    return true;
}

void close_client(int epoll_fd, int client_fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
    close(client_fd);
}

bool send_text(int client_fd, const std::string& text) {
    const ssize_t bytes_sent = send(client_fd, text.c_str(), text.size(), 0);
    if (bytes_sent == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }

        print_errno("send() failed");
        return false;
    }

    return true;
}

std::size_t hash_with_salt(const std::string& salt, const std::string& secret) {
    return std::hash<std::string>{}(salt + ":" + secret);
}

class ThreadPool {
public:
    explicit ThreadPool(std::size_t worker_count) {
        workers_.reserve(worker_count);

        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this]() {
                while (true) {
                    std::function<void()> task;

                    {
                        std::unique_lock<std::mutex> lock(queue_mutex_);
                        queue_cv_.wait(lock, [this]() {
                            return stop_ || !tasks_.empty();
                        });

                        if (stop_ && tasks_.empty()) {
                            return;
                        }

                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }

                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }

        queue_cv_.notify_all();

        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            tasks_.push(std::move(task));
        }

        queue_cv_.notify_one();
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    bool stop_ = false;
};

// Shared chat state for Version 11.
//
// We now have real multi-client semantics, so the server must remember:
// - registered accounts and their password hashes
// - which FD belongs to which authenticated username
// - which rooms exist and whether they are protected
// - which room a logged-in user is currently in
class ChatState {
public:
    void add_client(int fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[fd] = ClientSession{};
    }

    void remove_client(int fd) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = sessions_.find(fd);
        if (it != sessions_.end()) {
            if (!it->second.username.empty()) {
                usernames_.erase(it->second.username);
            }

            sessions_.erase(it);
        }
    }

    bool register_user(
        int fd,
        const std::string& username,
        const std::string& password,
        std::string& response
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (username.empty()) {
            response = "ERR username_required\n";
            return false;
        }

        if (password.empty()) {
            response = "ERR password_required\n";
            return false;
        }

        auto session_it = sessions_.find(fd);
        if (session_it == sessions_.end()) {
            response = "ERR unknown_client\n";
            return false;
        }

        if (!session_it->second.username.empty()) {
            response = "ERR already_logged_in\n";
            return false;
        }

        if (accounts_.count(username) > 0) {
            response = "ERR username_taken\n";
            return false;
        }

        const std::string salt = "user-" + std::to_string(++next_salt_id_);
        accounts_[username] = AccountInfo{salt, hash_with_salt(salt, password)};
        response = "OK REGISTER " + username + "\n";
        return true;
    }

    bool login(
        int fd,
        const std::string& username,
        const std::string& password,
        std::string& response
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (username.empty()) {
            response = "ERR username_required\n";
            return false;
        }

        if (password.empty()) {
            response = "ERR password_required\n";
            return false;
        }

        auto it = sessions_.find(fd);
        if (it == sessions_.end()) {
            response = "ERR unknown_client\n";
            return false;
        }

        if (!it->second.username.empty()) {
            response = "ERR already_logged_in\n";
            return false;
        }

        const auto account_it = accounts_.find(username);
        if (account_it == accounts_.end()) {
            response = "ERR invalid_login\n";
            return false;
        }

        if (account_it->second.password_hash != hash_with_salt(account_it->second.salt, password)) {
            response = "ERR invalid_login\n";
            return false;
        }

        if (usernames_.count(username) > 0) {
            response = "ERR already_logged_in_elsewhere\n";
            return false;
        }

        it->second.username = username;
        usernames_.insert(username);
        response = "OK LOGIN " + username + "\n";
        return true;
    }

    bool get_username(int fd, std::string& username) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(fd);
        if (it == sessions_.end() || it->second.username.empty()) {
            return false;
        }

        username = it->second.username;
        return true;
    }

    bool create_room(
        int fd,
        const std::string& room_name,
        const std::string& room_password_token,
        std::string& response
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (room_name.empty()) {
            response = "ERR room_required\n";
            return false;
        }

        if (room_password_token.empty()) {
            response = "ERR room_password_token_required\n";
            return false;
        }

        auto it = sessions_.find(fd);
        if (it == sessions_.end()) {
            response = "ERR unknown_client\n";
            return false;
        }

        if (it->second.username.empty()) {
            response = "ERR login_required\n";
            return false;
        }

        if (rooms_.count(room_name) > 0) {
            response = "ERR room_exists\n";
            return false;
        }

        RoomInfo room;
        room.owner_username = it->second.username;
        room.is_protected = (room_password_token != "-");
        if (room.is_protected) {
            room.salt = "room-" + std::to_string(++next_salt_id_);
            room.password_hash = hash_with_salt(room.salt, room_password_token);
        }

        rooms_[room_name] = room;
        it->second.room = room_name;
        response = "OK CREATE_ROOM " + room_name + "\n";
        return true;
    }

    bool join_room(
        int fd,
        const std::string& room_name,
        const std::string& room_password_token,
        std::string& response
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (room_name.empty()) {
            response = "ERR room_required\n";
            return false;
        }

        auto session_it = sessions_.find(fd);
        if (session_it == sessions_.end()) {
            response = "ERR unknown_client\n";
            return false;
        }

        if (session_it->second.username.empty()) {
            response = "ERR login_required\n";
            return false;
        }

        const auto room_it = rooms_.find(room_name);
        if (room_it == rooms_.end()) {
            response = "ERR room_not_found\n";
            return false;
        }

        if (room_it->second.is_protected) {
            if (room_password_token == "-" || room_password_token.empty()) {
                response = "ERR room_password_required\n";
                return false;
            }

            if (room_it->second.password_hash !=
                hash_with_salt(room_it->second.salt, room_password_token)) {
                response = "ERR room_auth_failed\n";
                return false;
            }
        }

        session_it->second.room = room_name;
        response = "OK JOIN_ROOM " + room_name + "\n";
        return true;
    }

    bool set_room_password(
        int fd,
        const std::string& room_name,
        const std::string& new_password_token,
        std::string& response
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (room_name.empty()) {
            response = "ERR room_required\n";
            return false;
        }

        if (new_password_token.empty()) {
            response = "ERR room_password_token_required\n";
            return false;
        }

        const auto session_it = sessions_.find(fd);
        if (session_it == sessions_.end()) {
            response = "ERR unknown_client\n";
            return false;
        }

        if (session_it->second.username.empty()) {
            response = "ERR login_required\n";
            return false;
        }

        auto room_it = rooms_.find(room_name);
        if (room_it == rooms_.end()) {
            response = "ERR room_not_found\n";
            return false;
        }

        if (room_it->second.owner_username != session_it->second.username) {
            response = "ERR owner_required\n";
            return false;
        }

        room_it->second.is_protected = (new_password_token != "-");
        if (room_it->second.is_protected) {
            room_it->second.salt = "room-" + std::to_string(++next_salt_id_);
            room_it->second.password_hash = hash_with_salt(room_it->second.salt, new_password_token);
        } else {
            room_it->second.salt.clear();
            room_it->second.password_hash = 0;
        }

        response = "OK SET_ROOM_PASS " + room_name + "\n";
        return true;
    }

    bool delete_room(int fd, const std::string& room_name, std::string& response) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (room_name.empty()) {
            response = "ERR room_required\n";
            return false;
        }

        const auto session_it = sessions_.find(fd);
        if (session_it == sessions_.end()) {
            response = "ERR unknown_client\n";
            return false;
        }

        if (session_it->second.username.empty()) {
            response = "ERR login_required\n";
            return false;
        }

        const auto room_it = rooms_.find(room_name);
        if (room_it == rooms_.end()) {
            response = "ERR room_not_found\n";
            return false;
        }

        if (room_it->second.owner_username != session_it->second.username) {
            response = "ERR owner_required\n";
            return false;
        }

        for (auto& [session_fd, session] : sessions_) {
            if (session.room == room_name) {
                session.room.clear();
            }
        }

        rooms_.erase(room_name);
        response = "OK DELETE_ROOM " + room_name + "\n";
        return true;
    }

    bool get_room(int fd, std::string& room_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(fd);
        if (it == sessions_.end() || it->second.room.empty()) {
            return false;
        }

        room_name = it->second.room;
        return true;
    }

    std::string build_users_response() {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::string> names;
        names.reserve(usernames_.size());

        for (const std::string& username : usernames_) {
            names.push_back(username);
        }

        std::sort(names.begin(), names.end());

        std::string response = "USERS";
        for (const std::string& name : names) {
            response += " " + name;
        }
        response += "\n";
        return response;
    }

    std::string build_room_users_response(int fd) {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto session_it = sessions_.find(fd);
        if (session_it == sessions_.end()) {
            return "ERR unknown_client\n";
        }

        if (session_it->second.username.empty()) {
            return "ERR login_required\n";
        }

        if (session_it->second.room.empty()) {
            return "ERR join_required\n";
        }

        std::vector<std::string> names;
        for (const auto& [session_fd, session] : sessions_) {
            if (!session.username.empty() && session.room == session_it->second.room) {
                names.push_back(session.username);
            }
        }

        std::sort(names.begin(), names.end());

        std::string response = "ROOM_USERS " + session_it->second.room;
        for (const std::string& name : names) {
            response += " " + name;
        }
        response += "\n";
        return response;
    }

    std::string build_rooms_response() {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::string> room_names;
        room_names.reserve(rooms_.size());

        for (const auto& [room_name, room_info] : rooms_) {
            room_names.push_back(
                room_name + (room_info.is_protected ? ":protected" : ":public")
            );
        }

        std::sort(room_names.begin(), room_names.end());

        std::string response = "ROOMS";
        for (const std::string& room : room_names) {
            response += " " + room;
        }
        response += "\n";
        return response;
    }

    std::vector<int> clients_in_room(const std::string& room_name) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<int> fds;
        for (const auto& [fd, session] : sessions_) {
            if (!session.username.empty() && session.room == room_name) {
                fds.push_back(fd);
            }
        }

        return fds;
    }

    bool prepare_private_message(
        int sender_fd,
        const std::string& target_username,
        const std::string& text,
        int& target_fd,
        std::string& target_message,
        std::string& sender_response
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (target_username.empty()) {
            sender_response = "ERR target_required\n";
            return false;
        }

        const auto sender_it = sessions_.find(sender_fd);
        if (sender_it == sessions_.end()) {
            sender_response = "ERR unknown_client\n";
            return false;
        }

        if (sender_it->second.username.empty()) {
            sender_response = "ERR login_required\n";
            return false;
        }

        int found_target_fd = -1;
        for (const auto& [fd, session] : sessions_) {
            if (session.username == target_username) {
                found_target_fd = fd;
                break;
            }
        }

        if (found_target_fd == -1) {
            sender_response = "ERR user_not_found\n";
            return false;
        }

        target_fd = found_target_fd;
        target_message = "PM " + sender_it->second.username + " " + text + "\n";
        sender_response = "OK PM " + target_username + "\n";
        return true;
    }

    std::vector<std::string> append_input_and_take_commands(int fd, const std::string& chunk) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::string> commands;
        auto it = sessions_.find(fd);
        if (it == sessions_.end()) {
            return commands;
        }

        it->second.input_buffer += chunk;

        while (true) {
            const std::size_t newline_pos = it->second.input_buffer.find('\n');
            if (newline_pos == std::string::npos) {
                break;
            }

            std::string command = it->second.input_buffer.substr(0, newline_pos);
            if (!command.empty() && command.back() == '\r') {
                command.pop_back();
            }

            commands.push_back(command);
            it->second.input_buffer.erase(0, newline_pos + 1);
        }

        return commands;
    }

private:
    std::unordered_map<int, ClientSession> sessions_;
    std::unordered_map<std::string, AccountInfo> accounts_;
    std::unordered_set<std::string> usernames_;
    std::unordered_map<std::string, RoomInfo> rooms_;
    std::mutex mutex_;
    std::size_t next_salt_id_ = 0;
};

std::string trim_trailing_newlines(const std::string& text) {
    std::string cleaned = text;
    while (!cleaned.empty() && (cleaned.back() == '\n' || cleaned.back() == '\r')) {
        cleaned.pop_back();
    }
    return cleaned;
}

bool process_command(int epoll_fd, int client_fd, ChatState& chat_state, const std::string& message) {
    std::istringstream input(message);
    std::string command;
    input >> command;

    if (command.empty()) {
        return true;
    }

    if (command == "REGISTER") {
        std::string username;
        std::string password;
        input >> username;
        input >> password;

        std::string response;
        chat_state.register_user(client_fd, username, password, response);
        send_text(client_fd, response);
        return true;
    }

    if (command == "LOGIN") {
        std::string username;
        std::string password;
        input >> username;
        input >> password;

        std::string response;
        chat_state.login(client_fd, username, password, response);
        send_text(client_fd, response);
        return true;
    }

    if (command == "CREATE_ROOM") {
        std::string room_name;
        std::string password_token;
        input >> room_name;
        input >> password_token;

        std::string response;
        chat_state.create_room(client_fd, room_name, password_token, response);
        send_text(client_fd, response);
        return true;
    }

    if (command == "JOIN_ROOM") {
        std::string room_name;
        std::string password_token;
        input >> room_name;
        input >> password_token;

        std::string response;
        chat_state.join_room(client_fd, room_name, password_token, response);
        send_text(client_fd, response);
        return true;
    }

    if (command == "JOIN") {
        std::string room_name;
        input >> room_name;

        std::string response;
        chat_state.join_room(client_fd, room_name, "-", response);
        send_text(client_fd, response);
        return true;
    }

    if (command == "USERS") {
        send_text(client_fd, chat_state.build_users_response());
        return true;
    }

    if (command == "ROOM_USERS") {
        send_text(client_fd, chat_state.build_room_users_response(client_fd));
        return true;
    }

    if (command == "SET_ROOM_PASS") {
        std::string room_name;
        std::string password_token;
        input >> room_name;
        input >> password_token;

        std::string response;
        chat_state.set_room_password(client_fd, room_name, password_token, response);
        send_text(client_fd, response);
        return true;
    }

    if (command == "DELETE_ROOM") {
        std::string room_name;
        input >> room_name;

        std::string response;
        chat_state.delete_room(client_fd, room_name, response);
        send_text(client_fd, response);
        return true;
    }

    if (command == "ROOMS") {
        send_text(client_fd, chat_state.build_rooms_response());
        return true;
    }

    if (command == "MSG") {
        std::string sender_name;
        if (!chat_state.get_username(client_fd, sender_name)) {
            send_text(client_fd, "ERR login_required\n");
            return true;
        }

        std::string sender_room;
        if (!chat_state.get_room(client_fd, sender_room)) {
            send_text(client_fd, "ERR join_required\n");
            return true;
        }

        std::string text;
        std::getline(input, text);
        if (!text.empty() && text.front() == ' ') {
            text.erase(text.begin());
        }

        if (text.empty()) {
            send_text(client_fd, "ERR message_required\n");
            return true;
        }

        const std::string broadcast =
            "MSG " + sender_room + " " + sender_name + " " + text + "\n";
        for (int room_client_fd : chat_state.clients_in_room(sender_room)) {
            send_text(room_client_fd, broadcast);
        }
        return true;
    }

    if (command == "PM") {
        std::string target_username;
        input >> target_username;

        std::string text;
        std::getline(input, text);
        if (!text.empty() && text.front() == ' ') {
            text.erase(text.begin());
        }

        if (text.empty()) {
            send_text(client_fd, "ERR message_required\n");
            return true;
        }

        int target_fd = -1;
        std::string target_message;
        std::string sender_response;
        if (!chat_state.prepare_private_message(
                client_fd,
                target_username,
                text,
                target_fd,
                target_message,
                sender_response)) {
            send_text(client_fd, sender_response);
            return true;
        }

        send_text(target_fd, target_message);
        send_text(client_fd, sender_response);
        return true;
    }

    if (command == "QUIT") {
        send_text(client_fd, "BYE\n");
        chat_state.remove_client(client_fd);
        close_client(epoll_fd, client_fd);
        return false;
    }

    send_text(client_fd, "ERR unknown_command\n");
    return true;
}

}  // namespace

int main() {
    const int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        print_errno("socket() failed");
        return 1;
    }

    if (!set_non_blocking(server_fd)) {
        close(server_fd);
        return 1;
    }

    const int reuse_addr = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) == -1) {
        print_errno("setsockopt(SO_REUSEADDR) failed");
        close(server_fd);
        return 1;
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(kPort);
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd,
             reinterpret_cast<sockaddr*>(&server_address),
             sizeof(server_address)) == -1) {
        print_errno("bind() failed");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, kBacklog) == -1) {
        print_errno("listen() failed");
        close(server_fd);
        return 1;
    }

    const int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        print_errno("epoll_create1() failed");
        close(server_fd);
        return 1;
    }

    if (!add_to_epoll(epoll_fd, server_fd, EPOLLIN)) {
        close(epoll_fd);
        close(server_fd);
        return 1;
    }

    ThreadPool thread_pool(kWorkerCount);
    ChatState chat_state;

    std::cout << "Server is listening on port " << kPort << '\n';
    std::cout << "Version 12: owner-controlled room management is active.\n";
    std::cout << "Commands: REGISTER <name> <pass>, LOGIN <name> <pass>, CREATE_ROOM <room> <pass_or_dash>, JOIN_ROOM <room> <pass_or_dash>, ROOM_USERS, SET_ROOM_PASS <room> <pass_or_dash>, DELETE_ROOM <room>, MSG <text>, PM <user> <text>, USERS, ROOMS, QUIT\n";

    epoll_event ready_events[kMaxEvents];

    while (true) {
        const int ready_count = epoll_wait(epoll_fd, ready_events, kMaxEvents, -1);
        if (ready_count == -1) {
            if (errno == EINTR) {
                continue;
            }

            print_errno("epoll_wait() failed");
            close(epoll_fd);
            close(server_fd);
            return 1;
        }

        for (int i = 0; i < ready_count; ++i) {
            const int ready_fd = ready_events[i].data.fd;
            const uint32_t ready_flags = ready_events[i].events;

            if (ready_fd == server_fd) {
                while (true) {
                    sockaddr_in client_address{};
                    socklen_t client_address_length = sizeof(client_address);

                    const int client_fd = accept(
                        server_fd,
                        reinterpret_cast<sockaddr*>(&client_address),
                        &client_address_length
                    );

                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }

                        print_errno("accept() failed");
                        close(epoll_fd);
                        close(server_fd);
                        return 1;
                    }

                    if (!set_non_blocking(client_fd)) {
                        close(client_fd);
                        continue;
                    }

                    chat_state.add_client(client_fd);

                    char client_ip[INET_ADDRSTRLEN] = {};
                    inet_ntop(AF_INET, &client_address.sin_addr, client_ip, sizeof(client_ip));

                    std::cout << "Accepted client "
                              << client_ip
                              << ':'
                              << ntohs(client_address.sin_port)
                              << '\n';

                    if (!add_to_epoll(epoll_fd, client_fd, EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
                        chat_state.remove_client(client_fd);
                        close(client_fd);
                        continue;
                    }

                    send_text(client_fd, "WELCOME Register or login first. Owners can later use SET_ROOM_PASS and DELETE_ROOM on their own rooms.\n");
                }

                continue;
            }

            if (ready_flags & EPOLLERR) {
                std::cout << "Client FD " << ready_fd << " encountered an epoll error event.\n";
                chat_state.remove_client(ready_fd);
                close_client(epoll_fd, ready_fd);
                continue;
            }

            if (ready_flags & (EPOLLHUP | EPOLLRDHUP)) {
                std::cout << "Client FD " << ready_fd << " disconnected.\n";
                chat_state.remove_client(ready_fd);
                close_client(epoll_fd, ready_fd);
                continue;
            }

            if (ready_flags & EPOLLIN) {
                thread_pool.enqueue([epoll_fd, ready_fd, &chat_state]() {
                    char buffer[kBufferSize] = {};
                    const ssize_t bytes_received = recv(ready_fd, buffer, sizeof(buffer) - 1, 0);

                    if (bytes_received == 0) {
                        std::cout << "Client FD " << ready_fd << " closed the connection.\n";
                        chat_state.remove_client(ready_fd);
                        close_client(epoll_fd, ready_fd);
                        return;
                    }

                    if (bytes_received < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            return;
                        }

                        print_errno("recv() failed");
                        chat_state.remove_client(ready_fd);
                        close_client(epoll_fd, ready_fd);
                        return;
                    }

                    buffer[bytes_received] = '\0';
                    const std::string raw_chunk(buffer, static_cast<std::size_t>(bytes_received));
                    const std::vector<std::string> commands =
                        chat_state.append_input_and_take_commands(ready_fd, raw_chunk);

                    if (commands.empty()) {
                        std::cout << "Worker buffered partial input for FD " << ready_fd << '\n';
                        return;
                    }

                    for (const std::string& command_text : commands) {
                        const std::string cleaned = trim_trailing_newlines(command_text);
                        std::cout << "Worker processed FD " << ready_fd
                                  << " command: " << cleaned << '\n';

                        if (!process_command(epoll_fd, ready_fd, chat_state, cleaned)) {
                            return;
                        }
                    }
                });
            }
        }
    }

    close(epoll_fd);
    close(server_fd);
    return 0;
}

#else

int main() {
    std::cout
        << "Version 12 uses Linux epoll and cannot run on macOS.\n"
        << "Current platform can still compile this fallback branch.\n"
        << "To run the real Version 12 server, use Linux, Docker, or a VM.\n";
    return 0;
}

#endif
