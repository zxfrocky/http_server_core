#include "stdafx.h"
#include "osl_epoll.h"
#include "osl_socket.h"
#include "osl_log.h"
#include "osl_thread.h"
#include "osl_mem.h"
#include "osl_url.h"
#include "osl_mutex.h"
#include "TcpSession.h"
#include "TcpServer.h"
#include "TcpGroup.h"
#include "TcpCache.cpp"
#ifdef _OPENSSL
#include "openssl/ssl.h"
#endif

#define __MODULE__ "TcpGroup"

//TCP会话组
CTcpGroup::CTcpGroup()
{
	m_epoll = NULL;
	m_dispatch_thread = NULL;
	//memset(m_work_thread,0,sizeof(m_dispatch_thread));
	m_server = NULL;
	m_rwlock = NULL;
	m_inner_queue_mutex = NULL;
	m_inner_queue_cond = NULL;
	//通过这个socket传递tcpserver accpet到的socket
	m_server_accpet_socket = -1;//server accpet 到soceket 就往这个socket写数据
	m_group_accpet_socket = -1;//和m_server_accpet_socket配套使用
	m_control_fd = -1;//往这个fd写数据表明有数据要发送了
	m_notify_fd = -1;//和m_control_fd配套使用
	//m_recv_buf = NULL;

	m_sessions.SetCompareCallback( CTcpGroup::CompareSessionCallback, this );
	//超时检测相关
	m_timerID = 0;
	m_timers.SetCompareCallback(CTcpGroup::CompareTaskByTimerIDCallback,this);
	m_timeout.SetCompareCallback(CTcpGroup::CompareTaskByTimeMsCallback,this);

	/*xtcsequnce removeByposition 是不会删掉的,而是放到回收链表中,
	如果遇到恶意攻击，xtcsequnce 瞬时会很大,即峰值很大,瞬间就会把内存撑爆,
	这样的话就需要自己定义free函数,这样当removeByposition 时就会立即释放内存*/
	/*
	当出现上述所说情况时,请把这段注释放开,3ks
	m_sessions.SetMemoryCallback( NULL, CTcpGroup::MyFree, this );
	m_timers.SetMemoryCallback( NULL, CTcpGroup::MyFree, this );
	m_timeout.SetMemoryCallback( NULL, CTcpGroup::MyFree, this );
	*/
	
	m_max_sessions_size = 0;
	m_max_timers_size = 0;
	m_max_timeout_size = 0;
	m_max_dump_session_size = 0;
}


CTcpGroup::~CTcpGroup()
{
	Stop();
}

void* CTcpGroup::MyMalloc(int32_t size,void *param)
{
	return (void*)XTC_MALLOC(size);
}

void* CTcpGroup::MyFree(void *buf,void *param)
{
	XTC_FREE(buf);
	return buf;
}

//启动服务
bool CTcpGroup::Start( void *server,int32_t cpu_num, SGroupParam *grp_param)
{
	SThreadArg *thread_arg;
	static int32_t thread_cpu_idx = 0;
	int fd[2];

	m_model = grp_param->model;
	
	m_newQueue.Create(8196);
	//m_outer_queue.Create( group_cache_size*1024*1024 );//外部数据发送缓冲区
	m_outer_queue.Create(grp_param->group_queue_size);

	m_epoll = osl_epoll_create( 65536 );
	m_server= server;
	//m_need_response = need_response;
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

//	thread_num = thread_num < MAX_WORK_THERAD_NUM ? thread_num : MAX_WORK_THERAD_NUM;
//	time_order_thread_num = time_order_thread_num < MAX_WORK_THERAD_NUM ? time_order_thread_num : MAX_WORK_THERAD_NUM;
	
	if(grp_param->model != 0x01 || grp_param->model != 0x02 || grp_param->model != 0x03)
	{
		grp_param->model = 0x01;//默认就是抢占式
	}
	/*如果work_thread_num 不大于0 ，表明所有的任务都在Dispatch 线程处理*/
	
	if(m_model & 0x01 )
	{
		//抢占式
		if(grp_param->thread_num > 0)
		{
			m_inner_queue.Create(grp_param->group_queue_size);//50万够了吧
			m_inner_queue_mutex = osl_mutex_create();//m_inner_queue 锁
			m_inner_queue_cond = (pthread_cond_t*)malloc(sizeof(pthread_cond_t));//与m_inner_queue_mutex 配套使用
			pthread_cond_init( m_inner_queue_cond, NULL );

			for(int i = 0;i< grp_param->thread_num ;i++)
			{
				thread_arg = (SThreadArg*)malloc(sizeof(SThreadArg));
				memset(thread_arg,0,sizeof(SThreadArg));
				thread_arg->mode = 0x01;
				thread_arg->m_thread = osl_thread_create( "tTcpGroup", 100, grp_param->thread_stack_size, CTcpGroup::WorkProc, this, NULL );
				if(thread_arg->m_thread)
				{
					m_work_thread.Add(thread_arg);
					osl_thread_bind_cpu( thread_arg->m_thread, (thread_cpu_idx++)%cpu_num );
					osl_thread_resume(thread_arg->m_thread);
				}

				osl_log_debug("[%s][%s] start thread num:%d thread:%x\n",__MODULE__,__FUNCTION__,i,thread_arg->m_thread);
			}
		}
	}
	if(m_model & 0x2)
	{
		//分配式
		if(grp_param->time_order_thread_num > 0)
		{
			for(int i = 0;i< grp_param->time_order_thread_num ;i++)
			{
				thread_arg = (SThreadArg*)malloc(sizeof(SThreadArg));
				memset(thread_arg,0,sizeof(SThreadArg));
				thread_arg->mode = 0x02;
				thread_arg->m_inner_queue.Create(grp_param->thread_queue_size);//
				thread_arg->m_inner_queue_mutex = osl_mutex_create();//
				thread_arg->m_inner_queue_cond = (pthread_cond_t*)malloc(sizeof(pthread_cond_t));//与m_inner_queue_mutex 配套使用
				thread_arg->m_handle_cnt = 0;
				thread_arg->m_thread = osl_thread_create( "tTcpGroup1", 100, grp_param->thread_stack_size, CTcpGroup::WorkProc1, this, thread_arg );
				if(thread_arg->m_thread)
				{
					m_order_work_thread.Add(thread_arg);
					osl_thread_bind_cpu( thread_arg->m_thread, (thread_cpu_idx++)%cpu_num );
					osl_thread_resume(thread_arg->m_thread);
				}

				osl_log_debug("[%s][%s] start thread num:%d thread:%x\n",__MODULE__,__FUNCTION__,i,thread_arg->m_thread);
			}
		}
	}
	
	m_dispatch_thread = osl_thread_create( "tTcpDispatch", 100, grp_param->thread_stack_size, CTcpGroup::DispatchProc, this, NULL );
	if ( m_dispatch_thread )
	{
		osl_thread_bind_cpu( m_dispatch_thread, (thread_cpu_idx++)%cpu_num );
		osl_thread_resume( m_dispatch_thread );
		//return true;
	}
	else
	{
		osl_log_debug("[%s][%s] error !!!\n",__MODULE__,__FUNCTION__);
	}
	
	return true;
}

//停止
void CTcpGroup::Stop()
{
    STcpLink link;
    void *position;
    CTcpSession *session;
	bool read_flag;
	SOutPkt *pkt;
	SInPkt *in_pkt;
    
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
        if(m_order_work_thread[i]->m_thread)
        {
            osl_thread_destroy( m_order_work_thread[i]->m_thread, -1 );
            m_order_work_thread[i]->m_thread = NULL;
        }
    }

	for(int i=0;i<m_work_thread.GetSize();i++)
	{
		if(m_work_thread[i])
		{
			m_work_thread[i]->m_inner_queue.Destroy();
			pthread_cond_destroy( m_work_thread[i]->m_inner_queue_cond );
			free(m_work_thread[i]->m_inner_queue_cond);
			m_work_thread[i]->m_inner_queue_cond = NULL;

			osl_mutex_destroy(m_work_thread[i]->m_inner_queue_mutex);
			m_work_thread[i]->m_inner_queue_mutex = NULL;
		}

		free(m_work_thread[i]);
		m_work_thread[i] = NULL;
	}

	for(int i=0;i<m_order_work_thread.GetSize();i++)
	{
		if(m_order_work_thread[i])
		{
			m_order_work_thread[i]->m_inner_queue.Destroy();
		}

		free(m_order_work_thread[i]);
		m_order_work_thread[i] = NULL;
	}

	m_work_thread.RemoveAll();

	if ( m_rwlock )
	{
		osl_rwlock_destroy( m_rwlock );
		m_rwlock = NULL;
	}

	read_flag = m_outer_queue.Read(&pkt);
	while(read_flag)
	{
		if(pkt->buf)
			free(pkt);
		free(pkt);
		read_flag = m_outer_queue.Read(&pkt);
	}
	m_outer_queue.Destroy();

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

	if(m_inner_queue_cond)
	{
		pthread_cond_destroy( m_inner_queue_cond );
		free(m_inner_queue_cond);
		m_inner_queue_cond = NULL;
	}

	if(m_inner_queue_mutex)
	{
		osl_mutex_destroy(m_inner_queue_mutex);
		m_inner_queue_mutex = NULL;
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

	read_flag = m_inner_queue.Read(&in_pkt);
	while(read_flag)
	{
		if(in_pkt->buf)
			free(in_pkt);
		free(in_pkt);
		read_flag = m_inner_queue.Read(&in_pkt);
	}
	m_inner_queue.Destroy();

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
bool CTcpGroup::SetLinkBorn( STcpLink link )
{
    //osl_log_debug("func :%s post skt:%d \n",__FUNCTION__,link.skt);
    
    bool result = m_newQueue.Post( link );
	if(!result)
	{
		osl_log_error("[%s][%d] full\n",__FUNCTION__,__LINE__);
		return false;
	}
    char buf = 'Z';
    int ret = write(m_server_accpet_socket,&buf,1);
    if(ret != 1)
    {
        if ( errno != EAGAIN )
        {
            osl_log_debug ( "[%s][%s] NOTIFY: write data failed fd %i size %i errno %i '%s'",
                           __MODULE__,__FUNCTION__, m_server_accpet_socket, ret, errno, strerror(errno) );
        }
    }

	return true;
}

//发送数据，送入缓冲立即返回，已加锁线程安全
bool CTcpGroup::PostData( SPacketHeader& packet_header, char_t *buf, int32_t size )
{
	//printf("PostData:%s\n",buf);
	int ret,ret1;
	SOutPkt *pkt = NULL; 

	if(size > 65536)
	{
		//超过65536 请分包传输,注意设置好PACKET_START 和 PACKET_END
		osl_log_debug("[%s][%s] size:%d too much,please Packet transmission\n",__MODULE__,__FUNCTION__,size);
		return false;
	}

	pkt = (SOutPkt*)malloc(sizeof(SOutPkt)); 
	pkt->header = packet_header;
	pkt->buflen = size;
	pkt->buf = (char_t*)malloc(pkt->buflen);
	if(NULL == pkt->buf)
	{
		osl_log_error("[%s][%s] no enough memory\n",__MODULE__,__FUNCTION__);
		return false;
	}
	memcpy(pkt->buf,buf,pkt->buflen);
	osl_rwlock_write_lock(m_rwlock);
	ret = m_outer_queue.Post(pkt);
	osl_rwlock_write_unlock(m_rwlock);

	if(!ret)
	{
		osl_log_error("[%s][%s] skt_idx:%lld outcache full,handle slow or cache too small\n",
			__MODULE__,__FUNCTION__,packet_header.link.skt_idx);

		free(pkt->buf);
		free(pkt);

		return false;
	}
	
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

bool CTcpGroup::AddRecvBuffer(SInPkt* pkt)
{
	bool ret = false;

	if(pkt->len > 65536)
	{
		osl_log_debug("[%s][%s] skt_idx:%llu size:%d overflow\n",__MODULE__,__FUNCTION__,pkt->header.link.skt_idx,pkt->len);
		return false;
	}
	ret = m_inner_queue.Post(pkt);
	if(ret)
		pthread_cond_signal( m_inner_queue_cond );

	return ret;
}

bool CTcpGroup::AddRecvBufferToThreadQueue(SInPkt* pkt)//往分配式的每个线程专属的队列里送
{
	bool ret = false;
	int32_t size = m_order_work_thread.GetSize();
	int32_t idx;
	SThreadArg *arg;

	if(size <= 0)
		return false;
	
	idx = pkt->header.link.skt_idx % size;
	arg =  m_order_work_thread[idx];

	if(pkt->len > 65536)
	{
		osl_log_debug("[%s][%s] skt_idx:%llu size:%d overflow\n",__MODULE__,__FUNCTION__,pkt->header.link.skt_idx,pkt->len);
		return false;
	}
	
	if(arg)
	{
		ret = arg->m_inner_queue.Post(pkt);
		if(ret)
			pthread_cond_signal( arg->m_inner_queue_cond );
		else
			osl_log_debug("[%s][%s] skt_idx:%lld innercache full,handle slow or cache too small\n",__MODULE__,__FUNCTION__,pkt->header.link.skt_idx);
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
    STcpLink link;
    SOutPkt *out_pkt;
    bool read_flag = false;
    void *position = NULL;
    bool dead_flag = false;
    STimeout timeout;

	UpdateTime();
	//获得wait_ms 的时间
	position = m_timeout.GetFirst(&timeout);
	if(position)
	{
		wait_ms = (int32_t)((uint64_t)timeout.currentMS - (uint64_t)m_currentMS);
		if(wait_ms < 0)
			wait_ms = 0;
	}

	//wait_ms 不可能大于60秒，不可能小于0(-1例外)
	if(wait_ms > 60000)
		wait_ms = 60000;


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
					osl_log_debug("[%s][%s] skt_idx:%lld EPOLL_ERR or EPOLL_HUP\n",__MODULE__,__FUNCTION__,session->m_link.skt_idx);
					CancelTimer(session->GetTimerId());
					KillLink(position);
				}
			}
			else
			{
				osl_log_debug("[%s][%s]it is not happen\n",__MODULE__,__FUNCTION__);
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
						osl_log_error( "[%s][%s] warn: session NULL\n",__MODULE__,__FUNCTION__ );
						osl_socket_destroy( link.skt );
					}
				}
			}
			else if (m_notify_fd == pv->data.fd)//发送队列有数据要发送
			{
				ReadSocket(m_notify_fd);		
				while(true)
				{
					read_flag = m_outer_queue.Read(&out_pkt);
					if (!read_flag)
						break;
					position = m_sessions.Search(&out_pkt->header.link.skt_idx, &session);
					if (position)
					{
						session->HandleBeforeSend(out_pkt->header,out_pkt->buf,out_pkt->buflen);
						dead_flag = session->OnSend(out_pkt->buf, out_pkt->buflen, m_currentMS);
						if(dead_flag)
						{
							osl_log_debug("[%s][%s] skt_idx:%llu socket:%d send error close it\n",__MODULE__,__FUNCTION__,session->m_link.skt_idx,session->m_link.skt);
							CancelTimer(session->GetTimerId());
							KillLink(position);
						}
						else
						{
							if(session->IsNeedSendData())//没发完，下次接着发
							{
								//osl_log_debug("===send_size :%d position:%x skt:%d\n",session->m_send_datsize,position,session->m_link.skt);
								SetEvent(session->m_link.skt,OSL_EPOLL_CTL_MOD,OSL_EPOLL_OUT,position,0);
							}
							else
							{
								//短连接,kill 0x4 表示是最后一个包，因为一个完整的包可能分多个包传输
								if((out_pkt->header.flag&PACKET_END )&&session->GetCloseFlag())
								{
									//osl_log_debug("[%s][%s] skt_idx:%llu socket:%d short connection later kill  it\n",__MODULE__,__FUNCTION__,session->m_link.skt_idx,session->m_link.skt);
									CancelTimer(session->GetTimerId());
									//KillLink(position);
									//尽量让客户端主动关闭，若不主动关闭就延迟1秒由服务器关掉
									session->SetTimerId(SetTimer(session->m_link.skt_idx,position,1000));//得重新设置timer
								}
							}
						}
					}

					if(out_pkt->buf)
						free(out_pkt->buf);
					free(out_pkt);
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
					dead_flag = session->OnRecv(m_currentMS);
					if(dead_flag)
					{
						osl_log_debug("[%s][%s] skt_idx:%llu socket:%d close by client\n",__MODULE__,__FUNCTION__,session->m_link.skt_idx,session->m_link.skt);
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
				osl_log_debug("[%s][%d] epoll out skt_idx:%llu skt:%d\n",__MODULE__,__FUNCTION__,session->m_link.skt_idx,session->m_link.skt);
				dead_flag = session->OnSend(NULL, 0, m_currentMS);
				if(dead_flag)
				{
					osl_log_debug("[%s][%s] skt_idx:%llu socket:%d send error close it\n",__MODULE__,__FUNCTION__,session->m_link.skt_idx,session->m_link.skt);
					CancelTimer(session->GetTimerId());
					KillLink(position);
				}
				else
				{
					if (!session->IsNeedSendData())//发送完成，从队列中删除SESSION
					{
						if(session->GetCloseFlag())//短连接,kill
						{
							osl_log_debug("[%s][%s] skt_idx:%llu short connection later will kill it\n",__MODULE__,__FUNCTION__,session->m_link.skt_idx);
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
		
		
	}
	
	HandleTimeout();//处理超时

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

//更新时间
void CTcpGroup::UpdateTime()
{
	gettimeofday(&m_time_val, 0);
    m_currentMS = (uint64_t)m_time_val.tv_sec * 1000 + (long long)m_time_val.tv_usec / 1000;
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

送者取模），但是分配式要分配不均匀的话，可能会导致有些线程很忙，有些却很闲，抢占式就可以充分的利用cpu,对于普通的http请求，就可以用抢占式*/
    SInPkt *pkt;
    bool read_flag = false; 
    //char_t buf[65536];
	CTcpServer *server = (CTcpServer*)m_server;

	UpdateTime();
    osl_mutex_lock(m_inner_queue_mutex,-1);
    read_flag = m_inner_queue.Read(&pkt);
    
    if (!read_flag)
    {       
        //当pthread_cond_signal 触发时，此wait会立即醒来
        pthread_cond_wait( m_inner_queue_cond, (pthread_mutex_t*)m_inner_queue_mutex);  

        read_flag = m_inner_queue.Read(&pkt);
        if (!read_flag)
        {
            osl_mutex_unlock( m_inner_queue_mutex );
            goto RET;
        }
    }
    osl_mutex_unlock( m_inner_queue_mutex );
        
    if(!m_inner_queue.GetCount())
    {
        pthread_cond_signal( m_inner_queue_cond );
        //osl_log_debug("signal\n");
    }

	if(server)
		server->m_pkt_proc(server,this, &pkt->header, pkt->buf, pkt->hlen, pkt->clen,pkt->keepalive,m_currentMS,server->m_pkt_param);

	if(pkt->buf)
		free(pkt->buf);
	free(pkt);
	
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
	SInPkt *pkt;
	SThreadArg *arg = (SThreadArg*)expend;
	bool read_flag;
	CTcpServer *server = (CTcpServer*)m_server;
	int32_t ret = 0;
	struct timespec timeouttime;

	osl_mutex_lock(arg->m_inner_queue_mutex,-1);
	read_flag = arg->m_inner_queue.Read(&pkt);
	if(!read_flag)
	{
		//当pthread_cond_signal 触发时，此wait会立即醒来
        //pthread_cond_wait( arg->m_inner_queue_cond, (pthread_mutex_t*)(arg->m_inner_queue_mutex));  
        UpdateTime();
		timeouttime.tv_sec = m_time_val.tv_sec + 1;//1秒超时
		timeouttime.tv_nsec = m_time_val.tv_usec * 1000;
        pthread_cond_timedwait(arg->m_inner_queue_cond, (pthread_mutex_t*)(arg->m_inner_queue_mutex),&timeouttime);
		arg->m_handle_cnt = 0;
        read_flag = arg->m_inner_queue.Read(&pkt);
		
        if ( !read_flag)
        {
            osl_mutex_unlock( arg->m_inner_queue_mutex );
			ret = 1;
            goto RET;
        }
	}
	osl_mutex_unlock( arg->m_inner_queue_mutex );
	arg->m_handle_cnt++;
	
	UpdateTime();

	if(server)
		server->m_pkt_proc(server,this, &pkt->header, pkt->buf, pkt->hlen, pkt->clen,pkt->keepalive,m_currentMS,server->m_pkt_param);

	if(arg->m_handle_cnt > 1000)//连续处理了1000个请求,以防cpu太高,强制休息1ms
	{
		arg->m_handle_cnt = 0;
		ret = 1;
	}

	if(pkt->buf)
		free(pkt->buf);
	free(pkt);
RET:
	return ret;
}

void CTcpGroup::HandleTimeout()
{
	STimeout timeout;
	STimer timer;
	void *position;
	void *nextpos;
	void *tmp_pos;
	CTcpSession *session;
	
	position = m_timeout.GetFirst(&timeout);
	while(position)
	{
		nextpos = m_timeout.GetNext(NULL, position);
		timeout = *m_timeout.GetValue(position);
		if(timeout.currentMS < m_currentMS)
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
					osl_log_debug("[%s][%s]socket idx:%lld timeout kill it\n",__MODULE__,__FUNCTION__,timer.skt_idx);
				}

				//删掉此timerId
				m_timers.RemoveByPosition(tmp_pos);
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

	if(NULL == session)
	{
		osl_log_debug("[%s][%s][%d] session new fail \n",__MODULE__,__FUNCTION__,__LINE__);
		return NULL;
	}

	session->Start(this, link);
	position = m_sessions.Insert(session);
	if(position)
	{
		session->SetTimerId( SetTimer(link.skt_idx,position,5000));//第一次socket连接时 5秒内要发数据
		SetEvent(link.skt,OSL_EPOLL_CTL_ADD,OSL_EPOLL_IN,position,0);
	}

	if(m_sessions.GetSize() > m_max_sessions_size)
		m_max_sessions_size = m_sessions.GetSize();
	
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

	//m_dumps.Add(session);
	delete session;

	if(m_dumps.GetSize() > m_max_dump_session_size)
		m_max_dump_session_size = m_dumps.GetSize() ;
}

uint64_t CTcpGroup::SetTimer(uint64_t idx,void *position,int32_t timems)
{
	//加入超时task任务
	STimer timer;
		
	timer.timerID  = m_timerID++;
	timer.skt_idx = idx;
	timer.position = position;
	//task.session_pos = session_pos;

	STimeout timeout;
	UpdateTime();
	timeout.currentMS = m_currentMS + timems;//60秒超时
	timeout.timerID = timer.timerID;
	
	timer.timeout_pos = m_timeout.Insert(timeout);
	m_timers.Insert(timer);

	if(m_timers.GetSize() > m_max_timers_size)
		m_max_timers_size = m_timers.GetSize();

	if(m_timeout.GetSize() > m_max_timeout_size)
		m_max_timeout_size = m_timeout.GetSize();

	//osl_log_debug("add timer %d skt_idx:%llu time:%u\n",timer.timerID,timer.skt_idx,timeout.currentMS);
	return timer.timerID;
}

void CTcpGroup::CancelTimer(uint64_t timerid)
{
	//m_timers.Remove(&timerid);
	STimer timer;
	void *pos;
	
	pos = m_timers.Search(&timerid,&timer);
	if(pos)
	{
		m_timers.RemoveByPosition(pos);
		if(timer.timeout_pos)
			m_timeout.RemoveByPosition(timer.timeout_pos);
	}
}


//m_sessions排序比较函数
int32_t CTcpGroup::CompareSessionCallback(bool item1_is_key, void* item1, void* item2, void *param )
{
	uint64_t  idx1, idx2;
	if( item1_is_key )
	{
		idx1 = *(uint64_t*)item1;
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
	uint64_t timerid1,timerid2;
	if(item1_is_key)
	{
		timerid1 = *(uint64_t*)item1;
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

	if(item1_is_key)
	{
		if(*(uint64_t*)item1 < ((STimeout*)item2)->currentMS)
			return -1;
		else if(*(uint64_t*)item1 > ((STimeout*)item2)->currentMS)
			return 1;
		else
			return 0;
	}
	else 
	{
		if (((STimeout*)item1)->currentMS < ((STimeout*)item2)->currentMS)
			return -1;
		else if(((STimeout*)item1)->currentMS > ((STimeout*)item2)->currentMS)
			return  1;
		else 
			return 0;
	}
}

void CTcpGroup::DisplayStatus(char_t *buf,int32_t bufsize)
{
	osl_str_snprintf(buf,bufsize,"{\"session_num\":%d,\"timers_num\":%d,\"timeout_num\":%d,\"dump_size\":%d,"
									"\"max_session_num\":%d,\"max_tiemrs_num\":%d,\"max_timeout_num\":%d,\"max_dump_size\":%d}",
									m_sessions.GetSize(),m_timers.GetSize(),m_timeout.GetSize(),m_dumps.GetSize(),
									m_max_sessions_size,m_max_timers_size,m_max_timeout_size,m_max_dump_session_size);
}