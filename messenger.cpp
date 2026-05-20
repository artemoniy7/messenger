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
#include <set>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace {

struct ServerInfo {
    std::string ip;
    int port;
};

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

// Network utilities for server discovery
#ifdef _WIN32
    void closeSocket(SocketHandle s) { closesocket(s); }
#else
    void closeSocket(SocketHandle s) { close(s); }
#endif

bool initSockets() {
#ifdef _WIN32
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void cleanupSockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}

std::optional<ServerInfo> discoverServerViaBroadcast(int timeoutSeconds = 2) {
    SocketHandle sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidSocket) return std::nullopt;
    
    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast)) < 0) {
        closeSocket(sock);
        return std::nullopt;
    }
    
    // Set timeout
#ifdef _WIN32
    DWORD timeout = timeoutSeconds * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = timeoutSeconds;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    
    sockaddr_in broadcastAddr{};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_port = htons(5001);
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;
    
    const char* discoverMsg = "DISCOVER_MESSENGER";
    sendto(sock, discoverMsg, strlen(discoverMsg), 0, 
           reinterpret_cast<sockaddr*>(&broadcastAddr), sizeof(broadcastAddr));
    
    char buffer[256];
    sockaddr_in fromAddr{};
#ifdef _WIN32
    int fromLen = sizeof(fromAddr);
#else
    socklen_t fromLen = sizeof(fromAddr);
#endif
    
    int bytes = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                         reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
    
    closeSocket(sock);
    
    if (bytes > 0) {
        buffer[bytes] = '\0';
        std::string response(buffer);
        
        if (response.rfind("MESSENGER_SERVER ", 0) == 0) {
            std::string addrStr = response.substr(17);
            size_t colonPos = addrStr.find(':');
            if (colonPos != std::string::npos) {
                ServerInfo info;
                info.ip = addrStr.substr(0, colonPos);
                info.port = std::stoi(addrStr.substr(colonPos + 1));
                return info;
            }
        }
    }
    
    return std::nullopt;
}

bool testConnection(const std::string& ip, int port, int timeoutMs = 300) {
    SocketHandle sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == kInvalidSocket) return false;
    
    // Set non-blocking mode
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    
    connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    
    // Wait for connection
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(sock, &fdset);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    
    int result = select(sock + 1, nullptr, &fdset, nullptr, &tv);
    
    closeSocket(sock);
    return result > 0;
}

std::vector<ServerInfo> scanLocalNetwork(int port = 5000) {
    std::vector<ServerInfo> servers;
    std::set<std::string> foundIPs;
    
    // Common local network patterns
    std::vector<std::string> subnets;
    
    // Add localhost
    subnets.push_back("127.0.0.");
    
    // Get local IP addresses
#ifdef _WIN32
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct hostent* he = gethostbyname(hostname);
        if (he) {
            for (int i = 0; he->h_addr_list[i] != nullptr; ++i) {
                struct in_addr addr;
                memcpy(&addr, he->h_addr_list[i], sizeof(addr));
                std::string ip = inet_ntoa(addr);
                if (ip.find("192.168.") == 0) {
                    subnets.push_back("192.168." + ip.substr(8, ip.find('.', 8) - 8) + ".");
                } else if (ip.find("10.") == 0) {
                    subnets.push_back("10." + ip.substr(2, ip.find('.', 2) - 2) + ".");
                } else if (ip.find("172.") == 0) {
                    subnets.push_back("172." + ip.substr(4, ip.find('.', 4) - 4) + ".");
                }
            }
        }
    }
#else
    // Linux: could use getifaddrs, but for simplicity use common subnets
    subnets.push_back("192.168.1.");
    subnets.push_back("192.168.0.");
    subnets.push_back("10.0.0.");
    subnets.push_back("172.16.");
#endif
    
    // Add common subnets if none found
    if (subnets.size() <= 1) {
        subnets.push_back("192.168.1.");
        subnets.push_back("192.168.0.");
        subnets.push_back("10.0.0.");
    }
    
    // Remove duplicates
    std::sort(subnets.begin(), subnets.end());
    subnets.erase(std::unique(subnets.begin(), subnets.end()), subnets.end());
    
    std::cout << "[DISCOVERY] Scanning local network for server..." << std::endl;
    
    for (const auto& subnet : subnets) {
        // Scan only first 25 IPs to be fast, or full range for small subnets
        int maxIP = (subnet.find("172.") == 0) ? 32 : 25;
        
        for (int i = 1; i <= maxIP; ++i) {
            std::string ip = subnet + std::to_string(i);
            if (foundIPs.find(ip) != foundIPs.end()) continue;
            
            if (testConnection(ip, port)) {
                servers.push_back({ip, port});
                foundIPs.insert(ip);
                std::cout << "[DISCOVERY] Found potential server at " << ip << ":" << port << std::endl;
            }
        }
    }
    
    return servers;
}

ServerInfo findServer() {
    std::cout << "[DISCOVERY] Searching for messenger server..." << std::endl;
    
    // First try UDP broadcast (fastest)
    auto server = discoverServerViaBroadcast(2);
    if (server.has_value()) {
        std::cout << "[DISCOVERY] Server found via broadcast at " << server->ip << ":" << server->port << std::endl;
        return *server;
    }
    
    // Then try localhost
    if (testConnection("127.0.0.1", 5000)) {
        std::cout << "[DISCOVERY] Server found on localhost" << std::endl;
        return {"127.0.0.1", 5000};
    }
    
    // Finally scan the local network
    auto servers = scanLocalNetwork(5000);
    if (!servers.empty()) {
        std::cout << "[DISCOVERY] Using first discovered server at " << servers[0].ip << ":" << servers[0].port << std::endl;
        return servers[0];
    }
    
    // No server found, use default
    std::cout << "[DISCOVERY] No server found, will try localhost (waiting for server to start)..." << std::endl;
    return {"127.0.0.1", 5000};
}

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
    std::string serverIp;
    int serverPort = 5000;
    std::string username = "sfml_user";
    
    // Parse command line arguments
    if (argc > 1) {
        // If IP is provided as argument, use it directly
        serverIp = argv[1];
        if (argc > 2) serverPort = std::stoi(argv[2]);
        if (argc > 3) username = argv[3];
        std::cout << "[CLIENT] Using manual server: " << serverIp << ":" << serverPort << std::endl;
    } else {
        // Auto-discover server
        ServerInfo found = findServer();
        serverIp = found.ip;
        serverPort = found.port;
        
        // Try to connect with retries
        int attempts = 0;
        bool connected = false;
        while (!connected && attempts < 5) {
            TcpClient testClient;
            if (testClient.connectTo(serverIp, serverPort, "test")) {
                connected = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            attempts++;
            std::cout << "[CLIENT] Retry " << attempts << "/5..." << std::endl;
        }
        
        if (!connected) {
            std::cout << "[CLIENT] Could not connect to discovered server, trying localhost..." << std::endl;
            serverIp = "127.0.0.1";
        }
    }

    sf::RenderWindow window(sf::VideoMode({1366, 768}), "Messenger SFML", sf::Style::Default);
    window.setFramerateLimit(60);

    sf::Font font;
    const std::vector<std::string> fontCandidates = {
        "C:/Windows/Fonts/arial.ttf", 
        "C:/Windows/Fonts/segoeui.ttf", 
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Helvetica.ttc"
    };
    bool fontLoaded = false;
    for (const auto& path : fontCandidates) {
        if (font.openFromFile(path)) { fontLoaded = true; break; }
    }
    if (!fontLoaded) {
        std::cerr << "FATAL: Could not load any font!" << std::endl;
        return 1;
    }

    ChatStorage storage("chat_history.txt");
    std::vector<ChatItem> chats = storage.load();
    std::size_t selectedChat = 0;
    bool settingsOpen = false;
    float settingsAnim = 0.f;
    bool profileOpen = false;
    float profileAnim = 0.f;
    const std::vector<std::string> settingsItems = {"My profile", "Create group", "Contacts", "Favorites", "Settings"};

    TcpClient client;
    bool connected = client.connectTo(serverIp, serverPort, username);
    
    // Update username in profile if connected
    if (connected) {
        username = "user_" + std::to_string(static_cast<int>(std::chrono::steady_clock::now().time_since_epoch().count() % 10000));
    }

    std::vector<std::string> serverLog;
    auto addLog = [&](const std::string& line) {
        serverLog.push_back(line);
        if (serverLog.size() > 10) serverLog.erase(serverLog.begin());
    };
    
    if (connected) {
        addLog("[CLIENT] Connected to " + serverIp + ":" + std::to_string(serverPort));
    } else {
        addLog("[CLIENT] Connection failed: " + client.lastError());
        addLog("[CLIENT] Make sure server is running (python server.py)");
    }

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
            if (const auto* pressed = event->getIf<sf::Event::MouseButtonPressed>()) {
                if (pressed->button == sf::Mouse::Button::Left) {
                    const float mx = static_cast<float>(pressed->position.x);
                    const float my = static_cast<float>(pressed->position.y);

                    const bool onSettingsButton = (mx >= 8.f * uiScale && mx <= leftW - 8.f * uiScale && my >= 12.f * uiScale && my <= 12.f * uiScale + settingsBtnH);
                    if (onSettingsButton) {
                        settingsOpen = true;
                        continue;
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

        // Process incoming messages
        for (const auto& message : client.consumeMessages()) {
            addLog(message);
            if (selectedChat < chats.size()) {
                chats[selectedChat].messages.push_back(message);
            } else if (!chats.empty()) {
                chats[0].messages.push_back(message);
            }
        }

        window.clear(sf::Color(3, 22, 46));

        // Left sidebar with avatars
        sf::RectangleShape avatarsBar({leftW, static_cast<float>(size.y)});
        avatarsBar.setFillColor(sf::Color(18, 33, 49));
        window.draw(avatarsBar);

        // Settings button
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

        // Chat avatars
        const float radius = clampf(22.f * uiScale, 16.f, 34.f);
        const float startY = topPad + 28.f * uiScale;
        const float stepY = 58.f * uiScale;
        for (std::size_t i = 0; i < chats.size(); ++i) {
            sf::CircleShape avatar(radius);
            avatar.setPosition({leftW * 0.5f - radius, startY + stepY * static_cast<float>(i)});
            avatar.setFillColor(i == selectedChat ? sf::Color(58, 149, 245) : avatarColor(i));
            window.draw(avatar);
        }

        // Main chat area
        const float chatX = leftW;
        sf::RectangleShape workspace({static_cast<float>(size.x) - chatX, static_cast<float>(size.y)});
        workspace.setPosition({chatX, 0.f});
        workspace.setFillColor(sf::Color(1, 17, 37));
        window.draw(workspace);

        // Chat messages
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

        // Server log area
        float logY = static_cast<float>(size.y) - (serverLog.size() * (18.f * uiScale)) - 12.f * uiScale;
        for (const auto& line : serverLog) {
            sf::Text row(font, line, fontSize(12, uiScale));
            row.setFillColor(sf::Color(120, 150, 176));
            row.setPosition({chatX + 20.f * uiScale, logY});
            window.draw(row);
            logY += 18.f * uiScale;
        }

        // Settings panel animation
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

            sf::Text profileName(font, username, fontSize(20, uiScale));
            profileName.setFillColor(sf::Color(220, 235, 247));
            const auto profileBounds = profileName.getLocalBounds();
            profileName.setPosition({avatarCx - profileBounds.size.x * 0.5f, avatarCy + avatarRadius + 18.f * uiScale});
            window.draw(profileName);

            sf::Text profileHandle(font, "@" + username, fontSize(15, uiScale));
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

        // Profile panel animation
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

            sf::Text pName(font, username, fontSize(22, uiScale));
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
            sf::Text t1(font, "@" + username, fontSize(18, uiScale)); 
            t1.setFillColor(sf::Color(104, 176, 255)); 
            t1.setPosition({infoX, infoY}); 
            window.draw(t1);
            infoY += 42.f * uiScale;
            sf::Text t2(font, "About me", fontSize(15, uiScale)); 
            t2.setFillColor(sf::Color(143, 172, 197)); 
            t2.setPosition({infoX, infoY}); 
            window.draw(t2);
            infoY += 25.f * uiScale;
            sf::Text t3(font, "Messenger user", fontSize(16, uiScale)); 
            t3.setFillColor(sf::Color(210, 225, 239)); 
            t3.setPosition({infoX, infoY}); 
            window.draw(t3);
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
