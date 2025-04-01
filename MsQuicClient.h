#pragma once

#define QUIC_API_ENABLE_PREVIEW_FEATURES 1
#define NOMINMAX

#include <msquic.hpp>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <queue>
#include <thread>
#include <condition_variable>
#include <memory>
#include <map>
#include "MsQuicClientFileTransfer.h"

// 消息回调函数类型
using MessageReceivedCallback = std::function<void(const std::string&)>;
// 连接状态回调函数类型
using ConnectionStateCallback = std::function<void(bool)>;

// MsQuic客户端类
class MsQuicClient {

    friend class MsQuicClientFileTransfer;

public:
    // 构造函数
    MsQuicClient(const MsQuicApi* msQuic);
    
    // 析构函数
    ~MsQuicClient();
    
    // 初始化客户端
    bool Initialize(const std::string& serverAddress, uint16_t serverPort, const std::string& alpn);
    
    // 关闭客户端
    void Shutdown();
    
    // 发送数据
    bool SendData(short msgId, const std::string& message);
    
    // 发送原始数据
    bool SendRawData(const std::string& message);
    
    // 设置消息回调
    void SetMessageCallback(MessageReceivedCallback callback);
    
    // 设置连接状态回调
    void SetConnectionCallback(ConnectionStateCallback callback);
    
    // 检查是否已连接
    bool IsConnected() const;
    
    // 获取连接信息
    std::string GetConnectionInfo() const;
    
    // 设置接收流回调
    void setReviceStreamCallBack(std::function<void(QUIC_STREAM_EVENT*)> streamCallBack);
    
    // 设置接收文件流回调
    void setReviceFileStreamCallBack(std::function<void(QUIC_STREAM_EVENT*, HQUIC)> streamCallBack);
    
    // 设置文件写入流回调
    void setFileWriterStreamCallBack(std::function<void(MsQuicClient* client, HQUIC)> fileWriterStream);
    
    // 写入文件
    void writerFile(std::function<void(MsQuicClient* client, HQUIC)> func);
    
    // 写入文件系统
    bool WriterFileSystem(const std::string& message, HQUIC streams);

    bool WriterFileSystem(const uint8_t* data, size_t dataLength, HQUIC streams);

  
    // 创建单向流
    HQUIC CreateUnidirectionalStream();
    
    // 检查流是否有效
    bool IsStreamValid(HQUIC stream);

    void setReviceDataHandler(std::map<short, std::function<void(MsQuicClient* client, MsQuicClientFileTransfer* transfer)>> reviceDataHandler);

private:
    // 处理连接事件
    QUIC_STATUS HandleConnectionEvent(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
    
    // 处理流事件
    static QUIC_STATUS HandleStreamEvent(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
    
    // 连接回调函数
    static QUIC_STATUS QUIC_API ConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
    
    // 流回调函数
    static QUIC_STATUS QUIC_API StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
    
    // 运行任务
    void runTask(std::function<void()> task);

        // 将服务器地址转换为QUIC_ADDR结构
    bool ConvertAddressToQuicAddr(const char* serverAddress, uint16_t port, QUIC_ADDR* quicAddr);


private:
    // MsQuic API
    const MsQuicApi* msQuic_;
    
    // 注册
    MsQuicRegistration* registration_;
    
    // 配置
    MsQuicConfiguration* configuration_;
    
    // 连接
    HQUIC connection_;
    
    // 服务器地址
    std::string serverAddress_;
    
    // 服务器端口
    uint16_t serverPort_;
    
    // ALPN
    std::string alpn_;
    
    // 是否已初始化
    bool initialized_;
    
    // 是否已连接
    bool connected_;
    
    // 消息回调
    MessageReceivedCallback messageCallback_;
    
    // 连接状态回调
    ConnectionStateCallback connectionCallback_;
    
    // 互斥锁
    mutable std::mutex mutex_;
    
    // 写入流
    HQUIC writerStream;
    
    // 接收流
    HQUIC reviceStream;
    
    // 接收流回调
    std::function<void(QUIC_STREAM_EVENT*)> reviceStreamCallBack;
    
    // 接收文件流回调
    std::function<void(QUIC_STREAM_EVENT*, HQUIC)> reviceFileStreamCallBack;
    
    // 文件写入流回调
    std::function<void(MsQuicClient* client, HQUIC)> fileWriterStreamCallBack;
    
    // 写入处理器映射
    std::map<HQUIC, std::function<void(MsQuicClient* client,HQUIC)>> writerHandler;

    std::map<HQUIC, std::unique_ptr<MsQuicClientFileTransfer>> reviceHandler;

    std::map<short, std::function<void(MsQuicClient* client, MsQuicClientFileTransfer* transfer)>> reviceDataHandler;
    
    // 任务队列
    std::queue<std::shared_ptr<std::function<void()>>> taskQueue;
    
    // 条件变量
    std::condition_variable condition;
    
    // 是否运行中
    std::atomic<bool> isRunning;
    
    // 写入线程
    std::thread writerThread;

    // 用于内存管理的上下文结构体（添加虚析构函数）
    struct SendContext {
        uint8_t* buffer;
        size_t size;
        bool aligned;
        void* userContext;
        // 添加显式构造函数
        SendContext(uint8_t* buf, size_t sz, bool align, void* ctx) :
            buffer(buf), size(sz), aligned(align), userContext(ctx) {}

        virtual ~SendContext() = default; // 添加虚析构函数支持dynamic_cast
    };

    // 分块发送上下文（继承自SendContext）
    struct ChunkedSendContext : public SendContext {
        std::vector<uint8_t*> *bufferPointers;

        ChunkedSendContext(std::vector<uint8_t*> * ptrs) :
            SendContext(nullptr, 0, true, nullptr),  // 显式调用基类构造函数
            bufferPointers(ptrs) {}

        virtual ~ChunkedSendContext() override = default;
    };
};

// 日志记录函数
void LogEvent(const std::string& eventType, const std::string& details);



