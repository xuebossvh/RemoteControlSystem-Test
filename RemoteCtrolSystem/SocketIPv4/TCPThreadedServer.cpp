#include "TCPThreadedServer.h"
#include <memory>
TCPThreadedServer::TCPThreadedServer() : m_listener(NULL), m_port(0), m_serviceFactory(NULL), m_iClientCounter(0) {}
TCPThreadedServer::~TCPThreadedServer() { Stop(); }
void TCPThreadedServer::Start(TCPServiceFactory& services, TCPSocketAddress::Port port) {
    Stop();
    try {
        m_server.Start(port); // Report bind/listen errors before the UI announces success.
        m_serviceFactory = &services;
        m_port = port;
        m_iClientCounter = 0;
        m_listener = new TCPListen;
        m_listener->m_pThreadedServer = this;
        m_listener->m_bListen = true;
        m_listener->Start();
    } catch (...) { Stop(); throw; }
}
void TCPThreadedServer::Stop() throw() {
    if (m_listener) m_listener->m_bListen = false;
    m_server.Stop(); // Unblock select/accept before joining.
    if (m_listener) { m_listener->WaitForEnd(); delete m_listener; m_listener = NULL; }
    for (auto service : m_services) service->StopConnection();
    for (auto service : m_services) { service->WaitForEnd(); delete service; }
    m_services.clear();
    m_serviceFactory = NULL;
}
TCPSocketAddress TCPThreadedServer::GetAddress() { return m_server.GetAddress(); }
int TCPThreadedServer::ProcessedClients() throw() { return m_iClientCounter.load(); }
void TCPListen::Run() {
    while (m_bListen) {
        try {
            auto& services = m_pThreadedServer->m_services;
            for (auto it = services.begin(); it != services.end();) {
                if (!(*it)->IsAlive()) { (*it)->WaitForEnd(); delete *it; it = services.erase(it); }
                else ++it;
            }
            if (!m_pThreadedServer->m_server.Wait4Connection(0, 100000)) continue;
            TCPSocketAddress address;
            std::unique_ptr<TCPConnection> connection(m_pThreadedServer->m_server.GetConnection(address));
            if (!m_bListen) break;
            std::unique_ptr<TCPService> service(m_pThreadedServer->m_serviceFactory->createService(connection.get()));
            connection.release();
            service->Start();
            services.push_back(service.get());
            service.release();
            ++m_pThreadedServer->m_iClientCounter;
        } catch (...) { if (!m_bListen) break; Sleep(10); }
    }
}
