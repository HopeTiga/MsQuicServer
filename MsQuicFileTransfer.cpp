#define NOMINMAX 
#include "MsQuicFileTransfer.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "MsQuicSession.h"
#include "Utils.h"

MsQuicFileTransfer::MsQuicFileTransfer()
{
    reset();
}

MsQuicFileTransfer::~MsQuicFileTransfer()
{
    if (fileStream.is_open()) {
        fileStream.close();
    }
}


bool MsQuicFileTransfer::processFileData(QUIC_STREAM_EVENT* streamEvent)
{
    if (streamEvent == nullptr || streamEvent->Type != QUIC_STREAM_EVENT_RECEIVE) {
        return false;
    }

    // 获取总数据长度
    uint64_t totalLength = streamEvent->RECEIVE.TotalBufferLength;

    std::stringstream ss;
    ss << "Received file data, total length: " << totalLength << " bytes";
    LogEvent("Info", ss.str());

    // 处理接收到的数据
    if (totalLength > 0 && streamEvent->RECEIVE.BufferCount > 0) {

        if (!headerReceived) {
            // 收集头部数据
            for (uint32_t i = 0; i < streamEvent->RECEIVE.BufferCount; ++i) {
                const QUIC_BUFFER* buffer = &streamEvent->RECEIVE.Buffers[i];
                if (buffer->Buffer != nullptr && buffer->Length > 0) {
                    // 将二进制数据添加到头部缓冲区
                    headerBuffer.insert(headerBuffer.end(),
                        buffer->Buffer,
                        buffer->Buffer + buffer->Length);
                }
            }
            
            // 尝试解析二进制头部
            if (headerBuffer.size() >= 20) { // 至少需要包含足够的数据来尝试解析
                
                // 解析二进制头部 - 使用与发送端相同的格式
                // 头部结构: 
                // 4字节: 文件名长度
                // N字节: 文件名
                // 4字节: 消息ID
                // 4字节: 元数据长度
                // N字节: 元数据JSON
                // 8字节: 文件大小
                
                size_t offset = 0;
                
                // 解析文件名长度 (4字节) - 考虑字节序问题
                uint32_t fileNameLen = 0;
                memcpy(&fileNameLen, headerBuffer.data() + offset, 4);
                
                // 检查文件名长度是否合理
                if (fileNameLen > 1024) {  // 假设文件名不会超过1KB
                    // 尝试字节序转换
                    // 尝试作为文本头部解析
                    std::string headerText(reinterpret_cast<char*>(headerBuffer.data()),
                        std::min(headerBuffer.size(), (size_t)200));

                    if (parseFileHeader(headerText)) {
                        // 文本头部解析成功
                        headerReceived = true;

                        // 处理后续逻辑
                        setupFileReception();

                        // 处理剩余数据
                        processRemainingData();

                        return true;
                    }
                    else {
                        LogEvent("Error", "Failed to parse as text header, invalid binary header");
                        reset();
                        return false;
                    }
                  
                }
                
                offset += 4;
                
                LogEvent("Debug", "Parsed filename length: " + std::to_string(fileNameLen));
                
                // 检查是否有足够的数据来解析文件名
                if (offset + fileNameLen > headerBuffer.size()) {
                    LogEvent("Debug", "Waiting for complete filename data... Need " + 
                             std::to_string(offset + fileNameLen) + " bytes, have " + 
                             std::to_string(headerBuffer.size()) + " bytes");
                    return true; // 等待更多数据
                }
                
                // 解析文件名
                fileName = std::string(reinterpret_cast<char*>(headerBuffer.data()) + offset, fileNameLen);
                offset += fileNameLen;
                
                // 检查是否有足够的数据来解析消息ID
                if (offset + 4 > headerBuffer.size()) {
                    LogEvent("Debug", "Waiting for message ID data...");
                    return true; // 等待更多数据
                }
                
                // 解析消息ID (4字节) - 考虑字节序问题
                uint32_t msgIdValue = 0;
                memcpy(&msgIdValue, headerBuffer.data() + offset, 4);
                
                fileMsgId = static_cast<short>(msgIdValue);
                offset += 4;
                
                // 检查是否有足够的数据来解析元数据长度
                if (offset + 4 > headerBuffer.size()) {
                    LogEvent("Debug", "Waiting for metadata length data...");
                    return true; // 等待更多数据
                }
                
                // 解析元数据长度 (4字节) - 考虑字节序问题
                uint32_t metadataLen = 0;
                memcpy(&metadataLen, headerBuffer.data() + offset, 4);
                
                offset += 4;
                
                // 检查是否有足够的数据来解析元数据
                if (offset + metadataLen > headerBuffer.size()) {
                    LogEvent("Debug", "Waiting for complete metadata... Need " + 
                             std::to_string(offset + metadataLen) + " bytes, have " + 
                             std::to_string(headerBuffer.size()) + " bytes");
                    return true; // 等待更多数据
                }
                
                // 解析元数据
                jsonMetadata = std::string(reinterpret_cast<char*>(headerBuffer.data()) + offset, metadataLen);
                offset += metadataLen;
                
                // 检查是否有足够的数据来解析文件大小
                if (offset + 8 > headerBuffer.size()) {
                    LogEvent("Debug", "Waiting for file size data...");
                    return true; // 等待更多数据
                }
                
                // 解析文件大小 (8字节) - 考虑字节序问题
                uint64_t fileSizeValue = 0;
                memcpy(&fileSizeValue, headerBuffer.data() + offset, 8);
                
                fileDataLength = static_cast<int64_t>(fileSizeValue);
                offset += 8;
                
                // 头部解析完成
                headerReceived = true;
                
                std::stringstream headerSs;
                headerSs << "Binary header parsed: fileName=\"" << fileName 
                        << "\", msgId=" << fileMsgId
                        << "\", jsonMetadata=\"" << jsonMetadata
                        << "\", dataLength=" << fileDataLength << " bytes";
                LogEvent("Info", headerSs.str());
                
                // 设置文件接收
                setupFileReception();
                
                // 处理剩余数据
                size_t headerSize = offset; // 头部数据的大小
                if (headerBuffer.size() > headerSize) {
                    // 有剩余数据需要处理
                    std::vector<uint8_t> initialData(headerBuffer.begin() + headerSize, headerBuffer.end());
                    
                    if (isLargeFile) {
                        // 大文件模式：直接写入文件
                        if (!initialData.empty() && fileStream.is_open()) {
                            fileStream.write(reinterpret_cast<const char*>(initialData.data()), initialData.size());
                            receivedDataSize += initialData.size();
                            
                            LogEvent("Debug", "Initial file data written: " + std::to_string(initialData.size()) + 
                                    " bytes, total: " + std::to_string(receivedDataSize) + " bytes");
                        }
                    } else {
                        // 小文件模式：存入内存缓冲区
                        dataBuffer.insert(dataBuffer.end(), initialData.begin(), initialData.end());
                        receivedDataSize = dataBuffer.size();
                        
                        LogEvent("Debug", "Initial file data buffered: " + std::to_string(initialData.size()) + 
                                " bytes, total: " + std::to_string(receivedDataSize) + " bytes");
                    }
                }
                
                // 清空头部缓冲区
                headerBuffer.clear();
                headerBuffer.shrink_to_fit();
                
                // 检查是否已接收完成
                if (receivedDataSize >= fileDataLength) {
                    completeFileReception();
                }
            }
        } else {
            // 头部已接收，处理文件数据
            for (uint32_t i = 0; i < streamEvent->RECEIVE.BufferCount; ++i) {
                const QUIC_BUFFER* buffer = &streamEvent->RECEIVE.Buffers[i];
                if (buffer->Buffer != nullptr && buffer->Length > 0) {
                    if (isLargeFile) {
                        // 大文件模式：直接写入文件
                        if (fileStream.is_open()) {
                            fileStream.write(reinterpret_cast<const char*>(buffer->Buffer), buffer->Length);
                            receivedDataSize += buffer->Length;
                        } else {
                            LogEvent("Error", "File stream not open when processing file content");
                        }
                    } else {
                        // 小文件模式：存入内存缓冲区
                        dataBuffer.insert(dataBuffer.end(), buffer->Buffer, buffer->Buffer + buffer->Length);
                        receivedDataSize = dataBuffer.size();
                    }
                    
                    // 记录进度
                    if (fileDataLength > 0 && receivedDataSize % (1024 * 1024) < buffer->Length) { // 每MB记录一次
                        double progress = (static_cast<double>(receivedDataSize) / fileDataLength) * 100.0;
                        LogEvent("Info", "File transfer progress: " + std::to_string(static_cast<int>(progress)) + "%");
                    }
                }
            }
            
            // 检查是否接收完成
            if (receivedDataSize >= fileDataLength) {
                completeFileReception();
            }
        }
    }
    
    return true;
}

// 设置文件接收
void MsQuicFileTransfer::setupFileReception() {
    // 如果文件数据很大，创建文件进行写入
    if (fileDataLength > 10 * 1024 * 1024) { // 10MB
        isLargeFile = true;
        
        // 构建文件路径
        std::string saveDir = "d:\\temp\\msquic_data\\";
        if (!createDirectory(saveDir)) {
            LogEvent("Error", "Failed to create directory: " + saveDir);
            return;
        }
        
        // 使用文件名创建文件
        filePath = saveDir + fileName;
        
        // 打开文件
        fileStream.open(filePath, std::ios::binary | std::ios::out);
        if (!fileStream.is_open()) {
            LogEvent("Error", "Failed to open file for writing: " + filePath);
            return;
        }
        
        LogEvent("Info", "Large file mode activated, writing to: " + filePath);
    }
}

// 处理剩余数据
void MsQuicFileTransfer::processRemainingData() {
    // 查找头部结束位置
    size_t headerEndPos = 0;
    
    // 查找第四个管道符号
    int pipeCount = 0;
    for (size_t i = 0; i < headerBuffer.size(); ++i) {
        if (headerBuffer[i] == '|') {
            pipeCount++;
            if (pipeCount == 4) {
                // 找到了头部结束位置
                headerEndPos = i + 1; // 包含最后一个管道符号
                break;
            }
        }
    }
    
    if (headerEndPos > 0 && headerEndPos < headerBuffer.size()) {
        std::vector<uint8_t> initialData(headerBuffer.begin() + headerEndPos, headerBuffer.end());
        
        if (isLargeFile) {
            // 大文件模式：直接写入文件
            if (!initialData.empty()) {
                fileStream.write(reinterpret_cast<const char*>(initialData.data()), initialData.size());
                receivedDataSize += initialData.size();
            }
        } else {
            // 小文件模式：存入内存缓冲区
            dataBuffer.insert(dataBuffer.end(), 
                initialData.begin(),
                initialData.end());
            receivedDataSize = dataBuffer.size();
        }
        
        LogEvent("Debug", "Initial data processed: " + std::to_string(receivedDataSize) + " bytes");
    }
    
    // 清空头部缓冲区
    headerBuffer.clear();
    headerBuffer.shrink_to_fit();
}

// 解析文件头信息
bool MsQuicFileTransfer::parseFileHeader(const std::string& headerStr) {
    LogEvent("Debug", "Parsing header: \"" + headerStr + "\"");
    
    // 查找分隔符
    size_t firstPipe = headerStr.find('|');
    if (firstPipe == std::string::npos) {
        LogEvent("Error", "First pipe not found in header");
        return false;
    }
    
    size_t secondPipe = headerStr.find('|', firstPipe + 1);
    if (secondPipe == std::string::npos) {
        LogEvent("Error", "Second pipe not found in header");
        return false;
    }
    
    size_t thirdPipe = headerStr.find('|', secondPipe + 1);
    if (thirdPipe == std::string::npos) {
        LogEvent("Error", "Third pipe not found in header");
        return false;
    }
    
    // 第四个管道符号是可选的，如果没有找到，使用字符串末尾
    size_t fourthPipe = headerStr.find('|', thirdPipe + 1);
    if (fourthPipe == std::string::npos) {
        fourthPipe = headerStr.length();
        LogEvent("Warning", "Fourth pipe not found, using end of string: " + std::to_string(fourthPipe));
    }
    
    // 提取文件名
    fileName = headerStr.substr(0, firstPipe);
    if (fileName.empty()) {
        LogEvent("Error", "Empty filename in header");
        return false;
    }
    
    // 提取消息ID
    std::string msgIdStr = headerStr.substr(firstPipe + 1, secondPipe - firstPipe - 1);
    try {
        // 检查是否全为数字
        if (msgIdStr.empty() || msgIdStr.find_first_not_of("0123456789") != std::string::npos) {
            LogEvent("Error", "Invalid message ID format: \"" + msgIdStr + "\"");
            return false;
        }
        
        fileMsgId = static_cast<short>(std::stoi(msgIdStr));
    }
    catch (const std::exception& e) {
        LogEvent("Error", "Failed to parse message ID: " + std::string(e.what()));
        return false;
    }
    
    // 提取JSON元数据
    jsonMetadata = headerStr.substr(secondPipe + 1, thirdPipe - secondPipe - 1);
    
    // 提取数据大小
    std::string dataSizeStr;
    if (fourthPipe > thirdPipe + 1) {
        dataSizeStr = headerStr.substr(thirdPipe + 1, fourthPipe - thirdPipe - 1);
    }
    
    if (dataSizeStr.empty()) {
        LogEvent("Error", "Empty data size in header");
        return false;
    }
    
    try {
        // 移除可能的非数字字符
        dataSizeStr.erase(std::remove_if(dataSizeStr.begin(), dataSizeStr.end(), 
                                        [](unsigned char c) { return !std::isdigit(c); }),
                         dataSizeStr.end());
                         
        if (dataSizeStr.empty()) {
            LogEvent("Error", "No digits found in data size");
            return false;
        }
        
        fileDataLength = std::stoll(dataSizeStr);
    }
    catch (const std::exception& e) {
        LogEvent("Error", "Failed to parse dataSize: " + std::string(e.what()) + ", raw: \"" + dataSizeStr + "\"");
        return false;
    }
    
    std::stringstream headerSs;
    headerSs << "File header parsed: fileName=\"" << fileName 
            << "\", msgId=" << fileMsgId
            << ", jsonMetadata=\"" << jsonMetadata
            << "\", dataLength=" << fileDataLength << " bytes";
    LogEvent("Info", headerSs.str());
    
    return true;
}

// 新增方法：完成文件接收
bool MsQuicFileTransfer::completeFileReception() {
    if (isLargeFile) {
        // 关闭文件
        fileStream.close();
        LogEvent("Info", "Large file completed: " + filePath);
    }
    else {
        // 从内存中保存文件
        std::string saveDir = "d:\\temp\\msquic_data\\";
        if (!createDirectory(saveDir)) {
            return false;
        }
        
        filePath = saveDir + fileName;
        std::ofstream outFile(filePath, std::ios::binary);
        if (outFile.is_open()) {
            outFile.write(dataBuffer.data(), dataBuffer.size());
            outFile.close();
        }
        else {
            LogEvent("Error", "Failed to open file for writing: " + filePath);
            return false;
        }
    }
    
    std::stringstream dataSs;
    dataSs << "File reception complete: " << fileName <<", msgId: " << fileMsgId
          << ", size: " << fileDataLength << " bytes"
          << ", metadata: " << jsonMetadata;
    LogEvent("Info", dataSs.str());
    
    // 调用回调函数
    if (reviceDataHandler.find(fileMsgId) != reviceDataHandler.end()) {
        reviceDataHandler[fileMsgId](this->session,this);
    } else {
        LogEvent("Warning", "No handler found for message ID: " + std::to_string(fileMsgId));
    }

    // 标记文件接收完成
    fileCompleted = true;

    if (this->stream != nullptr && this->session != nullptr) {
    
        this->session->msQuic->StreamClose(this->stream);

        this->session->reviceHandler.erase(this->stream);

    }

    // 清理资源
    headerBuffer.clear();
    headerBuffer.shrink_to_fit(); // 释放未使用的内存
    dataBuffer.clear();
    dataBuffer.shrink_to_fit(); // 释放未使用的内存
    
    return true;
}

void MsQuicFileTransfer::reset()
{
    headerBuffer.clear();
    dataBuffer.clear();
    headerReceived = false;
    fileCompleted = false;
    isLargeFile = false;
    receivedDataSize = 0;
    fileNameLength = 0;
    jsonLength = 0;
    fileMsgId = 0;
    fileDataLength = 0;
    expectedTotalLength = 0;
    fileName = "";
    jsonMetadata = "";
    filePath = "";
    tempFilePath = "";
    
    if (fileStream.is_open()) {
        fileStream.close();
    }
}

bool MsQuicFileTransfer::isComplete() const
{
    return fileCompleted;
}

void MsQuicFileTransfer::setReviceDataHandler(std::map<short, std::function<void(MsQuicSession* sssion, MsQuicFileTransfer* transfer)>> reviceDataHandler)
{
    this->reviceDataHandler = reviceDataHandler;
}

void MsQuicFileTransfer::setMsQuicSession(MsQuicSession* session)
{
    this->session = session;
}

void MsQuicFileTransfer::setMsQuicStream(HQUIC stream)
{
    this->stream = stream;
}

bool MsQuicFileTransfer::createDirectory(const std::string& path)
{
    try {
        std::filesystem::create_directories(path);
        return true;
    }
    catch (const std::exception& e) {
        std::stringstream ss;
        ss << "Failed to create directory: " << path << ", error: " << e.what();
        LogEvent("Error", ss.str());
        return false;
    }
}