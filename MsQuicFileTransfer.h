#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <functional>
#include <map>
#include <filesystem>
#include <msquic.h>

class MsQuicSession;

/**
 * @class MsQuicFileTransfer
 * @brief 负责通过QUIC协议传输文件的类
 * 
 * 该类处理文件的接收、解析和存储，支持大文件和小文件两种模式。
 * 大文件直接写入磁盘，小文件先存储在内存中。
 */
class MsQuicFileTransfer
{
public:
    /**
     * @brief 构造函数，初始化文件传输对象
     */
    MsQuicFileTransfer();
    
    /**
     * @brief 析构函数，清理资源
     */
    ~MsQuicFileTransfer();

    /**
     * @brief 处理从QUIC流接收的文件数据
     * @param streamEvent QUIC流事件，包含接收到的数据
     * @return 处理是否成功
     */
    bool processFileData(QUIC_STREAM_EVENT* streamEvent);
    
    /**
     * @brief 重置文件传输状态，准备接收新文件
     */
    void reset();
    
    /**
     * @brief 检查文件是否接收完成
     * @return 文件是否接收完成
     */
    bool isComplete() const;
    
    /**
     * @brief 设置文件接收完成后的回调处理函数
     * @param reviceDataHandler 回调函数映射，键为消息ID，值为回调函数
     */
    void setReviceDataHandler(std::map<short, std::function<void(MsQuicSession* session, MsQuicFileTransfer* transfer)>> reviceDataHandler);
    
    /**
     * @brief 设置关联的MsQuicSession对象
     * @param session MsQuicSession指针
     */
    void setMsQuicSession(MsQuicSession* session);
    
    /**
     * @brief 设置关联的QUIC流
     * @param stream QUIC流句柄
     */
    void setMsQuicStream(HQUIC stream);

private:
    /**
     * @brief 解析文件头信息
     * @param headerStr 包含文件头信息的字符串
     * @return 解析是否成功
     */
    bool parseFileHeader(const std::string& headerStr);
    
    /**
     * @brief 设置文件接收环境，根据文件大小决定存储方式
     */
    void setupFileReception();
    
    /**
     * @brief 处理头部解析后剩余的数据
     */
    void processRemainingData();
    
    /**
     * @brief 完成文件接收，关闭文件并调用回调
     * @return 操作是否成功
     */
    bool completeFileReception();
    
    /**
     * @brief 创建目录，确保文件保存路径存在
     * @param path 目录路径
     * @return 创建是否成功
     */
    bool createDirectory(const std::string& path);
    

private:
    // === 缓冲区 ===
    std::vector<uint8_t> headerBuffer;  ///< 存储文件头部数据的缓冲区
    std::vector<char> dataBuffer;       ///< 存储小文件数据的内存缓冲区
    std::ofstream fileStream;           ///< 用于大文件直接写入磁盘的文件流
    
    // === 状态标志 ===
    bool headerReceived;                ///< 文件头部是否已接收
    bool fileCompleted;                 ///< 文件是否接收完成
    bool isLargeFile;                   ///< 是否为大文件模式
    
    // === 文件信息 ===
    uint64_t receivedDataSize;          ///< 已接收的数据大小
    uint32_t fileNameLength;            ///< 文件名长度
    uint32_t jsonLength;                ///< JSON元数据长度
    short fileMsgId;                    ///< 文件消息ID
    int64_t fileDataLength;             ///< 文件数据总长度
    uint64_t expectedTotalLength;       ///< 预期总长度
    
    // === 文件元数据 ===
    std::string fileName;               ///< 文件名
    std::string jsonMetadata;           ///< JSON格式的元数据
    std::string filePath;               ///< 文件保存路径
    std::string tempFilePath;           ///< 临时文件路径
    
    // === 回调处理 ===
    std::map<short, std::function<void(MsQuicSession* session, MsQuicFileTransfer* transfer)>> reviceDataHandler;  ///< 文件接收完成回调函数映射
    
    // === MsQuic相关 ===
    MsQuicSession* session;             ///< 关联的MsQuicSession对象
    HQUIC stream;                       ///< 关联的QUIC流句柄
};