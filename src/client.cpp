#include "network/socket_utils.h"

#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string Quote(const std::string& value) {
    std::ostringstream output;
    output << std::quoted(value);
    return output.str();
}

void PrintUsage(const char* program) {
    std::cout << "Usage: " << program << " [--host <IPv4>] [--port <port>]\n";
}

bool ParseOptions(int argc, char* argv[], std::string* host, int* port) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return false;
        }
        if ((arg == "--host" || arg == "--port") && i + 1 >= argc) {
            std::cerr << "Missing value for " << arg << "\n";
            return false;
        }
        if (arg == "--host") *host = argv[++i];
        else if (arg == "--port") {
            try { *port = std::stoi(argv[++i]); }
            catch (...) { std::cerr << "Invalid port\n"; return false; }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            PrintUsage(argv[0]);
            return false;
        }
    }
    return *port >= 1 && *port <= 65535;
}

std::string BuildRequest(const std::string& line, bool* should_exit) {
    std::istringstream input(line);
    std::string operation;
    input >> operation;
    for (char& ch : operation) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    *should_exit = operation == "EXIT" || operation == "QUIT";

    if (operation == "HELP" || operation.empty() || *should_exit) return {};
    if (operation == "PUT") {
        std::string key;
        std::string value;
        input >> key;
        std::getline(input, value);
        const auto first = value.find_first_not_of(" \t");
        if (key.empty() || first == std::string::npos) return {};
        value = value.substr(first);
        return "PUT " + Quote(key) + " " + Quote(value) + "\n";
    }
    if (operation == "GET" || operation == "DELETE" || operation == "DEL") {
        std::string key;
        input >> key;
        if (key.empty()) return {};
        if (operation == "DEL") operation = "DELETE";
        return operation + " " + Quote(key) + "\n";
    }
    if (operation == "INFO") return "INFO\n";
    return {};
}

void PrintResponse(const std::string& response) {
    if (response.rfind("VALUE ", 0) == 0) {
        std::istringstream input(response.substr(6));
        std::string value;
        input >> std::quoted(value);
        std::cout << "VALUE: \"" << value << "\"\n";
    } else if (response.rfind("ERROR ", 0) == 0) {
        std::istringstream input(response.substr(6));
        std::string message;
        input >> std::quoted(message);
        std::cout << "ERROR: " << message << "\n";
    } else {
        std::cout << response << "\n";
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 50051;
    if (!ParseOptions(argc, argv, &host, &port)) return 1;

    lsm::net::Runtime runtime;
    if (!runtime.ready()) {
        std::cerr << "Could not initialize the socket runtime\n";
        return 1;
    }

    const auto connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connection == lsm::net::kInvalidSocket) {
        std::cerr << "Could not create client socket\n";
        return 1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1 ||
        connect(connection, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "Could not connect to " << host << ':' << port << ". Is the server running?\n";
        lsm::net::Close(connection);
        return 1;
    }

    std::cout << "Connected to LSM demo server at " << host << ':' << port << "\n"
              << "Commands: PUT <key> <value>, GET <key>, DELETE <key>, INFO, HELP, EXIT\n"
              << "--------------------------------------------------------\n";
    std::string line;
    std::cout << "client> " << std::flush;
    while (std::getline(std::cin, line)) {
        bool should_exit = false;
        const std::string request = BuildRequest(line, &should_exit);
        if (should_exit) break;
        if (line == "HELP" || line == "help" || line == "Help") {
            std::cout << "PUT <key> <value>  Store or overwrite a value\n"
                      << "GET <key>          Read a value\n"
                      << "DELETE <key>       Remove a key\n"
                      << "INFO               Show server storage stats\n"
                      << "EXIT               Disconnect\n";
        } else if (request.empty()) {
            std::cout << "Invalid command or arguments. Type HELP.\n";
        } else if (!lsm::net::SendAll(connection, request)) {
            std::cerr << "Connection lost while sending request\n";
            break;
        } else {
            std::string response;
            if (!lsm::net::ReceiveLine(connection, &response)) {
                std::cerr << "Connection lost while waiting for server response\n";
                break;
            }
            PrintResponse(response);
        }
        std::cout << "client> " << std::flush;
    }
    lsm::net::Close(connection);
    std::cout << "Disconnected from server.\n";
    return 0;
}
