#pragma once

#include <string>
#include <Windows.h>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include "Utils.h"

/**
 * @class SharedMemoryIPC
 * @brief 使用Windows共享内存实现进程间通信
 */
class SharedMemoryIPC {
public:
    /**
     * @brief 构造函数
     * @param name 共享内存名称
     * @param isServer 是否为服务器端
     * @param size 共享内存大小
     */
    SharedMemoryIPC(const std::string& name, bool isServer, size_t size = 4096);
    
    /**
     * @brief 析构函数
     */
    ~SharedMemoryIPC();
    
    /**
     * @brief 初始化共享内存
     * @return 初始化是否成功
     */
    bool initialize();
    
    /**
     * @brief 写入消息到共享内存
     * @param message 要写入的消息
     * @return 写入是否成功
     */
    bool write(const std::string& message);
    
    /**
     * @brief 开始监听消息
     * @param callback 接收到消息时的回调函数
     * @return 开始监听是否成功
     */
    bool startListening(std::function<void(const std::string&)> callback);
    
    /**
     * @brief 停止监听
     */
    void stopListening();

private:
    std::string name_;
    bool isServer_;
    size_t size_;
    HANDLE fileMapping_;
    void* mappedView_;
    HANDLE readEvent_;
    HANDLE writeEvent_;
    std::thread listenThread_;
    std::atomic<bool> running_;
    std::mutex mutex_;
    std::function<void(const std::string&)> messageCallback_;
    
    /**
     * @brief 监听线程函数
     */
    void listenThreadFunc();
};