#include "stdafx.h"
#include "osl_mem.h"
#include "osl_socket.h"
#include "osl_log.h"
#include "osl_url.h"
#include "osl_string.h"
#include "osl_dir.h"
#include "osl_file.h"
#include "osl_int64.h"
#include "main.h"
#include "Resource.h"
#include "TcpServer.h"
#ifdef _OPENSSL
#include "openssl/ssl.h"
#endif


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

	m_dead_flag = false;
	m_close_flag = false;
	m_act_tick = 0;

	m_recv_datsize = 0;
	m_recv_bufsize = 0;
	m_recv_buf = NULL;

	m_send_datsize = 0;
	m_send_bufsize = 0;
	m_send_buf = NULL;
}

CTcpSession::~CTcpSession()
{
	Stop();
	if( m_recv_buf )
	{
		free( m_recv_buf );
		m_recv_buf = NULL;
	}

	if( m_send_buf )
	{
		free( m_send_buf );
		m_send_buf = NULL;
	}
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

	m_dead_flag = false;
	m_close_flag = false;
	m_act_tick = osl_get_ms();

	m_recv_datsize = 0;
	m_send_datsize = 0;

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
	size = 65536;
	osl_socket_set_opt( m_link.skt, SOL_SOCKET, SO_SNDBUF, (char *)&size, sizeof(size) );
	size = 65536;
	osl_socket_set_opt( m_link.skt, SOL_SOCKET, SO_RCVBUF, (char *)&size, sizeof(size) );
	osl_socket_ioctl( m_link.skt, FIONBIO, &unblock );//设置为非堵塞方式
#ifdef _OPENSSL
	if( svr->m_ssl_flag )
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
#ifdef _OPENSSL
	if( m_ssl )
	{
		SSL_shutdown( m_ssl ); 
		SSL_free( m_ssl );
		m_ssl = NULL;
		m_handshaked = false;
	}
#endif

	if(m_recv_buf)
	{
		free(m_recv_buf);
		m_recv_buf = NULL;
	}

	if(m_send_buf)
	{
		free(m_send_buf);
		m_send_buf = NULL;
	}

	m_recv_bufsize = 0;
	m_send_bufsize = 0;
	m_recv_datsize = 0;
	m_send_datsize = 0;
	
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
}

bool CTcpSession::HandShake()
{
	CTcpServer *svr;

#ifdef _OPENSSL
	svr = (CTcpServer *)GetServer();
	if( svr->m_ssl_flag )
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
//设置死亡标志
void CTcpSession::SetDead( bool flag )
{
	m_dead_flag = flag;
}

//是否死亡
bool CTcpSession::IsDead( uint32_t now )
{
	bool ret = false;

	if( m_link.skt == -1 || m_dead_flag ) 
	{
		//osl_log_debug( "[CTcpSession][IsDead] socket=%d dead=%d\n", m_socket, m_dead_flag );
		ret = true;
	}
	if( m_close_flag && m_send_datsize <= 0 )
	{
//		osl_log_debug( "[CTcpSession][IsDead] m_close_flag=true, dat=0 sockt=%d\n", m_link.skt );
		ret = true;
	}

	//心跳处理：客户端长时间不发送数据，认为已经死亡
	if( now < m_act_tick )
		m_act_tick = now;
	if( m_act_tick + 60000 < now )
	{
		osl_log_debug( "[CTcpSession][IsDead] now-act=%u-%u=%u\n", now, m_act_tick, now-m_act_tick );
		ret = true;
	}

	return ret;
}

bool CTcpSession::Recv(void *ptr,char_t *buf,int32_t bufsize,bool is_ssl,int32_t *recvsize)
{
	bool ret = false;
	SSL *ssl = NULL;
	SOCKET skt = -1;
	int32_t n = 0;
	char_t *p = buf;
	int32_t errcode;

	//osl_log_debug("n:%d  bufsize:%d errno:%d is_ssl:%d\n",n,bufsize,errno,is_ssl);
	
	if(is_ssl)
	{
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
					osl_log_debug("======SSL_ERROR_ZERO_RETURN \n");
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
					osl_log_debug("======SSL_ERROR_SYSCALL \n");
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
					osl_log_debug("======other code:%d \n",errcode);
					ret = false;
					break;
				}
			}
		}
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
					osl_log_debug("errno %d\n",errno);
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
	SSL *ssl = NULL;
	SOCKET skt = -1;
	int32_t n = 0;
	char_t *p = buf;
	int32_t errcode;
	long mode;

	//osl_log_debug("n:%d  bufsize:%d errno:%d is_ssl:%d\n",n,bufsize,errno,is_ssl);
	
	if(is_ssl)
	{
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
					osl_log_debug("======SSL_ERROR_ZERO_RETURN \n");
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
					osl_log_debug("======SSL_ERROR_SYSCALL \n");
					if (errno == EAGAIN) 
					{ 
						ret = true;
	                    break;
	                } else if (errno == EINTR) {
	                    continue;
	                }
					else
					{
						osl_log_debug("======SSL_ERROR_SYSCALL code:%d \n",errno);
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
					
						osl_log_debug("maybe you not set mode SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER,please check your SSL_set_mode \n");
					}
					
					osl_log_debug("======other errcode:%d \n",errcode);
					ret = false;
					break;
				}
			}
		}
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
					osl_log_debug("other errno %d\n",errno);
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



bool CTcpSession::AnalysisBuf(char_t *buf,int32_t datsize,int32_t *pos)
{
	bool ret = false;
	char_t *p,*p0;
	int32_t hlen = 0,clen = 0;
	char_t str[32]={0};
	uint32_t now;
	int32_t leftsize = datsize;

	//缺省认为是长连接
	m_close_flag = false;

	p = p0 = buf;
	//osl_log_debug("======= buf:%s \n",buf);
	while( p0 + 4 < buf + datsize )
	{
		//提取头长度 和 负载长度
		p = osl_strstr( p0, "\r\n\r\n" );
		if( p == NULL )
		{
			ret = false;
			break;
		}

		hlen = 0;
		hlen = (int32_t)(p - p0 + 4);
	//	osl_log_debug("hlen:%d \n",hlen);
		clen = 0;
		if( osl_url_getheadval( (char*)p0, -1, "Content-Length", str, sizeof(str) ) )
			clen = atoi( str );
		if( clen < 0 )
			clen = 0;
			
		//osl_log_debug("[CTcpSession][OnRecv]  hlen=%d clen=%d datsize=%d\n", hlen, clen, datsize);

		
		/* 处理一个完整的数据包 */
		//if ( 0 < hlen && hlen + clen <= datsize )
		if ( 0 < hlen && hlen + clen <= leftsize )
		{
		#if 0
			//分发处理
			OnPacket( p0, hlen, clen, now);
			memset( &sta, 0, sizeof(sta) );
			sta.cmd_cnt = 1;
			svr->AddStatistics( sta );

			//如果是短连接，通知外部关闭socket
			memset( str, 0, sizeof(str) );
			if( osl_url_getheadval( p0 , -1, "Connection", str, sizeof(str) ) )
			{
				if( osl_strcmp_nocase( str, "close" ) == 0 )
					m_close_flag = true;
			}
		#endif
			m_act_tick = now;
			//分发处理
			now = osl_get_ms();
			OnPacket( p0, hlen, clen,m_link.remote_ip,m_link.remote_port ,m_link.skt,m_link.skt_idx,now);
			//memset( &sta, 0, sizeof(sta) );
			//sta.cmd_cnt = 1;
			//svr->AddStatistics( sta );
			
			//osl_log_debug("xxxxxxxx skt:%d ip:%d\n",m_link.skt,m_link.remote_ip);
			//如果是短连接，通知外部关闭socket
			memset( str, 0, sizeof(str) );
			if( osl_url_getheadval( p0 , -1, "Connection", str, sizeof(str) ) )
			{
				if( osl_strcmp_nocase( str, "close" ) == 0 )
					m_close_flag = true;
			}
			p0 += hlen + clen;
			leftsize  = leftsize - hlen - clen;
		}
		else //包不完整
		{
			//if ( datsize >= m_recv_bufsize  )
			//{
			//	osl_log_debug("[CTcpSession][OnRecv] packet inComplete  !! \n");
			//	goto ERROR_PACKET;
			//}
			//break;

			osl_log_debug("no imcomplete buf:%s\nhlen:%d clen:%d datasize:%d\n",buf,hlen,clen,datsize);
			break;
		}
		
	}
	//osl_log_debug("p - buf ;%d  p0 - buf :%d\n",p - buf,p0 - buf);
	if(p0 - buf == datsize)
		ret = true;
	else
	{
		*pos =  p0 - buf;
		osl_log_debug("func:%s on implete httpbuf ,datsize:%d pos:%d left \nbuf:%s\n",__func__,datsize,*pos,buf);
		ret = false;
	}

	return ret;
}

//接收数据，返回true表示需socket挂了，false表示继续工作
bool CTcpSession::OnRecv( uint32_t now )
{
	CTcpServer *svr = (CTcpServer*)GetServer();
	CTcpGroup *grp = (CTcpGroup*)m_group;
	char_t *buf = grp->m_recv_buf;
	int32_t bufsize = grp->m_recv_bufsize;
	STcpServerStatistics sta;
	int32_t n;
	bool ret = false;
	//int32_t errcode;
	int32_t recv_size = 0;
	int32_t read_size = 0;
	int32_t pos = 0;
	bool is_complete = false;
	bool recv_flag = false;
	
	if(m_recv_datsize > 0)//上次有数据
		recv_size = m_recv_bufsize - m_recv_datsize -1;//最多能收这么多数据
	else
		recv_size = bufsize-1;
	//osl_log_debug("====== read_size:%d %d\n",recv_size,grp->m_recv_bufsize);

	if(recv_size <= 0)
	{
		osl_log_debug("so much data,but no a complete http buf \n");
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

	memset( &sta, 0, sizeof(sta) );
	sta.cmd_cnt = 1;
	svr->AddStatistics( sta );
				
	if(m_recv_datsize > 0)//上次有数据，要合并处理
	{
		if(read_size > 0)
		{
			memcpy(m_recv_buf+m_recv_datsize,buf,read_size);
			m_recv_datsize += read_size;
			m_recv_buf[m_recv_datsize] = 0;
			//m_act_tick = now;
		}

		is_complete = AnalysisBuf(m_recv_buf,m_recv_datsize,&pos);
		if(is_complete)
		{
			if(m_recv_buf)
			{
				free(m_recv_buf);
				m_recv_buf = NULL;
				m_recv_bufsize = 0;
				m_recv_datsize = 0;
			}
		}
		else if(!is_complete && pos > 0)//还有数据没处理完
		{
			memmove(m_recv_buf,m_recv_buf + pos ,m_recv_datsize - pos);
			m_recv_datsize = m_recv_datsize - pos;
		}
		else 
		{
			if( now < m_act_tick )
				m_act_tick = now;
			if( m_act_tick + 60000 < now )
			{
				osl_log_debug( "[CTcpSession][%s] now-act=%u-%u=%u skt_idx:%llu\n",__func__, now, m_act_tick, now-m_act_tick ,m_link.skt_idx);
				return true;
			}
		}

		//osl_log_debug("========4  m_recv_datsize:%d complete buf\n",m_recv_datsize);
	}
	else//上次没数据
	{
		//osl_log_debug("========4.5 read_size:%d is_complete:%d\n",read_size,is_complete);
		buf[read_size] = 0;
		//osl_log_debug("== buf:%s\n",buf);
		is_complete = AnalysisBuf(buf,read_size,&pos);

		if(false == is_complete && pos >= 0)//不是完整的http包
		{

			if(NULL == m_recv_buf)
			{
				m_recv_bufsize = 65536;
				m_recv_buf = (char_t*)malloc(m_recv_bufsize);
			}

			if(pos < read_size)
			{
				memcpy(m_recv_buf,buf+pos,read_size-pos);
				m_recv_datsize = read_size-pos;
			}
			else
			{
				osl_log_error("[%s] pos:%d read_size:%d it is unpossible happen,if happen AnalysisBuf must be has problem\n",__func__,pos,read_size);
			}
		}

		//osl_log_debug("========5 m_recv_datsize:%d is_complete:%d\n",m_recv_datsize,is_complete);
	}
	ret = false;
	return ret;
#if 0
	CTcpServer *svr = (CTcpServer*)GetServer();
//	CTcpGroup *grp = (CTcpGroup*)m_group;
	char_t *buf = NULL, *p=NULL, *p0=NULL, str[64];
	int32_t bufsize=0, datsize=0, size=0;
	int32_t hlen=0, clen=0;
	STcpServerStatistics sta;
	bool ret = false;

	if (m_recv_buf)
	{
		//上次数据与本次新接收数据合并
		buf = m_recv_buf + m_recv_datsize;
		bufsize = m_recv_bufsize - m_recv_datsize;
		datsize = m_recv_datsize;
	}
	else
	{
		m_recv_datsize = 0;
		m_recv_bufsize = 65536;
		m_recv_buf = (char_t*)malloc( m_recv_bufsize );
		memset(m_recv_buf, 0, m_recv_bufsize);

		buf = m_recv_buf;
		bufsize = m_recv_bufsize;
		datsize = m_recv_datsize;
	}

#ifdef _OPENSSL
	if( m_ssl )
	{
		size = SSL_read( m_ssl, buf, bufsize-1 );
	}
	else
	{
		size = osl_socket_recv( m_link.skt, buf, bufsize-1 );
	}
#else
	size = osl_socket_recv( m_link.skt, buf, bufsize-1 );
#endif

	if( size == 0 )//&&(errno!=EAGAIN&&errno!=EINTR))
	{
		m_dead_flag = true;	
		osl_log_debug("[CTcpSession][OnRecv]  size=%d\n", size);
		ret = true;
		goto ERROR_EXIT;
	}
	else if (size < 0)
	{
		if(errno == ECONNRESET || errno == ETIMEDOUT)//对方关闭 || 连接超时
		//if(errno == ETIMEDOUT)//对方关闭 || 连接超时
		{
			osl_log_debug( "[CTcpSession][OnRecv] socket error %d %d %d\n", errno, ECONNRESET, ETIMEDOUT );
			m_dead_flag = true;	
			ret = true;
			goto ERROR_EXIT;
		}
		
		//if(osl_socket_get_state() < 0)
		//{
		//	osl_log_debug( "[CHttpSession][OnTask] socket error  %d %s\n",errno,m_peer_id );
		//	m_dead_flag = true;	
		//}
		/*else if( errno != EAGAIN && errno != EINTR )
		{
			printf( "2  socket error %d %d %d\n",errno, EAGAIN, EINTR );
			m_dead_flag = true;	
		}*/
	}
	else
	{
		datsize += size;
		buf[size]=0;
		ret = false;

		//缺省认为是长连接
		m_close_flag = false;

		p = p0 = m_recv_buf;
		while( p0 + 4 < m_recv_buf + datsize )
		{
			//提取头长度 和 负载长度
			p = osl_strstr( (const char*)p0, "\r\n\r\n" );
			if( p == NULL )
			{
				break;
			}

			hlen = 0;
			hlen = (int32_t)(p - p0 + 4);

			clen = 0;
			if( osl_url_getheadval( (char*)p0, -1, "Content-Length", str, sizeof(str) ) )
				clen = atoi( str );
			if( clen < 0 )
				clen = 0;
				
			//osl_log_debug("[CTcpSession][OnRecv]  hlen=%d clen=%d datsize=%d\n", hlen, clen, datsize);

			
			
			/* 处理一个完整的数据包 */
			if ( 0 < hlen && hlen + clen <= datsize )
			{
			#if 0
				//分发处理
				OnPacket( p0, hlen, clen, now);
				memset( &sta, 0, sizeof(sta) );
				sta.cmd_cnt = 1;
				svr->AddStatistics( sta );

				//如果是短连接，通知外部关闭socket
				memset( str, 0, sizeof(str) );
				if( osl_url_getheadval( p0 , -1, "Connection", str, sizeof(str) ) )
				{
					if( osl_strcmp_nocase( str, "close" ) == 0 )
						m_close_flag = true;
				}
			#endif

							//分发处理
				OnPacket( p0, hlen, clen,m_link.remote_ip,m_link.remote_port ,m_link.skt,now);
				memset( &sta, 0, sizeof(sta) );
				sta.cmd_cnt = 1;
				svr->AddStatistics( sta );
				
				//osl_log_debug("xxxxxxxx skt:%d ip:%d\n",m_link.skt,m_link.remote_ip);
				//如果是短连接，通知外部关闭socket
				memset( str, 0, sizeof(str) );
				if( osl_url_getheadval( p0 , -1, "Connection", str, sizeof(str) ) )
				{
					if( osl_strcmp_nocase( str, "close" ) == 0 )
						m_close_flag = true;
				}
				p0 += hlen + clen;
			}
			else //包不完整
			{
				if ( datsize >= m_recv_bufsize  )
				{
					osl_log_debug("[CTcpSession][OnRecv] packet inComplete  !! \n");
					goto ERROR_PACKET;
				}
				break;
			}
			
		}

		m_act_tick = now;

		//删除已经处理过的数据包，将剩余数据缓存在link中，等待下次继续接收
		if ( m_recv_buf != p0 )
		{
			datsize = (int32_t)(m_recv_buf+datsize-p0);
			if ( 0 < datsize )
			{
				memmove( m_recv_buf, p0, datsize );
				m_recv_buf[datsize]=0;
			}
			else
			{
				free(m_recv_buf);
				m_recv_buf = NULL;
			}
		}
		m_recv_datsize = datsize;
	}

ERROR_EXIT:
	return ret;
	
ERROR_PACKET:
	osl_log_debug("[CTcpSession][OnRecv] ERROR_PACKET m_recv_buf=%s\n", m_recv_buf);
	m_recv_datsize = 0;
	return ret;
#endif
}

//接收数据，返回true表示需socket挂了，false表示继续工作
bool CTcpSession::OnSend( char_t *buf, int32_t size, uint32_t now )
{
	CTcpServer *svr = (CTcpServer*)GetServer();
	STcpServerStatistics sta;
	int32_t sndsize = 0;
	bool flag = false;

	//osl_log_debug("size :%d\n",size);

	//先发送上次剩余数据
	if (m_send_buf && 0 < m_send_datsize)
	{
		//osl_log_debug("last m_send_buf:%x m_send_datsize:%d size:%d m_send_bufsize:%d\n",m_send_buf,m_send_datsize,size,m_send_bufsize);
	#ifdef _OPENSSL
		if( m_ssl )
		{
			//sndsize = SSL_write( m_ssl, m_send_buf, m_send_datsize );
			flag = Send(m_ssl,m_send_buf,m_send_datsize,true,&sndsize);
		}
		else
		{
			//sndsize = osl_socket_send( m_link.skt, m_send_buf, m_send_datsize );
			flag = Send(&m_link.skt,m_send_buf,m_send_datsize,false,&sndsize);
		}
	#else
		//sndsize = osl_socket_send( m_link.skt, m_send_buf, m_send_datsize );
		flag = Send(&m_link.skt,m_send_buf,m_send_datsize,false,&sndsize);
	#endif

		if(!flag)
			return true;
		
		if( 0 < sndsize )
		{
			m_send_datsize -= sndsize;
			if( 0 < m_send_datsize )
				memmove( m_send_buf, m_send_buf+sndsize, m_send_datsize );
			else
				m_send_datsize = 0;

			memset( &sta, 0, sizeof(sta) );
			sta.send_size = sndsize;
			svr->AddStatistics( sta );
			m_act_tick = now;
		}
	}

	//osl_log_debug("m_send_buf:%x m_send_datsize:%d size:%d m_send_bufsize:%d\n",m_send_buf,m_send_datsize,size,m_send_bufsize);
	//发送本次数据
	if ((m_send_buf == NULL || m_send_datsize <= 0 ))
	{
		if(size <= 0 )
		{
			free(m_send_buf);
			m_send_buf = NULL;
			m_send_bufsize = 0;
			return false;
		}
		
		sndsize = 0;
	#ifdef _OPENSSL
		if( m_ssl )
		{
			//sndsize = SSL_write( m_ssl, buf, size );
			flag = Send(m_ssl,buf,size,true,&sndsize);
		}
		else
		{
			//sndsize = osl_socket_send( m_link.skt,  buf, size );
			flag = Send(&m_link.skt,buf,size,false,&sndsize);
		}
	#else
		//sndsize = osl_socket_send( m_link.skt, buf, size );
		flag = Send(&m_link.skt,buf,size,false,&sndsize);
	#endif

		if(!flag)
			return true;
		if( 0 <= sndsize )//将剩余数据存入m_send_buf
		{
			m_send_datsize = size - sndsize;
			//osl_log_debug("xx m_send_datsize:%d m_send_bufsize:%d m_send_buf:%x\n",m_send_datsize,m_send_bufsize,m_send_buf);
			if( 0 < m_send_datsize )
			{
				if (m_send_buf && m_send_bufsize < m_send_datsize)
				{
					free(m_send_buf);
					m_send_buf = NULL;
				}
				if (m_send_buf == NULL)
				{
					m_send_bufsize = m_send_datsize > 65536 ?  65536 : m_send_datsize;
					m_send_buf = (char_t*)malloc(m_send_bufsize);
					//osl_log_debug("malloc size:%d m_send_datsize:%d\n",m_send_bufsize,m_send_datsize);
				}
				//memmove( m_send_buf, buf+sndsize, m_send_datsize );
				memcpy(m_send_buf,buf+sndsize,m_send_datsize);
			}
			else if(m_send_datsize == 0)
			{
				if(m_send_buf)
				{
					free(m_send_buf);
					m_send_buf = NULL;
					m_send_bufsize = 0;
					//osl_log_debug("free \n");
				}
			}
			memset( &sta, 0, sizeof(sta) );
			sta.send_size = sndsize;
			svr->AddStatistics( sta );
			m_act_tick = now;
		}
	}
	//将本次数据接入上次剩余数据尾巴
	else if (m_send_buf && m_send_datsize + size < m_send_bufsize )
	{
		//osl_log_warn("xxxxx\n");
		if(size > 0)
		{
			memcpy( m_send_buf + m_send_datsize, buf, size );
			m_send_datsize += size;
		}
	}
	else
	{
		osl_log_warn("data lost size:%d\n",size);
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
	
	//return ((CTcpGroup*)m_group)->m_outerCache.Post(header, buf, size);
	((CTcpGroup*)m_group)->PostData(header,buf, size );
}


//数据包处理函数
void CTcpSession::OnPacket(char_t *buf, int32_t hlen, int32_t clen,uint32_t ip,uint16_t port,SOCKET skt,uint64_t,uint32_t now)
{
	osl_log_debug("func:%s  line:%d it is unhappend posible\n",__func__,__LINE__);
}


//发送前要处理的一些回调
void CTcpSession::HandleBeforeSend(SPacketHeader packet_header,char_t *buf,int32_t bufsize)
{	

}


void *CTcpSession::GetServer()
{
	return ((CTcpGroup*)m_group)->m_server;
}

void *CTcpSession::GetGroup()
{
	return m_group;
}

void CTcpSession::SetTimerId(uint32_t timerid)
{
	m_timer_id = timerid;
}

uint32_t CTcpSession::GetTimerId()
{
	return m_timer_id; 
}

//取得m_close_flag
bool CTcpSession::GetCloseFlag()
{
	return m_close_flag;
}

