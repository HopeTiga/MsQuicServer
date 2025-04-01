
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

int main() {
    // 创建 MsQuicApi 实例并赋值给全局变量
    MsQuic = new MsQuicApi();
    
    // 创建服务器
    MsQuicServer server(MsQuic);
    
    // 在单独的线程中启动服务器
    std::thread serverThread([&server]() {
        try {
            if (!server.Initialize(4333, "quic")) {
                std::cerr << "Server initialization failed" << std::endl;
                running = false;
                return;
            }
         
            
            // 保持线程运行，直到主线程设置 running 为 false
            while (running) {
                // 添加短暂休眠，减少CPU占用
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        catch (const std::exception& e) {
            std::cerr << "[Error] Server thread exception: " << e.what() << std::endl;
        }
        catch (...) {
            std::cerr << "[Error] Unknown exception in server thread" << std::endl;
        }
        
        std::cout << "[Info] Server thread is exiting..." << std::endl;
    });
    
    // 主线程等待用户输入退出
    std::cout << "[Info] Press Enter to exit..." << std::endl;
    std::cin.get();
    
    // 通知服务器线程退出
    running = false;
    
    // 等待服务器线程结束
    if (serverThread.joinable()) {
        serverThread.join();
    }

    // 关闭服务器
    server.Shutdown();
    std::cout << "[Info] MsQuicServer is exit..." << std::endl;
    
    // 释放全局变量
    delete MsQuic;
    MsQuic = nullptr;

    return 0;
}