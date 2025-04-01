#define QUIC_API_ENABLE_PREVIEW_FEATURES 1
#define NOMINMAX 
#include "MsQuicServer.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iomanip>
#include <algorithm>
#include <sstream> // 添加这行以支持std::stringstream
#include <chrono>
#include "MsQuicSession.h"
#include "MsQuicThreadTaskPool.h"
#include "Utils.h"
#pragma comment(lib, "Ws2_32.lib")

// Define supported QUIC versions
#define QUIC_VERSION_2          0x6b3343cfU     // Second official version (host byte order)
#define QUIC_VERSION_1          0x00000001U     // First official version (host byte order)

// Constants definition
const uint32_t SupportedVersions[] = { QUIC_VERSION_1, QUIC_VERSION_2 };
const MsQuicVersionSettings VersionSettings(SupportedVersions, 2);



// Connection context structure definition
struct ConnectionContext {
    HQUIC connection = nullptr;
    std::string clientInfo;
};

// Constructor
MsQuicServer::MsQuicServer(const MsQuicApi* msQuic) :
    msQuic_(msQuic),
    registration_(nullptr),
    configuration_(nullptr), 
    listener_(nullptr),
    initialized_(false),
    port_(4433),
    alpn_("quic") {
}

// Destructor
MsQuicServer::~MsQuicServer() {

}

// Initialize server
// 修改 Initialize 方法中的输出
bool MsQuicServer::Initialize(uint16_t port, const std::string& alpn) {
    try {
        port_ = port;
        alpn_ = alpn;

        // Check if MsQuicApi is valid
        if (msQuic_ == nullptr) {
            LogEvent("Error", "MsQuicApi pointer is null");
            return false;
        }

        if (QUIC_FAILED(msQuic_->GetInitStatus())) {
            std::stringstream ss;
            ss << "MsQuicApi initialization failed: 0x" << std::hex << msQuic_->GetInitStatus() << std::dec;
            LogEvent("Error", ss.str());
            return false;
        }

        // Create registration
        registration_ = new MsQuicRegistration("MsQuicServer");
        if (!registration_->IsValid()) {
            LogEvent("Error", "MsQuicRegistration creation failed");
            return false;
        }

        // Configure server settings
        MsQuicSettings settings;
        settings.SetIdleTimeoutMs(30000);
        settings.SetKeepAlive(10000);
        settings.SetPeerBidiStreamCount(10);  // 允许对端创建10个双向流
        settings.SetPeerUnidiStreamCount(10); // 允许对端创建10个单向流
        settings.SetConnFlowControlWindow(100 * 1024 * 1024); // 100MB
        settings.SetStreamRecvWindowDefault(8 * 1024 * 1024); // 8MB
        settings.SetMaxAckDelayMs(20);
        settings.SetStreamMultiReceiveEnabled(true);
        settings.SetPacingEnabled(true);
        settings.SetCongestionControlAlgorithm(QUIC_CONGESTION_CONTROL_ALGORITHM::QUIC_CONGESTION_CONTROL_ALGORITHM_BBR); // 使用BBR算法（若可用）/
        settings.SetPacingEnabled(true);       // 启用数据包节奏控制
        settings.SetSendBufferingEnabled(true);// 启用发送缓冲减少系统调用
        settings.SetEcnEnabled(true);
        settings.SetEncryptionOffloadAllowed(true); // 启用显式拥塞通知
        // Configure server certificate

        QUIC_CERTIFICATE_HASH_STORE certHashStore = {};
        memcpy(certHashStore.ShaHash, 
            "\x2e\x35\xe4\x04\x12\xc8\x64\xae\x91\x40"
            "\x4b\xb9\x9f\x41\x3c\xa3\x57\x22\x23\xcb",
            20);
        strcpy_s(certHashStore.StoreName, "MY");
        certHashStore.Flags = QUIC_CERTIFICATE_HASH_STORE_FLAG_NONE;

        QUIC_CREDENTIAL_CONFIG credConfig = {0};
        credConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE;
        credConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
        credConfig.CertificateHashStore = &certHashStore;

        // Create ALPN buffer
        MsQuicAlpn alpnBuffer(alpn_.c_str());

        // Create configuration
        configuration_ = new MsQuicConfiguration(
            *registration_,
            alpnBuffer,
            settings,
            MsQuicCredentialConfig(credConfig)
        );

        if (!configuration_->IsValid()) {
            std::stringstream ss;
            ss << "MsQuicConfiguration create failed: 0x" 
                    << std::hex << configuration_->GetInitStatus() 
                    << std::dec << std::endl;
            return false;
        }

        // Set version
        configuration_->SetVersionSettings(VersionSettings);
        configuration_->SetVersionNegotiationExtEnabled();

        // Create listener
        QUIC_STATUS status = msQuic_->ListenerOpen(
            *registration_,
            ServerListenerCallback,
            this,
            &listener_);

        if (QUIC_FAILED(status)) {
            std::stringstream ss;
            ss << "ListenerOpen failed: 0x" << std::hex << status << std::dec;
            LogEvent("Error", ss.str());
            return false;
        }

        // Start listening
        QUIC_ADDR addr = {0};
        QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_INET);
        QuicAddrSetPort(&addr, port_);

        const QUIC_BUFFER alpnBufferList[] = {
            { (uint32_t)alpn_.length(), (uint8_t*)alpn_.c_str() }
        };

        status = msQuic_->ListenerStart(
            listener_,
            alpnBufferList,
            1,
            &addr);

        if (QUIC_FAILED(status)) {
            std::stringstream ss;
            ss << "ListenerStart failed: 0x" << std::hex << status << std::dec;
            LogEvent("Error", ss.str());
            return false;
        }

        initialized_ = true;
        std::stringstream ss;
        ss << "MsQuicServer started on port: " << port_;
        LogEvent("Info", ss.str());
        return true;
    } catch (const std::exception& e) {
        std::string errorMsg = "Initialization error: " + std::string(e.what());
        LogEvent("Error", errorMsg);
        Shutdown();
        return false;
    } catch (...) {
        LogEvent("Error", "Initialization error");
        Shutdown();
        return false;
    }
}

// Shutdown server
void MsQuicServer::Shutdown() {
    try {
        if (initialized_) {
            LogEvent("Info", "Shutting down MsQuicServer...");
            // Close listener
            if (listener_ != nullptr) {
                msQuic_->ListenerClose(listener_);
                listener_ = nullptr;
            }

            // Clean up configuration
            if (configuration_ != nullptr) {
                delete configuration_;
                configuration_ = nullptr;
            }

            // Clean up registration
            if (registration_ != nullptr) {
                delete registration_;
                registration_ = nullptr;
            }

            initialized_ = false;
            LogEvent("Info", "MsQuicServer is closed");
        }
    } catch (const std::exception& e) {
        std::string errorMsg = "Shutdown error: " + std::string(e.what());
        LogEvent("Error", errorMsg);
    } catch (...) {
        LogEvent("Error", "Shutdown error");
    }
}

// Listener callback
QUIC_STATUS QUIC_API MsQuicServer::ServerListenerCallback(
    HQUIC Listener,
    void* Context,
    QUIC_LISTENER_EVENT* Event) {
    
    auto server = static_cast<MsQuicServer*>(Context);
    if (server == nullptr || Event == nullptr) {
        return QUIC_STATUS_INVALID_PARAMETER;
    }

    try {
        switch (Event->Type) {
        case QUIC_LISTENER_EVENT_NEW_CONNECTION:
            if (Event->NEW_CONNECTION.Connection == nullptr) {
                LogEvent("Error", "Connection handle is null in new connection event");
                return QUIC_STATUS_INVALID_PARAMETER;
            }
            
            // Set connection callback
            server->msQuic_->SetCallbackHandler(
                Event->NEW_CONNECTION.Connection,
                (void*)ServerConnectionCallback,
                server);

            // Accept connection
            return server->msQuic_->ConnectionSetConfiguration(
                Event->NEW_CONNECTION.Connection,
                *server->configuration_);

        default:
            break;
        }
    } catch (const std::exception& e) {
        std::string errorMsg = "Exception in listener callback: " + std::string(e.what());
        LogEvent("Error", errorMsg);
    } catch (...) {
        LogEvent("Error", "Unknown exception in listener callback");
    }

    return QUIC_STATUS_SUCCESS;
}

// Connection callback
QUIC_STATUS QUIC_API MsQuicServer::ServerConnectionCallback(
    HQUIC Connection,
    void* Context,
    QUIC_CONNECTION_EVENT* Event) {
    
    auto server = static_cast<MsQuicServer*>(Context);
    if (server == nullptr || Event == nullptr) {
        return QUIC_STATUS_INVALID_PARAMETER;
    }

    try {
        switch (Event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            {
                LogEvent("Info", "Client connected!");
                
                // Get and print connection info
                QUIC_STATISTICS stats;
                uint32_t size = sizeof(stats);
                if (QUIC_SUCCEEDED(server->msQuic_->GetParam(
                    Connection,
                    QUIC_PARAM_CONN_STATISTICS,
                    &size,
                    &stats))) {
                    std::stringstream ss;
                    ss << "Server connection established, version negotiation count: " << stats.VersionNegotiation;
                    LogEvent("Info", ss.str());
                }

                // Print client address info
                QUIC_ADDR remoteAddr;
                size = sizeof(remoteAddr);
                if (QUIC_SUCCEEDED(server->msQuic_->GetParam(
                    Connection,
                    QUIC_PARAM_CONN_REMOTE_ADDRESS,
                    &size,
                    &remoteAddr))) {
                    char addrStr[INET6_ADDRSTRLEN] = { 0 };
                    uint16_t port = 0;

                    if (remoteAddr.si_family == QUIC_ADDRESS_FAMILY_INET) {
                        inet_ntop(AF_INET, &remoteAddr.Ipv4.sin_addr, addrStr, INET_ADDRSTRLEN);
                        port = ntohs(remoteAddr.Ipv4.sin_port);
                    }
                    else if (remoteAddr.si_family == QUIC_ADDRESS_FAMILY_INET6) {
                        inet_ntop(AF_INET6, &remoteAddr.Ipv6.sin6_addr, addrStr, INET6_ADDRSTRLEN);
                        port = ntohs(remoteAddr.Ipv6.sin6_port);
                    }

                    std::stringstream ss;
                    ss << "Client connection from: " << addrStr << ":" << port;
                    LogEvent("Info", ss.str());

                    std::shared_ptr<MsQuicSession> session = std::make_shared<MsQuicSession>(Connection, std::string(addrStr + std::string(":") + std::to_string(port)),server);

                    {
                        std::lock_guard<std::mutex> lock(server->connectionsMutex_);

                        server->msQuicConnections[Connection] = session;
                    }

                    if (server->connectFunc) {

                        server->connectFunc(session);

                    }
               
                }
            }
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            {
                std::stringstream ss;
                ss << "Connection closed by transport layer, status code: " << Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
                LogEvent("Info", ss.str());

                // Parse error code
                uint32_t errorCode = (uint32_t)Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
                std::string errorDetails = "Error details: ";

                if (errorCode == QUIC_STATUS_CONNECTION_TIMEOUT) {
                    LogEvent("Info", errorDetails + "Connection timeout");
                }
                else if (errorCode == QUIC_STATUS_HANDSHAKE_FAILURE) {
                    LogEvent("Info", errorDetails + "TLS handshake failure");
                }
                else if (errorCode == QUIC_STATUS_TLS_ERROR) {
                    LogEvent("Info", errorDetails + "TLS protocol error");
                }
                else if (errorCode == QUIC_STATUS_USER_CANCELED) {
                    LogEvent("Info", errorDetails + "User canceled connection");
                }
                else if (errorCode == QUIC_STATUS_ALPN_NEG_FAILURE) {
                    LogEvent("Info", errorDetails + "ALPN negotiation failure");
                }
                else if (errorCode == 0x80410005) {
                    LogEvent("Info", errorDetails + "Internal error (QUIC_STATUS_INTERNAL_ERROR)");
                    LogEvent("Info", "This may indicate protocol violation or implementation error");
                }
                else {
                    std::stringstream ss;
                    ss << errorDetails << "Unknown error: 0x" << std::hex << errorCode << std::dec;
                    LogEvent("Info", ss.str());
                }

                {

                    std::lock_guard<std::mutex> lock(server->connectionsMutex_);

                    if (server->msQuicConnections.find(Connection) != server->msQuicConnections.end()) {

                        if (server->connectOutFunc) {

                            server->connectOutFunc(server->msQuicConnections[Connection]);

                        }

                        server->msQuicConnections.erase(Connection);

                    }
                }

            }
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            LogEvent("Info", "Connection closed by peer");
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            LogEvent("Info", "Connection shutdown complete");
            // Remove from connection pool
            server->msQuic_->ConnectionClose(Connection);
            break;

        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            LogEvent("Info", "Peer stream started");
            // Set stream callback and save stream reference
            if (Event->PEER_STREAM_STARTED.Stream != nullptr) {
                
                server->msQuicConnections[Connection]->setReviceFileSystem(Event->PEER_STREAM_STARTED.Stream);
            }
            break;

        default:
            std::cout << "Unhandled connection event: " << Event->Type << std::endl;
            break;
        }
    } catch (const std::exception& e) {
        std::string errorMsg = "Exception in connection callback: " + std::string(e.what());
        LogEvent("Error", errorMsg);
    } catch (...) {
        LogEvent("Error", "Unknown exception in connection callback");
    }

    return QUIC_STATUS_SUCCESS;
}

// Start server thread
void MsQuicServer::StartServerThread() {
    if (running_) {
        LogEvent("Info", "Server thread is already running");
        return;
    }
    
    running_ = true;
    serverThread_ = std::thread(&MsQuicServer::ServerThreadFunc, this);
    LogEvent("Info", "Server is running in a separate thread...");
}

// Stop server thread
void MsQuicServer::StopServerThread() {
    if (!running_) {
        LogEvent("Info", "Server thread is not running");
        return;
    }
    
    running_ = false;
    
    if (serverThread_.joinable()) {
        serverThread_.join();
    }
    
    LogEvent("Info", "Server thread stopped");
}

void MsQuicServer::setConnectCallBack(std::function<void(std::shared_ptr<MsQuicSession>)> func)
{
    this->connectFunc = func;
}

void MsQuicServer::setConnectOutCallBack(std::function<void(std::shared_ptr<MsQuicSession>)> func)
{
   this->connectOutFunc = func;
}

// Server thread function
void MsQuicServer::ServerThreadFunc() {
    LogEvent("Info", "Server thread started");
    
    while (running_) {
        // Process any pending operations
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    LogEvent("Info", "Server thread exiting");
}

// Stream callback
QUIC_STATUS QUIC_API MsQuicServer::ServerStreamCallback(
    HQUIC Stream,
    void* Context,
    QUIC_STREAM_EVENT* Event) {
    
    // 流事件类型名称
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
    
    auto server = static_cast<MsQuicServer*>(Context);
    if (server == nullptr || Event == nullptr) {
        return QUIC_STATUS_INVALID_PARAMETER;
    }

    try {
        // 修改默认的事件日志输出，使用事件名称
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
            }
            break;

        // Add handler for QUIC_STREAM_EVENT_RECEIVE (type 1)
        case QUIC_STREAM_EVENT_RECEIVE:
            {
               
            }
            break;
            
        // Add handler for QUIC_STREAM_EVENT_SEND_COMPLETE (type 2)
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            {
                LogEvent("Info", "Stream send complete event");
                
                if (Event->SEND_COMPLETE.Canceled) {

                    LogEvent("Warning", "Send was canceled");

                } else {

                    LogEvent("Info", "Data send successfully");

                }
                
                // If there's a client context, process it
                if (Event->SEND_COMPLETE.ClientContext != nullptr) {
                    // Process client context if needed
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

        case QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE:
            {
                LogEvent("Info", "Stream send shutdown complete");
                
            }
            break;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            LogEvent("Info", "Stream shutdown complete");
            break;

        default:
            {
                std::stringstream ss;
                if (Event->Type < sizeof(eventNames) / sizeof(eventNames[0])) {
                    ss << "Unhandled stream event: " << eventNames[Event->Type] 
                       << " (" << Event->Type << ")";
                } else {
                    ss << "Unknown stream event type: " << Event->Type;
                }
                LogEvent("Info", ss.str());
            }
            break;
        }
    } catch (const std::exception& e) {
        std::string errorMsg = "Exception in stream callback: " + std::string(e.what());
        LogEvent("Error", errorMsg);
    } catch (...) {
        LogEvent("Error", "Unknown exception in stream callback");
    }

    return QUIC_STATUS_SUCCESS;
}


bool MsQuicServer::IsStreamValid(HQUIC stream) {
    if (stream == nullptr) return false;

    // 使用流统计信息验证流是否有效
    QUIC_STREAM_STATISTICS stats;
    uint32_t size = sizeof(stats);
    if (!QUIC_SUCCEEDED(msQuic_->GetParam(
        stream,
        QUIC_PARAM_STREAM_STATISTICS,
        &size,
        &stats))) {
        return false;
    }

    // 验证流ID
    uint64_t streamId = 0;
    size = sizeof(streamId);

    bool flag =  QUIC_SUCCEEDED(msQuic_->GetParam(
        stream,
        QUIC_PARAM_STREAM_ID,
        &size,
        &streamId));

    return flag;
}






