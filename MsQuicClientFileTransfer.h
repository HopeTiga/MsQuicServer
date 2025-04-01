#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <functional>
#include <map>
#include <filesystem>
#include <msquic.h>

class MsQuicClient;

class MsQuicClientFileTransfer
{
public:
    MsQuicClientFileTransfer();
    ~MsQuicClientFileTransfer();

    // 处理文件数据
    bool processFileData(QUIC_STREAM_EVENT* streamEvent);
    
    // 重置状态
    void reset();
    
    // 检查文件是否接收完成
    bool isComplete() const;
    
    // 设置接收数据处理回调
    void setReviceDataHandler(std::map<short, std::function<void(MsQuicClient* client, MsQuicClientFileTransfer* transfer)>> reviceDataHandler);
    
    // 设置MsQuic客户端
    void setMsQuicClient(MsQuicClient* client);
    
    // 设置MsQuic流
    void setMsQuicStream(HQUIC stream);
    
    // 获取文件名
    std::string getFileName() const { return fileName; }
    
    // 获取文件路径
    std::string getFilePath() const { return filePath; }
    
    // 获取JSON元数据
    std::string getJsonMetadata() const { return jsonMetadata; }
    
    // 获取文件消息ID
    short getFileMsgId() const { return fileMsgId; }

private:
    // 解析文件头信息
    bool parseFileHeader(const std::string& headerStr);
    
    // 设置文件接收
    void setupFileReception();
    
    // 处理剩余数据
    void processRemainingData();
    
    // 完成文件接收
    bool completeFileReception();
    
    // 创建目录
    bool createDirectory(const std::string& path);

private:
    // 头部缓冲区
    std::vector<uint8_t> headerBuffer;
    
    // 数据缓冲区（用于小文件）
    std::vector<char> dataBuffer;
    
    // 文件流（用于大文件）
    std::ofstream fileStream;
    
    // 状态标志
    bool headerReceived;
    bool fileCompleted;
    bool isLargeFile;
    
    // 文件信息
    uint64_t receivedDataSize;
    uint32_t fileNameLength;
    uint32_t jsonLength;
    short fileMsgId;
    int64_t fileDataLength;
    uint64_t expectedTotalLength;
    
    // 文件元数据
    std::string fileName;
    std::string jsonMetadata;
    std::string filePath;
    std::string tempFilePath;
    
    // 回调处理
    std::map<short, std::function<void(MsQuicClient* client, MsQuicClientFileTransfer* transfer)>> reviceDataHandler;
    
    // MsQuic相关
    MsQuicClient* client;
    HQUIC stream;
};