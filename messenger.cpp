#include <SFML/Graphics.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
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


        static const std::vector<std::string> fakeTitles = {"General", "Dev Team", "Design", "Random", "Support", "Server"};
        std::vector<ChatItem> filtered;
        for (const auto& chat : chats) {
            const bool isFake = std::find(fakeTitles.begin(), fakeTitles.end(), chat.title) != fakeTitles.end();
            if (!isFake) filtered.push_back(chat);
        }

        return filtered;
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

float scaleFactor(unsigned int w, unsigned int h) {
    const float sx = static_cast<float>(w) / 1366.f;
    const float sy = static_cast<float>(h) / 768.f;
    return std::min(sx, sy);
}

float px(float value, float scale) { return value * scale; }
unsigned int pt(unsigned int base, float scale) {
    const auto v = static_cast<unsigned int>(static_cast<float>(base) * scale);
    return std::max(12u, v);
}

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
            if (!chats.empty()) {
                chats[0].messages.push_back(message);
                if (chats[0].messages.size() > 200) chats[0].messages.erase(chats[0].messages.begin());
            }
        }

        const auto size = window.getSize();
        const float scale = scaleFactor(size.x, size.y);
        const float iconsW = px(78.f, scale);
        const float chatsW = px(280.f, scale);

        window.clear(sf::Color(3, 22, 46));

        sf::RectangleShape iconsBar({iconsW, static_cast<float>(size.y)});
        iconsBar.setFillColor(sf::Color(22, 35, 50));
        window.draw(iconsBar);

        sf::CircleShape searchDot(px(8.f, scale));
        searchDot.setPosition({px(24.f, scale), px(28.f, scale)});
        searchDot.setFillColor(sf::Color(104, 130, 158));
        window.draw(searchDot);

        const int avatarCount = std::min(11, static_cast<int>(chats.size()));
        for (int i = 0; i < avatarCount; ++i) {
            sf::CircleShape avatar(px(22.f, scale));
            avatar.setPosition({px(17.f, scale), px(95.f + i * 58.f, scale)});
            avatar.setFillColor(i == 0 ? sf::Color(58, 149, 245) : sf::Color(66 + i * 8, 90 + i * 6, 120 + i * 4));
            window.draw(avatar);
        }

        if (chats.empty()) {
            sf::Text emptyAvatars(font, "No conversations yet", pt(12, scale));
            emptyAvatars.setFillColor(sf::Color(120, 145, 170));
            emptyAvatars.setPosition({px(8.f, scale), px(96.f, scale)});
            window.draw(emptyAvatars);
        }

        sf::RectangleShape chatsBar({chatsW, static_cast<float>(size.y)});
        chatsBar.setPosition({iconsW, 0.f});
        chatsBar.setFillColor(sf::Color(8, 26, 49));
        window.draw(chatsBar);

        sf::Text chatTitle(font, "Conversations", pt(22, scale));
        chatTitle.setPosition({iconsW + px(20.f, scale), px(18.f, scale)});
        chatTitle.setFillColor(sf::Color(190, 213, 235));
        window.draw(chatTitle);

        float y = px(70.f, scale);
        if (chats.empty()) {
            sf::Text noChats(font, "No chats yet", pt(16, scale));
            noChats.setFillColor(sf::Color(145, 175, 202));
            noChats.setPosition({iconsW + px(20.f, scale), px(78.f, scale)});
            window.draw(noChats);
        }
        for (std::size_t i = 0; i < chats.size(); ++i) {
            sf::RectangleShape itemBg({chatsW - px(24.f, scale), px(58.f, scale)});
            itemBg.setPosition({iconsW + px(12.f, scale), y});
            itemBg.setFillColor(i == 0 ? sf::Color(20, 51, 82) : sf::Color(12, 34, 58));
            window.draw(itemBg);

            sf::Text t(font, chats[i].title, pt(17, scale));
            t.setFillColor(sf::Color(220, 235, 247));
            t.setPosition({iconsW + px(22.f, scale), y + px(7.f, scale)});
            window.draw(t);

            const std::string preview = chats[i].messages.empty() ? "No messages yet" : chats[i].messages.back();
            sf::Text p(font, preview, pt(13, scale));
            p.setFillColor(sf::Color(145, 175, 202));
            p.setPosition({iconsW + px(22.f, scale), y + px(32.f, scale)});
            window.draw(p);
            y += px(66.f, scale);
        }

        const float workspaceX = iconsW + chatsW;
        sf::RectangleShape workspace({static_cast<float>(size.x) - workspaceX, static_cast<float>(size.y)});
        workspace.setPosition({workspaceX, 0.f});
        workspace.setFillColor(sf::Color(1, 17, 37));
        window.draw(workspace);

        sf::FloatRect pill({workspaceX + (workspace.getSize().x - px(420.f, scale)) / 2.f, workspace.getSize().y / 2.f - px(16.f, scale)},
                           {px(420.f, scale), px(36.f, scale)});
        drawRoundedRect(window, pill, px(18.f, scale), sf::Color(18, 50, 84));

        sf::Text message(font, "Choose who you want to message", pt(24, scale));
        message.setFillColor(sf::Color::White);
        message.setPosition({pill.position.x + px(16.f, scale), pill.position.y + px(2.f, scale)});
        window.draw(message);

        sf::Text status(font, connected ? "ONLINE" : "OFFLINE", pt(14, scale));
        status.setFillColor(connected ? sf::Color(94, 230, 131) : sf::Color(250, 117, 117));
        status.setPosition({workspaceX + px(18.f, scale), px(20.f, scale)});
        window.draw(status);

        float logY = static_cast<float>(size.y) - px(190.f, scale);
        for (const auto& line : serverLog) {
            sf::Text row(font, line, pt(13, scale));
            row.setFillColor(sf::Color(151, 176, 204));
            row.setPosition({workspaceX + px(18.f, scale), logY});
            window.draw(row);
            logY += px(20.f, scale);
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
