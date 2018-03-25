#ifndef __TCPCACHE_H__
#define __TCPCACHE_H__

//#include "TcpSession.h"

template<class TYPE>
class CTcpCache
{
public:
	CTcpCache();
	~CTcpCache();

	//初始化
	void Create( int32_t bufsize );
	//释放
	void Destroy();

	//邮递消息
	bool Post( TYPE& type, char_t *buf=NULL, int32_t size=0 );
	//读取消息
	int32_t Read( TYPE *type, char_t *buf=NULL, int32_t size=0 );
	//清除
	void Clear();
	//判断数据是否为空
	bool IsEmpty();
	//读取消息的头部和消息的长度,但是不移动任何下标
	int32_t ReadHeader( TYPE *link);

protected:
	uchar_t *m_buf;
	int32_t m_bufsize;
	int32_t m_head;
	int32_t m_tail;
};

#endif //__TCPCACHE_H__
