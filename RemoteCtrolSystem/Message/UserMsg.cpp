#include "UserMsg.h"
#include <cstring>
UserMsg::UserMsg() { m_buffer = new char[41](); m_size = 41; m_buffer[0] = RCS_MSG_USER; }
UserMsg::~UserMsg() {}
bool UserMsg::GetUsername(char* value) {
    if (!value || m_size != 41 || GetType() != RCS_MSG_USER) return false;
    memcpy(value, m_buffer + 1, 19); value[19] = '\0';
    return memchr(m_buffer + 1, '\0', 20) != NULL;
}
bool UserMsg::GetPassword(char* value) {
    if (!value || m_size != 41 || GetType() != RCS_MSG_USER) return false;
    memcpy(value, m_buffer + 21, 19); value[19] = '\0';
    return memchr(m_buffer + 21, '\0', 20) != NULL;
}
bool UserMsg::PacketMsg(char* username, char* password) {
    if (!username || !password) return false;
    size_t nameLength = 0, passwordLength = 0;
    while (nameLength < 20 && username[nameLength]) ++nameLength;
    while (passwordLength < 20 && password[passwordLength]) ++passwordLength;
    if (!nameLength || nameLength >= 20 || passwordLength >= 20) return false;
    if (m_size != 41) { delete[] m_buffer; m_buffer = new char[41]; m_size = 41; }
    memset(m_buffer, 0, 41); m_buffer[0] = RCS_MSG_USER;
    memcpy(m_buffer + 1, username, nameLength);
    memcpy(m_buffer + 21, password, passwordLength);
    return true;
}
