
#define _CRT_SECURE_NO_WARNINGS 1
#define QUIC_API_ENABLE_PREVIEW_FEATURES 1

#include <stdio.h>
#include <iostream>
#include <memory>
#include "MsQuicServer.h"
#include <thread>
#include <chrono>
#include <atomic>

// 定义全局变量
const MsQuicApi* MsQuic = nullptr;
std::atomic<bool> running(true);

#include <iostream>
#include <string>
#include <map>
#include <memory>
#include <vector>
#include <Windows.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include "Utils.h"
#include "SharedMemoryIPC.h"
#include "MsQuicServer.h"

// 全局变量
// 在全局变量部分添加端口管理
std::map<int, std::unique_ptr<SharedMemoryIPC>> sharedMemories;
std::map<int, PROCESS_INFORMATION> childProcesses;
std::map<int, uint16_t> processPortMap; // 保存进程ID和端口的映射
std::map<uint16_t, bool> portInUseMap;  // 跟踪端口使用状态
bool isRunning = true;
int processCount = 3; // 默认启动3个子进程
uint16_t basePort = 4433; // 基础端口号
uint16_t maxPort = 4500;  // 最大端口号

// 函数声明
bool createChildProcess(int id);
void processCommand(const std::string& command);
int runChildProcess(int id);
void loadConfig();

int main(int argc, char* argv[]) {
    // 检查是主进程还是子进程
    if (argc > 1 && std::string(argv[1]) == "--child") {
        if (argc < 3) {
            std::cerr << "Missing child process ID parameter" << std::endl;
            return 1;
        }
        
        int id = std::stoi(argv[2]);
        return runChildProcess(id);
    }
    
    std::cout << R"(
  __  __   ____   _____  ___   ___  ___    ___ 
 |  \/  | /____| / / \ \ | |   | | |_ _| / ___|
 | |\/| | \__ \  | | | | | |   | |  | |  | |   
 | |  | | ___) | | |_| | | |   | |  | |  | |__ 
 |_|  |_||____/   \___\_\ \_\__/_/ |___| \____| 
                                                
    )" << std::endl;
    // 加载配置
    loadConfig();
    
    // 创建子进程
    for (int i = 0; i < processCount; i++) {
        if (!createChildProcess(i)) {
            LogEvent("Error", "Failed to create child process " + std::to_string(i));
        }
    }
    
    // 主循环，处理用户输入
    std::string command;
    std::cout << "Enter command (help for assistance): ";
    while (isRunning && std::getline(std::cin, command)) {
        if (command == "exit" || command == "quit") {
            isRunning = false;
        }
        // 在 help 命令中添加新命令说明
        else if (command == "help") {
            std::cout << "Available commands:\n"
                      << "  help - Show help information\n"
                      << "  list - List all child processes\n"
                      << "  send <id> <message> - Send message to specified child process\n"
                      << "  start <id> - Start a new child process\n"
                      << "  stop <id> - Stop specified child process\n"
                      << "  destroy <id> - Destroy child process and release port\n"
                      << "  ports - Display port usage status\n"
                      << "  clearport <port> - Manually clear a port's in-use status\n"
                      << "  stopall - Stop all child processes\n"
                      << "  exit/quit - Exit program\n";
        }
        
        // 在 processCommand 函数中添加新命令处理
        else if (command.find("clearport ") == 0) {
            try {
                uint16_t port = static_cast<uint16_t>(std::stoi(command.substr(10)));
                if (port >= basePort && port <= maxPort) {
                    // 检查端口是否被任何活跃进程使用
                    bool inUseByActiveProcess = false;
                    for (const auto& pair : childProcesses) {
                        DWORD exitCode = 0;
                        GetExitCodeProcess(pair.second.hProcess, &exitCode);
                        if (exitCode == STILL_ACTIVE && processPortMap[pair.first] == port) {
                            inUseByActiveProcess = true;
                            LogEvent("Error", "Port " + std::to_string(port) + " is in use by active process " + std::to_string(pair.first));
                            break;
                        }
                    }
                    
                    if (!inUseByActiveProcess) {
                        portInUseMap[port] = false;
                        LogEvent("Info", "Port " + std::to_string(port) + " has been manually cleared");
                    }
                } else {
                    LogEvent("Error", "Port " + std::to_string(port) + " is out of valid range (" + 
                             std::to_string(basePort) + "-" + std::to_string(maxPort) + ")");
                }
            } catch (const std::exception& e) {
                LogEvent("Error", "Failed to parse port number: " + std::string(e.what()));
            }
        }
        else {
            processCommand(command);
        }
        
        if (isRunning) {
            std::cout << "Enter command: ";
        }
    }
    
    // 停止所有子进程
    for (auto& pair : childProcesses) {
        int id = pair.first;
        LogEvent("Info", "Stopping child process " + std::to_string(id));
        
        if (sharedMemories.find(id) != sharedMemories.end()) {
            sharedMemories[id]->write("STOP");
            
            // 等待子进程退出
            WaitForSingleObject(pair.second.hProcess, 5000);
            
            // 关闭句柄
            CloseHandle(pair.second.hProcess);
            CloseHandle(pair.second.hThread);
        }
    }
    
    LogEvent("Info", "MsQuic Multi-process Manager has exited");
    return 0;
}

bool createChildProcess(int id) {
    // 减少日志输出，只在关键点记录
    
    // 创建共享内存
    std::string memoryName = "MsQuicSharedMem_" + std::to_string(id);
    sharedMemories[id] = std::make_unique<SharedMemoryIPC>(memoryName, true);
    
    if (!sharedMemories[id]->initialize()) {
        LogEvent("Error", "Failed to initialize shared memory");
        return false;
    }
    
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    
    // 获取当前可执行文件路径
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0) {
        LogEvent("Error", "Failed to get executable path, error code: " + std::to_string(GetLastError()));
        return false;
    }
    
    // 确定端口号 - 查找可用端口
    uint16_t port;
    
    // 首先尝试使用之前分配的端口
    if (processPortMap.find(id) != processPortMap.end()) {
        port = processPortMap[id];
        // 检查该端口是否被标记为使用中
        if (portInUseMap[port]) {
            LogEvent("Info", "Previously assigned port " + std::to_string(port) + " is in use, finding new port");
            port = 0; // 重置端口，寻找新端口
        }
    } else {
        port = 0; // 需要分配新端口
    }
    
    // 如果需要分配新端口
    if (port == 0) {
        // 查找可用端口
        for (uint16_t p = basePort; p <= maxPort; p++) {
            if (!portInUseMap[p]) {
                port = p;
                break;
            }
        }
        
        if (port == 0) {
            LogEvent("Error", "No available ports found");
            return false;
        }
    }
    
    // 标记端口为使用中
    portInUseMap[port] = true;
    processPortMap[id] = port;
    
    LogEvent("Info", "Assigning port " + std::to_string(port) + " to child process " + std::to_string(id));
    
    // 构建命令行 - 使用引号包围可执行文件路径，并传递端口号
    std::string cmdLine = "\"" + std::string(exePath) + "\" --child " + std::to_string(id) + " --port " + std::to_string(port);
    
    // 创建进程
    if (!CreateProcessA(
        NULL,                   // 应用程序名称
        const_cast<char*>(cmdLine.c_str()), // 命令行
        NULL,                   // 进程安全属性
        NULL,                   // 线程安全属性
        FALSE,                  // 不继承句柄
        0,                      // 创建标志
        NULL,                   // 环境块
        NULL,                   // 当前目录
        &si,                    // STARTUPINFO
        &pi                     // PROCESS_INFORMATION
    )) {
        LogEvent("Error", "Failed to create child process, error code: " + std::to_string(GetLastError()));
        return false;
    }
    
    // 保存进程信息
    childProcesses[id] = pi;
    
    // 启动监听，减少不必要的日志输出
    sharedMemories[id]->startListening([id](const std::string& message) {
        // 只记录重要消息，如错误或状态变更
        if (message.find("ERROR") == 0 || message.find("STARTED") == 0 || message.find("STOPPED") == 0) {
            LogEvent("Info", "Child process " + std::to_string(id) + ": " + message);
        }
        });
    
    return true;
}

void processCommand(const std::string& command) {
    if (command == "list") {
        std::cout << "Child process list:\n";
        for (const auto& pair : childProcesses) {
            DWORD exitCode = 0;
            GetExitCodeProcess(pair.second.hProcess, &exitCode);
            std::string status = (exitCode == STILL_ACTIVE) ? "Running" : "Exited";
            std::cout << "  ID: " << pair.first << " - " << status << "\n";
        }
    }
    else if (command.find("send ") == 0) {
        size_t spacePos = command.find(' ', 5);
        if (spacePos != std::string::npos) {
            std::string idStr = command.substr(5, spacePos - 5);
            std::string message = command.substr(spacePos + 1);
            
            try {
                int id = std::stoi(idStr);
                auto it = sharedMemories.find(id);
                if (it != sharedMemories.end()) {
                    LogEvent("Info", "Sending message to child process " + idStr + ": " + message);
                    it->second->write(message);
                }
                else {
                    LogEvent("Error", "Child process with ID " + idStr + " not found");
                }
            }
            catch (const std::exception& e) {
                LogEvent("Error", "Failed to parse process ID: " + std::string(e.what()));
            }
        }
        else {
            LogEvent("Error", "Invalid command format, should be: send <id> <message>");
        }
    }
    else if (command.find("start ") == 0) {
        try {
            int id = std::stoi(command.substr(6));
            if (childProcesses.find(id) == childProcesses.end()) {
                createChildProcess(id);
            }
            else {
                LogEvent("Error", "Child process with ID " + std::to_string(id) + " already exists");
            }
        }
        catch (const std::exception& e) {
            LogEvent("Error", "Failed to parse process ID: " + std::string(e.what()));
        }
    }
    else if (command.find("stop ") == 0) {
        try {
            int id = std::stoi(command.substr(5));
            auto it = sharedMemories.find(id);
            if (it != sharedMemories.end()) {
                LogEvent("Info", "Stopping child process " + std::to_string(id));
                it->second->write("STOP");
            }
            else {
                LogEvent("Error", "Child process with ID " + std::to_string(id) + " not found");
            }
        }
        catch (const std::exception& e) {
            LogEvent("Error", "Failed to parse process ID: " + std::string(e.what()));
        }
    }
    else if (command == "stopall") {
        for (auto& pair : sharedMemories) {
            LogEvent("Info", "Stopping child process " + std::to_string(pair.first));
            pair.second->write("STOP");
        }
    }
    else if (command.find("destroy ") == 0) {
        try {
            int id = std::stoi(command.substr(8));
            auto it = sharedMemories.find(id);
            if (it != sharedMemories.end()) {
                LogEvent("Info", "Destroying child process " + std::to_string(id));
                
                // 获取端口号
                uint16_t port = 0;
                if (processPortMap.find(id) != processPortMap.end()) {
                    port = processPortMap[id];
                }
                
                // 发送停止命令
                it->second->write("STOP");
                
                // 等待子进程退出，最多等待5秒
                bool processTerminated = false;
                if (childProcesses.find(id) != childProcesses.end()) {
                    // 等待进程退出，最多等待5秒
                    if (WaitForSingleObject(childProcesses[id].hProcess, 5000) == WAIT_TIMEOUT) {
                        // 如果超时，强制终止进程
                        LogEvent("Warning", "Child process " + std::to_string(id) + " did not exit gracefully, terminating forcefully");
                        TerminateProcess(childProcesses[id].hProcess, 1);
                        WaitForSingleObject(childProcesses[id].hProcess, 1000); // 再等待1秒确保终止
                    }
                    
                    // 关闭句柄
                    CloseHandle(childProcesses[id].hProcess);
                    CloseHandle(childProcesses[id].hThread);
                    
                    // 从映射中移除
                    childProcesses.erase(id);
                    processTerminated = true;
                }
                
                // 移除共享内存
                sharedMemories.erase(id);
                
                // 从进程-端口映射中移除
                if (port != 0) {
                    processPortMap.erase(id);
                    
                    // 创建一个线程来延迟释放端口
                    std::thread([port]() {
                        // 等待10秒，让操作系统完全释放端口
                        std::this_thread::sleep_for(std::chrono::seconds(10));
                        
                        // 确保端口被正确释放
                        portInUseMap[port] = false;
                        
                        // 记录端口释放事件
                        LogEvent("Info", "Port " + std::to_string(port) + " is now available for reuse");
                    }).detach();
                    
                    LogEvent("Info", "Child process " + std::to_string(id) + " " + 
                             (processTerminated ? "terminated" : "not found but resources cleaned up") + 
                             ", port " + std::to_string(port) + " will be released after cooldown");
                }
            }
            else {
                LogEvent("Error", "Child process with ID " + std::to_string(id) + " not found");
            }
        }
        catch (const std::exception& e) {
            LogEvent("Error", "Failed to parse process ID: " + std::string(e.what()));
        }
    }
    else if (command == "ports") {
        std::cout << "Port status:\n";
        for (const auto& pair : portInUseMap) {
            std::cout << "  Port: " << pair.first << " - " << (pair.second ? "In use" : "Available") << "\n";
        }
        
        std::cout << "Process-Port mapping:\n";
        for (const auto& pair : processPortMap) {
            std::cout << "  Process ID: " << pair.first << " - Port: " << pair.second << "\n";
        }
    }
    else {
        LogEvent("Error", "Unknown command: " + command);
    }
}

// 删除这里错误放置的代码块
// else if (command == "ports") { ... }

int runChildProcess(int id) {
    // 减少初始化日志
    
    // 解析命令行参数获取端口号
    uint16_t port = basePort + id; // 默认端口
    int argc = __argc;
    char** argv = __argv;
    
    for (int i = 1; i < argc - 1; i++) {
        if (std::string(argv[i]) == "--port") {
            port = static_cast<uint16_t>(std::stoi(argv[i + 1]));
            break;
        }
    }
    
    // 连接到共享内存
    std::string memoryName = "MsQuicSharedMem_" + std::to_string(id);
    SharedMemoryIPC sharedMemory(memoryName, false);
    
    if (!sharedMemory.initialize()) {
        LogEvent("Error", "Failed to connect to shared memory");
        return 1;
    }
    
    // 向主进程发送就绪消息
    sharedMemory.write("READY");
    
    // 创建MsQuic服务器
    std::shared_ptr<MsQuicServer> server = nullptr;
    try {
        // 初始化MsQuic，减少日志输出
        auto msQuic = std::make_shared<MsQuicApi>();
        MsQuic = msQuic.get();
        server = std::make_shared<MsQuicServer>(msQuic.get());
        
        // 使用指定的端口
        if (!server->Initialize(port)) {
            LogEvent("Error", "Failed to initialize MsQuic server on port " + std::to_string(port));
            sharedMemory.write("ERROR:Failed to initialize MsQuic server on port " + std::to_string(port));
            return 1;
        }
        
        // 只保留关键状态变更的日志
        sharedMemory.write("STARTED:port=" + std::to_string(port));
        
        // 启动服务器线程
        server->StartServerThread();
    }
    catch (const std::exception& e) {
        LogEvent("Error", "Exception starting MsQuic server: " + std::string(e.what()));
        sharedMemory.write("ERROR:Exception starting MsQuic server: " + std::string(e.what()));
        return 1;
    }
    
    // 启动监听来自主进程的命令，减少日志输出
    bool running = true;
    sharedMemory.startListening([&](const std::string& message) {
        // 只记录关键命令
        if (message == "STOP") {
            LogEvent("Info", "Received stop command");
            sharedMemory.write("STOPPING");
            
            if (server) {
                server->StopServerThread();
                server->Shutdown();
                server = nullptr;
            }
            
            sharedMemory.write("STOPPED");
            running = false;
        }
        else if (message == "STATUS") {
            sharedMemory.write("STATUS:RUNNING");
        }
    });
    
    // 主循环
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    return 0;
}


void loadConfig() {
    // 尝试加载配置文件
    std::ifstream configFile("msquic_config.txt");
    if (configFile.is_open()) {
        std::string line;
        while (std::getline(configFile, line)) {
            std::istringstream iss(line);
            std::string key, value;
            if (std::getline(iss, key, '=') && std::getline(iss, value)) {
                if (key == "process_count") {
                    try {
                        processCount = std::stoi(value);
                        LogEvent("Info", "Loaded process count from config file: " + std::to_string(processCount));
                    }
                    catch (...) {
                        LogEvent("Error", "Failed to parse process count, using default: 3");
                        processCount = 3;
                    }
                }
                // 可以添加更多配置项
            }
        }
        configFile.close();
    }
    else {
        LogEvent("Info", "Config file not found, using default settings");
    }
    
    // 初始化端口状态
    for (uint16_t p = basePort; p <= maxPort; p++) {
        portInUseMap[p] = false;
    }
}