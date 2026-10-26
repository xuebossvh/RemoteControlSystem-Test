#ifndef __TCPTHREADEDSERVER_H__
#define __TCPTHREADEDSERVER_H__
#include "../Thread/Thread.h"
#include "TCPServer.h"
#include <atomic>
#include <vector>
class TCPListen;
class TCPServiceFactory;
class TCPService;
class TCPThreadedServer {
public:
    TCPThreadedServer();
    ~TCPThreadedServer();
    void Start(TCPServiceFactory& services, TCPSocketAddress::Port port = 0);
    void Stop() throw();
    TCPSocketAddress GetAddress();
    int ProcessedClients() throw();
protected:
    friend class TCPListen;
    TCPListen* m_listener;
    TCPServer m_server;
    TCPSocketAddress::Port m_port;
    TCPServiceFactory* m_serviceFactory;
    std::atomic<int> m_iClientCounter;
    std::vector<TCPService*> m_services;
};
class TCPService : public Thread {
public:
    explicit TCPService(TCPConnection* value) : connection(*value) {}
    virtual ~TCPService() { connection.disconnect(); WaitForEnd(); delete &connection; }
    void StopConnection() { connection.disconnect(); }
protected:
    TCPConnection& connection;
};
class TCPServiceFactory {
public:
    virtual ~TCPServiceFactory() {}
    virtual TCPService* createService(TCPConnection* connection) = 0;
};
template<class SERVICE>
class TCPServiceFactoryTemplate : public TCPServiceFactory {
public:
    virtual TCPService* createService(TCPConnection* connection) { return new SERVICE(connection); }
};
class TCPListen : public Thread {
public:
    TCPListen() : m_pThreadedServer(NULL), m_bListen(false) {}
    TCPThreadedServer* m_pThreadedServer;
    std::atomic<bool> m_bListen;
protected:
    void Run();
};
#endif
