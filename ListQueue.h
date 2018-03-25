#ifndef __LIST_QUEUE_H__
#define __LIST_QUEUE_H__

template<class T> class CListQueue;  
template <class T>  
class QueueItem  
{  
    //因为Queue是需要使用到这个不公开接口的类中的成员,所以需要将Queue声明为友元类  
    friend class CListQueue<T>;  
private:  
    //复制构造函数  
    QueueItem<T>( const T &q ): item( q ) , next( 0 )  {}  
    //元素值  
    T item;  
    //指向下一个元素的指针  
    QueueItem<T> *next;  
};  

//类模板  
template <class T>  
class CListQueue  
{  
public:  
    //需要注意的一点是,一般在类中实现的函数都会是内联函数  
    //使用内联函数的要求是,该函数的代码一般只有几行,并且经常执行  
    //默认构造函数  
    //这样的声明也是可以的  
    //CListQueue() :head( 0 ) , tail( 0 ) {}  
    CListQueue<T>() :head( 0 ) , tail( 0 ) {}  
    //复制构造函数  
      
    //当然这样的声明也是可以的  
    //CListQueue( const CListQueue &q ): head( q.head ) , tail( q.tail ) { copy_elems( q ); }  
    CListQueue<T>( const CListQueue<T> &q) :head( q.head ) , tail( q.tail ) { copy_elems( q ); }  
    //操作符重载  
    CListQueue<T>& operator= ( const CListQueue<T>& );  
    //析构函数  
    ~CListQueue(){ destroy(); }    
    //获取头节点的元素  
    T& front();  
      
    //下面这个是const版本的  
    //const T& front() { return head->item; }  
      
    //入队列  
    void push( const T& );  
    //出队列  
    void pop();  
    //判断是否为空  
    bool empty() const { return NULL == head; }   
private:  
    //队列的头指针,尾指针,主要用于出入队列用  
    QueueItem<T> *head;  
    QueueItem<T> *tail;  
      
    //销毁队列数据  
    void destroy();  
    //复制元素  
    void copy_elems( const CListQueue& );  
}; 

#endif