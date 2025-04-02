#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WIN32_WINNT 0x0600  // 至少需要 Windows Vista
#include <winsock2.h>
#include <ws2ipdef.h>
#include "SharedMemoryIPC.h"
#include <iostream>

// 共享内存结构
struct SharedMemoryBuffer {
    uint32_t messageSize;
    char data[1]; // 可变长度数组
};

SharedMemoryIPC::SharedMemoryIPC(const std::string& name, bool isServer, size_t size)
    : name_(name), isServer_(isServer), size_(size), fileMapping_(NULL), mappedView_(NULL),
      readEvent_(NULL), writeEvent_(NULL), running_(false)
{
}

SharedMemoryIPC::~SharedMemoryIPC()
{
    stopListening();
    
    if (mappedView_) {
        UnmapViewOfFile(mappedView_);
        mappedView_ = NULL;
    }
    
    if (fileMapping_) {
        CloseHandle(fileMapping_);
        fileMapping_ = NULL;
    }
    
    if (readEvent_) {
        CloseHandle(readEvent_);
        readEvent_ = NULL;
    }
    
    if (writeEvent_) {
        CloseHandle(writeEvent_);
        writeEvent_ = NULL;
    }
}

// 在 initialize 方法中
bool SharedMemoryIPC::initialize()
{
    // Create event objects
    std::string readEventName = name_ + "_ReadEvent";
    std::string writeEventName = name_ + "_WriteEvent";
    
    if (isServer_) {
        // Server creates events
        readEvent_ = CreateEventA(NULL, FALSE, FALSE, readEventName.c_str());
        writeEvent_ = CreateEventA(NULL, FALSE, FALSE, writeEventName.c_str());
    } else {
        // Client opens events
        writeEvent_ = OpenEventA(EVENT_ALL_ACCESS, FALSE, readEventName.c_str());
        readEvent_ = OpenEventA(EVENT_ALL_ACCESS, FALSE, writeEventName.c_str());
    }
    
    if (!readEvent_ || !writeEvent_) {
        LogEvent("Error", "Failed to create or open event objects");
        return false;
    }
    
    // Create or open file mapping
    if (isServer_) {
        fileMapping_ = CreateFileMappingA(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(size_),
            name_.c_str());
    } else {
        fileMapping_ = OpenFileMappingA(
            FILE_MAP_ALL_ACCESS,
            FALSE,
            name_.c_str());
    }
    
    if (!fileMapping_) {
        LogEvent("Error", "Failed to create or open file mapping");
        return false;
    }
    
    // Map view
    mappedView_ = MapViewOfFile(
        fileMapping_,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        size_);
    
    if (!mappedView_) {
        LogEvent("Error", "Failed to map view of file");
        return false;
    }
    
    // Initialize shared memory
    if (isServer_) {
        SharedMemoryBuffer* buffer = static_cast<SharedMemoryBuffer*>(mappedView_);
        buffer->messageSize = 0;
    }
    
    return true;
}

// 在 write 方法中
bool SharedMemoryIPC::write(const std::string& message)
{
    if (!mappedView_) {
        LogEvent("Error", "Shared memory not initialized");
        return false;
    }
    
    if (message.size() > size_ - sizeof(uint32_t)) {
        LogEvent("Error", "Message too large to write to shared memory");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Write message
    SharedMemoryBuffer* buffer = static_cast<SharedMemoryBuffer*>(mappedView_);
    buffer->messageSize = static_cast<uint32_t>(message.size());
    memcpy(buffer->data, message.c_str(), message.size());
    
    // Notify other process
    if (!SetEvent(writeEvent_)) {
        LogEvent("Error", "Failed to set event");
        return false;
    }
    
    return true;
}

// 在 listenThreadFunc 方法中
void SharedMemoryIPC::listenThreadFunc()
{
    while (running_) {
        // Wait for event
        DWORD waitResult = WaitForSingleObject(readEvent_, 100);
        
        if (waitResult == WAIT_OBJECT_0) {
            std::lock_guard<std::mutex> lock(mutex_);
            
            // Read message
            SharedMemoryBuffer* buffer = static_cast<SharedMemoryBuffer*>(mappedView_);
            uint32_t messageSize = buffer->messageSize;
            
            if (messageSize > 0 && messageSize <= size_ - sizeof(uint32_t)) {
                std::string message(buffer->data, messageSize);
                
                // Call callback function
                if (messageCallback_) {
                    messageCallback_(message);
                }
            }
        }
        else if (waitResult != WAIT_TIMEOUT) {
            LogEvent("Error", "Failed to wait for event");
            break;
        }
    }
}

bool SharedMemoryIPC::startListening(std::function<void(const std::string&)> callback)
{
    if (running_) {
        return true;
    }
    
    if (!mappedView_) {
        LogEvent("Error", "Shared memory not initialized");
        return false;
    }
    
    messageCallback_ = callback;
    running_ = true;
    listenThread_ = std::thread(&SharedMemoryIPC::listenThreadFunc, this);
    
    return true;
}

void SharedMemoryIPC::stopListening()
{
    running_ = false;
    
    if (listenThread_.joinable()) {
        listenThread_.join();
    }
}