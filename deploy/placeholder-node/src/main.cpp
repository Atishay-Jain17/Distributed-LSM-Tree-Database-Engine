// Minimal placeholder node binary for Week 1 Kubernetes/Helm smoke testing.
//
// This is NOT the real database service (that's Vinayak's gRPC contract,
// coming later this week). Its only job is to:
//   1. Prove a C++ binary can be containerized and run as a pod
//   2. Read its identity from env vars injected by Kubernetes
//   3. Listen on a TCP port so `kubectl exec ... nc` / curl can prove
//      pod-to-pod reachability across the headless service
//
// Once Vinayak's gRPC service exists, this file gets replaced/wired in —
// the Dockerfile and Helm chart around it do not need to change.

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

int main() {
    const char* node_id  = std::getenv("NODE_ID");
    const char* pod_name = std::getenv("POD_NAME");
    const char* port_env = std::getenv("NODE_PORT");

    std::string node_id_str  = node_id  ? node_id  : "unknown-node";
    std::string pod_name_str = pod_name ? pod_name : "unknown-pod";
    int port = port_env ? std::atoi(port_env) : 50051;

    std::cout << "[placeholder-node] starting. NODE_ID=" << node_id_str
              << " POD_NAME=" << pod_name_str
              << " PORT=" << port << std::endl;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "socket() failed" << std::endl;
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind() failed on port " << port << std::endl;
        return 1;
    }

    if (listen(server_fd, 8) < 0) {
        std::cerr << "listen() failed" << std::endl;
        return 1;
    }

    std::cout << "[placeholder-node] listening on 0.0.0.0:" << port << std::endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        std::string response = "Hello from " + node_id_str +
                                " (pod: " + pod_name_str + ")\n";
        write(client_fd, response.c_str(), response.size());
        close(client_fd);
    }

    return 0;
}
