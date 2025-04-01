#pragma once

#include <msquic.hpp>
#include <string>
#include <iostream>
#include <vector>
#include <map>
#include<memory>
#include <mutex>
#include <thread>
#include <atomic>
#include "MsQuicSession.h"

// 在类定义中添加以下函数声明
/**
 * @class MsQuicServer
 * @brief QUIC服务器类，负责监听和处理QUIC连接
 * 
 * 该类继承自std::enable_shared_from_this，便于在回调中安全地获取自身的shared_ptr
 */
class MsQuicServer :public std::enable_shared_from_this<MsQuicServer>{
public:
    /**
     * @brief 构造函数
     * @param msQuic MsQuicApi指针，提供QUIC API访问
     */
    MsQuicServer(const MsQuicApi* msQuic);
    
    /**
     * @brief 析构函数，负责清理资源
     */
    ~MsQuicServer();
    
    /**
     * @brief 初始化服务器
     * @param port 监听端口，默认为4433
     * @param alpn 应用层协议协商字符串，默认为"quic"
     * @return 初始化是否成功
     */
    bool Initialize(uint16_t port = 4433, const std::string& alpn = "quic");
    
    /**
     * @brief 关闭服务器
     */
    void Shutdown();
    
    /**
     * @brief 启动服务器线程
     */
    void StartServerThread();
    
    /**
     * @brief 停止服务器线程
     */
    void StopServerThread();
    
    /**
     * @brief 检查服务器是否正在运行
     * @return 服务器是否正在运行
     */
    bool IsRunning() const { return running_; }

    /**
     * @brief 设置新连接建立时的回调函数
     * @param func 回调函数
     */
    void setConnectCallBack(std::function<void(std::shared_ptr<MsQuicSession>)> func);
    
    /**
     * @brief 设置连接断开时的回调函数
     * @param func 回调函数
     */
    void setConnectOutCallBack(std::function<void(std::shared_ptr<MsQuicSession>)> func);

private:
    // MsQuic API 指针
    const MsQuicApi* msQuic_;
    
    // MsQuic 注册
    MsQuicRegistration* registration_;
    
    // MsQuic 配置
    MsQuicConfiguration* configuration_;
    
    // MsQuic 监听器
    HQUIC listener_;
    
    // 初始化标志
    bool initialized_;
    
    // 服务器端口
    uint16_t port_;
    
    // ALPN 协议
    std::string alpn_;
    
    // 连接池互斥锁
    std::mutex connectionsMutex_;
    
    // 服务器线程
    std::thread serverThread_;
    
    // 运行标志
    std::atomic<bool> running_;

    std::map<HQUIC, std::shared_ptr<MsQuicSession>> msQuicConnections;

    std::function<void(std::shared_ptr<MsQuicSession>)> connectFunc;

    std::function<void(std::shared_ptr<MsQuicSession>)> connectOutFunc;

    // 服务器线程函数
    void ServerThreadFunc();

    /**
     * @brief 检查流是否有效
     * @param stream QUIC流句柄
     * @return 流是否有效
     */
    bool IsStreamValid(HQUIC stream);

    // === 静态回调函数 ===
    
    /**
     * @brief 监听器回调函数，处理新连接
     */
    static QUIC_STATUS QUIC_API ServerListenerCallback(
        HQUIC Listener,
        void* Context,
        QUIC_LISTENER_EVENT* Event);
        
    /**
     * @brief 连接回调函数，处理连接事件
     */
    static QUIC_STATUS QUIC_API ServerConnectionCallback(
        HQUIC Connection,
        void* Context,
        QUIC_CONNECTION_EVENT* Event);
        
    /**
     * @brief 流回调函数，处理流事件
     */
    static QUIC_STATUS QUIC_API ServerStreamCallback(
        HQUIC Stream,
        void* Context,
        QUIC_STREAM_EVENT* Event);
};