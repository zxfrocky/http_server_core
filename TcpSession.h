#ifndef __TCPSESSION_H__
#define __TCPSESSION_H__


#ifdef _OPENSSL
#include "openssl/ssl.h"
#endif
#include "ServerCommon.h"
#include "ListQueue.h"
#include "HttpParser.h"

typedef struct
{
	char_t *buf;
	int32_t bufsize;//buf所占的内存大小 按4KB 对齐
	int32_t buflen;//buf的真正长度
	int32_t sendpos;//buf发送的位置
}SBufNode;


//HTTP会话连接 
class CTcpSession
{
friend class CTcpServer;
friend class CTcpGroup;
public:
	CTcpSession();
	virtual ~CTcpSession();

	//开始工作
	virtual void Start( void *group, STcpLink link );
	//停止工作
	virtual void Stop();

	//发送数据，仅用于向自己发送
	virtual bool PostData( char_t *buf, int32_t size );

	//数据包处理函数
	virtual void OnPacket(char_t *buf, int32_t hlen, int32_t clen,bool keepalive,STcpLink *link,uint64_t now) ;
	//接收数据，返回true表示需socket挂了，false表示继续工作
	virtual bool OnRecv( uint64_t now );
	//接收数据，返回true表示需socket挂了，false表示继续工作
	virtual bool OnSend( char_t *buf, int32_t size, uint64_t now );
	//发送前要处理的一些回调
	virtual void HandleBeforeSend(SPacketHeader packet_header,char_t *buf,int32_t bufsize);
	//是否有数据需要发送
	bool IsNeedSendData();
	//取得所在服务器
	void *GetServer();
	//取得所在组
	void *GetGroup();

	//设置超时检测的timerid
	void SetTimerId(uint64_t timerid);
	//取得timerid
	uint64_t GetTimerId();
	//取得m_close_flag
	bool GetCloseFlag();
	/*从socket接收数据*/
	bool Recv(void *ptr,char_t *buf,int32_t bufsize,bool is_ssl,int32_t *recvsize);
	/*socket发送数据*/
	bool Send(void *ptr,char_t *buf,int32_t bufsize,bool is_ssl,int32_t *sndsize);
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
	uint64_t m_timer_id;//超时时间

	CListQueue<SBufNode> *m_out_list_queue;
	int32_t m_out_list_queue_size;//m_out_list_queue中所有buf的总大小
	CHttpParser *m_http_parser;//输入的http解析器
};

#endif // __TCPSESSION_H__
