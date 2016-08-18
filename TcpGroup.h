#ifndef __TCPGROUP_H__
#define __TCPGROUP_H__

#include "Xtc.h"
#include "XtcSequence.h"
#include "XtcArray.h"
#include "XtcQueue.h"
#include "TcpSession.h"
#include "TcpCache.h"


#ifdef _OPENSSL
#include "openssl/ssl.h"
#endif

typedef struct STimer
{
		uint32_t timerID;//key
		//SOCKET fd;
		uint64_t skt_idx;
		void *position;
}STimer;

typedef struct STimeout
{
        uint32_t currentMS;//key
		uint32_t timerID;
}STimeout;

typedef struct SThreadArg
{
	CTcpCache<SPacket> m_innerCache;
	void *m_thread;
}SThreadArg;


#define MAX_WORK_THERAD_NUM 32

//TCP会话组 
class CTcpGroup
{
friend class CTcpServer;
friend class CTcpSession;
public:
	CTcpGroup();
	~CTcpGroup();

	//启动服务,model = 1,抢占式,不保证时序性，可以充分利用cpu;model=2 分配式，保证时序性,need_response---对于无效的请求是否回复
	bool Start( void *server, int32_t cpu_idx,int thread_num ,int32_t cpu_num,int32_t model,bool need_response);
	//停止服务
	void Stop();
	//是否已经初始化
	bool IsStarted();

	void *GetServer();

	//添加新连接
	void SetLinkBorn( STcpLink link );
	//要求link死亡
	bool SetLinkDead( STcpLink link );
	//寻找连接，已加锁线程安全
	void* SearchLink( SOCKET *skt, CTcpSession **session );

	//发送数据，送入缓冲立即返回，已加锁线程安全
	bool PostData( SPacketHeader& packet_header, char_t *buf, int32_t size );

	uint32_t SetTimer(uint64_t skt_idx,void *position,int32_t timems = 60000);//超时默认一分钟
	//设置事件
	void SetEvent(int32_t fd,int ctrl,int event,void *ptr_param,int fd_param);
	//取消超时检测	
	void CancelTimer(uint32_t timerid);
	//处理超时	
	void HandleTimeout();

	void GetSocket();
	bool AddRecvBuffer(SPacket& buffer);//往抢占式队列里送,引用可以减少拷贝
	bool AddRecvBufferToThreadQueue(SPacket& buffer);//往分配式的每个线程专属的队列里送，引用可以减少拷贝
	//获取处理方式
	int32_t GetModel();
	//CTcpCache m_innerCache;//输入缓冲区
	CTcpCache <SPacketHeader> m_outerCache;//输出缓冲区
	bool m_need_response;//当没有匹配的请求是，是否需要给客户端回答
	/*供收数据时使用*/
	char_t *m_recv_buf;
	int32_t m_recv_bufsize;

private:
	CXtcQueue<STcpLink> m_newQueue;//新连接
	CXtcSequence<CTcpSession*> m_sessions;//所有活动客户端
	CXtcArray<CTcpSession*> m_dumps;//客户端连接回收池

	void *m_epoll;//epoll句柄
	//void *m_work_thread[MAX_WORK_THERAD_NUM];//工作线程
	CXtcArray<SThreadArg*> m_work_thread;
	void *m_dispatch_thread;//调度线程
	void *m_server;//指向所属CTcpServer
	uint32_t m_lastTick;
	//int m_work_thread_num;
	void *m_rwlock;//m_sessions/m_outer_cache专用保护锁

	//定时器相关的
	uint32_t m_timerID;
	CXtcSequence<STimer>m_timers;//保存任务，key 为 timerID
	CXtcSequence<STimeout>m_timeout;//保存任务，根据绝对时间排序
	//CXtcArray<SPacketHeader>m_dead_links;

	//通过这个socket传递tcpserver accpet到的socket
	int32_t m_server_accpet_socket;//server accpet 到soceket 就往这个socket写数据
	int32_t	m_group_accpet_socket;//和m_server_accpet_socket配套使用
	//CXtcQueue<SPacket> m_recvbufQueue;
	CTcpCache<SPacket> m_innerCache;

	int32_t m_control_fd;//往这个fd写数据表明有数据要发送了
	int32_t m_notify_fd;//和m_control_fd配套使用

	void *m_inner_cache_mutex;//m_recvbufQueue 锁
	pthread_cond_t  *m_inner_cache_cond;//与m_recvbuf_queue_mutex 配套使用

	int32_t m_model;//分配式or抢占式

private:
	//抢占式工作处理函数 
	static int32_t WorkProc( void* param, void* expend );
	//抢占式工作处理函数 
	int32_t OnWork();

	//分配式工作处理函数 
	static int32_t WorkProc1( void* param, void* expend );
	//分配式工作处理函数 
	int32_t OnWork1(void* expend);

	//数据检测函数 
	static int32_t DispatchProc( void* param, void* expend );
	//数据检测函数 
	int32_t OnDispatch();

	//激活新连接
	void* ActivateLink( STcpLink link );
	//杀死新连接 
	void KillLink( void *position );
	//将fd的数据读完
	void ReadSocket(int32_t fd);

	//排序比较函数 
//	static int32_t CompareLinkCallback(bool item1_is_key, void* item1, void* item2, void *param );
	static int32_t CompareSessionCallback(bool item1_is_key, void* item1, void* item2, void *param );

	//
	static int32_t CompareTaskByTimerIDCallback(bool item1_is_key, void* item1, void* item2, void *param );
	static int32_t CompareTaskByTimeMsCallback(bool item1_is_key, void* item1, void* item2, void *param );
};


#endif
