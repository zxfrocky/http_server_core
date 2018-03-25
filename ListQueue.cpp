#include "stdafx.h"
#include "ListQueue.h"

//出队列,即删除队列头部的元素  
template<typename T>   
void CListQueue<T>::pop()  
{     
    //判断是否为空指针  
    if( NULL == head )  
    {  
        return;  
    }  
      
    //保存当前指针的值  
    QueueItem<T> *p = head;  
    //将头指针指向下一个元素  
    head = head->next;  
    //删除  
    delete p;  
}  
  
//入队列,即从队列的尾部插入数据  
template<typename T>  
void CListQueue<T>::push( const T &t )  
{  
    //构造一个对象  
    QueueItem<T> *p = new QueueItem<T>( t );   
    //在插入队列尾部的时候需要判断队列是否为空的！  
    if( empty() )  
    {  
        head = tail = p;  
    }  
    else  
    {  
        //将尾指针的指向下一个元素的指针指向生成的数据  
        tail->next = p;  
          
        //将尾指针移动到最后一个数据上去  
        tail = p;  
    }  
}  
  
//销毁数据  
template<typename T>  
void CListQueue<T>::destroy()  
{  
    //不断地出队列即可  
    while( !empty() )  
    {  
        pop();  
    }  
}  
  
//赋值操作符重载  
template<typename T>  
CListQueue<T>& CListQueue<T>::operator= (const CListQueue<T> &q)  
{  
    //复制队列元素,结束条件是head为空的时候  
    for( QueueItem<T> *p= q.head ; p  ; p = p->next )  
    {  
        push( p->item );  
    }  
}  
   
template<typename T>  
T& CListQueue<T>::front()
{  
	T tmp;
	if(NULL != head)
		return head->item;

	return tmp;
}  