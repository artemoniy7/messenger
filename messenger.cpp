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

    std::vector<ChatItem> load() {
        std::vector<ChatItem> chats;
        std::ifstream in(filename_);
        if (!in) return chats;

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
        chats.erase(std::remove_if(chats.begin(), chats.end(), [&](const ChatItem& c) {
            return std::find(fakeTitles.begin(), fakeTitles.end(), c.title) != fakeTitles.end();
        }), chats.end());

        return chats;
    }

    void save(const std::vector<ChatItem>& chats) {
        std::ofstream out(filename_, std::ios::trunc);
        for (const auto& chat : chats) {
            out << "CHAT:" << chat.title << "\n";
            for (const auto& msg : chat.messages) out << "MSG:" << msg << "\n";
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
        if (receiver_.joinable()) receiver_.join();
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
                std::lock_guard<std::mutex> lock(mutex_);
                messages_.push_back(line);
                buffer.erase(0, pos + 1);
            }
        }
    }

    static void initSockets() {
#ifdef _WIN32
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
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

float clampf(float v, float a, float b) { return std::max(a, std::min(v, b)); }

unsigned int fontSize(unsigned int base, float s) {
    const float scaled = static_cast<float>(base) * s;
    return static_cast<unsigned int>(clampf(scaled, 11.f, 42.f));
}

sf::Color avatarColor(std::size_t i) {
    return sf::Color(70 + static_cast<int>((i * 23) % 110), 95 + static_cast<int>((i * 37) % 100), 120 + static_cast<int>((i * 17) % 90));
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
        if (font.openFromFile(path)) { fontLoaded = true; break; }
    }
    if (!fontLoaded) return 1;

    ChatStorage storage("chat_history.txt");
    std::vector<ChatItem> chats = storage.load();
    std::size_t selectedChat = 0;
    bool settingsOpen = false;
    float settingsAnim = 0.f;

    TcpClient client;
    bool connected = client.connectTo(serverIp, serverPort, "sfml_user");

    std::vector<std::string> serverLog;
    auto addLog = [&](const std::string& line) {
        serverLog.push_back(line);
        if (serverLog.size() > 10) serverLog.erase(serverLog.begin());
    };
    addLog(connected ? "[CLIENT] Connected" : "[CLIENT] Connection failed: " + client.lastError());

    while (window.isOpen()) {
        const auto size = window.getSize();
        const float uiScale = clampf(std::min(static_cast<float>(size.x) / 1366.f, static_cast<float>(size.y) / 768.f), 0.75f, 1.75f);
        const float leftW = clampf(84.f * uiScale, 70.f, 140.f);
        const float topPad = 72.f * uiScale;
        const float settingsBtnH = 38.f * uiScale;
        const float settingsPanelW = 280.f * uiScale;

        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                storage.save(chats);
                window.close();
            }
            if (const auto* pressed = event->getIf<sf::Event::MouseButtonPressed>()) {
                if (pressed->button == sf::Mouse::Button::Left) {
                    const float mx = static_cast<float>(pressed->position.x);
                    const float my = static_cast<float>(pressed->position.y);

                    const bool onSettingsButton = (mx >= 8.f * uiScale && mx <= leftW - 8.f * uiScale && my >= 12.f * uiScale && my <= 12.f * uiScale + settingsBtnH);
                    if (onSettingsButton) {
                        settingsOpen = true;
                        continue;
                    }

                    if (settingsOpen) {
                        const bool insidePanel = (mx <= settingsPanelW);
                        if (!insidePanel) {
                            settingsOpen = false;
                            continue;
                        }
                    }

                    const float radius = clampf(22.f * uiScale, 16.f, 34.f);
                    const float startY = topPad + 28.f * uiScale;
                    const float stepY = 58.f * uiScale;
                    for (std::size_t i = 0; i < chats.size(); ++i) {
                        sf::Vector2f c{leftW * 0.5f, startY + stepY * static_cast<float>(i) + radius};
                        const float dx = mx - c.x;
                        const float dy = my - c.y;
                        if (dx * dx + dy * dy <= radius * radius) {
                            selectedChat = i;
                            break;
                        }
                    }
                }
            }
        }

        for (const auto& message : client.consumeMessages()) {
            addLog(message);
            if (selectedChat < chats.size()) {
                chats[selectedChat].messages.push_back(message);
            }
        }

        window.clear(sf::Color(3, 22, 46));

        sf::RectangleShape avatarsBar({leftW, static_cast<float>(size.y)});
        avatarsBar.setFillColor(sf::Color(18, 33, 49));
        window.draw(avatarsBar);

        sf::RectangleShape settingsButton({leftW - 16.f * uiScale, settingsBtnH});
        settingsButton.setPosition({8.f * uiScale, 12.f * uiScale});
        settingsButton.setFillColor(sf::Color(27, 48, 69));
        window.draw(settingsButton);

        for (int i = 0; i < 3; ++i) {
            sf::RectangleShape line({(leftW - 40.f * uiScale), 3.f * uiScale});
            line.setPosition({20.f * uiScale, 20.f * uiScale + i * 9.f * uiScale});
            line.setFillColor(sf::Color(185, 205, 225));
            window.draw(line);
        }

        const float radius = clampf(22.f * uiScale, 16.f, 34.f);
        const float startY = topPad + 28.f * uiScale;
        const float stepY = 58.f * uiScale;
        for (std::size_t i = 0; i < chats.size(); ++i) {
            sf::CircleShape avatar(radius);
            avatar.setPosition({leftW * 0.5f - radius, startY + stepY * static_cast<float>(i)});
            avatar.setFillColor(i == selectedChat ? sf::Color(58, 149, 245) : avatarColor(i));
            window.draw(avatar);
        }

        const float chatX = leftW;
        sf::RectangleShape workspace({static_cast<float>(size.x) - chatX, static_cast<float>(size.y)});
        workspace.setPosition({chatX, 0.f});
        workspace.setFillColor(sf::Color(1, 17, 37));
        window.draw(workspace);

        float y = 28.f * uiScale;
        if (selectedChat < chats.size() && !chats[selectedChat].messages.empty()) {
            const auto& msgs = chats[selectedChat].messages;
            const std::size_t maxLines = 20;
            std::size_t from = msgs.size() > maxLines ? msgs.size() - maxLines : 0;
            for (std::size_t i = from; i < msgs.size(); ++i) {
                sf::Text row(font, msgs[i], fontSize(14, uiScale));
                row.setFillColor(sf::Color(170, 196, 220));
                row.setPosition({chatX + 20.f * uiScale, y});
                window.draw(row);
                y += 24.f * uiScale;
            }
        }

        float logY = static_cast<float>(size.y) - (serverLog.size() * (18.f * uiScale)) - 12.f * uiScale;
        for (const auto& line : serverLog) {
            sf::Text row(font, line, fontSize(12, uiScale));
            row.setFillColor(sf::Color(120, 150, 176));
            row.setPosition({chatX + 20.f * uiScale, logY});
            window.draw(row);
            logY += 18.f * uiScale;
        }


        const float targetAnim = settingsOpen ? 1.f : 0.f;
        settingsAnim += (targetAnim - settingsAnim) * 0.18f;

        if (settingsAnim > 0.01f) {
            sf::RectangleShape dim({static_cast<float>(size.x), static_cast<float>(size.y)});
            dim.setFillColor(sf::Color(0, 0, 0, static_cast<std::uint8_t>(70.f * settingsAnim)));
            window.draw(dim);

            const float panelX = -settingsPanelW + settingsPanelW * settingsAnim;
            sf::RectangleShape panel({settingsPanelW, static_cast<float>(size.y)});
            panel.setFillColor(sf::Color(14, 27, 42));
            panel.setPosition({panelX, 0.f});
            window.draw(panel);

            const float avatarRadius = clampf(44.f * uiScale, 30.f, 58.f);
            const float avatarCx = panelX + settingsPanelW * 0.5f;
            const float avatarCy = 52.f * uiScale + avatarRadius;

            sf::CircleShape profileAvatar(avatarRadius);
            profileAvatar.setFillColor(sf::Color(58, 149, 245));
            profileAvatar.setPosition({avatarCx - avatarRadius, avatarCy - avatarRadius});
            window.draw(profileAvatar);

            sf::Text profileName(font, "Username", fontSize(20, uiScale));
            profileName.setFillColor(sf::Color(220, 235, 247));
            const auto profileBounds = profileName.getLocalBounds();
            profileName.setPosition({avatarCx - profileBounds.size.x * 0.5f, avatarCy + avatarRadius + 18.f * uiScale});
            window.draw(profileName);

            sf::Text profileHandle(font, "@username", fontSize(15, uiScale));
            profileHandle.setFillColor(sf::Color(148, 178, 205));
            const auto handleBounds = profileHandle.getLocalBounds();
            profileHandle.setPosition({avatarCx - handleBounds.size.x * 0.5f, avatarCy + avatarRadius + 50.f * uiScale});
            window.draw(profileHandle);
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
