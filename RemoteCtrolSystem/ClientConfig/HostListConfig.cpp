#include "StdAfx.h"
#include "HostListConfig.h"
#include <cstring>
#include <windows.h>

HostListConfig::HostListConfig() : m_pRootElement(NULL), m_pDocument(NULL) {}
HostListConfig::~HostListConfig() { delete m_pDocument; }

bool HostListConfig::Init(char* path)
{
    delete m_pDocument;
    m_pDocument = new TiXmlDocument(path);
    if (!m_pDocument->LoadFile()) {
        // Do not overwrite an existing malformed configuration.
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return false;
        m_pDocument->LinkEndChild(new TiXmlDeclaration("1.0", "UTF-8", ""));
        m_pRootElement = new TiXmlElement("ClientConifg");
        m_pDocument->LinkEndChild(m_pRootElement);
        m_pRootElement->LinkEndChild(new TiXmlElement("HostList"));
        Host local = {};
        strcpy_s(local.name, "Local"); strcpy_s(local.ip, "127.0.0.1"); local.port = 8900;
        AddHost(local);
    }
    m_pRootElement = m_pDocument->RootElement();
    return m_pRootElement && m_pRootElement->FirstChildElement("HostList");
}

void HostListConfig::ReadHostList(std::vector<Host>& hosts)
{
    hosts.clear();
    if (!m_pRootElement) return;
    TiXmlElement* list = m_pRootElement->FirstChildElement("HostList");
    if (!list) return;
    for (TiXmlElement* item = list->FirstChildElement("Host"); item; item = item->NextSiblingElement("Host")) {
        const char* name = item->Attribute("Name");
        const char* ip = item->Attribute("Ip");
        int port = 0;
        if (!name || !ip || strlen(name) >= sizeof(Host().name) || strlen(ip) >= sizeof(Host().ip) ||
            item->QueryIntAttribute("Port", &port) != TIXML_SUCCESS || port < 1 || port > 65535) continue;
        Host host = {};
        strcpy_s(host.name, name); strcpy_s(host.ip, ip); host.port = port;
        hosts.push_back(host);
    }
}

void HostListConfig::AddHost(Host host)
{
    if (!m_pRootElement || host.port < 1 || host.port > 65535) return;
    TiXmlElement* list = m_pRootElement->FirstChildElement("HostList");
    if (!list) return;
    TiXmlElement* item = new TiXmlElement("Host");
    item->SetAttribute("Name", host.name);
    item->SetAttribute("Ip", host.ip);
    item->SetAttribute("Port", host.port);
    list->LinkEndChild(item);
    m_pDocument->SaveFile();
}

void HostListConfig::DeleteHost(char* name)
{
    if (!m_pRootElement || !name) return;
    TiXmlElement* list = m_pRootElement->FirstChildElement("HostList");
    if (!list) return;
    for (TiXmlElement* item = list->FirstChildElement("Host"); item; item = item->NextSiblingElement("Host")) {
        const char* existing = item->Attribute("Name");
        if (existing && strcmp(existing, name) == 0) {
            list->RemoveChild(item); m_pDocument->SaveFile(); return;
        }
    }
}
