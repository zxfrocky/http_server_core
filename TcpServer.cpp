#include "stdafx.h"
#include "osl_epoll.h"
#include "osl_socket.h"
#include "osl_log.h"
#include "osl_thread.h"
#include "osl_mutex.h"
#include "osl_string.h"
#include "Xtc.h"
#include "XtcArray.h"
#include "TcpServer.h"
#include "TcpGroup.h"
#ifdef _OPENSSL
#include "openssl/ssl.h"
#endif

#define __MODULE__ "TcpServer"

//TCP会话服务器
CTcpServer::CTcpServer()
{
#ifdef _OPENSSL
	m_ssl_ctx = NULL;
#endif

	memset(&m_statistics, 0, sizeof(m_statistics));

	m_thread = NULL;
	m_proc = NULL;
	m_param = NULL;
	m_pkt_proc = NULL;
	m_pkt_param = NULL;
	m_skt_idx = 0;
	m_epoll = NULL;
}


CTcpServer::~CTcpServer()
{
	Release();
}


/*启动服务*/
bool CTcpServer::Initialize( SListenParam *listen_param,int32_t listen_param_size,
		SGroupParam group_param, PTcpNewCallback proc, void *param,PHandlePacket pkt_proc,void *pkt_proc_param)	
{
	int32_t i, cpu_num;
	CTcpGroup *grp = NULL;
	uint32_t unblock = 1;
	int32_t flag = 1;
	//int32_t size = 65536*5;
	SOCKET skt;
	uchar_t *p;
	bool init = false;

#ifdef _OPENSSL
	m_ssl_ctx = NULL;
#endif
	memset(&m_statistics, 0, sizeof(m_statistics));
	m_thread = NULL;
	m_proc = proc;
	m_param = param;
	m_pkt_proc = pkt_proc;
	m_pkt_param = pkt_proc_param;

	for(int32_t i=0;i<listen_param_size;i++)
	{
		if(listen_param[i].ssl)
		{
#ifdef _OPENSSL
			if(m_ssl_ctx == NULL)
				m_ssl_ctx = SSL_CTX_new( SSLv23_server_method() );
			if( m_ssl_ctx == NULL )
			{
				osl_log_error("[%s][%s] create SSL_CTX_new error !!!\n",__MODULE__,__FUNCTION__);
				goto ERROR_EXIT;
			}
			else if(!init)
			{
				init = true;
				SSL_CTX_load_verify_locations( m_ssl_ctx, "./config", NULL );
				SSL_CTX_set_verify_depth( m_ssl_ctx, 1 );

				SSL_CTX_set_verify( m_ssl_ctx, SSL_VERIFY_NONE, NULL );

				//证书文件口令，用于保护证书文件
				SSL_CTX_set_default_passwd_cb_userdata( m_ssl_ctx, (void*)"pass phrase" );
				//SSL_CTX_set_default_passwd_cb_userdata( m_ssl_ctx, (void*)"123456" );
				//读取证书文件
				if( SSL_CTX_use_certificate_chain_file( m_ssl_ctx, "./config/certificate.crt" ) <= 0 )
				//if( SSL_CTX_use_certificate_chain_file( m_ssl_ctx, "./config/server.crt" ) <= 0 )
				{
					osl_log_error("[%s][%s] crt error\n",__MODULE__,__FUNCTION__);
					goto ERROR_EXIT;
				}
				//读取密钥文件
				if( SSL_CTX_use_PrivateKey_file(m_ssl_ctx,"./config/certificate.crt",SSL_FILETYPE_PEM) <= 0 )
				//if( SSL_CTX_use_PrivateKey_file(m_ssl_ctx,"./config/server.key",SSL_FILETYPE_PEM) <= 0 )
				{
					osl_log_error("[%s][%s] privkey key error\n",__MODULE__,__FUNCTION__);
					goto ERROR_EXIT;
				}
				//验证密钥是否与证书一致
				if( !SSL_CTX_check_private_key( m_ssl_ctx) )
				{
					osl_log_error("[%s][%s] key error\n",__MODULE__,__FUNCTION__);
					goto ERROR_EXIT;
				}
		//		SSL_CTX_set_mode( m_ssl_ctx, SSL_MODE_AUTO_RETRY);
			}
#endif
		}

		//创建Socket, 使用TCP协议
		skt = osl_socket_create( AF_INET, SOCK_STREAM, 0 );
		if( skt == -1 )
		{
			osl_log_error("[%s][%s] create socket error!\n",__MODULE__,__FUNCTION__);
			goto ERROR_EXIT;
		}

	    //非阻塞方式
	    osl_socket_ioctl( skt, FIONBIO, &unblock );
		//如果上次运行异常退出，端口可能出于TIME_WAIT状态，无法直接使用，设置SO_REUSEADDR可保证bind()成功
		osl_socket_set_opt( skt, SOL_SOCKET, SO_REUSEADDR, (char*)&flag, sizeof(flag) );
		//osl_socket_set_opt( skt, SOL_SOCKET, SO_SNDBUF, (char *)&size, sizeof(size) );
		//osl_socket_set_opt( skt, SOL_SOCKET, SO_RCVBUF, (char *)&size, sizeof(size) );

				//绑定接收地址和端口
		if( osl_socket_bind( skt, listen_param[i].listen_ip,  listen_param[i].listen_port ) != 0 )
		{
			osl_log_error("[%s][%s] bind error %d %d!!!\n",__MODULE__,__FUNCTION__,listen_param[i].listen_ip,listen_param[i].listen_port);
			goto ERROR_EXIT;
		}

		//启动侦听
		if( osl_socket_listen( skt, SOMAXCONN ) != 0 )
		{
			osl_log_error("[%s][%s] listen error !!!\n",__MODULE__,__FUNCTION__);
			goto ERROR_EXIT;
		}

		listen_param[i].listen_skt = skt;
		m_listen_param_array.Add(listen_param[i]);
		p = (uchar_t*)&listen_param[i].listen_ip;
		osl_log_debug( "[%s][%s] start http service at %d.%d.%d.%d:%d \n",__MODULE__,__FUNCTION__
			,p[0], p[1], p[2], p[3], ntohs(listen_param[i].listen_port) );
	}

	

	cpu_num = osl_get_cpu_count();
	if(cpu_num <= 0 )
		cpu_num = 4;
	if (group_param.thread_num == 0 )
		group_param.thread_num = cpu_num;// * 3;
	if(group_param.group_num == 0 )
		group_param.group_num = 4;
	if(group_param.time_order_thread_num == 0)
		group_param.time_order_thread_num = 4;
	if(group_param.thread_stack_size <= 0)
		group_param.thread_stack_size = 256*1024;

	group_param.thread_num = group_param.thread_num < MAX_WORK_THERAD_NUM ? group_param.thread_num : MAX_WORK_THERAD_NUM;
	group_param.time_order_thread_num = group_param.time_order_thread_num < MAX_WORK_THERAD_NUM ? group_param.time_order_thread_num : MAX_WORK_THERAD_NUM;

    osl_log_debug("[%s][%s]  mode:0x%x group_num:%d cpu_num:%d thread_num:%d time_order_thread_num:%d group_queue_size:%d thread_queue_size:%d\n",__MODULE__,__FUNCTION__,
		group_param.model,group_param.group_num,cpu_num,group_param.thread_num,group_param.time_order_thread_num,group_param.group_queue_size,group_param.thread_queue_size);
    m_groups.SetSize(group_param.group_num, cpu_num);
    for(i=0; i<group_param.group_num; i++)
    {
        grp = new CTcpGroup();
        if (grp != NULL)
        {
            grp->Start( this, cpu_num,&group_param);
            m_groups[i] = grp;
        }
        else
        {
            osl_log_error("[%s][%s] new CTcpGroup error !!!\n",__MODULE__,__FUNCTION__);
        }
    }

	m_epoll = osl_epoll_create(m_listen_param_array.GetSize());
	for(int32_t i=0;i< m_listen_param_array.GetSize();i++)
	{
		SEpollEvent ev;
		memset(&ev, 0, sizeof(ev));
		ev.events = OSL_EPOLL_IN;
		ev.data.fd = i;
		osl_epoll_ctl(m_epoll,OSL_EPOLL_CTL_ADD,m_listen_param_array[i].listen_skt,&ev);
	}

	m_thread = osl_thread_create( "tTcpServerListen", 200, group_param.thread_stack_size, CTcpServer::ListenProc, this, NULL );
	if ( m_thread )
	{
		osl_thread_bind_cpu( m_thread, 0 );
		osl_thread_resume( m_thread );
	}
	else
	{
		osl_log_error("[%s][%s] osl_thread_create error !!!\n",__MODULE__,__FUNCTION__);
	}

	osl_log_error("[%s][%s] success !!!\n",__MODULE__,__FUNCTION__);
	return true;

ERROR_EXIT:
	osl_log_error("[%s][%s] error !!!\n",__MODULE__,__FUNCTION__);
	Release();
	return false;
}

//停止服务
void CTcpServer::Release()
{
    CTcpGroup *grp;
    int32_t i;
	SEpollEvent ev;

    if( m_thread )
    {
        osl_thread_destroy( m_thread, -1 );
        m_thread = NULL;
    }
    for(i=0; i<m_groups.GetSize(); i++)
    {
        grp = m_groups[i];
        grp->Stop();
        delete grp;
    }
    m_groups.RemoveAll();

#ifdef _OPENSSL
    if( m_ssl_ctx )
    {
        SSL_CTX_free( m_ssl_ctx );
        m_ssl_ctx = NULL;
    }
#endif

	for(i = 0;i<m_listen_param_array.GetSize();i++)
	{
		if(m_listen_param_array[i].listen_skt != -1)
		{
			memset(&ev, 0, sizeof(ev));
			ev.events = OSL_EPOLL_IN;
			osl_epoll_ctl(m_epoll,OSL_EPOLL_CTL_DEL,m_listen_param_array[i].listen_skt,&ev);
			osl_socket_destroy( m_listen_param_array[i].listen_skt );
		}
		m_listen_param_array[i].listen_skt = -1;
	}
	m_listen_param_array.RemoveAll();
	if(m_epoll)
	{
		osl_epoll_destroy(m_epoll);
		m_epoll = NULL;
	}
}

//是否已经初始化
bool CTcpServer::IsInitialized()
{
	return m_thread != NULL;
}

//发送数据，送入缓冲立即返回，已加锁线程安全
bool CTcpServer::PostData( STcpLink link, char_t *buf, int32_t size )
{
	CTcpGroup *grp;
	SPacketHeader header;

	memset(&header,0,sizeof(header));
	header.link = link;
	header.flag = PACKET_START;
	
	grp = m_groups[link.skt_idx % m_groups.GetSize()];
	grp->PostData( header, buf, size );
	return true;
}

//取得统计信息
void CTcpServer::GetStatistics( STcpServerStatistics *sta )
{
	int32_t i;

	*sta = m_statistics;
	sta->session_cnt = 0;
	for (i=0; i<m_groups.GetSize(); i++)
	{
		sta->session_cnt += (m_groups[i]->m_sessions).GetSize();
	}
}

//添加统计信息
void CTcpServer::AddStatistics( STcpServerStatistics& sta )
{
	m_statistics.send_size	+= sta.send_size;
	m_statistics.recv_size	+= sta.recv_size;
	m_statistics.cmd_cnt	+= sta.cmd_cnt;
	m_statistics.connect_cnt+= sta.connect_cnt;
}

//监听处理函数
int32_t CTcpServer::ListenProc( void* param, void* expend )
{
	return ((CTcpServer*)param)->OnListen();
}

int32_t CTcpServer::OnListen()
{
    SOCKET skt;
    uint32_t remote_ip;
    uint16_t remote_port;
    CTcpGroup *grp = NULL;
    STcpLink link;
	SEpollEvent events[64] ,*pv;
	int num;
	int32_t index;
	int32_t cnt;
	
	num = osl_epoll_wait( m_epoll, events, sizeof(events)/sizeof(events[0]),60000);
	for( int i=0; i<num; i++ )
	{
		pv = events + i;	
		index = pv->data.fd;

		cnt = 0;
		while(1)
		{
			skt = osl_socket_accept( m_listen_param_array[index].listen_skt, &remote_ip, &remote_port );
			if( skt == -1 )
				break;
			//osl_log_debug(">>>>>>>>>>>>accept=skt=%d index:%d model:%d\n",skt,index,m_listen_param_array[index].model);

			m_statistics.connect_cnt++;
			m_statistics.session_cnt++;

			memset(&link, 0, sizeof(link));
			link.ssl = m_listen_param_array[index].ssl;
			link.skt = skt;
			link.remote_ip = remote_ip;
			link.remote_port = remote_port;
			link.skt_idx = m_skt_idx;
			link.need_response = m_listen_param_array[index].need_response;
			link.model = m_listen_param_array[index].model;
			grp = m_groups[m_skt_idx%m_groups.GetSize()];
			if(!grp->SetLinkBorn(link))
				osl_socket_destroy(skt);
			else
				m_skt_idx ++ ;

			cnt++;
			if(cnt == 2048)
				break;
		}
	}	
	
	return 0;
}

//显示
void CTcpServer::DisplayStatus(char_t *buf,int32_t bufsize)
{
	char_t tmp_buf[10240];
	int32_t len = 0;

	len = osl_str_snprintf(buf,bufsize,"[");
	
	for(int32_t i=0;i<m_groups.GetSize();i++)
	{
		m_groups[i]->DisplayStatus(tmp_buf,sizeof(tmp_buf));
		//osl_log_debug("tmp_buf:%s\n",tmp_buf);
		len += osl_str_snprintf(buf+len,bufsize-len,"{\"grp_%d\":%s}",i,tmp_buf);
		if(i != m_groups.GetSize() - 1)
			len += osl_str_snprintf(buf+len,bufsize-len,",");
	}
	
	len += osl_str_snprintf(buf+len,bufsize-len,"]");

	//osl_log_debug("buf:%s\n",buf);
}

uint64_t CTcpServer::GetSktIdx()
{
	return m_skt_idx;
}