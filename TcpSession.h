#ifndef __TCPSESSION_H__
#define __TCPSESSION_H__


#ifdef _OPENSSL
#include "openssl/ssl.h"
#endif
#include "Resource.h"


//HTTP会话连接 
class CTcpSession
{
friend class CTcpServer;
friend class CTcpGroup;
public:
	CTcpSession();
	~CTcpSession();

	//开始工作
	virtual void Start( void *group, STcpLink link );
	//停止工作
	virtual void Stop();

	//设置死亡标志
	virtual void SetDead( bool flag );
	//是否死亡（如连接断开）
	virtual bool IsDead( uint32_t now );

	//发送数据，仅用于向自己发送
	virtual bool PostData( char_t *buf, int32_t size );

	//数据包处理函数
	virtual void OnPacket(char_t *buf, int32_t hlen, int32_t clen,uint32_t ip,uint16_t port,SOCKET skt,uint64_t idx,uint32_t now) ;
	//接收数据，返回true表示需socket挂了，false表示继续工作
	virtual bool OnRecv( uint32_t now );
	//接收数据
	virtual void OnSend( char_t *buf, int32_t size, uint32_t now );
	//发送前要处理的一些回调
	virtual void HandleBeforeSend(SPacketHeader packet_header,char_t *buf,int32_t bufsize);

	//取得所在服务器
	void *GetServer();
	//取得所在组
	void *GetGroup();

	//设置超时检测的timerid
	void SetTimerId(uint32_t timerid);
	//取得timerid
	uint32_t GetTimerId();
	//取得m_close_flag
	bool GetCloseFlag();
	/*从socket接收数据*/
	bool Recv(void *ptr,char_t *buf,int32_t bufsize,bool is_ssl,int32_t *recvsize);
	/*分析buffer 分析结束返回true  or 返回false*/
	bool AnalysisBuf(char_t *buf,int32_t buflen,int32_t *pos);
	/*ssl处理握手*/
	bool HandShake();
public:
	void *m_group;
	STcpLink m_link;

protected:
#ifdef _OPENSSL
	SSL *m_ssl;
	bool m_handshaked;
#endif
	bool m_dead_flag;//是否已经死亡 
	bool m_close_flag;//是否等待关闭 
	uint32_t m_act_tick;//上次收发数据时间 

	char_t *m_recv_buf;      //数据接收缓冲区 
	int32_t m_recv_bufsize; //数据接收缓冲区大小 
	int32_t m_recv_datsize; //当前数据接收缓冲区中已接收到的数据大小

	char_t *m_send_buf;      //数据发送缓冲区 
	int32_t m_send_bufsize; //数据发送缓冲区大小 
	int32_t m_send_datsize; //发送数据大小

	uint32_t m_timer_id;//超时时间
};

#endif // __TCPSESSION_H__
