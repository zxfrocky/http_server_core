#ifndef __HTTPSESSION_H__
#define __HTTPSESSION_H__

#include "TcpSession.h"


/* 会话连接 */
class CHttpSession : public CTcpSession
{
public:
	CHttpSession();
	~CHttpSession();

	//处理其他OCS发送的消息
	void OnPacket(char_t *buf, int32_t hlen, int32_t clen,uint32_t ip,uint16_t port,SOCKET skt,uint64_t idx,uint32_t now);

	void HandleBeforeSend(SPacketHeader packet_header,char_t *buf,int32_t bufsize);


public:
	char_t m_peer_id[36];//对端PEER_ID
	char_t m_user_id[36];//对端USER_ID
};

#endif
