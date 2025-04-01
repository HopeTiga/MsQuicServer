#define QUIC_API_ENABLE_PREVIEW_FEATURES 1

#include "MsQuicClient.h"
#include <iomanip> 
#include <ws2tcpip.h> 
#include <iostream>
#include <thread>
#include <chrono>
#include <sstream>
#include <fstream>
#include "Utils.h"

// 添加库链接指令
#pragma comment(lib, "Ws2_32.lib")

// 构造函数
MsQuicClient::MsQuicClient(const MsQuicApi* msQuic) : 
    msQuic_(msQuic),  
    connection_(nullptr),
    initialized_(false),
    connected_(false),
    serverPort_(4433),
    alpn_("quic"),
    writerStream(nullptr),
    reviceStream(nullptr), 
    isRunning(true) {

    // 初始化任务处理线程
    writerThread = std::thread([this]() {
        while (isRunning) {
            std::shared_ptr<std::function<void()>> task = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition.wait(lock, [this] { 
                    return !taskQueue.empty() || !isRunning; 
                });
                
                if (!isRunning) break;
                
                if (!taskQueue.empty()) {
                    task = taskQueue.front();
                    taskQueue.pop();
                }
            }
            
            if (task && *task) {
                try {
                    (*task)();
                } catch (const std::exception& e) {
                    LogEvent("Error", "Exception in task execution: " + std::string(e.what()));
                }
            }
        }
    });
}

// 析构函数
MsQuicClient::~MsQuicClient() {
    // 停止任务线程
    isRunning = false;
    condition.notify_all();
    
    if (writerThread.joinable()) {
        try {
            writerThread.join();
        } catch (const std::exception& e) {
            LogEvent("Error", "Exception while joining thread: " + std::string(e.what()));
        }
    }
    
    // 关闭网络连接
    Shutdown();
}

// 初始化客户端
bool MsQuicClient::Initialize(const std::string& serverAddress, uint16_t serverPort, const std::string& alpn) {
    std::lock_guard<std::mutex> lock(mutex_);

    serverAddress_ = serverAddress;
    serverPort_ = serverPort;
    alpn_ = alpn;

    // 验证MsQuic API
    if (msQuic_ == nullptr) {
        LogEvent("Error", "MsQuicApi is nullptr");
        return false;
    }

    QUIC_STATUS initStatus = msQuic_->GetInitStatus();
    if (QUIC_FAILED(initStatus)) {
        std::stringstream ss;
        ss << "MsQuicApi initialization failed: 0x" << std::hex << (uint32_t)initStatus;
        LogEvent("Error", ss.str());
        return false;
    }

    // 创建注册
    registration_ = new MsQuicRegistration("MsQuicClient");
    if (!registration_->IsValid()) {
        LogEvent("Error", "MsQuicRegistration creation failed");
        return false;
    }

    // 配置设置
    MsQuicSettings settings;
    settings.SetIdleTimeoutMs(30000);
    settings.SetKeepAlive(10000);
    settings.SetPeerBidiStreamCount(10);  // 允许对端创建10个双向流
    settings.SetPeerUnidiStreamCount(10); // 允许对端创建10个单向流
    settings.SetConnFlowControlWindow(100 * 1024 * 1024); // 100MB
    settings.SetStreamRecvWindowDefault(8 * 1024 * 1024); // 8MB
    settings.SetMaxAckDelayMs(20);
    settings.SetStreamMultiReceiveEnabled(true);
    settings.SetSendBufferingEnabled(true);
    settings.SetPacingEnabled(true);
    settings.SetCongestionControlAlgorithm(QUIC_CONGESTION_CONTROL_ALGORITHM::QUIC_CONGESTION_CONTROL_ALGORITHM_BBR); // 使用BBR算法（若可用）/
    settings.SetPacingEnabled(true);       // 启用数据包节奏控制
    settings.SetSendBufferingEnabled(true);// 启用发送缓冲减少系统调用
    settings.SetEcnEnabled(true);
    settings.SetEncryptionOffloadAllowed(true); // 启用显式拥塞通知


    // 配置ALPN
    MsQuicAlpn alpnBuffer(alpn_.c_str());

    // 配置证书验证
    QUIC_CREDENTIAL_CONFIG credConfig = {};
    credConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    credConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    

    // 创建配置
    configuration_ = new MsQuicConfiguration(
        *registration_,
        alpnBuffer,
        settings,
        MsQuicCredentialConfig(credConfig));

    if (!configuration_->IsValid()) {
        std::stringstream ss;
        ss << "MsQuicConfiguration failed: 0x" << std::hex << configuration_->GetInitStatus();
        
        // 添加更详细的错误信息
        std::string errorMsg = ss.str();
        if (configuration_->GetInitStatus() == 0x80090331) {
            errorMsg += " (SEC_E_ALGORITHM_MISMATCH: Algorithm mismatch, client and server cannot negotiate a mutually supported encryption algorithm)";
        }
        
        LogEvent("Error", errorMsg);
        
        delete configuration_;
        delete registration_;
        return false;
    }



    // 打开连接
    QUIC_STATUS status = msQuic_->ConnectionOpen(
        registration_->Handle,  // 使用正确的方法名称
        ConnectionCallback,
        this,
        &connection_);

    if (QUIC_FAILED(status)) {
        std::stringstream ss;
        ss << "ConnectionOpen failed: 0x" << std::hex << status;
        LogEvent("Error", ss.str());
        delete configuration_;
        delete registration_;
        return false;
    }


    // 启动连接 - 修复参数数量
    QUIC_ADDR quicAddr = {0};
    if (!ConvertAddressToQuicAddr(serverAddress_.c_str(), serverPort_, &quicAddr)) {
        LogEvent("Error", "Failed to convert address");
        return false;
    }

    // 使用正确的参数调用ConnectionStart
    status = msQuic_->ConnectionStart(
        connection_,
        configuration_->Handle,      // 添加配置参数
        QUIC_ADDRESS_FAMILY_UNSPEC,  // 地址族
        serverAddress_.c_str(),      // 服务器地址
        serverPort_);                // 服务器端口

    if (QUIC_FAILED(status)) {
        std::stringstream ss;
        ss << "ConnectionStart failed: 0x" << std::hex << status;
        LogEvent("Error", ss.str());
        return false;
    }

    initialized_ = true;
    LogEvent("Info", "Client initialized successfully");
    return true;
}

// 关闭客户端
void MsQuicClient::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        LogEvent("Info", "Shutting down client...");

        if (connection_ != nullptr) {
            msQuic_->ConnectionShutdown(
                connection_,
                QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                0);
            msQuic_->ConnectionClose(connection_);
            connection_ = nullptr;
        }

        if (configuration_ != nullptr) {
            delete configuration_;
            configuration_ = nullptr;
        }

        if (registration_ != nullptr) {
            delete registration_;
            registration_ = nullptr;
        }

        initialized_ = false;
        connected_ = false;
        LogEvent("Info", "Client shutdown complete");
    }
}

// 发送数据
bool MsQuicClient::SendData(short msgId, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || !connected_ || connection_ == nullptr) {
        LogEvent("Error", "Cannot send data: client not connected");
        return false;
    }

    // 创建流
    if (writerStream == nullptr) {
        QUIC_STATUS status = msQuic_->StreamOpen(
            connection_,
            QUIC_STREAM_OPEN_FLAG_NONE,
            StreamCallback,
            this,
            &writerStream);

        if (QUIC_FAILED(status)) {
            std::stringstream ss;
            ss << "Failed to open stream: 0x" << std::hex << status;
            LogEvent("Error", ss.str());
            return false;
        }

        LogEvent("Info", "Client stream created successfully");

        // 启动流
        status = msQuic_->StreamStart(
            writerStream,
            QUIC_STREAM_START_FLAG_NONE);

        if (QUIC_FAILED(status)) {
            std::stringstream ss;
            ss << "Failed to start stream: 0x" << std::hex << status;
            LogEvent("Error", ss.str());
            msQuic_->StreamClose(writerStream);
            writerStream = nullptr;
            return false;
        }

        LogEvent("Info", "Client stream started successfully");
    }

    // 构建消息头
    std::string fullMessage;
    fullMessage.resize(2);
    fullMessage[0] = (msgId >> 8) & 0xFF;
    fullMessage[1] = msgId & 0xFF;
    fullMessage += message;

    return SendRawData(fullMessage);
}

// 检查流是否有效
bool MsQuicClient::IsStreamValid(HQUIC stream) {
    if (!stream || !msQuic_) {
        return false;
    }

    QUIC_STREAM_STATISTICS stats;
    uint32_t statsSize = sizeof(stats);
    QUIC_STATUS status = msQuic_->GetParam(
        stream,
        QUIC_PARAM_STREAM_STATISTICS,
        &statsSize,
        &stats);

    return QUIC_SUCCEEDED(status);
}

void MsQuicClient::setReviceDataHandler(std::map<short, std::function<void(MsQuicClient* client, MsQuicClientFileTransfer* transfer)>> reviceDataHandler)
{
    this->reviceDataHandler = reviceDataHandler;
}

// 发送原始数据
bool MsQuicClient::SendRawData(const std::string& message) {
    // 检查参数有效性
    if (!initialized_ || connection_ == nullptr) {
        LogEvent("Error", "Client not initialized or connection invalid");
        return false;
    }

    if (writerStream == nullptr) {
        LogEvent("Error", "Stream is nullptr");
        return false;
    }

    if (msQuic_ == nullptr) {
        LogEvent("Error", "MsQuic API is null");
        return false;
    }

    try {
        // 检查流状态
        QUIC_STREAM_STATISTICS stats;
        uint32_t statsSize = sizeof(stats);
        QUIC_STATUS checkStatus = msQuic_->GetParam(
            writerStream,
            QUIC_PARAM_STREAM_STATISTICS,
            &statsSize,
            &stats);

        if (QUIC_FAILED(checkStatus)) {
            LogEvent("Error", "Stream is invalid, cannot send data");
            writerStream = nullptr;
            return false;
        }

        // 分配内存并复制数据
        uint8_t* messageBuffer = new uint8_t[message.length()];
        memcpy(messageBuffer, message.c_str(), message.length());

        QUIC_BUFFER buffer;
        buffer.Buffer = messageBuffer;
        buffer.Length = (uint32_t)message.length();

        // 发送数据
        QUIC_STATUS status = msQuic_->StreamSend(
            writerStream,
            &buffer,
            1,
            QUIC_SEND_FLAG_NONE,
            messageBuffer);  // 传递内存指针作为上下文

        if (QUIC_FAILED(status)) {
            std::stringstream ss;
            ss << "Failed to send data: 0x" << std::hex << status;
            LogEvent("Error", ss.str());

            // 发送失败时释放内存
            delete[] messageBuffer;
            return false;
        }

        // 发送成功后，内存将在SEND_COMPLETE事件中释放
    } catch (const std::exception& e) {
        LogEvent("Error", "Exception in SendRawData: " + std::string(e.what()));
        return false;
    }

    return true;
}

// 写入文件系统
bool MsQuicClient::WriterFileSystem(const std::string& message, HQUIC streams) {
    if (!initialized_ || connection_ == nullptr) {
        LogEvent("Error", "Client not initialized or connection invalid");
        return false;
    }

    if (streams == nullptr) {
        LogEvent("Error", "stream is not available");
        return false;
    }

    if (!IsStreamValid(streams)) {
        return false;
    }

    try {
        // 使用智能指针管理内存，避免内存泄漏
        uint8_t* messageBuffer = new uint8_t[message.length()];
        memcpy(messageBuffer, message.c_str(), message.length());

        QUIC_BUFFER buffer;
        buffer.Buffer = messageBuffer;
        buffer.Length = (uint32_t)message.length();

        // 发送数据，使用contextBuffer作为上下文
        QUIC_STATUS status = msQuic_->StreamSend(
            streams,
            &buffer,
            1,
            QUIC_SEND_FLAG_NONE,
            messageBuffer);  // 传递内存指针作为上下文

        if (QUIC_FAILED(status)) {
            std::stringstream ss;
            ss << "Failed to send data through streams: 0x" << std::hex << status;
            LogEvent("Error", ss.str());
            delete[] messageBuffer;  // 失败时释放内存
            return false;
        }

        // messageBuffer会在函数结束时自动释放
        // contextBuffer将在SEND_COMPLETE事件中释放
    } catch (const std::exception& e) {
        LogEvent("Error", "Exception in WriterFileSystem: " + std::string(e.what()));
        return false;
    }

    return true;
}

bool MsQuicClient::WriterFileSystem(const uint8_t* data, size_t dataLength, HQUIC streams) {
    if (!initialized_ || connection_ == nullptr) {
        LogEvent("Error", "客户端未初始化或连接无效");
        return false;
    }

    if (streams == nullptr) {
        LogEvent("Error", "流不可用");
        return false;
    }

    if (!IsStreamValid(streams)) {
        return false;
    }

    if (data == nullptr || dataLength == 0) {
        LogEvent("Error", "无效的数据指针或长度为零");
        return false;
    }

    try {
        // 定义最佳分块大小，避免IP分片
        const size_t optimalChunkSize = 1400; // 典型QUIC MTU减去头部

        // 如果数据小于最佳分块大小，直接发送
        if (dataLength <= optimalChunkSize) {
            // 创建对齐的内存缓冲区
#ifdef _WIN32
            uint8_t* messageBuffer = (uint8_t*)_aligned_malloc(dataLength, 32);
            if (!messageBuffer) {
                LogEvent("Error", "can't malloc aligned memory");
                return false;
            }
#else
            size_t alignedSize = ((dataLength + 31) / 32) * 32;
            uint8_t* messageBuffer = (uint8_t*)aligned_alloc(32, alignedSize);
            if (!messageBuffer) {
                LogEvent("Error", "can't malloc aligned memory");
                return false;
            }
#endif

            // 复制数据到对齐的缓冲区
            memcpy_s(messageBuffer, dataLength, data, dataLength);

            QUIC_BUFFER * buffer = new QUIC_BUFFER;
            buffer->Buffer = messageBuffer;
            buffer->Length = (uint32_t)dataLength;

            // 创建上下文结构体
            SendContext* context = new SendContext(
                messageBuffer,
                dataLength,
                true,  // 使用对齐内存
                nullptr
            );

            // 发送数据
            QUIC_STATUS status = msQuic_->StreamSend(
                streams,
                buffer,
                1,
                QUIC_SEND_FLAG_ALLOW_0_RTT | QUIC_SEND_FLAG_PRIORITY_WORK,
                context);

            if (QUIC_FAILED(status)) {
                std::stringstream ss;
                ss << "send hex data failed: 0x" << std::hex << status;
                LogEvent("Error", ss.str());

                // 释放资源
#ifdef _WIN32
                _aligned_free(messageBuffer);
#else
                free(messageBuffer);
#endif
                delete context;

                return false;
            }
        }
        else {
            // 数据较大，使用分块策略
            // 计算需要多少个块
            size_t numChunks = (dataLength + optimalChunkSize - 1) / optimalChunkSize;

            // 创建缓冲区数组
            std::vector<QUIC_BUFFER> * buffers = new std::vector<QUIC_BUFFER> (numChunks);
            std::vector<uint8_t*> * bufferPointers = new std::vector<uint8_t*> (numChunks);
            bufferPointers->reserve(numChunks);

            // 为每个块分配内存并填充数据
            for (size_t i = 0; i < numChunks; i++) {
                size_t offset = i * optimalChunkSize;
                size_t currentChunkSize = std::min(optimalChunkSize, dataLength - offset);

                // 分配对齐的内存
#ifdef _WIN32
                uint8_t* chunkBuffer = (uint8_t*)_aligned_malloc(currentChunkSize, 32);
#else
                size_t alignedSize = ((currentChunkSize + 31) / 32) * 32;
                uint8_t* chunkBuffer = (uint8_t*)aligned_alloc(32, alignedSize);
#endif

                if (!chunkBuffer) {
                    // 内存分配失败，清理已分配的内存
                    for (size_t j = 0; j < bufferPointers->size(); j++) {
#ifdef _WIN32
                        _aligned_free(bufferPointers->at(j));
#else
                        free(bufferPointers[j]);
#endif
                    }
                    LogEvent("Error", "can't malloc aligned memory :" + std::to_string(i) );
                    return false;
                }

                // 复制数据
                memcpy_s(chunkBuffer, currentChunkSize, data + offset, currentChunkSize);

                // 设置缓冲区
                buffers->at(i).Buffer = chunkBuffer;
                buffers->at(i).Length = (uint32_t)currentChunkSize;
                bufferPointers->push_back(chunkBuffer);
            }

            // 创建一个特殊的上下文结构体来跟踪所有分配的内存
            ChunkedSendContext* context = new ChunkedSendContext(bufferPointers);

            // 发送所有数据块
            QUIC_STATUS status = msQuic_->StreamSend(
                streams,
                buffers->data(),
                (uint32_t)buffers->size(),
                QUIC_SEND_FLAG_ALLOW_0_RTT | QUIC_SEND_FLAG_PRIORITY_WORK,
                context);

            if (QUIC_FAILED(status)) {
                std::stringstream ss;
                ss << "wirter data failed: 0x" << std::hex << status;
                LogEvent("Error", ss.str());

                // 释放所有内存
                for (size_t i = 0; i < bufferPointers->size(); i++) {
#ifdef _WIN32
                    _aligned_free(bufferPointers->at(i));
#else
                    free(bufferPointers[i]);
#endif
                }
                delete context;

                return false;
            }

            }
        }
    catch (const std::exception& e) {
        LogEvent("Error", "WriterFileSystem 异常: " + std::string(e.what()));
        return false;
    }

    return true;
    }

// 设置消息回调
void MsQuicClient::SetMessageCallback(MessageReceivedCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    messageCallback_ = callback;
}

// 设置连接状态回调
void MsQuicClient::SetConnectionCallback(ConnectionStateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    connectionCallback_ = callback;
}

// 检查是否已连接
bool MsQuicClient::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

// 获取连接信息
std::string MsQuicClient::GetConnectionInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!connected_ || connection_ == nullptr) {
        return "Not connected";
    }
    
    std::stringstream ss;
    ss << "Connected to " << serverAddress_ << ":" << serverPort_;
    
    // 获取连接统计信息
    QUIC_STATISTICS stats;
    uint32_t statsSize = sizeof(stats);
    if (QUIC_SUCCEEDED(msQuic_->GetParam(
            connection_,
            QUIC_PARAM_CONN_STATISTICS,
            &statsSize,
            &stats))) {
        
        ss << "\nSent: " << stats.Send.TotalBytes << " bytes in " 
           << stats.Send.TotalPackets << " packets"
           << "\nReceived: " << stats.Recv.TotalBytes << " bytes in " 
           << stats.Recv.TotalPackets << " packets";
    }
    
    return ss.str();
}

// 设置接收流回调
void MsQuicClient::setReviceStreamCallBack(std::function<void(QUIC_STREAM_EVENT*)> streamCallBack) {
    this->reviceStreamCallBack = streamCallBack;
}

// 设置接收文件流回调
void MsQuicClient::setReviceFileStreamCallBack(std::function<void(QUIC_STREAM_EVENT*, HQUIC)> streamCallBack) {
    this->reviceFileStreamCallBack = streamCallBack;
}

// 设置文件写入流回调
void MsQuicClient::setFileWriterStreamCallBack(std::function<void(MsQuicClient* client, HQUIC)> fileWriterStream) {
    this->fileWriterStreamCallBack = fileWriterStream;
}

// 创建单向流
HQUIC MsQuicClient::CreateUnidirectionalStream() {
    HQUIC stream = nullptr;
    QUIC_STATUS status;

    // 设置流选项为单向流
    QUIC_STREAM_OPEN_FLAGS flags = QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL;

    // 打开新的单向流
    status = msQuic_->StreamOpen(
        connection_,
        flags,
        StreamCallback,
        this,
        &stream);

    if (QUIC_FAILED(status)) {
        std::stringstream ss;
        ss << "创建单向流失败，错误码: 0x" << std::hex << status;
        LogEvent("Error", ss.str());
        return nullptr;
    }

    // 启动流
    status = msQuic_->StreamStart(
        stream,
        QUIC_STREAM_START_FLAG_PRIORITY_WORK);

    if (QUIC_FAILED(status)) {
        std::stringstream ss;
        ss << "启动单向流失败，错误码: 0x" << std::hex << status;
        LogEvent("Error", ss.str());
        msQuic_->StreamClose(stream);
        return nullptr;
    }

    return stream;
}

// 发送文件函数
void MsQuicClient::writerFile(std::function<void(MsQuicClient* client, HQUIC)> func) {
    // 保存回调函数
    this->fileWriterStreamCallBack = func;
    
    // 检查连接状态
    if (!initialized_ || !connected_ || connection_ == nullptr) {
        LogEvent("Error", "Cannot send file request: client not connected");
        return;
    }
    
    HQUIC stream = CreateUnidirectionalStream();
    
    this->writerHandler[stream] = func;
}

// 运行任务
void MsQuicClient::runTask(std::function<void()> task) {
    std::shared_ptr<std::function<void()>> taskPtr = 
        std::make_shared<std::function<void()>>(std::move(task));
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        taskQueue.push(taskPtr);
    }
    
    condition.notify_one();
}

// 处理连接事件
QUIC_STATUS MsQuicClient::HandleConnectionEvent(
    HQUIC Connection,
    void* Context,
    QUIC_CONNECTION_EVENT* Event) {

    auto client = static_cast<MsQuicClient*>(Context);
    
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        {
            connected_ = true;
            LogEvent("Info", "Connection established successfully");
            
            // 通知连接状态变化
            if (connectionCallback_) {
                connectionCallback_(true);
            }
        }
        break;
        
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        {
            std::stringstream ss;
            ss << "Connection shutdown by transport, status: 0x" 
               << std::hex << Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
            LogEvent("Warning", ss.str());
            
            connected_ = false;
            
            // 通知连接状态变化
            if (connectionCallback_) {
                connectionCallback_(false);
            }
        }
        break;
        
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        {
            std::stringstream ss;
            ss << "Connection shutdown by peer, error code: 0x" 
               << std::hex << Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode;
            LogEvent("Warning", ss.str());
            
            connected_ = false;
            
            // 通知连接状态变化
            if (connectionCallback_) {
                connectionCallback_(false);
            }
        }
        break;
        
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        {
            LogEvent("Info", "Connection shutdown complete");
            
            connected_ = false;
            
            // 通知连接状态变化
            if (connectionCallback_) {
                connectionCallback_(false);
            }
        }
        break;
        
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        {
            HQUIC Stream = Event->PEER_STREAM_STARTED.Stream;
            
            // 根据流类型处理
            if (Stream!=nullptr) {
               
                if (client->reviceStream == nullptr) {
                    client->reviceStream = Stream;
                }
                else if (client->writerStream == nullptr) {
                    client->writerStream = Stream;
                }
                else {
                    std::lock_guard<std::mutex> lock(mutex_);

                    // 创建新的文件传输处理器
                    auto fileTransfer = std::make_unique<MsQuicClientFileTransfer>();
                    // 添加回调函数...

                    fileTransfer->setReviceDataHandler(this->reviceDataHandler);

                    fileTransfer->setMsQuicClient(this);

                    fileTransfer->setMsQuicStream(Stream);

                    // 存储到映射中
                    reviceHandler[Stream] = std::move(fileTransfer);
                }

                LogEvent("Info", "Peer stream started");

                msQuic_->SetCallbackHandler(
                    Event->PEER_STREAM_STARTED.Stream,
                    (void*)HandleStreamEvent,
                    this);
                
            }
        }
        break;
        
    default:
        break;
    }
    
    return QUIC_STATUS_SUCCESS;
}

// 处理流事件
QUIC_STATUS MsQuicClient::HandleStreamEvent(
    HQUIC Stream,
    void* Context,
    QUIC_STREAM_EVENT* Event) {
    
    auto client = static_cast<MsQuicClient*>(Context);
    if (!client) {
        return QUIC_STATUS_INVALID_PARAMETER;
    }
    
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
    {
        std::stringstream ss;
        ss << "Stream start complete, status: " << Event->START_COMPLETE.Status;
        LogEvent("Info", ss.str());

        if (client->writerHandler.find(Stream) != client->writerHandler.end()) {

            std::thread writerThread([client, Stream]() {

                std::this_thread::sleep_for(std::chrono::milliseconds(1000));

                client->writerHandler[Stream](client,Stream);

                });

            writerThread.detach();
        }

    }
    break;
    case QUIC_STREAM_EVENT_RECEIVE:
        {
            // 检查是否为接收流
            if (client->reviceStream == Stream) {
                // 如果有接收流回调，则调用
                if (client->reviceStreamCallBack) {
                    client->reviceStreamCallBack(Event);
                } else {
                    // 默认处理
                    const uint64_t bufferLength = Event->RECEIVE.TotalBufferLength;
                    const QUIC_BUFFER* buffer = Event->RECEIVE.Buffers;
                    const uint32_t bufferCount = Event->RECEIVE.BufferCount;
                    
                    std::string message;
                    for (uint32_t i = 0; i < bufferCount; ++i) {
                        message.append(reinterpret_cast<const char*>(buffer[i].Buffer), buffer[i].Length);
                    }
                    
                    // 调用消息回调
                    if (client->messageCallback_) {
                        client->messageCallback_(message);
                    }
                    
                    std::stringstream msgSs;
                    msgSs << "Received message: \"" << message << "\"";
                    LogEvent("Info", msgSs.str());
                }
            } else {
                
                if (client->reviceHandler.find(Stream) != client->reviceHandler.end()) {
                    client->reviceHandler[Stream]->processFileData(Event);
                }
               
            }
        }
        break;
        
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        {
        if (Event->SEND_COMPLETE.ClientContext) {
            try {
                auto context = static_cast<SendContext*>(Event->SEND_COMPLETE.ClientContext);

       
                // 统一释放资源
                if (auto chunkedContext = dynamic_cast<ChunkedSendContext*>(context)) {
                    // 处理分块内存释放
                    for (auto ptr : *(chunkedContext->bufferPointers)) {
                        if (ptr) {
#ifdef _WIN32
                            _aligned_free(ptr);
#else
                            free(ptr);
#endif
                        }
                    }
                }
                else {
                    // 处理单个缓冲区释放
                    if (context->aligned) {
#ifdef _WIN32
                        _aligned_free(context->buffer);
#else
                        free(context->buffer);
#endif
                    }
                    else {
                        delete[] context->buffer;
                    }
                }

                // 安全删除上下文
                delete context;

            }
            catch (const std::exception& e) {
                LogEvent("Error", std::string("SEND_COMPLETE处理异常: ") + e.what());
            }
        }
        }
        break;
        
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        {
            LogEvent("Info", "Peer has shut down sending");
        }
        break;
        
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        {
            std::stringstream ss;
            ss << "Peer aborted sending, error code: 0x" 
               << std::hex << Event->PEER_SEND_ABORTED.ErrorCode;
            LogEvent("Warning", ss.str());
            
            // 检查是否为接收流
            if (client->reviceStream == Stream) {
                client->reviceStream = nullptr;
            } else {
                LogEvent("Error", "Invalid state in PEER_SEND_ABORTED event");
            }
        }
        break;
        
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        {
            // 添加空指针检查
            if (client && client->msQuic_ && Stream) {
                client->msQuic_->StreamClose(Stream);
                
                // 清除对应的流指针
                if (client->reviceStream == Stream) {
                    client->reviceStream = nullptr;
                } else if (client->writerStream == Stream) {
                    client->writerStream = nullptr;
                } 
            }
        }
        break;
        
    case QUIC_STREAM_EVENT_PEER_ACCEPTED:
        {
            LogEvent("Info", "Stream accepted by peer");
        }
        break;
        
    case QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE:
        {
            // 获取理想发送缓冲区大小
            uint64_t idealSize = Event->IDEAL_SEND_BUFFER_SIZE.ByteCount;
            std::stringstream ss;
            ss << "buffSize: " << idealSize << " byte";
            LogEvent("Info", ss.str());
            
            // 可以在这里根据新的缓冲区大小调整发送策略
            // 例如，调整发送数据块的大小
        }
        break;
        
    default:
        break;
    }
    
    return QUIC_STATUS_SUCCESS;
}

// 连接回调函数
QUIC_STATUS QUIC_API MsQuicClient::ConnectionCallback(
    HQUIC Connection,
    void* Context,
    QUIC_CONNECTION_EVENT* Event) {
    auto client = static_cast<MsQuicClient*>(Context);
    client->HandleConnectionEvent(Connection, Context, Event);
    return QUIC_STATUS_SUCCESS;
}

// 流回调函数
QUIC_STATUS QUIC_API MsQuicClient::StreamCallback(
    HQUIC Stream,
    void* Context,
    QUIC_STREAM_EVENT* Event) {
    return HandleStreamEvent(Stream, Context, Event);
}

// 将服务器地址转换为QUIC_ADDR结构
bool MsQuicClient::ConvertAddressToQuicAddr(const char* serverAddress, uint16_t port, QUIC_ADDR* quicAddr) {
    if (!serverAddress || !quicAddr) {
        LogEvent("Error", "Invalid parameters: address or QUIC_ADDR is NULL");
        return false;
    }

    // 清空地址结构
    memset(quicAddr, 0, sizeof(QUIC_ADDR));

    // 尝试将地址解析为IPv4
    IN_ADDR ipv4Addr;
    if (inet_pton(AF_INET, serverAddress, &ipv4Addr) == 1) {
        // 设置IPv4地址
        quicAddr->Ipv4.sin_family = AF_INET;
        quicAddr->Ipv4.sin_port = htons(port);
        quicAddr->Ipv4.sin_addr = ipv4Addr;
        return true;
    }

    // 尝试将地址解析为IPv6
    IN6_ADDR ipv6Addr;
    if (inet_pton(AF_INET6, serverAddress, &ipv6Addr) == 1) {
        // 设置IPv6地址
        quicAddr->Ipv6.sin6_family = AF_INET6;
        quicAddr->Ipv6.sin6_port = htons(port);
        quicAddr->Ipv6.sin6_addr = ipv6Addr;
        return true;
    }

    // 尝试通过主机名解析
    struct addrinfo hints = {0};
    struct addrinfo* result = nullptr;
    
    hints.ai_family = AF_UNSPEC;    // 允许IPv4或IPv6
    hints.ai_socktype = SOCK_DGRAM; // 对于QUIC，使用UDP
    
    // 将端口转换为字符串
    char portStr[16];
    sprintf_s(portStr, sizeof(portStr), "%hu", port);
    
    // 解析主机名
    int status = getaddrinfo(serverAddress, portStr, &hints, &result);
    if (status != 0) {
        std::stringstream ss;
        ss << "Failed to resolve hostname: " << serverAddress << ", error: " << gai_strerror(status);
        LogEvent("Error", ss.str());
        return false;
    }
    
    // 使用第一个返回的地址
    if (result->ai_family == AF_INET) {
        // IPv4 address
        memcpy(&quicAddr->Ipv4, result->ai_addr, sizeof(struct sockaddr_in));
    } else if (result->ai_family == AF_INET6) {
        // IPv6 address
        memcpy(&quicAddr->Ipv6, result->ai_addr, sizeof(struct sockaddr_in6));
    } else {
        freeaddrinfo(result);
        LogEvent("Error", "Unsupported address type");
        return false;
    }
    
    freeaddrinfo(result);
    return true;
}