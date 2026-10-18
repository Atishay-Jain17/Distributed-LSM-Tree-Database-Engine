#include "test_framework.h"
#include "common/config.h"
#include <fstream>
#include <filesystem>

using namespace lsm;

TEST_CASE(TestConfigDefaults) {
    NodeConfig config = NodeConfig::Default();
    ASSERT_EQ(config.node_id, "node-1");
    ASSERT_EQ(config.storage.data_dir, "./data");
    ASSERT_EQ(config.storage.max_memtable_size, 64ULL * 1024 * 1024);
    ASSERT_TRUE(config.storage.sync_wal);
    ASSERT_EQ(config.network.client_port, 50051);
    ASSERT_EQ(config.network.peer_port, 50052);
    ASSERT_EQ(config.network.metrics_port, 9090);

    ASSERT_STATUS_OK(config.Validate());
}

TEST_CASE(TestConfigValidation) {
    NodeConfig config = NodeConfig::Default();

    // Empty node_id
    config.node_id = "";
    ASSERT_STATUS_CODE(config.Validate(), StatusCode::InvalidArgument);
    config.node_id = "node-1";

    // Empty data_dir
    config.storage.data_dir = "";
    ASSERT_STATUS_CODE(config.Validate(), StatusCode::InvalidArgument);
    config.storage.data_dir = "./data";

    // Invalid client port
    config.network.client_port = 0;
    ASSERT_STATUS_CODE(config.Validate(), StatusCode::InvalidArgument);
    config.network.client_port = 70000;
    ASSERT_STATUS_CODE(config.Validate(), StatusCode::InvalidArgument);
    config.network.client_port = 50051;

    // Port collision
    config.network.peer_port = 50051; // same as client_port
    ASSERT_STATUS_CODE(config.Validate(), StatusCode::InvalidArgument);
    config.network.peer_port = 50052;
    ASSERT_STATUS_OK(config.Validate());
}

TEST_CASE(TestConfigFileParsing) {
    std::string temp_config_file = "./test_node.conf";
    {
        std::ofstream out(temp_config_file);
        out << "# Test node configuration\n";
        out << "node.id = node-cluster-3\n";
        out << "storage.data_dir = ./cluster_data/node3\n";
        out << "storage.max_memtable_size = 33554432\n";
        out << "storage.sync_wal = false\n";
        out << "network.client_port = 60001\n";
        out << "network.peer_port = 60002\n";
        out << "network.metrics_port = 9100\n";
    }

    NodeConfig config;
    Status s = NodeConfig::LoadFromFile(temp_config_file, config);
    std::filesystem::remove(temp_config_file);

    ASSERT_STATUS_OK(s);
    ASSERT_EQ(config.node_id, "node-cluster-3");
    ASSERT_EQ(config.storage.data_dir, "./cluster_data/node3");
    ASSERT_EQ(config.storage.max_memtable_size, 33554432ULL);
    ASSERT_FALSE(config.storage.sync_wal);
    ASSERT_EQ(config.network.client_port, 60001);
    ASSERT_EQ(config.network.peer_port, 60002);
    ASSERT_EQ(config.network.metrics_port, 9100);
}

TEST_CASE(TestCommandLineParsing) {
    char prog[] = "lsm_node";
    char flag1[] = "--node_id";
    char val1[] = "node-alpha";
    char flag2[] = "--client_port";
    char val2[] = "51000";
    char flag3[] = "--peer_port";
    char val3[] = "52000";
    char flag4[] = "--metrics_port";
    char val4[] = "9900";
    char flag5[] = "--data_dir";
    char val5[] = "/tmp/lsm_data";
    char flag6[] = "--sync_wal";
    char val6[] = "false";

    char* argv[] = { prog, flag1, val1, flag2, val2, flag3, val3, flag4, val4, flag5, val5, flag6, val6 };
    int argc = sizeof(argv) / sizeof(argv[0]);

    NodeConfig config = NodeConfig::Default();
    ASSERT_STATUS_OK(NodeConfig::ParseCommandLine(argc, argv, config));

    ASSERT_EQ(config.node_id, "node-alpha");
    ASSERT_EQ(config.network.client_port, 51000);
    ASSERT_EQ(config.network.peer_port, 52000);
    ASSERT_EQ(config.network.metrics_port, 9900);
    ASSERT_EQ(config.storage.data_dir, "/tmp/lsm_data");
    ASSERT_FALSE(config.storage.sync_wal);
}
