#include <SFML/Graphics.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct ChatItem {
    std::string title;
    std::string preview;
};

class TcpClient {
public:
    ~TcpClient() { disconnect(); }

    bool connectTo(std::string_view host, int port, std::string_view username) {
        disconnect();

        sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) {
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
        if (sock_ >= 0) {
            ::shutdown(sock_, SHUT_RDWR);
            ::close(sock_);
            sock_ = -1;
        }
        if (receiver_.joinable()) {
            receiver_.join();
        }
    }

    bool sendLine(const std::string& line) {
        if (sock_ < 0) return false;
        std::string data = line + "\n";
        return ::send(sock_, data.c_str(), data.size(), 0) >= 0;
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
            ssize_t n = ::recv(sock_, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                connected_.store(false);
                break;
            }

            buffer.append(chunk, chunk + n);

            std::size_t pos = 0;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    messages_.push_back(line);
                }
                buffer.erase(0, pos + 1);
            }
        }
    }

    int sock_ = -1;
    std::atomic<bool> connected_{false};
    std::thread receiver_;
    std::mutex mutex_;
    std::vector<std::string> messages_;
    std::string lastError_;
};

void drawRoundedRect(sf::RenderTarget& target, const sf::FloatRect& rect, float radius, const sf::Color& color) {
    sf::RectangleShape center({rect.width - 2.f * radius, rect.height});
    center.setPosition({rect.left + radius, rect.top});
    center.setFillColor(color);
    target.draw(center);

    sf::RectangleShape left({radius, rect.height - 2.f * radius});
    left.setPosition({rect.left, rect.top + radius});
    left.setFillColor(color);
    target.draw(left);

    sf::RectangleShape right({radius, rect.height - 2.f * radius});
    right.setPosition({rect.left + rect.width - radius, rect.top + radius});
    right.setFillColor(color);
    target.draw(right);

    sf::CircleShape corner(radius);
    corner.setFillColor(color);

    corner.setPosition({rect.left, rect.top});
    target.draw(corner);
    corner.setPosition({rect.left + rect.width - 2.f * radius, rect.top});
    target.draw(corner);
    corner.setPosition({rect.left, rect.top + rect.height - 2.f * radius});
    target.draw(corner);
    corner.setPosition({rect.left + rect.width - 2.f * radius, rect.top + rect.height - 2.f * radius});
    target.draw(corner);
}

} // namespace

int main(int argc, char** argv) {
    std::string serverIp = argc > 1 ? argv[1] : "127.0.0.1";
    int serverPort = argc > 2 ? std::stoi(argv[2]) : 5000;

    sf::RenderWindow window(sf::VideoMode({1366, 768}), "Messenger SFML", sf::Style::Default);
    window.setFramerateLimit(60);

    sf::Font font;
    if (!font.openFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")) {
        std::cerr << "Font not found\n";
        return 1;
    }

    std::vector<ChatItem> chats = {
        {"General", "Добро пожаловать!"},
        {"Dev Team", "Пуш в main после review"},
        {"Design", "Новый макет готов"},
        {"Random", "Мемы только после 18:00"},
        {"Support", "Статус серверов зелёный"},
    };

    std::vector<std::string> serverLog;

    TcpClient client;
    const std::string username = "sfml_user";
    bool connected = client.connectTo(serverIp, serverPort, username);

    auto addLog = [&](const std::string& line) {
        serverLog.push_back(line);
        if (serverLog.size() > 14) serverLog.erase(serverLog.begin());
    };

    if (connected) {
        addLog("[CLIENT] Connected to " + serverIp + ":" + std::to_string(serverPort));
    } else {
        addLog("[CLIENT] Connection failed: " + client.lastError());
    }

    while (window.isOpen()) {
        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
            }
        }

        for (const auto& message : client.consumeMessages()) {
            addLog(message);
        }

        const auto size = window.getSize();

        const float sidebarIconsW = 78.f;
        const float sidebarChatsW = 280.f;

        window.clear(sf::Color(3, 22, 46));

        sf::RectangleShape iconsBar({sidebarIconsW, static_cast<float>(size.y)});
        iconsBar.setFillColor(sf::Color(22, 35, 50));
        window.draw(iconsBar);

        sf::CircleShape searchDot(8.f);
        searchDot.setPosition({24.f, 28.f});
        searchDot.setFillColor(sf::Color(104, 130, 158));
        window.draw(searchDot);

        for (int i = 0; i < 11; ++i) {
            sf::CircleShape avatar(22.f);
            avatar.setPosition({17.f, 95.f + i * 58.f});
            avatar.setFillColor(i == 2 ? sf::Color(58, 149, 245) : sf::Color(66 + i * 6, 90 + i * 5, 115 + i * 4));
            window.draw(avatar);
        }

        sf::RectangleShape chatsBar({sidebarChatsW, static_cast<float>(size.y)});
        chatsBar.setPosition({sidebarIconsW, 0.f});
        chatsBar.setFillColor(sf::Color(8, 26, 49));
        window.draw(chatsBar);

        sf::Text chatTitle(font, "Chats", 24);
        chatTitle.setPosition({sidebarIconsW + 20.f, 18.f});
        chatTitle.setFillColor(sf::Color(190, 213, 235));
        window.draw(chatTitle);

        float y = 70.f;
        for (std::size_t i = 0; i < chats.size(); ++i) {
            sf::RectangleShape itemBg({sidebarChatsW - 24.f, 58.f});
            itemBg.setPosition({sidebarIconsW + 12.f, y});
            itemBg.setFillColor(i == 0 ? sf::Color(20, 51, 82) : sf::Color(12, 34, 58));
            window.draw(itemBg);

            sf::Text t(font, chats[i].title, 17);
            t.setFillColor(sf::Color(220, 235, 247));
            t.setPosition({sidebarIconsW + 22.f, y + 7.f});
            window.draw(t);

            sf::Text p(font, chats[i].preview, 13);
            p.setFillColor(sf::Color(145, 175, 202));
            p.setPosition({sidebarIconsW + 22.f, y + 32.f});
            window.draw(p);
            y += 66.f;
        }

        const float workspaceX = sidebarIconsW + sidebarChatsW;
        sf::RectangleShape workspace({static_cast<float>(size.x) - workspaceX, static_cast<float>(size.y)});
        workspace.setPosition({workspaceX, 0.f});
        workspace.setFillColor(sf::Color(1, 17, 37));
        window.draw(workspace);

        sf::FloatRect pill{workspaceX + (workspace.getSize().x - 420.f) / 2.f, workspace.getSize().y / 2.f - 16.f, 420.f, 36.f};
        drawRoundedRect(window, pill, 18.f, sf::Color(18, 50, 84));

        sf::Text message(font, "Выберите, кому хотели бы написать", 24);
        message.setFillColor(sf::Color::White);
        message.setPosition({pill.left + 16.f, pill.top + 2.f});
        window.draw(message);

        sf::Text status(font, connected ? "ONLINE" : "OFFLINE", 14);
        status.setFillColor(connected ? sf::Color(94, 230, 131) : sf::Color(250, 117, 117));
        status.setPosition({workspaceX + 18.f, 20.f});
        window.draw(status);

        float logY = static_cast<float>(size.y) - 190.f;
        for (const auto& line : serverLog) {
            sf::Text row(font, line, 13);
            row.setFillColor(sf::Color(151, 176, 204));
            row.setPosition({workspaceX + 18.f, logY});
            window.draw(row);
            logY += 20.f;
        }

        connected = client.isConnected();
        window.display();
    }

    client.disconnect();
    return 0;
}
