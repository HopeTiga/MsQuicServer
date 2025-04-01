#define QUIC_API_ENABLE_PREVIEW_FEATURES
#define NOMINMAX 
#include "MsQuicSession.h"
#include <sstream>
#include <vector>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>  // 添加这个头文件用于格式化输出
#include <atomic>   // 添加这个头文件用于std::atomic
#include "MsQuicServer.h"
#include "MsQuicThreadTaskPool.h"
#include "Utils.h"

// 在MsQuicSession构造函数中，应该添加连接状态检查和错误处理
MsQuicSession::MsQuicSession(HQUIC connect, std::string ip, MsQuicServer* server)
    :connect(connect), writerStream(nullptr), reviceStream(nullptr)
    ,ip(ip), msQuic(nullptr), server(server)
{
    msQuic = std::make_shared<MsQuicApi>();
    
    // 检查连接状态
    if (connect == nullptr) {
        LogEvent("Error", "is not vaild");
        return;
    }
    
    // 使用异常处理创建流
    try {
        writerStream = createMsQuicSessionStream(connect);
        if (writerStream == nullptr) {
            LogEvent("Error", "wrtierStream is failed");
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        reviceStream = createMsQuicSessionStream(connect);
        if (reviceStream == nullptr) {
            LogEvent("Error", "reviceStream is failed");
        }
    } catch (const std::exception& e) {
        LogEvent("Error", std::string("创建流异常: ") + e.what());
    }
}


MsQuicSession::~MsQuicSession()
{
    // 确保所有流都被正确关闭
    if (writerStream) {
        msQuic->StreamClose(writerStream);
        writerStream = nullptr;
    }
    
    if (reviceStream) {
        msQuic->StreamClose(reviceStream);
        reviceStream = nullptr;
    }
    
    // 清理所有文件传输相关的资源
    writerHandler.clear();
    reviceHandler.clear();
    
    // 最后关闭连接
    if (connect) {
        msQuic->ConnectionClose(connect);
        connect = nullptr;
    }
}


bool MsQuicSession::isStreamValid(HQUIC stream) const {
    if (stream == nullptr) {
        LogEvent("Warning", "stream is nullptr");
        return false;
    }

    // 尝试获取流ID，如果成功则表示流有效
    uint64_t streamId = 0;
    uint32_t size = sizeof(streamId);
    QUIC_STATUS status = msQuic->GetParam(
        stream,
        QUIC_PARAM_STREAM_ID,
        &size,
        &streamId);

    uint64_t sendBufferSize = 0;
    uint32_t paramSize = sizeof(sendBufferSize);

    status = msQuic->GetParam(
        stream,
        QUIC_PARAM_STREAM_IDEAL_SEND_BUFFER_SIZE,
        &paramSize,
        &sendBufferSize);

    if (!QUIC_SUCCEEDED(status)) {
        std::stringstream errorSs;
        errorSs << "get stream buffer failed: 0x" << std::hex << status;
        LogEvent("Warning", errorSs.str());

        return false;
    }


    if (QUIC_FAILED(status)) {
        std::stringstream ss;
        ss << "stream is noValid,get streamID is failed: 0x" << std::hex << status;
        LogEvent("Warning", ss.str());
        return false;
    }

    return true;
}

void MsQuicSession::setReviceDataHandler(std::map<short, std::function<void(MsQuicSession* sssion, MsQuicFileTransfer* transfer)>> reviceDataHandler)
{
    this->reviceDataHandler = reviceDataHandler;
}


// 在writerData方法中添加更好的异常处理
bool MsQuicSession::writerData(const std::string& data)
{
    try {
        // +++ 前置防御性检查 +++
        if (!msQuic) {
            LogEvent("Critical", "msQuic instance is null!");
            return false;
        }

        if (!isStreamValid(writerStream)) {
            LogEvent("Error", "Writer stream is invalid");
            
            // 尝试重新创建流
            writerStream = createMsQuicSessionStream(connect);
            if (!isStreamValid(writerStream)) {
                LogEvent("Error", "Failed to recreate writer stream");
                return false;
            }
            LogEvent("Info", "Successfully recreated writer stream");
        }
        
        // 分配消息缓冲区
        uint8_t* messageBuffer = new uint8_t[data.length()];
        memcpy(messageBuffer, data.c_str(), data.length());

        // 创建缓冲区结构
        QUIC_BUFFER  * buffer = new QUIC_BUFFER;
        buffer->Buffer = messageBuffer;
        buffer->Length = (uint32_t)data.length();

        // +++ 流有效性二次验证 +++
        if (writerStream == nullptr) {
            LogEvent("Error", "Writer stream is null before sending");
            delete[] messageBuffer;
            return false;
        }

        // 添加更多错误检查
        QUIC_STATUS status = msQuic->StreamSend(
            writerStream,
            buffer,
            1,
            QUIC_SEND_FLAG_NONE,
            messageBuffer);

        if (QUIC_FAILED(status)) {
            std::stringstream ss;
            ss << "Failed to send data: 0x" << std::hex << status;
            LogEvent("Error", ss.str());
            try {
                delete[] messageBuffer;
            }
            catch (...) {
                LogEvent("Error", "Exception while freeing memory in error path");
            }

            try {
                msQuic->StreamClose(writerStream);
            }
            catch (...) {
                LogEvent("Error", "Exception while closing stream in error path");
            }
            return false;
        }

        LogEvent("Info", "MsQuicSession send data successfully: " + data);

	return true;
    } catch (const std::exception& e) {
        LogEvent("Error", std::string("Exception in writerData: ") + e.what());
        return false;
    } catch (...) {
        LogEvent("Error", "Unknown exception in writerData");
        return false;
    }
    
    return true;
}

bool MsQuicSession::writerData(const std::string& data,HQUIC stream)
{
    if (!isStreamValid(stream)) {
        std::cerr << "Writer stream is invalid" << std::endl;
        return false;
    }

    // 分配消息缓冲区
    uint8_t* messageBuffer = new uint8_t[data.length()];
    memcpy(messageBuffer, data.c_str(), data.length());

    // 创建缓冲区结构
    QUIC_BUFFER  * buffer = new QUIC_BUFFER;
    buffer->Buffer = messageBuffer;
    buffer->Length = (uint32_t)data.length();

    // 添加更多错误检查
    QUIC_STATUS status = msQuic->StreamSend(
        stream,
        buffer,
        1,
        QUIC_SEND_FLAG_NONE,
        messageBuffer);

    if (QUIC_FAILED(status)) {
        std::stringstream ss;
        ss << "Failed to send data: 0x" << std::hex << status;
        LogEvent("Error", ss.str());
        try {
            delete[] messageBuffer;
        }
        catch (...) {
            LogEvent("Error", "Exception while freeing memory in error path");
        }

        try {
            msQuic->StreamClose(stream);
        }
        catch (...) {
            LogEvent("Error", "Exception while closing stream in error path");
        }
        return false;
    }

    delete[] messageBuffer;

    LogEvent("Info", "MsQuicSession send data successfully: " + data);

    return true;
}

// 优化数据接收处理
void MsQuicSession::reviceData(QUIC_STREAM_EVENT* revice)
{
    LogEvent("Info", "Stream receive event");

    // Get total data length
    uint64_t totalLength = revice->RECEIVE.TotalBufferLength;

    std::stringstream ss;
    ss << "Received data, total length: " << totalLength << " bytes";
    LogEvent("Info", ss.str());

    // Process received data if needed
    if (totalLength > 0 && revice->RECEIVE.BufferCount > 0) {

        if (header.load()) {
            // 如果头部数据不完整，继续收集头部数据
            if (receivedHeader.size() < (sizeof(short) + sizeof(int64_t))) {
                for (uint32_t i = 0; i < revice->RECEIVE.BufferCount; ++i) {
                    const QUIC_BUFFER* buffer = &revice->RECEIVE.Buffers[i];
                    if (buffer->Buffer != nullptr && buffer->Length > 0) {
                        receivedHeader.insert(receivedHeader.end(),
                            buffer->Buffer,
                            buffer->Buffer + buffer->Length);
                    }
                }
                
                // 检查头部是否已经收集完整
                if (receivedHeader.size() >= (sizeof(short) + sizeof(int64_t))) {
                    // 从头部提取 short 类型的消息类型
                    messageType = *reinterpret_cast<short*>(receivedHeader.data());
                    
                    // 从头部提取 int64_t 类型的数据长度
                    dataLength = *reinterpret_cast<int64_t*>(receivedHeader.data() + sizeof(short));
                    
                    std::stringstream headerSs;
                    headerSs << "Header parsed: messageType=" << messageType 
                            << ", dataLength=" << dataLength << " bytes";
                    LogEvent("Info", headerSs.str());
                    
                    // 设置期望的数据长度
                    expectedDataLength = dataLength;
                    
                    // 将剩余的数据（如果有）移动到 receivedData
                    if (receivedHeader.size() > (sizeof(short) + sizeof(int64_t))) {
                        size_t extraDataSize = receivedHeader.size() - (sizeof(short) + sizeof(int64_t));
                        receivedData.insert(receivedData.end(),
                            receivedHeader.begin() + sizeof(short) + sizeof(int64_t),
                            receivedHeader.end());
                        
                        std::stringstream extraSs;
                        extraSs << "Extra data moved from header: " << extraDataSize << " bytes";
                        LogEvent("Info", extraSs.str());
                    }
                    
                    // 头部已处理完毕，设置标志
                    header.store(false);
                }
            }
        }
        else {
            // 继续收集数据部分
            for (uint32_t i = 0; i < revice->RECEIVE.BufferCount; ++i) {
                const QUIC_BUFFER* buffer = &revice->RECEIVE.Buffers[i];
                if (buffer->Buffer != nullptr && buffer->Length > 0) {
                    receivedData.insert(receivedData.end(),
                        buffer->Buffer,
                        buffer->Buffer + buffer->Length);
                }
            }
            
            // 检查是否已收集到足够的数据
            if (receivedData.size() >= expectedDataLength) {
                std::stringstream dataSs;
                dataSs << "Complete message received: " << receivedData.size() 
                      << " bytes (expected " << expectedDataLength << " bytes)";
                LogEvent("Info", dataSs.str());
                
                // 如果收到的数据超过了预期长度，截断多余部分
                if (receivedData.size() > expectedDataLength) {
                    size_t excessDataSize = receivedData.size() - expectedDataLength;
                    std::stringstream excessSs;
                    excessSs << "Truncating excess data: " << excessDataSize << " bytes";
                    LogEvent("Info", excessSs.str());
                    
                    // 保留多余数据以备下一次处理
                    std::vector<char> excessData(
                        receivedData.begin() + expectedDataLength,
                        receivedData.end()
                    );
                    
                    // 截断当前数据到预期长度
                    receivedData.resize(expectedDataLength);
                    
                    // 处理完整消息
                    processCompleteMessage(messageType,dataLength,receivedData);
                    
                    // 清空接收缓冲区，准备接收新消息
                    receivedData.clear();
                    receivedHeader.clear();
                    header.store(true);
                    
                    // 将多余数据放回接收缓冲区，如果是头部数据则放入 receivedHeader
                    if (excessData.size() >= (sizeof(short) + sizeof(int64_t))) {
                        // 足够形成新的头部，处理新消息的头部
                        short newMessageType = *reinterpret_cast<short*>(excessData.data());
                        int64_t newDataLength = *reinterpret_cast<int64_t*>(excessData.data() + sizeof(short));
                        
                        std::stringstream newHeaderSs;
                        newHeaderSs << "New header in excess data: messageType=" << newMessageType 
                                  << ", dataLength=" << newDataLength << " bytes";
                        LogEvent("Info", newHeaderSs.str());
                        
                        expectedDataLength = newDataLength;
                        header.store(false);
                        
                        // 将剩余数据移动到 receivedData
                        if (excessData.size() > (sizeof(short) + sizeof(int64_t))) {
                            receivedData.insert(receivedData.end(),
                                excessData.begin() + sizeof(short) + sizeof(int64_t),
                                excessData.end());
                        }
                    } else {
                        // 不足以形成新的头部，放入 receivedHeader
                        receivedHeader = std::move(excessData);
                    }
                } else {
                    // 数据长度正好，处理完整消息
                    processCompleteMessage(messageType, dataLength, receivedData);
                    
                    // 清空接收缓冲区，准备接收新消息
                    receivedData.clear();
                    receivedHeader.clear();
                    header.store(true);
                }
            }
        }
    }
    
}

// 添加处理完整消息的方法
/**
 * @brief 处理完整接收到的消息
 * @param msgId 消息ID
 * @param dataSize 数据大小
 * @param messageData 消息数据
 * 
 * 该方法在完整接收到一条消息后被调用，负责解析和处理消息内容
 */
void MsQuicSession::processCompleteMessage(short msgId, int64_t dataSize, const std::vector<char>& messageData)
{
    // 将二进制数据转换为字符串（用于日志记录）
    std::string message(messageData.begin(), messageData.end());
    std::stringstream msgSs;
    msgSs << "Processing complete message: \"" << message << "\"";
    LogEvent("Info", msgSs.str());
    
    // 这里可以添加实际的消息处理逻辑
    // 根据msgId分发到不同的处理函数
}

// 在createMsQuicSessionStream方法中添加更多连接设置
HQUIC MsQuicSession::createMsQuicSessionStream(HQUIC connection)
{
    // +++ 连接有效性检查 +++
    if (connection == nullptr) {
        LogEvent("Error", "Cannot create stream on null connection");
        return nullptr;
    }

    HQUIC Stream = nullptr;
    QUIC_STATUS status = msQuic->StreamOpen(
        connection,
        QUIC_STREAM_OPEN_FLAG_NONE,
        sessionStreamCallback,
        this,
        &Stream);

    if (QUIC_FAILED(status)) {
        std::stringstream ss;
        ss << "Failed to open stream: 0x" << std::hex << status;
        LogEvent("Error", ss.str());
        return nullptr;
    }

    // 优化连接设置，增加超时时间和保活间隔
    LogEvent("Info", "MsQuicSession stream created successfully");

    status = msQuic->StreamStart(
        Stream,
        QUIC_STREAM_START_FLAG_IMMEDIATE | QUIC_STREAM_START_FLAG_INDICATE_PEER_ACCEPT | QUIC_STREAM_START_FLAG_PRIORITY_WORK);

    if (QUIC_FAILED(status)) {
        std::stringstream ss;
        ss << "Failed to start stream: 0x" << std::hex << status;
        LogEvent("Error", ss.str());
        msQuic->StreamClose(Stream);
        return nullptr;
    }

    return Stream;
}

// 在CreateUnidirectionalStream方法中添加更好的错误处理
// 在CreateUnidirectionalStream中添加流优先级设置
HQUIC MsQuicSession::CreateUnidirectionalStream()
{
    HQUIC stream = nullptr;
    QUIC_STATUS status;
    
    // 设置流选项为单向流
    QUIC_STREAM_OPEN_FLAGS flags = QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL;
    
    // 检查连接状态
    if (connect == nullptr) {
        LogEvent("Error", "Cannot create unidirectional stream: Connection closed or invalid");
        return nullptr;
    }
    
    
    // 打开新的单向流
    status = msQuic->StreamOpen(
        connect,
        flags,
        sessionStreamCallback,
        this,
        &stream);
    
    if (QUIC_FAILED(status)) {
        std::stringstream ss;
        ss << "Failed to create unidirectional stream, error code: 0x" << std::hex << status;
        LogEvent("Error", ss.str());
        return nullptr;
    }
    
    // 启动流
    status = msQuic->StreamStart(
        stream,
        QUIC_STREAM_START_FLAG_NONE);
    
    if (QUIC_FAILED(status)) {
        std::stringstream ss;
        ss << "Failed to start unidirectional stream, error code: 0x" << std::hex << status;
        LogEvent("Error", ss.str());
        msQuic->StreamClose(stream);
        return nullptr;
    }
    
    LogEvent("Info", "Successfully created and started unidirectional stream");
    return stream;
}

// Stream callback
QUIC_STATUS QUIC_API MsQuicSession::sessionStreamCallback(
    HQUIC Stream,
    void* Context,
    QUIC_STREAM_EVENT* Event) {

    static const char* eventNames[] = {
        "QUIC_STREAM_EVENT_START_COMPLETE",            // 0
        "QUIC_STREAM_EVENT_RECEIVE",                   // 1
        "QUIC_STREAM_EVENT_SEND_COMPLETE",            // 2
        "QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN",       // 3
        "QUIC_STREAM_EVENT_PEER_SEND_ABORTED",        // 4
        "QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED",     // 5
        "QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE",   // 6
        "QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE",        // 7
        "QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE",   // 8
        "QUIC_STREAM_EVENT_PEER_ACCEPTED",            // 9
        "QUIC_STREAM_EVENT_CANCEL_ON_LOSS"           // 10
    };

    auto session = static_cast<MsQuicSession*>(Context);
    if (session == nullptr || Event == nullptr) {
        return QUIC_STATUS_INVALID_PARAMETER;
    }

    try {
        if (Event->Type < sizeof(eventNames) / sizeof(eventNames[0])) {
            std::stringstream ss;
            ss << "Stream event: " << eventNames[Event->Type];
            LogEvent("Info", ss.str());

        }

        switch (Event->Type) {
        case QUIC_STREAM_EVENT_START_COMPLETE:
        {
            std::stringstream ss;
            ss << "Stream start complete, status: " << Event->START_COMPLETE.Status;
            LogEvent("Info", ss.str());

            if (session->writerHandler.find(Stream) != session->writerHandler.end()) {

                MsQuicThreadTaskPool::getInstannce()->runTask([session, Stream]() {

                    session->writerHandler[Stream](Stream);

                    });
            }

           
            
        }
        break;

        // Add handler for QUIC_STREAM_EVENT_RECEIVE (type 1)
        // 在sessionStreamCallback中添加缓冲区释放代码
        case QUIC_STREAM_EVENT_RECEIVE:
        {
            if (Stream == session->reviceStream) {
                
                if (session->reviceDataFunc) {

                    session->reviceDataFunc(Event);

                }
                else {
                    session->reviceData(Event);
                }

            } else if (session->reviceHandler.find(Stream) != session->reviceHandler.end()) {

                session->reviceHandler[Stream]->processFileData(Event);

            } else if (session->reviceHandler.find(Stream) == session->reviceHandler.end()) {

                std::lock_guard<std::mutex> lock(session->mutexs);
                
                // 创建新的文件传输处理器
                auto fileTransfer = std::make_unique<MsQuicFileTransfer>();
               
                // 添加回调函数...
                
                fileTransfer->setReviceDataHandler(session->reviceDataHandler);
                fileTransfer->setMsQuicSession(session);
                fileTransfer->setMsQuicStream(Stream);
                
                // 存储到映射中
                session->reviceHandler[Stream] = std::move(fileTransfer);
            }
            
        
        }
        break;

        // Add handler for QUIC_STREAM_EVENT_SEND_COMPLETE (type 2)
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
                    LogEvent("Error", std::string("SEND_COMPLETE EXCUTE: ") + e.what());
                }
            }
       
        }
        break;

        // Add handler for QUIC_STREAM_EVENT_PEER_SEND_ABORTED (type 6)
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        {
            std::stringstream ss;
            ss << "Peer aborted sending with error code: 0x" << std::hex
                << Event->PEER_SEND_ABORTED.ErrorCode << std::dec;
            LogEvent("Warning", ss.str());

        }
        break;

        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
            session->removeWriterFileSystem(Stream);
        }
        break;
        case QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE:
        {
            LogEvent("Info", "Stream send shutdown complete");
        }
        break;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            LogEvent("Info", "Stream shutdown complete");
            break;

        case QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE:
        {
            uint64_t idealSize = Event->IDEAL_SEND_BUFFER_SIZE.ByteCount;
            std::stringstream ss;
            ss << "MsQuic want buffer size: " << idealSize << " byte";
            LogEvent("Info", ss.str());
        }
        break;
       
        default:
        {
            std::stringstream ss;
            if (Event->Type < sizeof(eventNames) / sizeof(eventNames[0])) {
                ss << "Unhandled stream event: " << eventNames[Event->Type]
                    << " (" << Event->Type << ")";
            }
            else {
                ss << "Unknown stream event type: " << Event->Type;
            }
            LogEvent("Info", ss.str());
        }
        break;
        }
    }
    catch (const std::exception& e) {
        std::string errorMsg = "Exception in stream callback: " + std::string(e.what());
        LogEvent("Error", errorMsg);
    }
    catch (...) {
        LogEvent("Error", "Unknown exception in stream callback");
    }

    return QUIC_STATUS_SUCCESS;
}

void MsQuicSession::writerFile(std::function<void(HQUIC)> func) {

    HQUIC writerStream = CreateUnidirectionalStream();

    writerHandler[writerStream] = func;
        
}

void MsQuicSession::setReviceDataFunc(std::function<void(QUIC_STREAM_EVENT* revice)> func)
{
    this->reviceDataFunc = func;
}


void MsQuicSession::setReviceFileSystem(HQUIC stream)
{
    std::lock_guard<std::mutex> lock(mutexs);
    
    // 创建新的文件传输处理器
    auto fileTransfer = std::make_unique<MsQuicFileTransfer>();
    // 添加回调函数...
    
    fileTransfer->setReviceDataHandler(this->reviceDataHandler);

    fileTransfer->setMsQuicSession(this);

    fileTransfer->setMsQuicStream(stream);
    
    // 存储到映射中
    reviceHandler[stream] = std::move(fileTransfer);

    msQuic->SetCallbackHandler(
        stream,
        (void*)sessionStreamCallback,
        this);
}

void MsQuicSession::removeWriterFileSystem(HQUIC stream)
{
    std::lock_guard<std::mutex> lock(mutexs);
    
    // 从映射中移除
    auto it = writerHandler.find(stream);
    if (it != writerHandler.end()) {
        writerHandler.erase(it);
    }
}

// 通过 fileSystemStream 发送数据
bool MsQuicSession::writerFileSystem(const std::string& message,HQUIC stream)
{
    std::lock_guard<std::mutex> lock(mutexs);

    if (connect == nullptr)
    {
        LogEvent("Error", "MsQuicSession not connection invalid");
        return false;
    }

    if (stream == nullptr)
    {
        LogEvent("Error", "fileSystemStream is not available");
        return false;
    }

    try
    {
        // 分配消息缓冲区
        uint8_t* messageBuffer = new uint8_t[message.length()];
        memcpy(messageBuffer, message.c_str(), message.length());

        QUIC_BUFFER buffer;
        buffer.Buffer = messageBuffer;
        buffer.Length = (uint32_t)message.length();

        // 发送数据
        QUIC_STATUS status = msQuic->StreamSend(
            stream,
            &buffer,
            1,
            QUIC_SEND_FLAG_NONE,
            messageBuffer);

        if (QUIC_FAILED(status))
        {
            std::stringstream ss;
            ss << "Failed to send data through fileSystemStream: 0x" << std::hex << status;
            LogEvent("Error", ss.str());

            // 确保在失败时安全释放内存
            try
            {
                delete[] messageBuffer;
            }
            catch (...)
            {
                LogEvent("Error", "Exception while freeing memory in error path");
            }
            return false;
        }
    }
    catch (const std::exception& e)
    {
        LogEvent("Error", "Exception in WriterFileSystem: " + std::string(e.what()));
        return false;
    }

    return true;
}

bool MsQuicSession::WriterFileSystem(const uint8_t* data, size_t dataLength, HQUIC streams) {

    if (streams == nullptr) {
        LogEvent("Error", "流不可用");
        return false;
    }

    if (!isStreamValid(streams)) {
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

            QUIC_BUFFER* buffer = new QUIC_BUFFER;
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
            QUIC_STATUS status = msQuic->StreamSend(
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
            std::vector<QUIC_BUFFER>* buffers = new std::vector<QUIC_BUFFER>(numChunks);
            std::vector<uint8_t*>* bufferPointers = new std::vector<uint8_t*>(numChunks);
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
                    LogEvent("Error", "can't malloc aligned memory :" + std::to_string(i));
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
            QUIC_STATUS status = msQuic->StreamSend(
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


