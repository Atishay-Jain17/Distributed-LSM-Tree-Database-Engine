#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

fs::path DetermineDataDir() {
    const char* env_temp = std::getenv("LSMDB_TEST_TEMP");
    if (env_temp && *env_temp) {
        return fs::path(env_temp) / "lsmdb-cli-runner-test";
    }
    const char* temp_env = std::getenv("TEMP");
    if (temp_env && *temp_env) {
        return fs::path(temp_env) / "lsmdb-cli-runner-test";
    }
    return fs::current_path() / "build" / "test-data" / "lsmdb-cli-runner-test";
}

struct ProcessResult {
    int exit_code = -1;
    std::string stdout_output;
    std::string stderr_output;
};

#if defined(_WIN32)

ProcessResult RunCommand(const std::string& cmd_line, const std::string& stdin_input) {
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = nullptr;

    HANDLE hChildStd_IN_Rd = nullptr;
    HANDLE hChildStd_IN_Wr = nullptr;
    HANDLE hChildStd_OUT_Rd = nullptr;
    HANDLE hChildStd_OUT_Wr = nullptr;

    if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0)) {
        throw std::runtime_error("Stdout pipe creation failed");
    }
    SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&hChildStd_IN_Rd, &hChildStd_IN_Wr, &saAttr, 0)) {
        CloseHandle(hChildStd_OUT_Rd);
        CloseHandle(hChildStd_OUT_Wr);
        throw std::runtime_error("Stdin pipe creation failed");
    }
    SetHandleInformation(hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA siStartInfo;
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFOA));
    siStartInfo.cb = sizeof(STARTUPINFOA);
    siStartInfo.hStdError = hChildStd_OUT_Wr;
    siStartInfo.hStdOutput = hChildStd_OUT_Wr;
    siStartInfo.hStdInput = hChildStd_IN_Rd;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION piProcInfo;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

    std::vector<char> cmd_buf(cmd_line.begin(), cmd_line.end());
    cmd_buf.push_back('\0');

    BOOL bSuccess = CreateProcessA(
        nullptr,
        cmd_buf.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        nullptr,
        &siStartInfo,
        &piProcInfo
    );

    if (!bSuccess) {
        CloseHandle(hChildStd_OUT_Rd);
        CloseHandle(hChildStd_OUT_Wr);
        CloseHandle(hChildStd_IN_Rd);
        CloseHandle(hChildStd_IN_Wr);
        throw std::runtime_error("CreateProcess failed for command: " + cmd_line);
    }

    CloseHandle(hChildStd_OUT_Wr);
    CloseHandle(hChildStd_IN_Rd);

    // Write input
    DWORD bytesWritten = 0;
    if (!stdin_input.empty()) {
        WriteFile(hChildStd_IN_Wr, stdin_input.data(), static_cast<DWORD>(stdin_input.size()), &bytesWritten, nullptr);
    }
    CloseHandle(hChildStd_IN_Wr);

    // Read stdout
    std::string output;
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(hChildStd_OUT_Rd, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        output.append(buffer, bytesRead);
    }
    CloseHandle(hChildStd_OUT_Rd);

    WaitForSingleObject(piProcInfo.hProcess, 10000);
    DWORD exitCode = 0;
    GetExitCodeProcess(piProcInfo.hProcess, &exitCode);

    CloseHandle(piProcInfo.hProcess);
    CloseHandle(piProcInfo.hThread);

    ProcessResult res;
    res.exit_code = static_cast<int>(exitCode);
    res.stdout_output = output;
    return res;
}

#else

ProcessResult RunCommand(const std::string& cmd_line, const std::string& stdin_input) {
    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
        throw std::runtime_error("Pipe creation failed");
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(stdin_pipe[1]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        close(stdin_pipe[0]);

        close(stdout_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);

        execl("/bin/sh", "sh", "-c", cmd_line.c_str(), (char*)nullptr);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    if (!stdin_input.empty()) {
        write(stdin_pipe[1], stdin_input.data(), stdin_input.size());
    }
    close(stdin_pipe[1]);

    std::string output;
    char buf[4096];
    ssize_t n = 0;
    while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0) {
        output.append(buf, n);
    }
    close(stdout_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    ProcessResult res;
    res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    res.stdout_output = output;
    return res;
}

#endif

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path-to-lsmdb_local_cli>\n";
        return 1;
    }

    const std::string cli_path = argv[1];
    const fs::path data_dir = DetermineDataDir();
    std::error_code ec;
    fs::remove_all(data_dir, ec);
    fs::create_directories(data_dir.parent_path(), ec);

    try {
        std::cout << "[Step 1] Running lsmdb_local_cli Session 1: PUT, GET, DEL, INFO, EXIT..." << std::endl;
        std::string session1_cmd = cli_path + " --data-dir \"" + data_dir.string() + "\"";
        std::string session1_in =
            "PUT alpha \"first value\"\n"
            "PUT beta \"second value\"\n"
            "GET alpha\n"
            "DEL beta\n"
            "GET beta\n"
            "INFO\n"
            "EXIT\n";

        ProcessResult r1 = RunCommand(session1_cmd, session1_in);
        if (r1.exit_code != 0) {
            std::cerr << "Session 1 exited with code " << r1.exit_code << "\nOutput:\n" << r1.stdout_output << std::endl;
            throw std::runtime_error("Session 1 failed non-zero exit code");
        }

        std::cout << "  Output 1:\n" << r1.stdout_output << std::endl;
        if (r1.stdout_output.find("VALUE \"first value\"") == std::string::npos)
            throw std::runtime_error("GET alpha failed in Session 1");
        if (r1.stdout_output.find("NOT_FOUND") == std::string::npos)
            throw std::runtime_error("GET beta did not report NOT_FOUND after DEL in Session 1");
        if (r1.stdout_output.find("INFO sequence=") == std::string::npos)
            throw std::runtime_error("INFO telemetry header missing in Session 1");
        if (r1.stdout_output.find("Database closed cleanly.") == std::string::npos)
            throw std::runtime_error("Clean exit confirmation missing in Session 1");

        std::cout << "[Step 2] Running lsmdb_local_cli Session 2: Reopen, check persisted alpha & beta..." << std::endl;
        std::string session2_cmd = cli_path + " --data-dir \"" + data_dir.string() + "\"";
        std::string session2_in =
            "GET alpha\n"
            "GET beta\n"
            "PUT gamma \"third value\"\n"
            "FLUSH\n"
            "INFO\n"
            "EXIT\n";

        ProcessResult r2 = RunCommand(session2_cmd, session2_in);
        if (r2.exit_code != 0) {
            std::cerr << "Session 2 exited with code " << r2.exit_code << "\nOutput:\n" << r2.stdout_output << std::endl;
            throw std::runtime_error("Session 2 failed non-zero exit code");
        }

        std::cout << "  Output 2:\n" << r2.stdout_output << std::endl;
        if (r2.stdout_output.find("VALUE \"first value\"") == std::string::npos)
            throw std::runtime_error("GET alpha failed across process restart in Session 2");
        if (r2.stdout_output.find("NOT_FOUND") == std::string::npos)
            throw std::runtime_error("Deleted key beta was resurrected across restart in Session 2");
        if (r2.stdout_output.find("OK sequence=") == std::string::npos)
            throw std::runtime_error("PUT gamma failed in Session 2");
        if (r2.stdout_output.find("OK flushed_up_to=") == std::string::npos)
            throw std::runtime_error("FLUSH command failed in Session 2");

        std::cout << "[Step 3] Running lsmdb_local_cli Session 3: Reopen and verify gamma after explicit flush..." << std::endl;
        std::string session3_cmd = cli_path + " --data-dir \"" + data_dir.string() + "\"";
        std::string session3_in =
            "GET gamma\n"
            "INFO\n"
            "EXIT\n";

        ProcessResult r3 = RunCommand(session3_cmd, session3_in);
        if (r3.exit_code != 0) {
            std::cerr << "Session 3 exited with code " << r3.exit_code << "\nOutput:\n" << r3.stdout_output << std::endl;
            throw std::runtime_error("Session 3 failed non-zero exit code");
        }

        std::cout << "  Output 3:\n" << r3.stdout_output << std::endl;
        if (r3.stdout_output.find("VALUE \"third value\"") == std::string::npos)
            throw std::runtime_error("GET gamma failed across restart in Session 3");

        fs::remove_all(data_dir, ec);
        std::cout << "PASS: True interactive CLI binary execution, command parsing, and process restart verified." << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: " << ex.what() << std::endl;
        fs::remove_all(data_dir, ec);
        return 1;
    }
}
