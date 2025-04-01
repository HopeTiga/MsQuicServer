#define NOMINMAX 
#pragma once
#include "Utils.h"
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <functional>



void LogEvent(const std::string& level, const std::string& message)
{
    // 获取当前时间
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
    localtime_s(&now_tm, &now_time_t);
    
    std::stringstream timestamp;
    timestamp << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S");
    
    
    // 输出带时间戳的日志
    if (level == "Error") {
        std::cerr << "[" << timestamp.str() << "] [ERROR] " << message << std::endl;
    } else {
        std::cout << "[" << timestamp.str() << "] [" << level << "] " << message << std::endl;
    }
    
    // 可以添加日志文件写入功能
    // ...
}

std::function<void(MsQuicClient*, HQUIC)> func = [=](MsQuicClient* client, HQUIC stream) {
    try {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        LogEvent("Info", "Start sending file");

        // 文件信息
        std::string fileName = "test.zip";
        int msgId = 1001;
        std::string jsonMetadata = "{}";
        // 使用更合理的数据大小
        size_t dataSize = 5ULL * 1024 * 1024 * 1024; // 1GB，可根据实际需求调整

        // 优化1: 使用结构体定义头部格式，提高可读性和可维护性
#pragma pack(push, 1)
        struct FileHeader {
            uint32_t fileNameLength;
            uint8_t fileName[256]; // 改为uint8_t数组
            uint32_t msgId;
            uint32_t metadataLength;
            uint8_t metadata[1024]; // 改为uint8_t数组
            uint64_t fileSize;
        };
#pragma pack(pop)

        // 优化2: 使用结构体直接构建头部，避免手动计算偏移量
        FileHeader header = { 0 };
        header.fileNameLength = static_cast<uint32_t>(fileName.size());
        // 使用memcpy将字符串数据复制到uint8_t数组
        memcpy(header.fileName, fileName.c_str(), fileName.size());
        header.msgId = static_cast<uint32_t>(msgId);
        header.metadataLength = static_cast<uint32_t>(jsonMetadata.size());
        memcpy(header.metadata, jsonMetadata.c_str(), jsonMetadata.size());
        header.fileSize = static_cast<uint64_t>(dataSize);

        // 计算实际头部大小
        size_t actualHeaderSize = sizeof(uint32_t) + header.fileNameLength +
            sizeof(uint32_t) + sizeof(uint32_t) +
            header.metadataLength + sizeof(uint64_t);

        // 打印头部信息摘要
        std::stringstream headerInfo;
        headerInfo << "Preparing to send file header: size=" << actualHeaderSize
            << " bytes, filename=" << fileName
            << ", messageID=" << msgId
            << ", data size=" << dataSize;
        LogEvent("Info", headerInfo.str());

        // 直接使用结构体指针发送数据，但只发送实际需要的字节数
        // 注意：这种方法要求结构体内存布局与网络协议完全匹配
        // 创建一个对齐的缓冲区
        std::vector<uint8_t> headerBuffer(actualHeaderSize);
        size_t offset = 0;

        // 手动填充缓冲区，确保内存对齐
        // 1. 文件名长度
        memcpy(headerBuffer.data() + offset, &header.fileNameLength, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        // 2. 文件名内容
        memcpy(headerBuffer.data() + offset, header.fileName, header.fileNameLength);
        offset += header.fileNameLength;

        // 3. 消息ID
        memcpy(headerBuffer.data() + offset, &header.msgId, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        // 4. 元数据长度
        memcpy(headerBuffer.data() + offset, &header.metadataLength, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        // 5. 元数据内容
        memcpy(headerBuffer.data() + offset, header.metadata, header.metadataLength);
        offset += header.metadataLength;

        // 6. 文件大小
        memcpy(headerBuffer.data() + offset, &header.fileSize, sizeof(uint64_t));

        // 发送对齐的缓冲区数据
        if (!client->WriterFileSystem(headerBuffer.data(), headerBuffer.size(), stream)) {
            LogEvent("Error", "Failed to send file header");
            return;
        }

        LogEvent("Info", "File header sent successfully, size: " + std::to_string(actualHeaderSize) + " bytes");

        // 重写数据发送逻辑，简化并提高效率
        const size_t chunkSize = 256 * 1024; // 使用256KB的块大小
        size_t totalSent = 0;

        // 分配一个固定大小的缓冲区
        std::unique_ptr<uint8_t[]> buffer(new uint8_t[chunkSize]);

        // 填充随机数据用于测试
        for (size_t i = 0; i < chunkSize; i++) {
            buffer[i] = static_cast<uint8_t>(i % 256);
        }

        // 性能统计变量
        auto startTime = std::chrono::high_resolution_clock::now();
        size_t lastReportedBytes = 0;
        auto lastReportTime = startTime;

        // 简化的发送循环
        while (totalSent < dataSize) {
            // 定期检查连接状态
            if (totalSent % (chunkSize * 5) == 0) {
                if (!client->IsConnected()) {
                    LogEvent("Error", "Connection lost, transmission aborted at: " + std::to_string(totalSent) + " bytes");
                    break;
                }
            }

            // 计算当前块的大小
            size_t currentChunkSize = std::min(chunkSize, dataSize - totalSent);

            // 发送数据块，最多重试3次
            bool sendSuccess = false;
            for (int retry = 0; retry < 3 && !sendSuccess; retry++) {
                if (client->WriterFileSystem(buffer.get(), currentChunkSize, stream)) {
                    sendSuccess = true;
                }
                else {
                    if (retry < 2) {
                        LogEvent("Warning", "Failed to send data chunk, retrying... (" + std::to_string(retry + 1) + "/3)");
                        std::this_thread::sleep_for(std::chrono::milliseconds(100 * (retry + 1)));
                    }
                }
            }

            if (!sendSuccess) {
                LogEvent("Error", "Failed to send data chunk after multiple retries at position: " + std::to_string(totalSent));
                break;
            }

            // 更新已发送字节数
            totalSent += currentChunkSize;

            // 定期报告进度
            auto currentTime = std::chrono::high_resolution_clock::now();
            auto elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastReportTime).count();

            if (totalSent % (chunkSize * 20) == 0 || elapsedSeconds >= 3 || totalSent == dataSize) {
                // 计算进度和速度
                double percentComplete = (static_cast<double>(totalSent) / dataSize) * 100.0;
                double speedMBps = 0.0;

                if (elapsedSeconds > 0) {
                    size_t bytesSinceLastReport = totalSent - lastReportedBytes;
                    speedMBps = (bytesSinceLastReport / (1024.0 * 1024.0)) / elapsedSeconds;
                }

                // 输出进度信息
                std::stringstream ss;
                ss.str("");
                ss.clear();
                ss << "Progress: " << std::fixed << std::setprecision(2) << percentComplete
                    << "% (" << totalSent / (1024 * 1024) << "/" << dataSize / (1024 * 1024) << " MB), "
                    << "Speed: " << std::fixed << std::setprecision(2) << speedMBps << " MB/s";
                LogEvent("Info", ss.str());

                // 更新统计数据
                lastReportedBytes = totalSent;
                lastReportTime = currentTime;
            }

            // 短暂延迟，避免占用过多CPU
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // 计算总传输时间和平均速度
        auto endTime = std::chrono::high_resolution_clock::now();
        auto totalTimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();
        double avgSpeedMBps = (totalSent / (1024.0 * 1024.0)) / std::max(1.0, static_cast<double>(totalTimeSeconds));

        std::stringstream summary;
        summary << "File transfer completed: " << fileName
            << " (" << totalSent / (1024 * 1024) << "/" << dataSize / (1024 * 1024) << " MB), "
            << "Total time: " << totalTimeSeconds << " seconds, "
            << "Average speed: " << std::fixed << std::setprecision(2) << avgSpeedMBps << " MB/s";
        LogEvent("Info", summary.str());
    }
    catch (const std::exception& e) {
        LogEvent("Error", "File transfer exception: " + std::string(e.what()));
    }
    catch (...) {
        LogEvent("Error", "Unknown exception during file transfer");
    }
};