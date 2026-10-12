#include "common/config.h"
#include "network/socket_utils.h"
#include "storage/basic_storage_engine.h"
#include "storage/storage_metrics.h"

#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::string Quote(const std::string& value) {
    std::ostringstream output;
    output << std::quoted(value);
    return output.str();
}

void PrintUsage(const char* program) {
    std::cout << "Usage: " << program << " [--port <port>] [--metrics-port <port>] [--data_dir <path>]\n";
}

bool ParseOptions(int argc, char* argv[], int* port, int* metrics_port, std::string* data_dir) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return false;
        }
        if ((arg == "--port" || arg == "--metrics-port" || arg == "--data_dir") && i + 1 >= argc) {
            std::cerr << "Missing value for " << arg << "\n";
            return false;
        }
        if (arg == "--port") {
            try { *port = std::stoi(argv[++i]); }
            catch (...) { std::cerr << "Invalid port\n"; return false; }
        } else if (arg == "--metrics-port") {
            try { *metrics_port = std::stoi(argv[++i]); }
            catch (...) { std::cerr << "Invalid metrics port\n"; return false; }
        } else if (arg == "--data_dir") {
            *data_dir = argv[++i];
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            PrintUsage(argv[0]);
            return false;
        }
    }
    if (*port < 1 || *port > 65535 || *metrics_port < 1 || *metrics_port > 65535 || *port == *metrics_port) {
        std::cerr << "Ports must be distinct values between 1 and 65535\n";
        return false;
    }
    return true;
}

lsm::net::Socket OpenMetricsListener(int port) {
    const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == lsm::net::kInvalidSocket) return listener;
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 8) != 0) {
        lsm::net::Close(listener);
        return lsm::net::kInvalidSocket;
    }
    return listener;
}

void ServeMetrics(lsm::net::Socket listener, const lsm::StorageMetrics& metrics,
                  const std::atomic<bool>& stop) {
    while (!stop.load()) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval timeout{};
        timeout.tv_usec = 100000;
#ifdef _WIN32
        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
#else
        const int ready = select(listener + 1, &readable, nullptr, nullptr, &timeout);
#endif
        if (ready <= 0 || !FD_ISSET(listener, &readable)) continue;
        sockaddr_in peer{};
#ifdef _WIN32
        int peer_length = sizeof(peer);
#else
        socklen_t peer_length = sizeof(peer);
#endif
        const auto client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (client == lsm::net::kInvalidSocket) continue;
        std::string request;
        bool got_request = lsm::net::ReceiveLine(client, &request);
        std::string header;
        while (got_request && lsm::net::ReceiveLine(client, &header) && !header.empty()) {}
        const std::string body = got_request && request.rfind("GET /metrics ", 0) == 0
            ? metrics.ExportPrometheus() : "not found\n";
        const std::string status = got_request && request.rfind("GET /metrics ", 0) == 0
            ? "200 OK" : "404 Not Found";
        const std::string response = "HTTP/1.1 " + status + "\r\nContent-Type: text/plain; version=0.0.4; charset=utf-8\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        lsm::net::SendAll(client, response);
        lsm::net::Close(client);
    }
}

std::string HandleRequest(lsm::BasicStorageEngine& storage,
                          lsm::StorageMetrics& metrics,
                          const std::string& request,
                          std::string* description) {
    std::istringstream input(request);
    std::string operation;
    std::string key;
    std::string value;
    input >> operation;
    for (char& ch : operation) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

    if (operation == "PUT") {
        if (!(input >> std::quoted(key) >> std::quoted(value))) {
            *description = "malformed PUT";
            return "ERROR \"PUT requires a key and value\"\n";
        }
        const auto start = std::chrono::steady_clock::now();
        const auto status = storage.Put(key, value);
        metrics.ObserveOperation("PUT", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()), status.ok());
        *description = "PUT key=" + Quote(key);
        return status.ok() ? "OK\n" : "ERROR " + Quote(status.ToString()) + "\n";
    }
    if (operation == "GET") {
        if (!(input >> std::quoted(key))) {
            *description = "malformed GET";
            return "ERROR \"GET requires a key\"\n";
        }
        const auto start = std::chrono::steady_clock::now();
        const auto status = storage.Get(key, &value);
        metrics.ObserveOperation("GET", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()), status.ok());
        *description = "GET key=" + Quote(key);
        if (status.ok()) return "VALUE " + Quote(value) + "\n";
        return "ERROR " + Quote(status.ToString()) + "\n";
    }
    if (operation == "DELETE" || operation == "DEL") {
        if (!(input >> std::quoted(key))) {
            *description = "malformed DELETE";
            return "ERROR \"DELETE requires a key\"\n";
        }
        const auto start = std::chrono::steady_clock::now();
        const auto status = storage.Delete(key);
        metrics.ObserveOperation("DELETE", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()), status.ok());
        *description = "DELETE key=" + Quote(key);
        return status.ok() ? "OK\n" : "ERROR " + Quote(status.ToString()) + "\n";
    }
    if (operation == "INFO") {
        *description = "INFO";
        return "INFO entries=" + std::to_string(storage.EntryCount()) +
               " bytes=" + std::to_string(storage.ApproximateSize()) + "\n";
    }
    *description = "unknown command";
    return "ERROR \"Supported commands: PUT, GET, DELETE, INFO\"\n";
}

} // namespace

int main(int argc, char* argv[]) {
    int port = 50051;
    int metrics_port = 9090;
    std::string data_dir = "./data/demo-server";
    if (!ParseOptions(argc, argv, &port, &metrics_port, &data_dir)) return 1;

    lsm::net::Runtime runtime;
    if (!runtime.ready()) {
        std::cerr << "Could not initialize the socket runtime\n";
        return 1;
    }

    lsm::StorageConfig config;
    config.data_dir = data_dir;
    lsm::BasicStorageEngine storage(config);
    const auto open_status = storage.Open();
    if (!open_status.ok()) {
        std::cerr << open_status.ToString() << "\n";
        return 1;
    }

    lsm::StorageMetrics metrics;
    const auto metrics_listener = OpenMetricsListener(metrics_port);
    if (metrics_listener == lsm::net::kInvalidSocket) {
        std::cerr << "Could not listen for metrics on 127.0.0.1:" << metrics_port << "\n";
        return 1;
    }

    const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == lsm::net::kInvalidSocket) {
        std::cerr << "Could not create listening socket\n";
        lsm::net::Close(metrics_listener);
        return 1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 8) != 0) {
        std::cerr << "Could not listen on 127.0.0.1:" << port << " (port may be in use)\n";
        lsm::net::Close(listener);
        lsm::net::Close(metrics_listener);
        return 1;
    }

    std::atomic<bool> stop_metrics{false};
    std::thread metrics_thread(ServeMetrics, metrics_listener, std::cref(metrics), std::cref(stop_metrics));

    std::cout << "========================================================\n"
              << " Distributed LSM-Tree Database Engine — Demo Server\n"
              << "========================================================\n"
              << "Listening: 127.0.0.1:" << port << "\n"
              << "Metrics:   http://127.0.0.1:" << metrics_port << "/metrics\n"
              << "Storage:   " << data_dir << " (in-memory demo engine)\n"
              << "Waiting for client requests. Press Ctrl+C to stop.\n"
              << "--------------------------------------------------------\n" << std::flush;

    std::size_t request_number = 0;
    while (true) {
        sockaddr_in client_address{};
#ifdef _WIN32
        int address_length = sizeof(client_address);
#else
        socklen_t address_length = sizeof(client_address);
#endif
        const auto client = accept(listener, reinterpret_cast<sockaddr*>(&client_address), &address_length);
        if (client == lsm::net::kInvalidSocket) break;

        char host[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &client_address.sin_addr, host, sizeof(host));
        std::cout << "Client connected: " << host << ':' << ntohs(client_address.sin_port) << "\n" << std::flush;

        std::string request;
        while (lsm::net::ReceiveLine(client, &request)) {
            std::string description;
            const std::string response = HandleRequest(storage, metrics, request, &description);
            ++request_number;
            const bool ok = response.rfind("ERROR ", 0) != 0;
            std::cout << "[" << request_number << "] " << description
                      << " => " << (ok ? "OK" : "ERROR")
                      << " | entries=" << storage.EntryCount()
                      << " | bytes=" << storage.ApproximateSize() << "\n" << std::flush;
            if (!lsm::net::SendAll(client, response)) break;
        }
        lsm::net::Close(client);
        std::cout << "Client disconnected\n" << std::flush;
    }

    lsm::net::Close(listener);
    stop_metrics.store(true);
    metrics_thread.join();
    lsm::net::Close(metrics_listener);
    storage.Close();
    return 0;
}
