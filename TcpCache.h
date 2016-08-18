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
	bool Post( TYPE& type, char_t *buf, int32_t size );
	//读取消息
	int32_t Read( TYPE *type, char_t *buf, int32_t size );
	//清除
	void Clear();

	bool IsEmpty();

protected:
	uchar_t *m_buf;
	int32_t m_bufsize;
	int32_t m_head;
	int32_t m_tail;
};

#endif //__TCPCACHE_H__
