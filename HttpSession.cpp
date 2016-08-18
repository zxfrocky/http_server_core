#include "stdafx.h"
#include "osl.h"
#include "osl_spin.h"
#include "osl_thread.h"
#include "osl_socket.h"
#include "osl_log.h"
#include "osl_string.h"
#include "main.h"
#include "HttpSession.h"

/* HTTP会话连接 */
CHttpSession::CHttpSession()
{
	memset( m_peer_id, 0, sizeof(m_peer_id) );
	memset(m_user_id,0,sizeof(m_user_id));
}

CHttpSession::~CHttpSession()
{
}

void CHttpSession::HandleBeforeSend(SPacketHeader packet_header,char_t *buf,int32_t bufsize)
{

	if(packet_header.peer_id[0])
	{
		memset(m_peer_id,0,sizeof(m_peer_id));
		osl_strncpy(m_peer_id,packet_header.peer_id,sizeof(m_peer_id));
	}

	if(packet_header.user_id[0])
	{
		memset(m_user_id,0,sizeof(m_user_id));
		osl_strncpy(m_user_id,packet_header.user_id,sizeof(m_user_id));
	}
		
}

//将完整的包发到接收队列里
void CHttpSession::OnPacket(char_t *buf, int32_t hlen, int32_t clen,uint32_t ip,uint16_t port,SOCKET skt,uint64_t idx,uint32_t now)
{
	//g_service_disp.OnHttpPacekt( GetServer(), m_group, this, buf, hlen, clen, now );
		//osl_log_debug("==== %s\n",buf);
		CTcpGroup *grp = (CTcpGroup*)m_group;
		SPacket packet;
		memset(&packet,0,sizeof(packet));
		packet.header.link.remote_ip = ip;
		packet.header.link.remote_port = port;
		packet.header.link.skt = skt;
		packet.header.link.skt_idx = idx;
		packet.header.mode = PACKET_MODE_HTTP;
		packet.header.flag = PACKET_END;//默认就是一个完成的包
		if(m_peer_id[0])
			osl_strncpy(packet.header.peer_id,m_peer_id,sizeof(packet.header.peer_id));
		if(m_user_id[0])
			osl_strncpy(packet.header.user_id,m_user_id,sizeof(packet.header.user_id));
		packet.len = hlen + clen+1;//最后一个为'\0'
		packet.hlen = hlen;
		packet.clen = clen;

		packet.buf = buf;
		
		//memcpy(packet.buf,buf,hlen + clen);
		//*((char_t*)(packet.buf+packet.len -1)) = 0;

		//osl_log_debug("recv:%s \n",packet.buf);

		if(grp->GetModel() == 1)
		{
			if(!grp->AddRecvBuffer(packet))
			{
				osl_log_debug("[%s] recvqueue too small or handle too slow\n",__func__);
			}
		}
		else if(grp->GetModel() == 2)
		{
			if(!grp->AddRecvBufferToThreadQueue(packet))
			{
				osl_log_debug("[%s] thread queue too small or handle too slow\n",__func__);
			}
		}
		else
		{
			osl_log_error("[%s] maybe something error\n",__func__);
		}
				
}
