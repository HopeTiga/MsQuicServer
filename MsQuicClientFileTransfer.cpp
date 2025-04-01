#include "MsQuicClientFileTransfer.h"
#include "MsQuicClient.h"
#include "Utils.h"
#include <sstream>
#include <iomanip>

MsQuicClientFileTransfer::MsQuicClientFileTransfer() :
    headerReceived(false),
    fileCompleted(false),
    isLargeFile(false),
    receivedDataSize(0),
    fileNameLength(0),
    jsonLength(0),
    fileMsgId(0),
    fileDataLength(0),
    expectedTotalLength(0),
    client(nullptr),
    stream(nullptr)
{
    // 初始化头部缓冲区
    headerBuffer.reserve(1024);
}

MsQuicClientFileTransfer::~MsQuicClientFileTransfer()
{
    // 关闭文件流
    if (fileStream.is_open()) {
        fileStream.close();
    }
}

bool MsQuicClientFileTransfer::processFileData(QUIC_STREAM_EVENT* streamEvent)
{
    if (!streamEvent) {
        LogEvent("Error", "无效的流事件指针");
        return false;
    }

    // 处理接收事件
    if (streamEvent->Type == QUIC_STREAM_EVENT_RECEIVE) {
        const QUIC_BUFFER* buffers = streamEvent->RECEIVE.Buffers;
        const uint32_t bufferCount = streamEvent->RECEIVE.BufferCount;

        // 处理所有缓冲区
        for (uint32_t i = 0; i < bufferCount; ++i) {
            const uint8_t* data = buffers[i].Buffer;
            const uint32_t dataLength = buffers[i].Length;

            if (!headerReceived) {
                // 继续收集头部数据
                headerBuffer.insert(headerBuffer.end(), data, data + dataLength);
                
                // 尝试解析头部
                std::string headerStr(reinterpret_cast<const char*>(headerBuffer.data()), headerBuffer.size());
                if (parseFileHeader(headerStr)) {
                    // 头部解析成功，设置文件接收
                    setupFileReception();
                    
                    // 处理剩余数据
                    processRemainingData();
                }
            } else {
                // 头部已接收，直接处理文件数据
                if (isLargeFile) {
                    // 大文件写入文件流
                    fileStream.write(reinterpret_cast<const char*>(data), dataLength);
                } else {
                    // 小文件存入内存
                    dataBuffer.insert(dataBuffer.end(), 
                                     reinterpret_cast<const char*>(data),
                                     reinterpret_cast<const char*>(data) + dataLength);
                }
                
                // 更新接收大小
                receivedDataSize += dataLength;
                
                // 检查是否接收完成
                if (receivedDataSize >= fileDataLength) {
                    return completeFileReception();
                }
            }
        }
    }
    
    return false;
}

void MsQuicClientFileTransfer::reset()
{
    // 重置所有状态
    headerBuffer.clear();
    dataBuffer.clear();
    
    if (fileStream.is_open()) {
        fileStream.close();
    }
    
    headerReceived = false;
    fileCompleted = false;
    isLargeFile = false;
    receivedDataSize = 0;
    fileNameLength = 0;
    jsonLength = 0;
    fileMsgId = 0;
    fileDataLength = 0;
    expectedTotalLength = 0;
    fileName.clear();
    jsonMetadata.clear();
    filePath.clear();
    tempFilePath.clear();
}

bool MsQuicClientFileTransfer::isComplete() const
{
    return fileCompleted;
}

void MsQuicClientFileTransfer::setReviceDataHandler(
    std::map<short, std::function<void(MsQuicClient* client, MsQuicClientFileTransfer* transfer)>> handler)
{
    this->reviceDataHandler = handler;
}

void MsQuicClientFileTransfer::setMsQuicClient(MsQuicClient* client)
{
    this->client = client;
}

void MsQuicClientFileTransfer::setMsQuicStream(HQUIC stream)
{
    this->stream = stream;
}

bool MsQuicClientFileTransfer::parseFileHeader(const std::string& headerStr)
{
    // 查找头部结束标记
    size_t endPos = headerStr.find("\r\n\r\n");
    if (endPos == std::string::npos) {
        // 头部不完整
        return false;
    }
    
    // 提取头部信息
    std::string header = headerStr.substr(0, endPos);
    
    // 解析头部字段
    std::istringstream iss(header);
    std::string line;
    
    while (std::getline(iss, line)) {
        if (line.empty() || line == "\r") continue;
        
        // 移除可能的\r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        size_t colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string key = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);
            
            // 去除前导空格
            while (!value.empty() && value.front() == ' ') {
                value.erase(0, 1);
            }
            
            if (key == "File-Name-Length") {
                fileNameLength = std::stoul(value);
            } else if (key == "JSON-Length") {
                jsonLength = std::stoul(value);
            } else if (key == "File-Length") {
                fileDataLength = std::stoll(value);
            } else if (key == "Msg-ID") {
                fileMsgId = static_cast<short>(std::stoi(value));
            }
        }
    }
    
    // 计算总长度
    expectedTotalLength = endPos + 4 + fileNameLength + jsonLength + fileDataLength;
    
    // 提取文件名和JSON
    size_t currentPos = endPos + 4;
    if (headerBuffer.size() >= currentPos + fileNameLength) {
        fileName = std::string(
            reinterpret_cast<const char*>(headerBuffer.data() + currentPos),
            fileNameLength);
        currentPos += fileNameLength;
        
        if (headerBuffer.size() >= currentPos + jsonLength) {
            jsonMetadata = std::string(
                reinterpret_cast<const char*>(headerBuffer.data() + currentPos),
                jsonLength);
            
            // 头部解析完成
            headerReceived = true;
            return true;
        }
    }
    
    return false;
}

void MsQuicClientFileTransfer::setupFileReception()
{
    // 确定文件保存路径
    std::string saveDir = "downloads";
    createDirectory(saveDir);
    
    filePath = saveDir + "\\" + fileName;
    tempFilePath = filePath + ".tmp";
    
    // 判断是否为大文件（超过10MB）
    isLargeFile = (fileDataLength > 10 * 1024 * 1024);
    
    if (isLargeFile) {
        // 大文件直接写入磁盘
        fileStream.open(tempFilePath, std::ios::binary);
        if (!fileStream.is_open()) {
            LogEvent("Error", "无法创建文件: " + tempFilePath);
        }
    } else {
        // 小文件先存入内存
        dataBuffer.reserve(static_cast<size_t>(fileDataLength));
    }
}

void MsQuicClientFileTransfer::processRemainingData()
{
    // 计算头部结束位置
    size_t headerEndPos = 0;
    std::string headerStr(reinterpret_cast<const char*>(headerBuffer.data()), headerBuffer.size());
    size_t endPos = headerStr.find("\r\n\r\n");
    if (endPos != std::string::npos) {
        headerEndPos = endPos + 4;
    }
    
    // 计算文件数据开始位置
    size_t fileDataStartPos = headerEndPos + fileNameLength + jsonLength;
    
    // 处理已接收的文件数据
    if (headerBuffer.size() > fileDataStartPos) {
        size_t dataSize = headerBuffer.size() - fileDataStartPos;
        
        if (isLargeFile) {
            // 大文件写入文件流
            fileStream.write(
                reinterpret_cast<const char*>(headerBuffer.data() + fileDataStartPos),
                dataSize);
        } else {
            // 小文件存入内存
            dataBuffer.insert(
                dataBuffer.end(),
                headerBuffer.begin() + fileDataStartPos,
                headerBuffer.end());
        }
        
        // 更新接收大小
        receivedDataSize += dataSize;
        
        // 检查是否接收完成
        if (receivedDataSize >= fileDataLength) {
            completeFileReception();
        }
    }
    
    // 清空头部缓冲区，释放内存
    headerBuffer.clear();
    headerBuffer.shrink_to_fit();
}

bool MsQuicClientFileTransfer::completeFileReception()
{
    if (isLargeFile) {
        // 关闭文件流
        if (fileStream.is_open()) {
            fileStream.close();
        }
        
        // 重命名临时文件
        std::error_code ec;
        std::filesystem::rename(tempFilePath, filePath, ec);
        if (ec) {
            LogEvent("Error", "重命名文件失败: " + ec.message());
            return false;
        }
    } else {
        // 小文件从内存写入磁盘
        std::ofstream outFile(filePath, std::ios::binary);
        if (!outFile.is_open()) {
            LogEvent("Error", "无法创建文件: " + filePath);
            return false;
        }
        
        outFile.write(dataBuffer.data(), dataBuffer.size());
        outFile.close();
    }
    
    // 标记完成
    fileCompleted = true;
    
    // 调用回调函数
    if (reviceDataHandler.find(fileMsgId) != reviceDataHandler.end()) {
        reviceDataHandler[fileMsgId](client, this);
    }

    if (this->client != nullptr && this->stream != nullptr) {

        this->client->msQuic_->StreamClose(this->stream);

        this->client->reviceHandler.erase(this->stream);

    }

    LogEvent("Info", "文件接收完成: " + fileName);
    return true;
}

bool MsQuicClientFileTransfer::createDirectory(const std::string& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (!std::filesystem::create_directories(path, ec)) {
            LogEvent("Error", "创建目录失败: " + path + ", 错误: " + ec.message());
            return false;
        }
    }
    return true;
}