#ifndef __HTTP_PARSE_H__
#define __HTTP_PARSE_H__

typedef enum
{
	HTTP_INIT = 0,
	HTTP_LAST_HAVE_DATA,
}EHttpAction;

class CHttpParser
{
public:
	CHttpParser(void *session);
	~CHttpParser();

	bool AnalysisBuf(char_t *buf,int32_t datsize,int32_t *pos,uint64_t ms);
	void Stop();
	bool GetCloseFlag();
	int32_t OnData(char_t*buf,int32_t buflen,uint64_t now_ms);
	int32_t NeedRecvSize();//需要继续收多少数据,-1 不知道要收多少数据 ,> 0 需要收多少数据
private:
	char_t m_status;
	char_t *m_buf;
	int32_t m_buflen;
	int32_t m_datasize;
	uint64_t m_act_tick;
	int32_t m_head_length;
	int32_t m_content_length;
	bool m_close_flag;
	void *m_session;
};
#endif