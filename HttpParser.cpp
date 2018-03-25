#include "stdafx.h"
#include "osl.h"
#include "osl_string.h"
#include "osl_url.h"
#include "osl_log.h"	
#include "HttpParser.h"
#include "TcpSession.h"

#define __MODULE__ "HttpParser"
#define ROUND_UP(x, align) (x+(align-1))&~(align-1) //按align字节对齐

CHttpParser::CHttpParser(void *session)
{
	m_status = HTTP_INIT;
	m_head_length = -1;
	m_content_length = -1;

	m_buf = NULL;
	m_buflen = 0;
	m_datasize = 0;
	m_close_flag = false;
	m_act_tick = 0;

	m_session = session;
}

CHttpParser::~CHttpParser()
{
	Stop();
}

bool CHttpParser::GetCloseFlag()
{
	return m_close_flag;
}

void CHttpParser::Stop()
{
	m_status = HTTP_INIT;
	m_head_length = -1;
	m_content_length = -1;
	if(m_buf)
	{
		free(m_buf);
		m_buf = NULL;
	}
	m_buflen = 0;
	m_datasize = 0;
	m_act_tick = 0;
}

bool CHttpParser::AnalysisBuf(char_t *buf,int32_t datsize,int32_t *pos,uint64_t ms)
{
	bool ret = false;
	char_t *p,*p0;
	int32_t hlen = 0,clen = 0;
	char_t str[32]={0};
	int32_t leftsize = datsize;
	CTcpSession *session = ((CTcpSession*)m_session);

	//缺省认为是长连接
	m_close_flag = false;

	p = p0 = buf;
	//osl_log_debug("======= buf:%s \n",buf);
	while( p0 + 4 < buf + datsize )
	{
		//提取头长度 和 负载长度
		p = osl_strstr( p0, "\r\n\r\n" );
		if( p == NULL  || p > buf + datsize)
		{
			ret = false;
			break;
		}

		hlen = 0;
		hlen = (int32_t)(p - p0 + 4);
	//	osl_log_debug("hlen:%d \n",hlen);
		clen = 0;
		if( osl_url_getheadval( (char*)p0, hlen, "Content-Length", str, sizeof(str) ) )
			clen = atoi( str );
		if( clen < 0 )
			clen = 0;

		m_head_length = hlen;
		m_content_length = clen;
			
		//osl_log_debug("[CTcpSession][OnRecv]  hlen=%d clen=%d datsize=%d\n", hlen, clen, datsize);

		
		/* 处理一个完整的数据包 */
		if ( 0 < hlen && hlen + clen <= leftsize )
		{
			m_act_tick = ms;
			//如果是短连接，通知外部关闭socket
			memset( str, 0, sizeof(str) );
			if( osl_url_getheadval( p0 , hlen, "Connection", str, sizeof(str) ) )
			{
				if( osl_strcmp_nocase( str, "close" ) == 0 )
					m_close_flag = true;
			}
			//分发处理
			//osl_log_debug("[%s][%s][%d] hlen:%d clen:%d leftsize:%d datsize:%d\n",__MODULE__,__FUNCTION__,__LINE__,hlen,clen,leftsize,datsize);
			session->OnPacket( p0, hlen, clen,!m_close_flag,&session->m_link,ms);
			p0 += hlen + clen;
			leftsize  = leftsize - hlen - clen;

			m_head_length = -1;
			m_content_length = -1;
		}
		else //包不完整
		{
			//osl_log_debug("[%s][%s][%d] no imcomplete buf:%s\nhlen:%d clen:%d datasize:%d\n",__MODULE__,__FUNCTION__,__LINE__,buf,hlen,clen,datsize);
			break;
		}
		
	}
	//osl_log_debug("[%s][%s][%d] p - buf ;%d  p0 - buf :%d\n",__MODULE__,__FUNCTION__,__LINE__,p - buf,p0 - buf);
	if(p0 - buf == datsize)
		ret = true;
	else
	{
		*pos =  p0 - buf;
		//osl_log_debug("[%s][%s][%d] on implete httpbuf ,datsize:%d pos:%d left \nbuf:%s\n",__MODULE__,__FUNCTION__,__LINE__,datsize,*pos,buf);
		ret = false;
	}

	return ret;
}

//返回-1->结束 ,>0 正常且要继续收的字节 , ==0 表示刚好收满
int32_t CHttpParser::OnData(char_t*buf,int32_t datsize,uint64_t now_ms)
{
	bool ret = false;
	int32_t pos = 0;
	int32_t result = 0;//
	CTcpSession *session = ((CTcpSession*)m_session);
	char_t *tmp_buf;

	//osl_log_debug( "[%s][%s][%d]  datsize:%d buf:%s\n",__MODULE__,__FUNCTION__,__LINE__,datsize,buf);
	
	switch(m_status)
	{
		case HTTP_INIT:
			//osl_log_debug("HTTP_INIT datsize:%d %s \n",datsize,buf);
			ret = AnalysisBuf(buf,datsize,&pos,now_ms);
			if(!ret)
			{
				if( now_ms < m_act_tick  || m_act_tick == 0)
					m_act_tick = now_ms;
				if( m_act_tick + 60000 < now_ms )
				{
					osl_log_debug( "[%s][%s][%d] now-act=%u-%u=%u skt_idx:%llu\n",
						__MODULE__,__FUNCTION__,__LINE__, now_ms, m_act_tick, now_ms-m_act_tick ,session->m_link.skt_idx);
					result = -1;
					goto EXIT;
				}

				if(datsize - pos >= 4096 && m_head_length <= 0)
				{
					osl_log_error( "[%s][%s][%d]  datsize:%d pos:%d m_head_length:%d too much data!\n",__MODULE__,__FUNCTION__,__LINE__,datsize,pos,m_head_length);
					result = -1;
					goto EXIT;
				}
				else
					osl_log_debug( "[%s][%s][%d]  datsize:%d pos:%d \n",__MODULE__,__FUNCTION__,__LINE__,datsize,pos);
				if(m_buf)
				{
					free(m_buf);
					m_buf = NULL;
					m_buflen = 0;
					m_datasize = 0;
				}

				m_datasize = 0;
				if(m_head_length > 0)
				{
					m_buflen = m_head_length+m_content_length;
				}
				else	
					//默认分配4k
					m_buflen = 4096;
				
				if(m_buflen > 65536)
				{
					osl_log_debug("[%s][%s][%d] content_length:%d + m_head_length:%d > 64k skt_idx:%llu\n",
						__MODULE__,__FUNCTION__,__LINE__,m_content_length,m_head_length,session->m_link.skt_idx);
					result = -1;
					goto EXIT;
				}
				
				m_buf = (char_t *)malloc(ROUND_UP(m_buflen,1024));

				memcpy(m_buf,buf+pos,datsize-pos);
				m_datasize = datsize-pos;

				//osl_log_debug("[%s][%d]  m_datasize:%d datsize:%d pos:%d m_buflen:%d buf:%s \n",__func__,__LINE__,m_datasize,datsize,pos,m_buflen,buf);

				m_status = HTTP_LAST_HAVE_DATA;
				result = m_buflen - m_datasize;
				osl_log_debug("[%s][%s][%d] not complete need_recvsize:%d m_head_length:%d m_content_length:%d m_buflen:%d skt_idx:%llu\n",
					__MODULE__,__FUNCTION__,__LINE__,result,m_head_length,m_content_length,m_buflen,session->m_link.skt_idx);
				goto EXIT;
			}
			else
			{
				result = 0;
				goto EXIT;
			}
			break;
		case HTTP_LAST_HAVE_DATA:
			if(datsize > m_buflen - m_datasize)
			{
				osl_log_debug("[%s][%s][%d] datsize:%d > m_buflen:%d - m_datasize:%d skt_idx:%llu\n",
					__MODULE__,__FUNCTION__,__LINE__,datsize,m_buflen,m_datasize,session->m_link.skt_idx);
				result = -1;
				goto EXIT;
			}

			memcpy(m_buf+m_datasize,buf,datsize);
			m_datasize += datsize;
			//osl_log_debug("[%s] m_datasize:%d m_buf:%s\n",__func__,m_datasize,m_buf);
			ret = AnalysisBuf(m_buf,m_datasize,&pos,now_ms);
			if(!ret)
			{
				if( now_ms < m_act_tick  || m_act_tick == 0)
					m_act_tick = now_ms;
				if( m_act_tick + 60000 < now_ms )
				{
					osl_log_debug( "[%s][%s][%d] now-act=%u-%u=%u skt_idx:%llu\n",
						__MODULE__,__FUNCTION__,__LINE__, now_ms, m_act_tick, now_ms-m_act_tick ,session->m_link.skt_idx);
					result = -1;
					goto EXIT;
				}

				if(m_head_length > 0)
				{
					if(0 == pos)
					{
						//上一次http数据不足
						if( m_buflen < m_head_length + m_content_length)
						{
							osl_log_debug("[%s][%s][%d] shouldbe realloc m_buflen:%d m_head_length:%d m_content_length:%d skt_idx:%llu\n"
								,__MODULE__,__FUNCTION__,__LINE__,m_buflen,m_head_length,m_content_length,session->m_link.skt_idx);
							m_buflen = m_head_length + m_content_length;
							if(m_buflen > 65536)
							{
								osl_log_debug("[%s][%s][%d] content_length:%d + m_head_length:%d > 64k skt_idx:%llu\n",
									__MODULE__,__FUNCTION__,__LINE__,m_content_length,m_head_length,session->m_link.skt_idx);
								result = -1;
								goto EXIT;
							}
							
							m_buf = (char_t *)realloc(m_buf,ROUND_UP(m_buflen,1024));
						}
					}
					else if(pos > 0)
					{
						//已经处理完一个http包,但是多余的又不够一个http包

						m_buflen = m_head_length+m_content_length;
						if(m_buflen > 65536)
						{
							osl_log_debug("[%s][%s][%d] content_length:%d + m_head_length:%d > 64k skt_idx:%llu\n"
								,__MODULE__,__FUNCTION__,__LINE__,m_content_length,m_head_length,session->m_link.skt_idx);
							result = -1;
							goto EXIT;
						}

						tmp_buf = (char_t *)malloc(ROUND_UP(m_buflen,1024));
						memcpy(tmp_buf,m_buf+pos,m_datasize-pos);

						free(m_buf);
						m_buf = tmp_buf;

						m_datasize = m_datasize-pos;
					}

					result = m_buflen - m_datasize;
				}
				else if(m_head_length == -1)
				{
					//http数据太多了，但是多的那部分又不足一个完整的http包,连头部都没解析出来
					if(pos > 0)
					{						
						m_buflen = 4096;//默认就分配4096
						tmp_buf = (char_t *)malloc(ROUND_UP(m_buflen,1024));
						memcpy(tmp_buf,m_buf+pos,m_datasize-pos);
						free(m_buf);
						m_buf = tmp_buf;
						m_datasize = m_datasize-pos;
					}

					result = m_buflen - m_datasize;
					//osl_log_debug(" pos:%d result:%d m_buflen:%d m_datasize:%d\n",pos,result,m_buflen,m_datasize);
				}
								
				//osl_log_debug("jjjj m_buflen:%d m_datasize:%d m_buf:%s\n",m_buflen,m_datasize,m_buf);
				osl_log_debug("[%s][%s][%d] not complete pos:%d need_recvsize:%d m_head_length:%d m_content_length:%d m_buflen:%d m_datasize:%d skt_idx:%llu\n",
					__MODULE__,__FUNCTION__,__LINE__,pos,result,m_head_length,m_content_length,m_buflen,m_datasize,session->m_link.skt_idx);	
				goto EXIT;
			}
			else
			{
				result = 0;
				goto EXIT;
			}
			
			break;
	}

EXIT:
	//osl_log_debug("[%s] << status:%d m_buflen:%d m_datasize:%d\n",__func__,m_status,m_buflen,m_datasize);
	return result;
}

//需要继续收多少数据,-1 不知道要收多少数据 ,> 0 需要收多少数据
int32_t  CHttpParser::NeedRecvSize()
{
	if(m_status == HTTP_LAST_HAVE_DATA)
	{
		//osl_log_debug("[%s] need_size%d \n",__func__,m_buflen - m_datasize);
		return m_buflen - m_datasize;
	}

	//osl_log_debug("[%s] -1\n",__func__);
	return -1;
}
	
