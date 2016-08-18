#include "stdafx.h"
#include "osl_epoll.h"
#include "osl_socket.h"
#include "osl_log.h"
#include "osl_thread.h"
#include "osl_mem.h"
#include "osl_url.h"
#include "osl_mutex.h"
#include "TcpServer.h"
#include "TcpGroup.h"
#include "TcpCache.cpp"
#ifdef _OPENSSL
#include "openssl/ssl.h"
#endif
#include "main.h"


//TCP会话组
CTcpGroup::CTcpGroup()
{
	m_epoll = NULL;
	m_dispatch_thread = NULL;
	//memset(m_work_thread,0,sizeof(m_dispatch_thread));
	m_server = NULL;
	m_lastTick = 0;
	m_rwlock = NULL;
	m_inner_cache_mutex = NULL;
	m_inner_cache_cond = NULL;
	//通过这个socket传递tcpserver accpet到的socket
	m_server_accpet_socket = -1;//server accpet 到soceket 就往这个socket写数据
	m_group_accpet_socket = -1;//和m_server_accpet_socket配套使用
	m_control_fd = -1;//往这个fd写数据表明有数据要发送了
	m_notify_fd = -1;//和m_control_fd配套使用
	//m_recv_buf = NULL;

	m_sessions.SetCompareCallback( CTcpGroup::CompareSessionCallback, this );
	//m_recvLinks.SetCompareCallback( CTcpGroup::CompareLinkCallback, this );
	//m_sendLinks.SetCompareCallback( CTcpGroup::CompareLinkCallback, this );


	//超时检测相关
	m_timerID = 0;
	m_timers.SetCompareCallback(CTcpGroup::CompareTaskByTimerIDCallback,this);
	m_timeout.SetCompareCallback(CTcpGroup::CompareTaskByTimeMsCallback,this);
	m_need_response = false;
}


CTcpGroup::~CTcpGroup()
{
	Stop();
}


//启动服务
bool CTcpGroup::Start( void *server, int32_t cpu_idx,int32_t thread_num ,int32_t cpu_num,int32_t model,bool need_response)
{
	SThreadArg *thread_arg;
	int32_t work_thread_num;
	static int32_t thread_cpu_idx = 0;
	int fd[2];
	
	m_newQueue.Create(8196);
	m_outerCache.Create( g_settings.group_cache_size*1024*1024 );//外部数据发送缓冲区

	m_epoll = osl_epoll_create( 65536 );
	m_server= server;
	m_lastTick = 0;
	m_need_response = need_response;
	m_recv_bufsize = 65536;
	m_recv_buf = (char_t*)malloc(m_recv_bufsize);
	
	if ( 0 != socketpair(AF_UNIX, SOCK_STREAM, 0, fd) )
	 	return false;

	int n = 65536;
	setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &n, sizeof(n));
	fcntl(fd[0], F_SETFL, O_RDWR|O_NONBLOCK);

	setsockopt(fd[1], SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
	fcntl(fd[1], F_SETFL, O_RDWR|O_NONBLOCK);

	m_server_accpet_socket   = fd[0];
	m_group_accpet_socket    = fd[1]; 

	SetEvent(m_group_accpet_socket,OSL_EPOLL_CTL_ADD,OSL_EPOLL_IN,NULL,m_group_accpet_socket);

	if ( 0 != socketpair(AF_UNIX, SOCK_STREAM, 0, fd) )
	 	return false;

	setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &n, sizeof(n));
	fcntl(fd[0], F_SETFL, O_RDWR|O_NONBLOCK);

	setsockopt(fd[1], SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
	fcntl(fd[1], F_SETFL, O_RDWR|O_NONBLOCK);

	m_control_fd   = fd[0];
	m_notify_fd    = fd[1]; 

	SetEvent(m_notify_fd,OSL_EPOLL_CTL_ADD,OSL_EPOLL_IN,NULL,m_notify_fd);		
	m_rwlock = osl_rwlock_create();

	work_thread_num = thread_num < MAX_WORK_THERAD_NUM ? thread_num : MAX_WORK_THERAD_NUM;
	if(model != 1 && model != 2)
	{
		model = 1;//默认就是抢占式
	}
	/*如果work_thread_num 不大于0 ，表明所有的任务都在Dispatch 线程处理*/
	m_model = model;
	if(model == 1)
	{
		//抢占式
		if(work_thread_num > 0)
		{
			m_innerCache.Create(g_settings.group_cache_size*1024*1024);//
			m_inner_cache_mutex = osl_mutex_create();//m_recvbufQueue 锁
			m_inner_cache_cond = (pthread_cond_t*)malloc(sizeof(pthread_cond_t));//与m_recvbuf_queue_mutex 配套使用
			pthread_cond_init( m_inner_cache_cond, NULL );

			for(int i = 0;i< work_thread_num ;i++)
			{
				thread_arg = (SThreadArg*)malloc(sizeof(SThreadArg));
				memset(thread_arg,0,sizeof(SThreadArg));
				thread_arg->m_thread = osl_thread_create( "tTcpGroup", 100, 3*1024*1024, CTcpGroup::WorkProc, this, NULL );
				if(thread_arg->m_thread)
				{
					m_work_thread.Add(thread_arg);
					osl_thread_bind_cpu( thread_arg->m_thread, (thread_cpu_idx++)%cpu_num );
					osl_thread_resume(thread_arg->m_thread);
				}

				osl_log_debug("func:%s start thread num:%d thread:%x\n",__func__,i,thread_arg->m_thread);
			}
		}
	}
	else if(model == 2)
	{
		//分配式
		if(work_thread_num > 0)
		{
			for(int i = 0;i< work_thread_num ;i++)
			{
				thread_arg = (SThreadArg*)malloc(sizeof(SThreadArg));
				memset(thread_arg,0,sizeof(SThreadArg));
				//thread_arg->m_innerCache = new CTcpCache<SPacket>();
				thread_arg->m_innerCache.Create(g_settings.group_cache_size*1024*1024);//
				thread_arg->m_thread = osl_thread_create( "tTcpGroup1", 100, 3*1024*1024, CTcpGroup::WorkProc1, this, thread_arg );
				if(thread_arg->m_thread)
				{
					m_work_thread.Add(thread_arg);
					osl_thread_bind_cpu( thread_arg->m_thread, (thread_cpu_idx++)%cpu_num );
					osl_thread_resume(thread_arg->m_thread);
				}

				osl_log_debug("func:%s start thread num:%d thread:%x\n",__func__,i,thread_arg->m_thread);
			}
		}
	}

	
	m_dispatch_thread = osl_thread_create( "tTcpDispatch", 100, 3*1024*1024, CTcpGroup::DispatchProc, this, NULL );
	if ( m_dispatch_thread )
	{
		osl_thread_bind_cpu( m_dispatch_thread, cpu_idx );
		osl_thread_resume( m_dispatch_thread );
		//return true;
	}
	else
	{
		osl_log_debug("[CTcpGroup][Start] error !!!\n");
	}
	
	return true;
}

//停止
void CTcpGroup::Stop()
{
	STcpLink link;
	void *position;
	CTcpSession *session;
	SPacket packet;

	if( m_dispatch_thread )
	{
		osl_thread_destroy( m_dispatch_thread, -1 );
		m_dispatch_thread = NULL;
	}
	
	for(int i=0;i<m_work_thread.GetSize();i++)
	{
		if(m_work_thread[i]->m_thread)
		{
			osl_thread_destroy( m_work_thread[i]->m_thread, -1 );
			m_work_thread[i]->m_thread = NULL;
		}
	}

	for(int i=0;i<m_work_thread.GetSize();i++)
	{
		if(m_work_thread[i])
		{
			m_work_thread[i]->m_innerCache.Destroy();
		}

		free(m_work_thread[i]);
		m_work_thread[i] = NULL;
	}

	m_work_thread.RemoveAll();

	if ( m_rwlock )
	{
		osl_rwlock_destroy( m_rwlock );
		m_rwlock = NULL;
	}

	m_outerCache.Destroy();

	while(m_newQueue.Read(&link))
	{
		if (link.skt != -1 )
			osl_socket_destroy( link.skt );
	}
	m_newQueue.Destroy();

	position = m_sessions.GetFirst( &session );
	while( position )
	{
		session->Stop();
		delete session;
		position = m_sessions.GetNext( &session, position );
	}
	m_sessions.RemoveAll();

	if( m_epoll )
	{
		osl_epoll_destroy( m_epoll );
		m_epoll = NULL;
	}

	if(m_inner_cache_cond)
	{
		pthread_cond_destroy( m_inner_cache_cond );
		free(m_inner_cache_cond);
		m_inner_cache_cond = NULL;
	}

	if(m_inner_cache_mutex)
	{
		osl_mutex_destroy(m_inner_cache_mutex);
		m_inner_cache_mutex = NULL;
	}

	if(-1 != m_server_accpet_socket)
	{
		close(m_server_accpet_socket);
		m_server_accpet_socket = -1;
	}

	if(-1 != m_control_fd)
	{
		close(m_control_fd);
		m_control_fd = -1;
	}

	if(-1 != m_group_accpet_socket)
	{
		close(m_group_accpet_socket);
		m_group_accpet_socket = -1;
	}

	if(-1 != m_notify_fd)
	{
		close(m_notify_fd);
		m_notify_fd = -1;
	}

	
	if(m_recv_buf)
	{	
		free(m_recv_buf);
		m_recv_buf = NULL;
	}

	m_innerCache.Destroy();

	m_timers.RemoveAll();
	m_timeout.RemoveAll();
}


//是否已经初始化
bool CTcpGroup::IsStarted()
{
	return m_epoll != NULL;
}

void* CTcpGroup::GetServer()
{
	return m_server;
}

//添加新连接
void CTcpGroup::SetLinkBorn( STcpLink link )
{
	osl_log_debug("func :%s post skt:%d \n",__func__,link.skt);
	m_newQueue.Post( link );
	char buf = 'Z';
	int ret = write(m_server_accpet_socket,&buf,1);
	if(ret != 1)
	{
		if ( errno != EAGAIN )
		{
			printf ( "NOTIFY: write data failed fd %i size %i errno %i '%s'",
				m_server_accpet_socket, ret, errno, strerror(errno) );
		}
	}
}

//要求link死亡
bool CTcpGroup::SetLinkDead( STcpLink link )
{
	bool ret = false;
	CTcpSession* session;

	osl_rwlock_read_lock(m_rwlock);
	if (m_sessions.Search(&link.skt, &session))
	{
		if (link.remote_ip == session->m_link.remote_ip && 
			link.remote_port == session->m_link.remote_port)
		{
			session->SetDead( true );
			ret = true;
		}
	}
	osl_rwlock_read_unlock(m_rwlock);
	return ret;
}

//寻找session
void* CTcpGroup::SearchLink( uint64_t *skt_idx, CTcpSession **session )
{
	void *position = NULL;

	position = m_sessions.Search(skt, session);

	return position;
}

//发送数据，送入缓冲立即返回，已加锁线程安全
bool CTcpGroup::PostData( SPacketHeader& packet_header, char_t *buf, int32_t size )
{
	//printf("PostData:%s\n",buf);
	int ret,ret1;

	if(size > 65536)
	{
		//超过65536 请分包传输,注意设置好PACKET_START 和 PACKET_END
		osl_log_debug("[%s] size:%d too much,please Packet transmission\n",__func__,size);
		return false;
	}
	
	osl_rwlock_write_lock(m_rwlock);
	ret = m_outerCache.Post(packet_header, buf, size);
	osl_rwlock_write_unlock(m_rwlock);
	
	char bufchar = 'Z';
	ret1 = write(m_control_fd,&bufchar,1);
	if(ret1 != 1)
	{
		if ( errno != EAGAIN )
		{
			osl_log_debug ( "NOTIFY: write data failed fd %i size %i errno %i '%s'",
				m_control_fd, ret, errno, strerror(errno) );
		}
	}

	return ret;
}

//设置事件
void CTcpGroup::SetEvent(int32_t fd,int ctrl,int event,void *ptr_param,int fd_param)
{
	SEpollEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.events = event ;//LT 和 ET 都可以，LT更保险一些，因为系统的瓶颈不在这里
	if(ptr_param)
		ev.data.ptr = ptr_param;
	else if(fd_param > 0)
		ev.data.fd = fd_param;
	
	osl_epoll_ctl( m_epoll, ctrl, fd, &ev );
}

bool CTcpGroup::AddRecvBuffer(SPacket& packet)
{
	bool ret = false;
	//osl_log_debug("func:%s skt:%d size:%d\n",__func__,packet.header.link.skt,packet.len);
	//ret = m_recvbufQueue.Post(packet);
	if(packet.len > 65535)
	{
		osl_log_debug("func:%s skt_idx:%llu size:%d overflow\n",__func__,packet.header.link.skt_idx,packet.len);
		return false;
	}
	ret = m_innerCache.Post(packet,packet.buf,packet.len);
	if(ret)
		pthread_cond_signal( m_inner_cache_cond );

	return ret;
}

bool CTcpGroup::AddRecvBufferToThreadQueue(SPacket& packet)//往分配式的每个线程专属的队列里送
{
	bool ret = false;
	int32_t idx = packet.header.link.skt_idx % m_work_thread.GetSize();
	SThreadArg *arg =  m_work_thread[idx];

	if(packet.len > 65535)
	{
		osl_log_debug("func:%s skt_idx:%llu size:%d overflow\n",__func__,packet.header.link.skt_idx,packet.len);
		return false;
	}
	
	if(arg)
	{
		ret = arg->m_innerCache.Post( packet,packet.buf,packet.len );
	}

	return ret;
}

// 
int32_t CTcpGroup::DispatchProc( void* param, void* expend )
{
	return ((CTcpGroup *)param)->OnDispatch();
}

// 
int32_t CTcpGroup::OnDispatch()
{
	int num;
	int wait_ms = -1;
	SEpollEvent events[4096], *pv;
	CTcpSession *session;
	uint32_t now = osl_get_ms();
	STcpLink link;
	SPacketHeader packet_header;
	char_t buf[65536];
	int size;
	void *position = NULL;
	bool dead_flag = false;
	void *nextpos = NULL;
	STimeout timeout;
	int ret=0;

	now = osl_get_ms();

	if(m_lastTick == 0)
		m_lastTick = now;

	//获得wait_ms 的时间
	position = m_timeout.GetFirst(&timeout);
	if(position)
		wait_ms = (int32_t)((int64_t)timeout.currentMS - (int64_t)now);

	//wait_ms 不可能大于60秒，不可能小于0(-1例外)
	if(wait_ms > 60000)
		wait_ms = 60000;

	if(now < m_lastTick || (wait_ms != -1 && wait_ms < 0))//时间回溯
		wait_ms = 0;
	num = osl_epoll_wait( m_epoll, events, sizeof(events)/sizeof(events[0]), wait_ms );
	for( int i=0; i<num; i++ )
	{
		pv = events + i;

		if (pv->events & (OSL_EPOLL_HUP | OSL_EPOLL_ERR))//出错了，必须得删掉
		{
			if(pv->data.fd != m_group_accpet_socket && pv->data.fd != m_notify_fd)
			{
				position = pv->data.ptr;
				session = *m_sessions.GetValue( position ); 
				if( session!=NULL && session->m_link.skt != -1 )
				{
					//有数据接收了，上一次的timer得删掉
					osl_log_debug("[%s] EPOLL_ERR or EPOLL_HUP\n",__func__);
					CancelTimer(session->GetTimerId());
					KillLink(position);
				}
			}
			
		}
		else if( pv->events & OSL_EPOLL_IN)//有数据要接收
		{
			if(m_group_accpet_socket == pv->data.fd)//新的socket
			{
				ReadSocket(m_group_accpet_socket);

				//激活新连接
				while(m_newQueue.Read(&link))
				{
					position = ActivateLink( link );
					if (position == NULL)
					{
						osl_log_error( "[CTcpGroup][OnWork] warn: session NULL\n" );
						osl_socket_destroy( link.skt );
					}
				}
			}
			else if (m_notify_fd == pv->data.fd)//发送队列有数据要发送
			{
				ReadSocket(m_notify_fd);		
				while(true)
				{
					size = m_outerCache.Read(&packet_header, buf, sizeof(buf));
					if (size <= 0)
						break;
					position = m_sessions.Search(&packet_header.link.skt_idx, &session);
					if (position)
					{
						session->HandleBeforeSend(packet_header,buf,size);
						session->OnSend(buf, size, now);
						if(0 < session->m_send_datsize)//没发完，下次接着发
						{
							osl_log_debug("===send_size :%d position:%x skt:%d\n",session->m_send_datsize,position,session->m_link.skt);
							SetEvent(session->m_link.skt,OSL_EPOLL_CTL_MOD,OSL_EPOLL_OUT,position,0);
						}
						else
						{
							//短连接,kill 0x4 表示是最后一个包，因为一个完整的包可能分多个包传输
							if((packet_header.flag&PACKET_END )&&session->GetCloseFlag())
							{
								osl_log_debug("[%s] skt_idx:%llu socket:%d short connection later kill  it\n",__func__,session->m_link.skt_idx,session->m_link.skt);
								CancelTimer(session->GetTimerId());
								//KillLink(position);
								//尽量让客户端主动关闭，若不主动关闭就延迟1秒由服务器关掉
								session->SetTimerId(SetTimer(session->m_link.skt_idx,position,1000));//得重新设置timer
							}
						}
					}
				}//while(true)
			}
			else
			{
				position = pv->data.ptr;
				session = *m_sessions.GetValue( position );
				if( session!=NULL && session->m_link.skt != -1 && session->HandShake())
				{
					//有数据接收了，上一次的timer得删掉
					CancelTimer(session->GetTimerId());
					dead_flag = session->OnRecv(now);
					if(dead_flag)
					{
						osl_log_debug("[%s] skt_idx:%llu socket:%d close by client\n",__func__,session->m_link.skt_idx,session->m_link.skt);
						KillLink(position);
					}
					else
					{
						//重新设置超时
						session->SetTimerId(SetTimer(session->m_link.skt_idx,position));//得重新设置timer
					}
				}
			}
		}
		else if( pv->events & OSL_EPOLL_OUT )//有数据要发送
		{
			position = pv->data.ptr;
			session = *m_sessions.GetValue( position );
			if( session!=NULL && session->m_link.skt != -1 && session->HandShake())
			{
				osl_log_debug("epoll out position:%x skt:%d\n",position,session->m_link.skt);
				session->OnSend(NULL, 0, now);
				if (session->m_send_datsize <= 0)//发送完成，从队列中删除SESSION
				{
						if(session->GetCloseFlag())//短连接,kill
						{
							osl_log_debug("short connection later will kill it\n");
							CancelTimer(session->GetTimerId());
							//KillLink(position);
							//尽量让客户端主动关闭，若不主动关闭就延迟1秒由服务器关掉
							session->SetTimerId(SetTimer(session->m_link.skt_idx,position,1000));//得重新设置timer
						}
						SetEvent(session->m_link.skt,OSL_EPOLL_CTL_MOD,OSL_EPOLL_IN,position,0);
				}
				else
				{
					//没发完，就接着发
					SetEvent(session->m_link.skt,OSL_EPOLL_CTL_MOD,OSL_EPOLL_OUT,position,0);
				}
			}
		}
		
		
	}

	if(now < m_lastTick)//osl_get_ms 时间回溯了
	{
		osl_log_debug("func:%s time backstrace\n",__func__);
		m_timeout.RemoveAll();
		m_timers.RemoveAll();
		//时间回溯了，就全部重新设一遍
		position = m_sessions.GetFirst( NULL );
		while( position )
		{
			nextpos = m_sessions.GetNext(NULL, position);
			session = *m_sessions.GetValue(position);
			session->SetTimerId(SetTimer(session->m_link.skt_idx,position));//得重新设置timer
			position = nextpos;
		}	
	}
	
	HandleTimeout();//处理超时

	m_lastTick = now;

	return 0;
}


//将fd的数据读完
void CTcpGroup::ReadSocket(int32_t fd)
{
	//ret = read ( m_group_accpet_socket, (char *)data, sizeof(data) );
	int32_t ret;
	char_t data[1024];
	
	//if ( (0 > ret) && (errno != EAGAIN) )
	//{
	//	osl_log_debug ( "NOTIFY: read data failed fd %i size %i errno %i '%s'", m_group_accpet_socket, ret, errno, strerror(errno) );
	//}

	while(1)
	{
		ret = read ( fd, (char *)data, sizeof(data) );
		if(ret <= 0)
			break;
	}
}

//获取处理方式
int32_t CTcpGroup::GetModel()
{
	return m_model;
}

//工作线程处理函数
int32_t CTcpGroup::WorkProc( void* param, void* expend )
{
	return ((CTcpGroup *)param)->OnWork();
}

//抢占式处理函数
int32_t CTcpGroup::OnWork()
{
	/*线程处理模型一般有两种,一种是抢占式，一种是分配式，抢占式不能保证消息的顺序性，分配式取模可以保证消息的顺序性（通过对发

送者取模），但是分配式要分配不均匀的话，可能会导致有些线程很忙，有些却很闲，抢占式就可以充分的利用cpu，对于聊天的话，接收

、处理、发送就全部在Dispatch中完成，这样就可以保证有序性,对于普通的http请求，就可以用抢占式*/

	int ret = 1;	
	uint32_t now = osl_get_ms();
	SPacket packet;
	//SPacketHeader packet_header;
	int32_t size; 
	char_t buf[65536];

	osl_mutex_lock(m_inner_cache_mutex,-1);
	size = m_innerCache.Read(&packet, buf, sizeof(buf));
	
	if (size <= 0)
	{		
		//当pthread_cond_signal 触发时，此wait会立即醒来
		pthread_cond_wait( m_inner_cache_cond, (pthread_mutex_t*)m_inner_cache_mutex);	

		size = m_innerCache.Read(&packet, buf, sizeof(buf));
		if ( size <= 0)
		{
			osl_mutex_unlock( m_inner_cache_mutex );
			goto RET;
		}
	}
	osl_mutex_unlock( m_inner_cache_mutex );
		
	if(!m_innerCache.IsEmpty())
	{
		pthread_cond_signal( m_inner_cache_cond );
		//osl_log_debug("signal\n");
	}
	
	if(size != packet.len)
	{
		osl_log_debug("[%s] maybe something error size:%d packet.len:%d \n",__func__,size,packet.len);
		goto RET;
	}

	if(size < sizeof(buf))
		buf[size] = 0;
	packet.buf = buf;
	if(packet.header.mode == PACKET_MODE_HTTP)
		g_service_disp.OnHttpPacekt( GetServer(), this, &packet.header, packet.buf, packet.hlen, packet.clen, now );
	
RET:
	return 0;
}

//工作线程处理函数
int32_t CTcpGroup::WorkProc1( void* param, void* expend )
{
	return ((CTcpGroup *)param)->OnWork1(expend);
}

//分配式处理函数
int32_t CTcpGroup::OnWork1(void *expend)
{
	SPacket packet;
	SThreadArg *arg = (SThreadArg*)expend;
	int32_t cnt = 0;
	uint32_t now = osl_get_ms();
	char_t buf[65536];
	int32_t size;
	
	while(arg)
	{
		size = arg->m_innerCache.Read(&packet,buf,sizeof(buf));
		if(size <= 0)
			break;
		cnt++;

		if(packet.len != size)
		{
			osl_log_error("maybe something error size:%d packet.len\n",size,packet.len);
			continue;
		}

		if(size < sizeof(buf))
			buf[size] = 0;
		packet.buf = buf;
		//osl_log_debug("get buffer hlen:%d clen:%d packet.buf:%x\n",packet.hlen,packet.clen,packet.buf);
		if(packet.header.mode == PACKET_MODE_HTTP)
			g_service_disp.OnHttpPacekt( GetServer(), this, &packet.header, packet.buf, packet.hlen, packet.clen, now );

		if(cnt > 100)
		{
			//最多每次处理100个，防止cpu达到100%
			goto RET;
		}
	}
	
RET:
	return 1;
}

void CTcpGroup::HandleTimeout()
{
	uint32_t time_now = osl_get_ms();
	STimeout timeout;
	STimer timer;
	void *position;
	void *nextpos;
	void *tmp_pos;
	void *session_pos;
	CTcpSession *session;
	
	position = m_timeout.GetFirst(&timeout);
	while(position)
	{
		nextpos = m_timeout.GetNext(NULL, position);
		timeout = *m_timeout.GetValue(position);
		if(timeout.currentMS < time_now)
		{
			//超时了，干掉
			//找到timeid;
			tmp_pos = m_timers.Search(&timeout.timerID,&timer);
			if(tmp_pos)
			{
				session = *m_sessions.GetValue( timer.position );
				if(session!=NULL && session->m_link.skt != -1)
				{
					KillLink( timer.position);
					osl_log_debug("socket idx:%lld timeout kill it\n",timer.skt_idx);
				}
			}

			m_timeout.RemoveByPosition(position);
			position = nextpos;
		}
		else//根据currentMS从小到达排序的
			break;
	}
}


//激活新连接
void* CTcpGroup::ActivateLink( STcpLink link )
{
	CTcpServer *server = (CTcpServer*)m_server;
	void *position;
	CTcpSession *session;
	int32_t num;

	//position = m_sessions.Search( &link.skt, &session );
	//if( position )//出现重复skt，删除旧的
	//{
	//	session->Stop();
	//	m_sessions.RemoveByPosition( position );
	//}
	//else
	num = m_dumps.GetSize();
	if( 0 < num )//寻找一个空闲连接回收利用
	{
		session = m_dumps.GetAt( num-1 );
		m_dumps.RemoveAt( num-1 );
	}
	else//无空闲session，新建
	{
		if (server->m_proc)
		{
			session = server->m_proc(server->m_param);
		}
		else
		{
			session = new CTcpSession();
		}
	}

	session->Start(this, link);
	position = m_sessions.Insert(session);
	if(position)
	{
		session->SetTimerId( SetTimer(link.skt_idx,position));
		SetEvent(link.skt,OSL_EPOLL_CTL_ADD,OSL_EPOLL_IN,position,0);
	}

	return position;
}

//杀死新连接，未加锁内部调用
void CTcpGroup::KillLink( void *position )
{
	CTcpSession *session;

	session = *m_sessions.GetValue( position );
	SetEvent(session->m_link.skt,OSL_EPOLL_CTL_DEL,OSL_EPOLL_IN,position,0);
	session->Stop();
	m_sessions.RemoveByPosition(position);

	m_dumps.Add(session);
}

uint32_t CTcpGroup::SetTimer(uint64_t idx,void *position,int32_t timems)
{
	//加入超时task任务
	STimer timer;
	
	timer.timerID  = m_timerID++;
	timer.skt_idx = idx;
	timer.position = position;
	//task.session_pos = session_pos;

	STimeout timeout;
	timeout.currentMS = osl_get_ms() + timems;//60秒超时
	timeout.timerID = timer.timerID;
	
	m_timers.Insert(timer);
	m_timeout.Insert(timeout);

	//osl_log_debug("add timer %d skt_idx:%llu time:%u\n",timer.timerID,timer.skt_idx,timeout.currentMS);
	return timer.timerID;
}

void CTcpGroup::CancelTimer(uint32_t timerid)
{
	//uint32_t tmp_timerid = timerid
	m_timers.Remove(&timerid);
	//osl_log_debug("caceltime :%u \n",timerid);
}


//m_sessions排序比较函数
int32_t CTcpGroup::CompareSessionCallback(bool item1_is_key, void* item1, void* item2, void *param )
{
	int64_t  idx1, idx2;
	if( item1_is_key )
	{
		idx1 = *(int64_t*)item1;
		idx2 = (*(CTcpSession**)item2)->m_link.skt_idx;
	}
	else
	{
		idx1 = (*(CTcpSession**)item1)->m_link.skt_idx;
		idx2 = (*(CTcpSession**)item2)->m_link.skt_idx;
	}

	if( idx1 < idx2 )
		return -1;
	else if( idx1 == idx2 )
		return 0;
	else
		return 1;
}

int32_t CTcpGroup::CompareTaskByTimerIDCallback(bool item1_is_key, void* item1, void* item2, void *param )
{
	uint32_t timerid1,timerid2;
	if(item1_is_key)
	{
		timerid1 = *(uint32_t*)item1;
		timerid2 = ((STimer*)item2)->timerID;
		if (timerid1 < timerid2)
			return -1;
		else if(timerid1 > timerid2)
			return  1;
		else 
			return 0;
	}
	else 
	{
		timerid1 = ((STimer*)item1)->timerID;
		timerid2 = ((STimer*)item2)->timerID;
		if (timerid1 < timerid2)
			return -1;
		else if(timerid1 > timerid2)
			return  1;
		else 
			return 0;
	}
}


int32_t CTcpGroup::CompareTaskByTimeMsCallback(bool item1_is_key, void* item1, void* item2, void *param )
{
	uint64_t currentms1,currentms2;
	if(item1_is_key)
	{
		currentms1 = *(uint64_t*)item1;
		currentms2 = ((STimeout*)item2)->currentMS;
		if (currentms1 < currentms2)
			return -1;
		else if(currentms1 > currentms2)
			return  1;
		else 
			return 0;
	}
	else 
	{
		currentms1 = ((STimeout*)item1)->currentMS;
		currentms2 = ((STimeout*)item2)->currentMS;
		if (currentms1 < currentms2)
			return -1;
		else if(currentms1 > currentms2)
			return  1;
		else 
			return 0;
	}
}

