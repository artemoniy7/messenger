#include <SFML/Graphics.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace {

struct ChatItem {
    std::string title;
    std::vector<std::string> messages;
};

class ChatStorage {
public:
    explicit ChatStorage(std::string filename) : filename_(std::move(filename)) {}

    std::vector<ChatItem> loadDefault() {
        std::vector<ChatItem> chats;
        std::ifstream in(filename_);
        if (!in) {
            return chats;
        }

        std::string line;
        ChatItem* current = nullptr;
        while (std::getline(in, line)) {
            if (line.rfind("CHAT:", 0) == 0) {
                chats.push_back(ChatItem{line.substr(5), {}});
                current = &chats.back();
            } else if (line.rfind("MSG:", 0) == 0 && current) {
                current->messages.push_back(line.substr(4));
            }
        }

        return chats;
    }

    void save(const std::vector<ChatItem>& chats) {
        std::ofstream out(filename_, std::ios::trunc);
        for (const auto& chat : chats) {
            out << "CHAT:" << chat.title << "\n";
            for (const auto& msg : chat.messages) {
                out << "MSG:" << msg << "\n";
            }
        }
    }

private:
    std::string filename_;
};

class TcpClient {
public:
    TcpClient() { initSockets(); }
    ~TcpClient() { disconnect(); cleanupSockets(); }

    bool connectTo(std::string_view host, int port, std::string_view username) {
        disconnect();

        sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == kInvalidSocket) {
            lastError_ = "socket() failed";
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));

        if (::inet_pton(AF_INET, std::string(host).c_str(), &addr.sin_addr) <= 0) {
            lastError_ = "Invalid server IP";
            disconnect();
            return false;
        }

        if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            lastError_ = std::string("connect() failed: ") + std::strerror(errno);
            disconnect();
            return false;
        }

        connected_.store(true);
        if (!sendLine(std::string(username))) {
            lastError_ = "Failed to send username";
            disconnect();
            return false;
        }

        receiver_ = std::thread([this] { recvLoop(); });
        return true;
    }

    void disconnect() {
        connected_.store(false);
        if (sock_ != kInvalidSocket) {
#ifdef _WIN32
            ::shutdown(sock_, SD_BOTH);
            ::closesocket(sock_);
#else
            ::shutdown(sock_, SHUT_RDWR);
            ::close(sock_);
#endif
            sock_ = kInvalidSocket;
        }
        if (receiver_.joinable()) {
            receiver_.join();
        }
    }

    bool sendLine(const std::string& line) {
        if (sock_ == kInvalidSocket) return false;
        std::string data = line + "\n";
        return ::send(sock_, data.c_str(), static_cast<int>(data.size()), 0) >= 0;
    }

    std::vector<std::string> consumeMessages() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        out.swap(messages_);
        return out;
    }

    bool isConnected() const { return connected_.load(); }
    std::string lastError() const { return lastError_; }

private:
    void recvLoop() {
        std::string buffer;
        char chunk[1024];

        while (connected_.load()) {
            int n = static_cast<int>(::recv(sock_, chunk, sizeof(chunk), 0));
            if (n <= 0) {
                connected_.store(false);
                break;
            }

            buffer.append(chunk, chunk + n);
            std::size_t pos = 0;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    messages_.push_back(line);
                }
                buffer.erase(0, pos + 1);
            }
        }
    }

    static void initSockets() {
#ifdef _WIN32
        static bool initialized = false;
        if (!initialized) {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) == 0) initialized = true;
        }
#endif
    }

    static void cleanupSockets() {
#ifdef _WIN32
        WSACleanup();
#endif
    }

    SocketHandle sock_ = kInvalidSocket;
    std::atomic<bool> connected_{false};
    std::thread receiver_;
    std::mutex mutex_;
    std::vector<std::string> messages_;
    std::string lastError_;
};

void drawRoundedRect(sf::RenderTarget& target, const sf::FloatRect& rect, float radius, const sf::Color& color) {
    const float left = rect.position.x;
    const float top = rect.position.y;
    const float width = rect.size.x;
    const float height = rect.size.y;

    sf::RectangleShape center({width - 2.f * radius, height});
    center.setPosition({left + radius, top});
    center.setFillColor(color);
    target.draw(center);

    sf::RectangleShape leftRect({radius, height - 2.f * radius});
    leftRect.setPosition({left, top + radius});
    leftRect.setFillColor(color);
    target.draw(leftRect);

    sf::RectangleShape rightRect({radius, height - 2.f * radius});
    rightRect.setPosition({left + width - radius, top + radius});
    rightRect.setFillColor(color);
    target.draw(rightRect);

    sf::CircleShape corner(radius);
    corner.setFillColor(color);
    corner.setPosition({left, top});
    target.draw(corner);
    corner.setPosition({left + width - 2.f * radius, top});
    target.draw(corner);
    corner.setPosition({left, top + height - 2.f * radius});
    target.draw(corner);
    corner.setPosition({left + width - 2.f * radius, top + height - 2.f * radius});
    target.draw(corner);
}

float sx(unsigned int w, float v) { return v * static_cast<float>(w) / 1366.f; }
float sy(unsigned int h, float v) { return v * static_cast<float>(h) / 768.f; }

}  // namespace

int main(int argc, char** argv) {
    std::string serverIp = argc > 1 ? argv[1] : "127.0.0.1";
    int serverPort = argc > 2 ? std::stoi(argv[2]) : 5000;

    sf::RenderWindow window(sf::VideoMode({1366, 768}), "Messenger SFML", sf::Style::Default);
    window.setFramerateLimit(60);

    sf::Font font;
    const std::vector<std::string> fontCandidates = {
        "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    bool fontLoaded = false;
    for (const auto& path : fontCandidates) {
        if (font.openFromFile(path)) {
            fontLoaded = true;
            break;
        }
    }
    if (!fontLoaded) return 1;

    ChatStorage storage("chat_history.txt");
    std::vector<ChatItem> chats = storage.loadDefault();

    TcpClient client;
    const std::string username = "sfml_user";
    bool connected = client.connectTo(serverIp, serverPort, username);

    std::vector<std::string> serverLog;
    auto addLog = [&](const std::string& line) {
        serverLog.push_back(line);
        if (serverLog.size() > 14) serverLog.erase(serverLog.begin());
    };

    if (connected) addLog("[CLIENT] Connected to " + serverIp + ":" + std::to_string(serverPort));
    else addLog("[CLIENT] Connection failed: " + client.lastError());

    while (window.isOpen()) {
        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                storage.save(chats);
                window.close();
            }
        }

        for (const auto& message : client.consumeMessages()) {
            addLog(message);
            if (chats.empty()) {
                chats.push_back({"Server", {}});
            }
            chats[0].messages.push_back(message);
            if (chats[0].messages.size() > 200) chats[0].messages.erase(chats[0].messages.begin());
        }

        const auto size = window.getSize();
        const float iconsW = sx(size.x, 78.f);
        const float chatsW = sx(size.x, 280.f);

        window.clear(sf::Color(3, 22, 46));

        sf::RectangleShape iconsBar({iconsW, static_cast<float>(size.y)});
        iconsBar.setFillColor(sf::Color(22, 35, 50));
        window.draw(iconsBar);

        sf::CircleShape searchDot(sx(size.x, 8.f));
        searchDot.setPosition({sx(size.x, 24.f), sy(size.y, 28.f)});
        searchDot.setFillColor(sf::Color(104, 130, 158));
        window.draw(searchDot);

        const int avatarCount = std::min(11, static_cast<int>(chats.size()));
        for (int i = 0; i < avatarCount; ++i) {
            sf::CircleShape avatar(sx(size.x, 22.f));
            avatar.setPosition({sx(size.x, 17.f), sy(size.y, 95.f + i * 58.f)});
            avatar.setFillColor(i == 0 ? sf::Color(58, 149, 245) : sf::Color(66 + i * 8, 90 + i * 6, 120 + i * 4));
            window.draw(avatar);
        }

        if (chats.empty()) {
            sf::Text emptyAvatars(font, "No conversations yet", static_cast<unsigned int>(sx(size.x, 12.f)));
            emptyAvatars.setFillColor(sf::Color(120, 145, 170));
            emptyAvatars.setPosition({sx(size.x, 8.f), sy(size.y, 96.f)});
            window.draw(emptyAvatars);
        }

        sf::RectangleShape chatsBar({chatsW, static_cast<float>(size.y)});
        chatsBar.setPosition({iconsW, 0.f});
        chatsBar.setFillColor(sf::Color(8, 26, 49));
        window.draw(chatsBar);

        sf::Text chatTitle(font, "Conversations", static_cast<unsigned int>(sx(size.x, 22.f)));
        chatTitle.setPosition({iconsW + sx(size.x, 20.f), sy(size.y, 18.f)});
        chatTitle.setFillColor(sf::Color(190, 213, 235));
        window.draw(chatTitle);

        float y = sy(size.y, 70.f);
        if (chats.empty()) {
            sf::Text noChats(font, "No chats yet", static_cast<unsigned int>(sx(size.x, 16.f)));
            noChats.setFillColor(sf::Color(145, 175, 202));
            noChats.setPosition({iconsW + sx(size.x, 20.f), sy(size.y, 78.f)});
            window.draw(noChats);
        }
        for (std::size_t i = 0; i < chats.size(); ++i) {
            sf::RectangleShape itemBg({chatsW - sx(size.x, 24.f), sy(size.y, 58.f)});
            itemBg.setPosition({iconsW + sx(size.x, 12.f), y});
            itemBg.setFillColor(i == 0 ? sf::Color(20, 51, 82) : sf::Color(12, 34, 58));
            window.draw(itemBg);

            sf::Text t(font, chats[i].title, static_cast<unsigned int>(sx(size.x, 17.f)));
            t.setFillColor(sf::Color(220, 235, 247));
            t.setPosition({iconsW + sx(size.x, 22.f), y + sy(size.y, 7.f)});
            window.draw(t);

            const std::string preview = chats[i].messages.empty() ? "No messages yet" : chats[i].messages.back();
            sf::Text p(font, preview, static_cast<unsigned int>(sx(size.x, 13.f)));
            p.setFillColor(sf::Color(145, 175, 202));
            p.setPosition({iconsW + sx(size.x, 22.f), y + sy(size.y, 32.f)});
            window.draw(p);
            y += sy(size.y, 66.f);
        }

        const float workspaceX = iconsW + chatsW;
        sf::RectangleShape workspace({static_cast<float>(size.x) - workspaceX, static_cast<float>(size.y)});
        workspace.setPosition({workspaceX, 0.f});
        workspace.setFillColor(sf::Color(1, 17, 37));
        window.draw(workspace);

        sf::FloatRect pill({workspaceX + (workspace.getSize().x - sx(size.x, 420.f)) / 2.f, workspace.getSize().y / 2.f - sy(size.y, 16.f)},
                           {sx(size.x, 420.f), sy(size.y, 36.f)});
        drawRoundedRect(window, pill, sx(size.x, 18.f), sf::Color(18, 50, 84));

        sf::Text message(font, "Choose who you want to message", static_cast<unsigned int>(sx(size.x, 24.f)));
        message.setFillColor(sf::Color::White);
        message.setPosition({pill.position.x + sx(size.x, 16.f), pill.position.y + sy(size.y, 2.f)});
        window.draw(message);

        sf::Text status(font, connected ? "ONLINE" : "OFFLINE", static_cast<unsigned int>(sx(size.x, 14.f)));
        status.setFillColor(connected ? sf::Color(94, 230, 131) : sf::Color(250, 117, 117));
        status.setPosition({workspaceX + sx(size.x, 18.f), sy(size.y, 20.f)});
        window.draw(status);

        float logY = static_cast<float>(size.y) - sy(size.y, 190.f);
        for (const auto& line : serverLog) {
            sf::Text row(font, line, static_cast<unsigned int>(sx(size.x, 13.f)));
            row.setFillColor(sf::Color(151, 176, 204));
            row.setPosition({workspaceX + sx(size.x, 18.f), logY});
            window.draw(row);
            logY += sy(size.y, 20.f);
        }

        connected = client.isConnected();
        window.display();

        static auto lastSave = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - lastSave > std::chrono::seconds(2)) {
            storage.save(chats);
            lastSave = std::chrono::steady_clock::now();
        }
    }

    storage.save(chats);
    client.disconnect();
    return 0;
}
