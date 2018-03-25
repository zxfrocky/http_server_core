#ifndef __TCPSERVER_H__
#define __TCPSERVER_H__

#include "Xtc.h"
#include "XtcSequence.h"
#include "XtcArray.h"
#include "TcpGroup.h"
#ifdef _OPENSSL
#include "openssl/ssl.h"
#endif


//统计
typedef struct 
{
	int64_t send_size;//累计发送字节数
	int64_t recv_size;//累计接收字节数
	int64_t cmd_cnt;//累计处理指令个数
	int64_t connect_cnt;//累计收到连接个数
	int64_t session_cnt;//活动连接个数
}STcpServerStatistics;//统计


//session构造回调接口
typedef CTcpSession* (*PTcpNewCallback)(void *param);

//TCP会话服务器
class CTcpServer
{
friend class CTcpGroup;
friend class CTcpSession;
public:
	CTcpServer();
	~CTcpServer();

	/*启动服务*/
	bool Initialize( SListenParam *listen_param,int32_t listen_param_size,
			SGroupParam group_param, PTcpNewCallback proc, void *param,PHandlePacket pkt_proc,void *pkt_proc_param);
	//停止服务，释放资源
	void Release();
	//是否已经初始化
	bool IsInitialized();

	//发送数据，送入缓冲立即返回，已加锁线程安全
	bool PostData( STcpLink link, char_t *buf, int32_t size );

	//取得统计信息
	void GetStatistics( STcpServerStatistics *sta );
	//添加统计信息
	void AddStatistics( STcpServerStatistics& sta );

	//显示
	void DisplayStatus(char_t *buf,int32_t bufsize);

	uint64_t GetSktIdx();

protected:

#ifdef _OPENSSL
	SSL_CTX *m_ssl_ctx;//SSL环境
#endif
	STcpServerStatistics m_statistics;//统计信息

	CXtcArray<CTcpGroup*> m_groups;//所有客户端连接的工作线程池

	void* m_thread;

	PTcpNewCallback m_proc;//SESSION构造函数
	void *m_param;

	PHandlePacket m_pkt_proc;//数据包处理函数
	void *m_pkt_param;

	CXtcArray<SListenParam> m_listen_param_array;

private:
	//监听处理函数
	static int32_t ListenProc( void* param, void* expend );
	//监听处理函数
	int32_t OnListen();

	uint64_t m_skt_idx;//socket唯一识别计数器,uint64_t一辈子也用不完
	void *m_epoll;
};


#endif //__TCPSERVER_H__
