#include <SFML/Graphics.hpp>

#include <algorithm>
#include <array>
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
#include <set>
#include <future>

#ifdef _WIN32
#include <fcntl.h>
#else
#include <fcntl.h>
#include <netdb.h>
#endif

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


std::optional<std::string> localPrefix() {
    char host[256]{};
    if (gethostname(host, sizeof(host)) != 0) return std::nullopt;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &result) != 0) return std::nullopt;

    std::optional<std::string> prefix;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        auto* in = reinterpret_cast<sockaddr_in*>(it->ai_addr);
        char ip[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip))) continue;
        std::string ipStr = ip;
        if (ipStr.rfind("127.", 0) == 0) continue;
        const auto pos = ipStr.rfind('.');
        if (pos != std::string::npos) {
            prefix = ipStr.substr(0, pos + 1);
            break;
        }
    }
    freeaddrinfo(result);
    return prefix;
}

bool canConnectFast(const std::string& ip, int port, int timeoutMs) {
    SocketHandle s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return false;
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return false;
    }
    int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#ifdef _WIN32
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return false; }
#else
    if (rc < 0 && errno != EINPROGRESS) { close(s); return false; }
#endif
    fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    rc = select(static_cast<int>(s) + 1, nullptr, &wfds, nullptr, &tv);
    bool ok = false;
    if (rc > 0 && FD_ISSET(s, &wfds)) {
        int err = 0; socklen_t len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        ok = (err == 0);
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return ok;
}

std::vector<std::string> candidatePrefixes() {
    std::set<std::string> prefixes = {"192.168.0.", "192.168.1.", "10.0.0.", "10.0.1.", "172.16.0.", "172.20.10."};
    if (auto p = localPrefix(); p.has_value()) prefixes.insert(*p);
    return std::vector<std::string>(prefixes.begin(), prefixes.end());
}

std::optional<std::string> scanPrefix(const std::string& prefix, int port, int timeoutMs) {
    for (int i = 1; i <= 254; ++i) {
        std::string ip = prefix + std::to_string(i);
        if (canConnectFast(ip, port, timeoutMs)) return ip;
    }
    return std::nullopt;
}

std::string discoverServerIp(int port) {
    SocketHandle sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock != kInvalidSocket) {
        int opt = 1;
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&opt), sizeof(opt));
        DWORD timeout = 1200;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
        timeval tv{1, 200000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        std::array<const char*, 8> broadcasts = {"255.255.255.255", "192.168.0.255", "192.168.1.255", "192.168.43.255", "10.0.0.255", "10.0.1.255", "172.16.0.255", "172.20.10.255"};
        const char* q = "MESSENGER_DISCOVER";
        for (const char* b : broadcasts) {
            sockaddr_in dst{};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(5001);
            inet_pton(AF_INET, b, &dst.sin_addr);
            sendto(sock, q, static_cast<int>(std::strlen(q)), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }

        for (int attempt = 0; attempt < 6; ++attempt) {
            char buf[128]{};
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n > 0) {
                char ip[INET_ADDRSTRLEN]{};
                if (inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip))) {
#ifdef _WIN32
                    closesocket(sock);
#else
                    close(sock);
#endif
                    return std::string(ip);
                }
            }
        }
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
    }

    auto prefixes = candidatePrefixes();
    std::vector<std::future<std::optional<std::string>>> tasks;
    for (const auto& pref : prefixes) {
        tasks.push_back(std::async(std::launch::async, [pref, port] { return scanPrefix(pref, port, 40); }));
    }
    for (auto& task : tasks) {
        auto ip = task.get();
        if (ip.has_value()) return *ip;
    }

    return "127.0.0.1";
}

sf::Color avatarColor(std::size_t i) {
    return sf::Color(70 + static_cast<int>((i * 23) % 110), 95 + static_cast<int>((i * 37) % 100), 120 + static_cast<int>((i * 17) % 90));
}

}  // namespace

int main(int argc, char** argv) {
    std::string serverIp = argc > 1 ? argv[1] : "";
    int serverPort = argc > 2 ? std::stoi(argv[2]) : 5000;
    if (serverIp.empty()) {
        serverIp = discoverServerIp(serverPort);
    }

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
    bool profileOpen = false;
    float profileAnim = 0.f;
    bool advancedSettingsOpen = false;
    float advancedSettingsAnim = 0.f;
    bool serverSettingsOpen = false;
    float serverSettingsAnim = 0.f;
    bool serverIpInputActive = false;
    bool serverPortInputActive = false;
    std::string serverIpDraft = serverIp;
    std::string serverPortDraft = std::to_string(serverPort);
    const std::vector<std::string> settingsItems = {"My profile", "Create group", "Contacts", "Favorites", "Settings", "Additional settings"};
    const std::vector<std::string> advancedSettingsItems = {
        "My account", "Notifications and sounds", "Privacy", "Chat settings",
        "Language", "Saved messages"};

    TcpClient client;
    bool connected = client.connectTo(serverIp, serverPort, "sfml_user");

    std::vector<std::string> serverLog;
    auto addLog = [&](const std::string& line) {
        serverLog.push_back(line);
        if (serverLog.size() > 10) serverLog.erase(serverLog.begin());
    };
    addLog("[CLIENT] Target: " + serverIp + ":" + std::to_string(serverPort));
    addLog(connected ? "[CLIENT] Connected" : "[CLIENT] Connection failed: " + client.lastError());

    while (window.isOpen()) {
        const auto size = window.getSize();
        const auto mousePixel = sf::Mouse::getPosition(window);
        const sf::Vector2f mousePos{static_cast<float>(mousePixel.x), static_cast<float>(mousePixel.y)};
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
            if (const auto* textEntered = event->getIf<sf::Event::TextEntered>()) {
                if (serverIpInputActive || serverPortInputActive) {
                    const char32_t code = textEntered->unicode;
                    if (code == 8) {
                        if (serverIpInputActive && !serverIpDraft.empty()) serverIpDraft.pop_back();
                        if (serverPortInputActive && !serverPortDraft.empty()) serverPortDraft.pop_back();
                    } else if (code == 13 || code == 10) {
                        serverIpInputActive = false;
                        serverPortInputActive = false;
                    } else if (code >= 32 && code < 127) {
                        const char ch = static_cast<char>(code);
                        if (serverIpInputActive && ((ch >= '0' && ch <= '9') || ch == '.')) {
                            if (serverIpDraft.size() < 15) serverIpDraft.push_back(ch);
                        }
                        if (serverPortInputActive && (ch >= '0' && ch <= '9')) {
                            if (serverPortDraft.size() < 5) serverPortDraft.push_back(ch);
                        }
                    }
                }
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

                    if (serverSettingsAnim > 0.01f) {
                        const float cardW = clampf(520.f * uiScale, 380.f, 640.f);
                        const float cardH = clampf(420.f * uiScale, 320.f, 520.f);
                        const float cardX = (static_cast<float>(size.x) - cardW) * 0.5f;
                        const float cardY = (static_cast<float>(size.y) - cardH) * 0.5f;
                        const bool insideCard = (mx >= cardX && mx <= cardX + cardW && my >= cardY && my <= cardY + cardH);
                        if (!insideCard) {
                            serverSettingsOpen = false;
                            serverIpInputActive = false;
                            serverPortInputActive = false;
                            continue;
                        }
                    }

                    if (advancedSettingsAnim > 0.01f) {
                        const float cardW = clampf(500.f * uiScale, 360.f, 600.f);
                        const float cardH = clampf(700.f * uiScale, 520.f, static_cast<float>(size.y) - 24.f * uiScale);
                        const float cardX = (static_cast<float>(size.x) - cardW) * 0.5f;
                        const float cardY = (static_cast<float>(size.y) - cardH) * 0.5f;
                        const bool insideCard = (mx >= cardX && mx <= cardX + cardW && my >= cardY && my <= cardY + cardH);
                        if (!insideCard) {
                            advancedSettingsOpen = false;
                            continue;
                        }
                    }

                    if (profileAnim > 0.01f) {
                        const float cardW = clampf(420.f * uiScale, 320.f, 560.f);
                        const float cardH = clampf(640.f * uiScale, 440.f, static_cast<float>(size.y) - 40.f * uiScale);
                        const float cardX = (static_cast<float>(size.x) - cardW) * 0.5f;
                        const float cardY = (static_cast<float>(size.y) - cardH) * 0.5f;
                        const bool insideCard = (mx >= cardX && mx <= cardX + cardW && my >= cardY && my <= cardY + cardH);
                        if (!insideCard) {
                            profileOpen = false;
                            continue;
                        }
                    }

                    if (settingsOpen) {
                        const bool insidePanel = (mx <= settingsPanelW);
                        if (!insidePanel) {
                            settingsOpen = false;
                            continue;
                        }

                        const float avatarRadius = clampf(44.f * uiScale, 30.f, 58.f);
                        const float avatarCy = 52.f * uiScale + avatarRadius;
                        const float itemsStartY = avatarCy + avatarRadius + 96.f * uiScale;
                        const float itemH = 34.f * uiScale;
                        const float itemGap = 10.f * uiScale;
                        const float itemX = 16.f * uiScale;
                        const float itemW = settingsPanelW - 32.f * uiScale;

                        for (std::size_t i = 0; i < settingsItems.size(); ++i) {
                            const float itemY = itemsStartY + static_cast<float>(i) * (itemH + itemGap);
                            const bool hit = (mx >= itemX && mx <= itemX + itemW && my >= itemY && my <= itemY + itemH);
                            if (hit && i == 0) {
                                settingsOpen = false;
                                profileOpen = true;
                                break;
                            }
                            if (hit && i == settingsItems.size() - 2) {
                                settingsOpen = false;
                                advancedSettingsOpen = true;
                                break;
                            }
                            if (hit && i == settingsItems.size() - 1) {
                                settingsOpen = false;
                                serverSettingsOpen = true;
                                serverIpDraft = serverIp;
                                serverPortDraft = std::to_string(serverPort);
                                break;
                            }
                        }
                    }

                    if (serverSettingsOpen) {
                        const float cardW = clampf(520.f * uiScale, 380.f, 640.f);
                        const float cardH = clampf(420.f * uiScale, 320.f, 520.f);
                        const float cardX = (static_cast<float>(size.x) - cardW) * 0.5f;
                        const float cardY = (static_cast<float>(size.y) - cardH) * 0.5f;
                        const sf::FloatRect ipRect({cardX + 28.f * uiScale, cardY + 150.f * uiScale}, {cardW - 56.f * uiScale, 52.f * uiScale});
                        const sf::FloatRect portRect({cardX + 28.f * uiScale, cardY + 232.f * uiScale}, {cardW - 56.f * uiScale, 52.f * uiScale});
                        const sf::FloatRect cancelRect({cardX + cardW - 232.f * uiScale, cardY + cardH - 60.f * uiScale}, {96.f * uiScale, 36.f * uiScale});
                        const sf::FloatRect okRect({cardX + cardW - 124.f * uiScale, cardY + cardH - 60.f * uiScale}, {96.f * uiScale, 36.f * uiScale});

                        if (okRect.contains({mx, my})) {
                            serverIp = serverIpDraft;
                            if (!serverPortDraft.empty()) serverPort = std::stoi(serverPortDraft);
                            serverSettingsOpen = false;
                            serverIpInputActive = false;
                            serverPortInputActive = false;

                            connected = client.connectTo(serverIp, serverPort, "sfml_user");
                            addLog("[CLIENT] Target: " + serverIp + ":" + std::to_string(serverPort));
                            addLog(connected ? "[CLIENT] Reconnected" : "[CLIENT] Connection failed: " + client.lastError());
                        } else if (cancelRect.contains({mx, my})) {
                            serverIpDraft = serverIp;
                            serverPortDraft = std::to_string(serverPort);
                            serverSettingsOpen = false;
                            serverIpInputActive = false;
                            serverPortInputActive = false;
                        } else {
                            serverIpInputActive = ipRect.contains({mx, my});
                            serverPortInputActive = portRect.contains({mx, my});
                        }
                    }

                    if (serverSettingsOpen) {
                        const float cardW = clampf(520.f * uiScale, 380.f, 640.f);
                        const float cardH = clampf(420.f * uiScale, 320.f, 520.f);
                        const float cardX = (static_cast<float>(size.x) - cardW) * 0.5f;
                        const float cardY = (static_cast<float>(size.y) - cardH) * 0.5f;
                        const sf::FloatRect inputRect({cardX + 28.f * uiScale, cardY + 182.f * uiScale}, {cardW - 56.f * uiScale, 52.f * uiScale});
                        serverIpInputActive = inputRect.contains({mx,my});
                        if (!serverIpInputActive) serverIp = serverIpDraft;
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

            const float itemsStartY = avatarCy + avatarRadius + 96.f * uiScale;
            const float itemH = 34.f * uiScale;
            const float itemGap = 10.f * uiScale;
            for (std::size_t i = 0; i < settingsItems.size(); ++i) {
                const float itemY = itemsStartY + static_cast<float>(i) * (itemH + itemGap);
                const sf::FloatRect itemRect({panelX + 16.f * uiScale, itemY}, {settingsPanelW - 32.f * uiScale, itemH});
                const bool hovered = itemRect.contains(mousePos);

                sf::RectangleShape itemBg(itemRect.size);
                itemBg.setPosition(itemRect.position);
                itemBg.setFillColor(hovered ? sf::Color(255, 255, 255, 44) : sf::Color(255, 255, 255, 16));
                window.draw(itemBg);

                sf::Text itemText(font, settingsItems[i], fontSize(14, uiScale));
                itemText.setFillColor(hovered ? sf::Color(245, 250, 255) : sf::Color(186, 209, 230));
                itemText.setPosition({itemRect.position.x + 12.f * uiScale, itemRect.position.y + 7.f * uiScale});
                window.draw(itemText);
            }
        }


        const float targetServerSettings = serverSettingsOpen ? 1.f : 0.f;
        serverSettingsAnim += (targetServerSettings - serverSettingsAnim) * 0.16f;

        if (serverSettingsAnim > 0.01f) {
            sf::RectangleShape dimSrv({static_cast<float>(size.x), static_cast<float>(size.y)});
            dimSrv.setFillColor(sf::Color(0, 0, 0, static_cast<std::uint8_t>(120.f * serverSettingsAnim)));
            window.draw(dimSrv);

            const float cardW = clampf(520.f * uiScale, 380.f, 640.f);
            const float cardH = clampf(420.f * uiScale, 320.f, 520.f);
            const float cardX = (static_cast<float>(size.x) - cardW) * 0.5f;
            const float cardY = (static_cast<float>(size.y) - cardH) * 0.5f;
            const float offsetY = (1.f - serverSettingsAnim) * 20.f * uiScale;

            sf::RectangleShape cardSrv({cardW, cardH});
            cardSrv.setPosition({cardX, cardY + offsetY});
            cardSrv.setFillColor(sf::Color(14, 27, 42));
            window.draw(cardSrv);

            sf::Text titleSrv(font, "Additional settings", fontSize(24, uiScale));
            titleSrv.setFillColor(sf::Color(228, 238, 248));
            titleSrv.setPosition({cardX + 28.f * uiScale, cardY + 22.f * uiScale + offsetY});
            window.draw(titleSrv);

            const sf::FloatRect ipRect({cardX + 28.f * uiScale, cardY + 150.f * uiScale + offsetY}, {cardW - 56.f * uiScale, 52.f * uiScale});
            sf::RectangleShape ipBg(ipRect.size);
            ipBg.setPosition(ipRect.position);
            ipBg.setFillColor(sf::Color(20, 36, 54));
            window.draw(ipBg);

            sf::Text ipLabel(font, "Editing: server ip", fontSize(13, uiScale));
            ipLabel.setFillColor(sf::Color(138, 169, 195));
            ipLabel.setPosition({ipRect.position.x + 12.f * uiScale, ipRect.position.y + 6.f * uiScale});
            window.draw(ipLabel);

            sf::Text ipValue(font, serverIpDraft.empty() ? "0.0.0.0" : serverIpDraft, fontSize(18, uiScale));
            ipValue.setFillColor(sf::Color(225, 238, 249));
            ipValue.setPosition({ipRect.position.x + 12.f * uiScale, ipRect.position.y + 24.f * uiScale});
            window.draw(ipValue);

            sf::RectangleShape ipUnderline({ipRect.size.x, 3.f * uiScale});
            ipUnderline.setPosition({ipRect.position.x, ipRect.position.y + ipRect.size.y - 3.f * uiScale});
            ipUnderline.setFillColor(serverIpInputActive ? sf::Color(86, 177, 255) : sf::Color(109, 125, 139));
            window.draw(ipUnderline);

            const sf::FloatRect portRect({cardX + 28.f * uiScale, cardY + 232.f * uiScale + offsetY}, {cardW - 56.f * uiScale, 52.f * uiScale});
            sf::RectangleShape portBg(portRect.size);
            portBg.setPosition(portRect.position);
            portBg.setFillColor(sf::Color(20, 36, 54));
            window.draw(portBg);

            sf::Text portLabel(font, "Editing: server port", fontSize(13, uiScale));
            portLabel.setFillColor(sf::Color(138, 169, 195));
            portLabel.setPosition({portRect.position.x + 12.f * uiScale, portRect.position.y + 6.f * uiScale});
            window.draw(portLabel);

            sf::Text portValue(font, serverPortDraft.empty() ? "5000" : serverPortDraft, fontSize(18, uiScale));
            portValue.setFillColor(sf::Color(225, 238, 249));
            portValue.setPosition({portRect.position.x + 12.f * uiScale, portRect.position.y + 24.f * uiScale});
            window.draw(portValue);

            sf::RectangleShape portUnderline({portRect.size.x, 3.f * uiScale});
            portUnderline.setPosition({portRect.position.x, portRect.position.y + portRect.size.y - 3.f * uiScale});
            portUnderline.setFillColor(serverPortInputActive ? sf::Color(86, 177, 255) : sf::Color(109, 125, 139));
            window.draw(portUnderline);

            const sf::FloatRect cancelRect({cardX + cardW - 232.f * uiScale, cardY + cardH - 60.f * uiScale + offsetY}, {96.f * uiScale, 36.f * uiScale});
            const sf::FloatRect okRect({cardX + cardW - 124.f * uiScale, cardY + cardH - 60.f * uiScale + offsetY}, {96.f * uiScale, 36.f * uiScale});
            sf::RectangleShape cancelBtn(cancelRect.size); cancelBtn.setPosition(cancelRect.position); cancelBtn.setFillColor(sf::Color(63, 78, 95)); window.draw(cancelBtn);
            sf::RectangleShape okBtn(okRect.size); okBtn.setPosition(okRect.position); okBtn.setFillColor(sf::Color(49, 118, 188)); window.draw(okBtn);
            sf::Text cancelText(font, "Cancel", fontSize(14, uiScale)); cancelText.setFillColor(sf::Color(226, 236, 246)); cancelText.setPosition({cancelRect.position.x + 18.f * uiScale, cancelRect.position.y + 8.f * uiScale}); window.draw(cancelText);
            sf::Text okText(font, "OK", fontSize(14, uiScale)); okText.setFillColor(sf::Color(236, 246, 255)); okText.setPosition({okRect.position.x + 34.f * uiScale, okRect.position.y + 8.f * uiScale}); window.draw(okText);
        }


        const float targetAdvancedSettings = advancedSettingsOpen ? 1.f : 0.f;
        advancedSettingsAnim += (targetAdvancedSettings - advancedSettingsAnim) * 0.16f;

        if (advancedSettingsAnim > 0.01f) {
            sf::RectangleShape dim3({static_cast<float>(size.x), static_cast<float>(size.y)});
            dim3.setFillColor(sf::Color(0, 0, 0, static_cast<std::uint8_t>(130.f * advancedSettingsAnim)));
            window.draw(dim3);

            const float cardW = clampf(500.f * uiScale, 360.f, 600.f);
            const float cardH = clampf(700.f * uiScale, 520.f, static_cast<float>(size.y) - 24.f * uiScale);
            const float cardX = (static_cast<float>(size.x) - cardW) * 0.5f;
            const float cardY = (static_cast<float>(size.y) - cardH) * 0.5f;
            const float offsetY = (1.f - advancedSettingsAnim) * 20.f * uiScale;

            sf::RectangleShape card({cardW, cardH});
            card.setPosition({cardX, cardY + offsetY});
            card.setFillColor(sf::Color(14, 27, 42));
            window.draw(card);

            sf::Text title(font, "Settings", fontSize(24, uiScale));
            title.setFillColor(sf::Color(228, 238, 248));
            title.setPosition({cardX + 24.f * uiScale, cardY + 16.f * uiScale + offsetY});
            window.draw(title);

            const float topBlockY = cardY + 64.f * uiScale + offsetY;
            sf::RectangleShape topBlock({cardW, 99.2f * uiScale});
            topBlock.setPosition({cardX, topBlockY});
            topBlock.setFillColor(sf::Color(19, 34, 51));
            window.draw(topBlock);

            sf::CircleShape avatar(clampf(34.f * uiScale, 24.f, 42.f));
            avatar.setFillColor(sf::Color(192, 44, 56));
            avatar.setPosition({cardX + 20.f * uiScale, topBlockY + 18.f * uiScale});
            window.draw(avatar);

            sf::Text name(font, "Username", fontSize(20, uiScale));
            name.setFillColor(sf::Color(231, 240, 249));
            name.setPosition({cardX + 102.f * uiScale, topBlockY + 26.f * uiScale});
            window.draw(name);

            sf::Text handle(font, "@username", fontSize(15, uiScale));
            handle.setFillColor(sf::Color(134, 171, 202));
            handle.setPosition({cardX + 102.f * uiScale, topBlockY + 58.f * uiScale});
            window.draw(handle);

            float itemY = topBlockY + 112.f * uiScale;
            const float itemH = 35.2f * uiScale;
            for (std::size_t i = 0; i < advancedSettingsItems.size(); ++i) {
                const sf::FloatRect itemRect({cardX + 18.f * uiScale, itemY}, {cardW - 36.f * uiScale, itemH});
                const bool hovered = itemRect.contains(mousePos);

                sf::RectangleShape bg(itemRect.size);
                bg.setPosition(itemRect.position);
                bg.setFillColor(hovered ? sf::Color(255, 255, 255, 30) : sf::Color(255, 255, 255, 10));
                window.draw(bg);

                sf::Text row(font, advancedSettingsItems[i], fontSize(16, uiScale));
                row.setFillColor(sf::Color(214, 230, 242));
                row.setPosition({itemRect.position.x + 14.f * uiScale, itemRect.position.y + 7.f * uiScale});
                window.draw(row);

                itemY += itemH + 6.4f * uiScale;
                if (itemY > cardY + cardH - 60.f * uiScale + offsetY) break;
            }
        }


        const float targetProfile = profileOpen ? 1.f : 0.f;
        profileAnim += (targetProfile - profileAnim) * 0.16f;

        if (profileAnim > 0.01f) {
            sf::RectangleShape dim2({static_cast<float>(size.x), static_cast<float>(size.y)});
            dim2.setFillColor(sf::Color(0, 0, 0, static_cast<std::uint8_t>(120.f * profileAnim)));
            window.draw(dim2);

            const float cardW = clampf(420.f * uiScale, 320.f, 560.f);
            const float cardH = clampf(640.f * uiScale, 440.f, static_cast<float>(size.y) - 40.f * uiScale);
            const float cardX = (static_cast<float>(size.x) - cardW) * 0.5f;
            const float cardY = (static_cast<float>(size.y) - cardH) * 0.5f;

            sf::RectangleShape card({cardW, cardH});
            card.setPosition({cardX, cardY + (1.f - profileAnim) * 20.f * uiScale});
            card.setFillColor(sf::Color(24, 40, 58));
            window.draw(card);

            const float topH = cardH * 0.34f;
            sf::RectangleShape topPart({cardW, topH});
            topPart.setPosition({cardX, cardY + (1.f - profileAnim) * 20.f * uiScale});
            topPart.setFillColor(sf::Color(43, 62, 86));
            window.draw(topPart);

            const float bodyY = cardY + (1.f - profileAnim) * 20.f * uiScale + topH;
            sf::RectangleShape bodyPart({cardW, cardH - topH});
            bodyPart.setPosition({cardX, bodyY});
            bodyPart.setFillColor(sf::Color(28, 44, 63));
            window.draw(bodyPart);

            const float pAvatarR = clampf(46.f * uiScale, 32.f, 64.f);
            const float pAvatarCX = cardX + cardW * 0.5f;
            const float pAvatarCY = cardY + (1.f - profileAnim) * 20.f * uiScale + 42.f * uiScale + pAvatarR;
            sf::CircleShape pAvatar(pAvatarR);
            pAvatar.setFillColor(sf::Color(207, 56, 67));
            pAvatar.setPosition({pAvatarCX - pAvatarR, pAvatarCY - pAvatarR});
            window.draw(pAvatar);

            sf::Text pName(font, "Username", fontSize(22, uiScale));
            pName.setFillColor(sf::Color(232, 241, 252));
            auto b1 = pName.getLocalBounds();
            pName.setPosition({pAvatarCX - b1.size.x * 0.5f, pAvatarCY + pAvatarR + 12.f * uiScale});
            window.draw(pName);

            sf::Text pStatus(font, connected ? "online" : "offline", fontSize(16, uiScale));
            pStatus.setFillColor(connected ? sf::Color(105, 223, 151) : sf::Color(170, 186, 206));
            auto b2 = pStatus.getLocalBounds();
            pStatus.setPosition({pAvatarCX - b2.size.x * 0.5f, pAvatarCY + pAvatarR + 42.f * uiScale});
            window.draw(pStatus);

            const float infoX = cardX + 26.f * uiScale;
            float infoY = bodyY + 22.f * uiScale;
            sf::Text t1(font, "@username", fontSize(18, uiScale)); t1.setFillColor(sf::Color(104, 176, 255)); t1.setPosition({infoX, infoY}); window.draw(t1);
            infoY += 42.f * uiScale;
            sf::Text t2(font, "About me", fontSize(15, uiScale)); t2.setFillColor(sf::Color(143, 172, 197)); t2.setPosition({infoX, infoY}); window.draw(t2);
            infoY += 25.f * uiScale;
            sf::Text t3(font, "I like coding and music", fontSize(16, uiScale)); t3.setFillColor(sf::Color(210, 225, 239)); t3.setPosition({infoX, infoY}); window.draw(t3);
            infoY += 48.f * uiScale;
            sf::Text t4(font, "Birthday", fontSize(15, uiScale)); t4.setFillColor(sf::Color(143, 172, 197)); t4.setPosition({infoX, infoY}); window.draw(t4);
            infoY += 25.f * uiScale;
            sf::Text t5(font, "10 Jan", fontSize(16, uiScale)); t5.setFillColor(sf::Color(210, 225, 239)); t5.setPosition({infoX, infoY}); window.draw(t5);
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
