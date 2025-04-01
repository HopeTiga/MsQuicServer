#pragma once
#include <msquic.hpp>
#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <map>
#include <mutex>
#include "MsQuicFileTransfer.h"

class MsQuicServer;

class MsQuicSession : public std::enable_shared_from_this<MsQuicSession>
{

    friend class MsQuicFileTransfer;

public:

	MsQuicSession(HQUIC connect, std::string ip, MsQuicServer* server);

	~MsQuicSession();

	HQUIC connect; //连接句柄
	HQUIC writerStream; //输出数据流
	HQUIC reviceStream; //输入数据流

	std::string ip; //客户端IP

	bool writerData(const std::string& data);

	bool writerData(const std::string& data, HQUIC stream);

	void reviceData(QUIC_STREAM_EVENT* revice);

	bool writerFileSystem(const std::string& message, HQUIC stream);

    bool WriterFileSystem(const uint8_t* data, size_t dataLength, HQUIC streams);

	void writerFile(std::function<void(HQUIC)> func);

    void setReviceDataFunc(std::function<void (QUIC_STREAM_EVENT* revice)> func);

	void setReviceFileSystem(HQUIC stream);

	void removeWriterFileSystem(HQUIC stream);

private:
    std::shared_ptr<MsQuicApi> msQuic;

    MsQuicServer* server;
    
    // 添加数据接收相关变量
    std::atomic<bool> header{true};
    std::vector<char> receivedHeader;
    std::vector<char> receivedData;
    short messageType{0};
    int64_t dataLength{0};
    int64_t expectedDataLength{0};
    
    // 添加回调处理器映射
    std::map<short, std::function<void(MsQuicSession* sssion,MsQuicFileTransfer* transfer)>> reviceDataHandler;
    
    // 现有的互斥锁
    std::mutex mutexs;
    
    // 修改后的处理完整消息的方法，增加了消息ID和数据大小参数
    void processCompleteMessage(short msgId, int64_t dataSize, const std::vector<char>& messageData);

    // 添加其他必要的方法声明
    void setReviceDataHandler(std::map<short, std::function<void(MsQuicSession* sssion, MsQuicFileTransfer* transfer)>> reviceDataHandler);

    HQUIC createMsQuicSessionStream(HQUIC connection);

    HQUIC CreateUnidirectionalStream();

    static QUIC_STATUS QUIC_API sessionStreamCallback(
        HQUIC Stream,
        void* Context,
        QUIC_STREAM_EVENT* Event);

    // 检查流是否有效
    bool isStreamValid(HQUIC stream) const;
   

    std::map<HQUIC, std::function<void(HQUIC)>> writerHandler;
    std::map<HQUIC, std::unique_ptr<MsQuicFileTransfer>> reviceHandler;

    std::function<void(QUIC_STREAM_EVENT* revice)> reviceDataFunc;

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
        std::vector<uint8_t*>* bufferPointers;

        ChunkedSendContext(std::vector<uint8_t*>* ptrs) :
            SendContext(nullptr, 0, true, nullptr),  // 显式调用基类构造函数
            bufferPointers(ptrs) {}

        virtual ~ChunkedSendContext() override = default;
    };
};








