#include "stdafx.h"
#include "osl_mem.h"
#include "osl_socket.h"
#include "osl_log.h"
#include "osl_url.h"
#include "osl_string.h"
#include "osl_dir.h"
#include "osl_file.h"
#include "osl_int64.h"
#include "TcpSession.h"
#include "TcpServer.h"
#ifdef _OPENSSL
#include "openssl/ssl.h"
#endif
#include "ListQueue.cpp"

#define __MODULE__ "TcpSession"

#define ROUND_UP(x, align) (x+(align-1))&~(align-1) //按align字节对齐
#define MAX_BUFSIZE 4194304

//HTTP会话连接
CTcpSession::CTcpSession()
{
#ifdef _OPENSSL
	m_ssl = NULL;
	m_handshaked = false;
#endif
	m_group = NULL;
	memset(&m_link, 0, sizeof(m_link));
	m_link.skt = -1;

	m_out_list_queue = NULL;
	m_http_parser = NULL;
	m_out_list_queue_size = 0;
}

CTcpSession::~CTcpSession()
{
	Stop();
}


//开始工作
void CTcpSession::Start( void *group, STcpLink link )
{
	CTcpServer *svr;
	uint32_t unblock = 1;
	int32_t flag = 1;
	int32_t size;
	
	m_group = group;
	svr = (CTcpServer *)GetServer();
	m_link = link;

	if(m_http_parser)
		delete m_http_parser;

	m_http_parser = new CHttpParser(this);
	m_out_list_queue_size = 0;

//	uchar_t *p = (uchar_t*)&remote_ip;
//	osl_log_error(">>>>client session connect:ip=%d.%d.%d.%d:%d\n",*p,*(p+1),*(p+2),*(p+3),remote_port);
/*
	ling有三种组合方式：
	(l_onoff == 0)//l_linger忽略
		: 调用closesocket的时候立刻返回，底层会将未发送完的数据发送完成后再释放资源，也就是优雅的退出，这是系统的缺省情况
	(l_onoff != 0 && l_linger == 0)
		: 调用closesocket的时候立刻返回，但不会发送未发送完成的数据，而是通过一个REST包强制的关闭socket描述符，也就是强制的退出。
	[l_onoff != 0 && l_linger > 0 )
		: 调用closesocket的时候不会立刻返回，内核会延迟一段时间，这个时间就由l_linger得值来决定。
		  如果超时时间到达之前，发送完未发送的数据(包括FIN包)并得到另一端的确认，closesocket会返回正确，socket描述符优雅性退出。
		  否则，closesocket会直接返回错误值，未发送数据丢失，socket描述符被强制性退出。
		  需要注意的时，如果socket描述符被设置为非堵塞型，则closesocket会直接返回值。
*/
/*	linger mode;
	mode.l_onoff = 0;//是否延时退出
	mode.l_linger = 0;//延时时间（单位秒）,l_onoff=0时忽略
	osl_socket_set_opt( m_socket, SOL_SOCKET, SO_LINGER, (char*)&mode, sizeof(linger) );
*/
	osl_socket_set_opt( m_link.skt, SOL_SOCKET, SO_REUSEADDR, (const char*)&flag, sizeof(flag) );

	//osl_socket_ioctl( m_link.skt, FIONBIO, &unblock );//设置为非堵塞方式
	size = 16384;//接收缓冲区没必要这么大 16KB足以
	osl_socket_set_opt( m_link.skt, SOL_SOCKET, SO_SNDBUF, (char *)&size, sizeof(size) );
	size = 16384;//
	osl_socket_set_opt( m_link.skt, SOL_SOCKET, SO_RCVBUF, (char *)&size, sizeof(size) );
	//如果是50万个长连接 则这里占用内存为 (16+16)KB * 500000 = 16G 
	osl_socket_ioctl( m_link.skt, FIONBIO, &unblock );//设置为非堵塞方式
#ifdef _OPENSSL
	if( link.ssl)
	{
		m_handshaked = false;
		m_ssl = SSL_new( svr->m_ssl_ctx );
		SSL_set_fd( m_ssl, m_link.skt );/*把建立好的socket和SSL结构联系起来*/
		SSL_accept( m_ssl );
		
		//if(SSL_accept( m_ssl ) == -1)/*接受SSL链接*/
		//{
		//	osl_log_debug("=======  =======!!!!!!!!!!!!SSL_accept error\n");
		//	int ret = SSL_get_error(m_ssl,1);
		//	osl_log_debug("%s\n",SSL_state_string_long(m_ssl));
		//}
		/*SSL_write往socket缓冲区写时，如果缓冲区满了，第一次会提示 SSL_ERROR_WANT_WRITE,errno为EAGAIN,
		 如果是默认mode，此时再SL_write往缓冲区写，就会报SSL_ERROR_SSL错误，
		 但如果设置mode为SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER，
		 再SL_write往缓冲区写，则会返回SSL_ERROR_WANT_WRITE，errno为EAGAIN。
		*/
		SSL_set_mode(m_ssl,SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	}
#endif
	
}

//停止工作
void CTcpSession::Stop()
{
	SBufNode bufnode;
	
#ifdef _OPENSSL
	if( m_ssl )
	{
		SSL_shutdown( m_ssl ); 
		SSL_free( m_ssl );
		m_ssl = NULL;
		m_handshaked = false;
	}
#endif

	/* 1. close()是一种优雅的关闭方式，发送剩余数据之后关闭socket，不会产生大量的TIME_WAIT
	   2. 如果linger设置了延时，close()可能会阻塞（huanghuaming:经过验证），从而对程序逻辑产生影响
	   3. shutdown()是一种粗暴的关闭方式，会抛弃未发送数据，立即关闭，会产生大量的TIME_WAIT
	   4. 大量的TIME_WAIT会占用太多socket资源，超出(ulimit -n)的限制后，客户端再次连接会失败
	   5. 要同时解决TIME_WAIT和延时的问题，最好是让client先关闭socket，server随后关闭socket
	   6. 修改文件/etc/sysctl.confg的如下参数，然后执行"/sbin/sysctl -p"能改善TIME_WAIT状况：
			net.ipv4.tcp_syncookies = 1
			net.ipv4.tcp_tw_reuse = 1
			net.ipv4.tcp_tw_recycle = 1
			net.ipv4.tcp_fin_timeout = 20
	*/
	if (m_link.skt != -1)
	{
		osl_socket_destroy( m_link.skt );
		m_link.skt = -1;
	}
	//里面调用的close()容易产生延时和阻塞
	//shutdown( m_socket, SHUT_RDWR );//shutdown()不适用于短连接客户端

	if(m_out_list_queue)
	{
		while(!m_out_list_queue->empty())
		{
			bufnode = m_out_list_queue->front();
			free(bufnode.buf);
			m_out_list_queue->pop();
		}

		delete m_out_list_queue;
		m_out_list_queue = NULL;
	}

	delete m_http_parser;
	m_http_parser = NULL;
	m_out_list_queue_size = 0;
}

bool CTcpSession::HandShake()
{
	CTcpServer *svr;

#ifdef _OPENSSL
	svr = (CTcpServer *)GetServer();
	if( m_link.ssl)
	{
		if(!m_handshaked)
		{
			if(1 == SSL_accept( m_ssl ))
				m_handshaked = true;
		}
		return m_handshaked;
	}
	else
		return true;
#else
	return true;
#endif
}

bool CTcpSession::Recv(void *ptr,char_t *buf,int32_t bufsize,bool is_ssl,int32_t *recvsize)
{
	bool ret = false;
#ifdef _OPENSSL
	SSL *ssl = NULL;
#endif
	SOCKET skt = -1;
	int32_t n = 0;
	char_t *p = buf;
	int32_t errcode;

	//osl_log_debug("n:%d  bufsize:%d errno:%d is_ssl:%d\n",n,bufsize,errno,is_ssl);
	
	if(is_ssl)
	{
#ifdef _OPENSSL
		ssl = (SSL*)ptr;
		while(1)
		{
			n = SSL_read( ssl, p, bufsize );
			if( n > 0)
			{
				p += n;
				bufsize -= n;
				if(bufsize <= 0)
				{
					ret = true;
					break;
				}
					
			}
			else if (n == 0)
			{
				ret = false;
				break;
			}
			else
			{	
				errcode = SSL_get_error(ssl, n);
				if(errcode == SSL_ERROR_ZERO_RETURN)
				{
					osl_log_debug("[%s][%s]======SSL_ERROR_ZERO_RETURN \n",__MODULE__,__FUNCTION__);
					ret = false;
					break;
				}
				else if(errcode == SSL_ERROR_WANT_READ)
				{
//					osl_log_debug("======SSL_ERROR_WANT_READ \n");
					ret = true;
					break;
				}
				else if(errcode == SSL_ERROR_SYSCALL)
				{
					osl_log_debug("[%s][%s]======SSL_ERROR_SYSCALL \n",__MODULE__,__FUNCTION__);
					if (errno == EAGAIN) 
					{ 
						ret = true;
	                    break;
	                } else if (errno == EINTR) {
	                    continue;
	                }
				}
				else
				{
					osl_log_debug("[%s][%s]======other code:%d \n",__MODULE__,__FUNCTION__,errcode);
					ret = false;
					break;
				}
			}
		}
#endif
	}
	else 
	{
		while(1)
		{
			skt = *(SOCKET*)ptr;
			n = osl_socket_recv( skt, p, bufsize );
			//osl_log_debug("n:%d  bufsize:%d errno:%d \n",n,bufsize,errno);
			if(n > 0)
			{
				p += n;
				bufsize -= n;
				if(bufsize <= 0)
				{
					ret = true;
					break;
				}

				//osl_log_debug("n:%d  bufsize:%d errno:%d buf:%s\n",n,bufsize,errno,buf);
			}
			else if (n == 0)
			{
				ret = false;
				break;
			}
			else 
			{
				if(errno == EAGAIN)
				{
					//osl_log_debug("again %d\n %s\n",p - buf,buf);
					ret = true;
					break;
				}
				else if(errno == EINTR)
					continue;
				else
				{
					osl_log_debug("[%s][%s] errno %d\n",__MODULE__,__FUNCTION__,errno);
					ret = false;
					break;
				}
			}
		}
	}

	*recvsize = p - buf;
	return ret;
}

bool CTcpSession::Send(void *ptr,char_t *buf,int32_t bufsize,bool is_ssl,int32_t *sndsize)
{
	bool ret = false;
#ifdef _OPENSSL	
	SSL *ssl = NULL;
#endif
	SOCKET skt = -1;
	int32_t n = 0;
	char_t *p = buf;
	int32_t errcode;

	//osl_log_debug("n:%d  bufsize:%d errno:%d is_ssl:%d\n",n,bufsize,errno,is_ssl);
	
	if(is_ssl)
	{
#ifdef _OPENSSL	
		ssl = (SSL*)ptr;
		while(1)
		{
			n = SSL_write( ssl, p, bufsize );
			//errcode = SSL_get_error(ssl, n);
			//osl_log_debug("n:%d  bufsize:%d errno:%d errcode:%d\n",n,bufsize,errno,errcode);
			
			if( n > 0)
			{
				p += n;
				bufsize -= n;
				if(bufsize <= 0)
				{
					ret = true;
					break;
				}
					
			}
			else if (n == 0)
			{
				ret = false;
				break;
			}
			else
			{	
				errcode = SSL_get_error(ssl, n);
				if(errcode == SSL_ERROR_ZERO_RETURN)
				{
					osl_log_debug("[%s][%s] ======SSL_ERROR_ZERO_RETURN \n",__MODULE__,__FUNCTION__);
					ret = false;
					break;
				}
				else if(errcode == SSL_ERROR_WANT_WRITE)
				{
					//osl_log_debug("======SSL_ERROR_WANT_WRITE \n");
					ret = true;
					break;
				}
				else if(errcode == SSL_ERROR_SYSCALL)
				{
					osl_log_debug("[%s][%s] ======SSL_ERROR_SYSCALL \n",__MODULE__,__FUNCTION__);
					if (errno == EAGAIN) 
					{ 
						ret = true;
	                    break;
	                } else if (errno == EINTR) {
	                    continue;
	                }
					else
					{
						osl_log_debug("[%s][%s] ======SSL_ERROR_SYSCALL code:%d \n",__MODULE__,__FUNCTION__,errno);
						ret = false;
						break;
					}
				}
				else
				{
					if(errcode == SSL_ERROR_SSL)
					{
						/*SSL_write往socket缓冲区写时，如果缓冲区满了，第一次会提示 SSL_ERROR_WANT_WRITE,errno为EAGAIN,
						 如果是默认mode，此时再SL_write往缓冲区写，就会报SSL_ERROR_SSL错误，
						 但如果设置mode为SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER，
						 再SL_write往缓冲区写，则会返回SSL_ERROR_WANT_WRITE，errno为EAGAIN。
						 SSL_set_mode(m_ssl,SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
						*/
					
						osl_log_debug("[%s][%s] maybe you not set mode SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER,please check your SSL_set_mode \n",__MODULE__,__FUNCTION__);
					}
					
					osl_log_debug("[%s][%s] ======other errcode:%d \n",__MODULE__,__FUNCTION__,errcode);
					ret = false;
					break;
				}
			}
		}
#endif		
	}
	else 
	{
		while(1)
		{
			skt = *(SOCKET*)ptr;
			n = osl_socket_send( skt, p, bufsize );
			//osl_log_debug("n:%d  bufsize:%d errno:%d \n",n,bufsize,errno);
			if(n > 0)
			{
				p += n;
				bufsize -= n;
				if(bufsize <= 0)
				{
					ret = true;
					break;
				}
			}
			else if (n == 0)
			{
				ret = false;
				break;
			}
			else 
			{
				if(errno == EAGAIN)
				{
					//osl_log_debug("again %d\n",p - buf);
					ret = true;
					break;
				}
				else if(errno == EINTR)
					continue;
				else
				{
					osl_log_debug("[%s][%s] other errno %d\n",__MODULE__,__FUNCTION__,errno);
					ret = false;
					break;
				}
			}
		}
	}

	*sndsize = p - buf;
	//osl_log_debug("ret:%d *sndsize :%d\n",ret,*sndsize);
	return ret;
}

//接收数据，返回true表示需socket挂了，false表示继续工作
bool CTcpSession::OnRecv( uint64_t now )
{
	CTcpServer *svr = (CTcpServer*)GetServer();
	CTcpGroup *grp = (CTcpGroup*)m_group;
	char_t *buf = grp->m_recv_buf;
	int32_t bufsize = grp->m_recv_bufsize;
	STcpServerStatistics sta;
	int32_t ret ;
	int32_t recv_size = 0;
	int32_t read_size = 0;
	bool recv_flag = false;

	recv_size = m_http_parser->NeedRecvSize();

	if(recv_size == -1)
		recv_size = bufsize;

	if(recv_size <= 0)
	{
		osl_log_debug("[%s][%s] so much data,but no a complete http buf \n",__MODULE__,__FUNCTION__);
		return true;
	}
#ifdef _OPENSSL

	if( m_ssl )
	{
		recv_flag = Recv(m_ssl,buf,recv_size,true,&read_size);
	}
	else// 不是sssl
	{
		recv_flag = Recv(&m_link.skt,buf,recv_size,false,&read_size);
	}

#else
		recv_flag = Recv(&m_link.skt,buf,recv_size,false,&read_size);
#endif

	//osl_log_debug("======recv_flag:%d read_size:%d\n",recv_flag,read_size);
	if(!recv_flag )
		return true;
	if(read_size <= 0)
		return false;

	if(read_size < recv_size || read_size < bufsize)
		buf[read_size] = 0;

	memset( &sta, 0, sizeof(sta) );
	sta.cmd_cnt = 1;
	svr->AddStatistics( sta );

	ret = m_http_parser->OnData(buf,read_size,now);

	if(ret == 0)
		m_http_parser->Stop();
	else if(ret == -1)
	{
		m_http_parser->Stop();
		return true;
	}
		
	return false;
}

//接收数据，返回true表示需socket挂了，false表示继续工作
bool CTcpSession::OnSend( char_t *buf, int32_t size, uint64_t now )
{
	int32_t sndsize = 0;
	bool flag = false;
	SBufNode bufnode;

	//osl_log_debug("size :%d\n",size);

	//先发旧的数据
	if(m_out_list_queue && !m_out_list_queue->empty())
	{
		bufnode = m_out_list_queue->front();
	#ifdef _OPENSSL
		if( m_ssl )
		{
			flag = Send(m_ssl,bufnode.buf+bufnode.sendpos,bufnode.buflen - bufnode.sendpos,true,&sndsize);
		}
		else
		{
			flag = Send(&m_link.skt,bufnode.buf+bufnode.sendpos,bufnode.buflen - bufnode.sendpos,false,&sndsize);
		}
	#else
		flag = Send(&m_link.skt,bufnode.buf+bufnode.sendpos,bufnode.buflen - bufnode.sendpos,false,&sndsize);
	#endif

		if(!flag)
		{
			osl_log_debug("[%s][%s] skt_idx:%llu down\n",__MODULE__,__FUNCTION__,m_link.skt_idx);
			return true;
		}

		//osl_log_debug("sendsize :%d\n",sndsize);
		
		if(bufnode.sendpos + sndsize == bufnode.buflen)
		{
			//发完了
			free(bufnode.buf);
			m_out_list_queue->pop();
			m_out_list_queue_size -= bufnode.bufsize;
		}
		else if(bufnode.sendpos + sndsize < bufnode.buflen)
		{
			//没发完
			m_out_list_queue->front().sendpos += sndsize;
			//osl_log_debug("dequeue p_bufnode->sendpos:%d sndsize:%d \n",p_bufnode->sendpos,sndsize);
		}
		else
		{
			osl_log_debug("[%s][%s]it is maybe not happen,if happen ,must something error\n",__MODULE__,__FUNCTION__);
			free(bufnode.buf);
			m_out_list_queue->pop();
			m_out_list_queue_size -= bufnode.bufsize;
		}
	}

	if(buf == NULL || size <= 0)
		return false;//继续

	if(m_out_list_queue && !m_out_list_queue->empty())
	{

		if(size > 0 && m_out_list_queue_size +size > MAX_BUFSIZE )//m_out_list_queue发送不允许大于4M
		{
			osl_log_debug("[%s][%s] sorry,skt_idx:%llu have so much bytes to send m_out_list_queue_size:%d size:%d\n",
				__MODULE__,__FUNCTION__,m_out_list_queue_size,size);
			return false;
		}
			
		//输出队列不为空
		//按1024字节对齐
		bufnode.buflen = size;
		bufnode.bufsize = ROUND_UP(bufnode.buflen,1024);
		bufnode.buf = (char_t*)malloc(bufnode.bufsize);
		bufnode.sendpos = 0;

		if(bufnode.buf)
		{
			memcpy(bufnode.buf,buf,bufnode.buflen);

			m_out_list_queue->push(bufnode);
			m_out_list_queue_size += bufnode.bufsize;
		}
	}
	else
	{
		if(size > 0 && m_out_list_queue_size +size > MAX_BUFSIZE )//m_out_list_queue发送不允许大于4M
		{
			osl_log_debug("[%s][%s] sorry,skt_idx:%llu have so much bytes to send m_out_list_queue_size:%d size:%d\n",
				__MODULE__,__FUNCTION__,m_out_list_queue_size,size);
			return false;
		}
				
		//输出队列为空
	#ifdef _OPENSSL
		if( m_ssl )
		{
			flag = Send(m_ssl,buf,size,true,&sndsize);
		}
		else
		{
			flag = Send(&m_link.skt,buf,size,false,&sndsize);
		}
	#else
		flag = Send(&m_link.skt,buf,size,false,&sndsize);
	#endif

		//osl_log_debug("flag :%d  sndsize:%d\n",flag,sndsize);

		if(!flag)
			return true;

		if(sndsize < size)
		{
			//按1024字节对齐
			bufnode.buflen = size - sndsize;
			bufnode.bufsize = ROUND_UP(bufnode.buflen,1024);
			bufnode.buf = (char_t*)malloc(bufnode.bufsize);
			bufnode.sendpos = 0;

			if(bufnode.buf)
			{
				memcpy(bufnode.buf,buf+sndsize,bufnode.buflen);

				if(NULL == m_out_list_queue)
				{
					m_out_list_queue = new CListQueue<SBufNode>();
				}
				
				m_out_list_queue->push(bufnode);
				m_out_list_queue_size += bufnode.bufsize;
				//osl_log_debug("enqueue bufsize:%d buflen:%d \n",bufnode.bufsize,bufnode.buflen);
			}
		}
		
	}

	return false;
}

//发送外部数据(不能多线程同时执行）
bool CTcpSession::PostData( char_t *buf, int32_t size )
{
	SPacketHeader header;
	memset(&header,0,sizeof(header));
	header.link = m_link;
	header.flag = PACKET_END;
	
	return ((CTcpGroup*)m_group)->PostData(header,buf, size );
}


//数据包处理函数
void CTcpSession::OnPacket(char_t *buf, int32_t hlen, int32_t clen,bool keepalive,STcpLink *link,uint64_t now)
{
	osl_log_debug("[%s][%s][%d] it is unhappend posible\n",__MODULE__,__FUNCTION__,__LINE__);
}


//发送前要处理的一些回调
void CTcpSession::HandleBeforeSend(SPacketHeader packet_header,char_t *buf,int32_t bufsize)
{	
	//osl_log_debug("[%s] can not happen\n",__func__);
}

//是否有数据需要发送
bool CTcpSession::IsNeedSendData()
{
	SBufNode bufnode;
	
	if(m_out_list_queue && !m_out_list_queue->empty())
	{
		bufnode = m_out_list_queue->front();
		if(bufnode.buflen == bufnode.sendpos)
		{
			free(bufnode.buf);
			m_out_list_queue->pop();
		}
		else
		{
			//osl_log_debug(" p_bufnode->buflen:%d senpos:%d\n",p_bufnode->buflen,p_bufnode->sendpos);
			return true;
		}
	}

	return false;
}


void *CTcpSession::GetServer()
{
	return ((CTcpGroup*)m_group)->m_server;
}

void *CTcpSession::GetGroup()
{
	return m_group;
}

void CTcpSession::SetTimerId(uint64_t timerid)
{
	m_timer_id = timerid;
}

uint64_t CTcpSession::GetTimerId()
{
	return m_timer_id; 
}

//取得m_close_flag
bool CTcpSession::GetCloseFlag()
{
	if(m_http_parser)
		return m_http_parser->GetCloseFlag();

	return false;
}

