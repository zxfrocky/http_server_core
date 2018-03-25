#ifndef __SERVER_COMMON_H__
#define __SERVER_COMMON_H__

//数据包处理
typedef void (*PHandlePacket)(void *server, void *group, void *packet_header_ptr, 
		char_t *buf, int32_t header_length, int32_t content_length,bool keepalive, uint64_t now,void *param);

typedef enum
{
    PACKET_START = 0x1,/*起始包*/
    PACKET_MIDDLE = 0x2,/*中间包*/
    PACKET_END = 0x4,/*结束包*/
}EPacketPos;

typedef struct
{
    uint32_t listen_ip;
    uint16_t listen_port;
    bool ssl;
    char_t model;//分配式还是抢占式
    bool need_response;
    SOCKET listen_skt;
}SListenParam;
	
typedef struct
{
    int32_t group_num;//server group个数,0表示四个
    int32_t thread_num;//每个group下抢占处理线程的个数,0表示cpu个数个,最大不超过128个
    int32_t time_order_thread_num;//每个group下时序处理线程的个数,0表示4个,最大不超过128个线程
    int32_t group_queue_size;//in tcpcache out tcpcache 能存多少个 packet ,默认20万
    int32_t thread_queue_size;//分配式线程 缓存大小 个数
    int32_t thread_stack_size;//栈内存大小
    char_t model;//1--抢占式 2--分配式 3 (0x01|0x02)--抢占式和分配式
}SGroupParam;

//内存对齐,从小到大排列,节省内存
typedef struct
{
    bool ssl;
    char_t need_response;
    char_t model;//分配式式还是抢占式
    uint16_t remote_port;
    uint32_t remote_ip;
    SOCKET skt;
    int64_t skt_idx;
}STcpLink;

typedef struct
{
    char_t flag;//是结束还是开始，1 开始 2 中间 4结束 
    int64_t uin; //uid
    STcpLink link;
}SPacketHeader;

typedef struct
{
	char_t *buf;//输入的bufffer
	int32_t buflen;//buffer长度
	SPacketHeader header;
}SOutPkt;

typedef struct
{	
	bool keepalive;
    int len;
    int hlen;
    int clen;
    char_t  *buf;
	int32_t bufsize;
	SPacketHeader header;
}SInPkt;

#endif
