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

	/*启动服务,timeout=-1表示不检测超时 thread_num等于0的话，收、处理、发就全在dispatch函数完成，否则就要开启
		need_response 表示一问就要一答,model==1 ，抢占式处理(不保证时序性)，model==2,分配式处理(保证时序性)
	*/
	bool Initialize( uint32_t listen_ip, uint16_t listen_port, bool ssl_flag,int group_num,
			int32_t thread_num,int32_t model,bool need_response, PTcpNewCallback proc, void *param);
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


	uint32_t m_listen_ip;//监听地址，网络顺序
	uint16_t m_listen_port;//监听端口，网络顺序
	bool m_ssl_flag;//是否是用ssl

protected:

#ifdef _OPENSSL
	SSL_CTX *m_ssl_ctx;//SSL环境
#endif

	SOCKET m_listen_skt;//监听套接字
	STcpServerStatistics m_statistics;//统计信息

	CXtcArray<CTcpGroup*> m_groups;//所有客户端连接的工作线程池

	void* m_thread;

	PTcpNewCallback m_proc;//SESSION构造函数
	void *m_param;

private:
	//监听处理函数
	static int32_t ListenProc( void* param, void* expend );
	//监听处理函数
	int32_t OnListen();

	uint64_t m_skt_idx;//socket唯一识别计数器,uint64_t一辈子也用不完
};


#endif //__TCPSERVER_H__
