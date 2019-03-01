#include "ThreadCache.h"
#include "CentralCache.h"


//第一种方法：每次都是从CentralCache获取固定数量的对象：10 【申请固定数量，到达一定数量后自动回收，问题就是64k也是固定申请数量，那么就很过分了哦】
//void* ThreadCache::FetchFromCentralCache(size_t index, size_t byte)
//{
//	FreeList* freelist = &_freelist[index];
//	size_t num = 10;
//
//	void* start, *end;
//	//申请num，中心不一定有，需要获取实际申请的个数，但是至少保证有一个
//	size_t fetchnum = CentralCache::GetInstance()->FetchRangeObj(start, end, num, byte);
//	if (fetchnum == 1)
//		return start;
//	//多个的话，将第一个返回，其余的挂在自由链表上
//	freelist->PushRange(NEXT_OBJ(start), end, fetchnum - 1);
//	return start;
//}
//方法二：每次获取时，获取数量呈现线性增长的过程
//比如喝水与跑步一样，应该是一个慢性增长的过程，但是应该有一个巅峰，
//但是有个问题，比如thread链表超过2M就回收，申请几个16字节对象，那么这几个永远不能回到CentralCache,
//CentralCache对应span永远回不到PageCache，也就是不能合成更大的页，
//还有另外一个问题：就是一次申请批量的结点，多的挂在链表以备下次使用，但是申请多了话就造成空间浪费问题，
//基于这样的原因就是：申请的数量设置成线程增长，才开始少量，不用该大小对象就不申请，在次用就申请更多，
//也就解决了如果这个字节大小对象频繁申请释放，他也可以自动进行合并在开辟，很好的减少了内碎片问题
//综前：每次申请对象的大小数目由字节决定，（逐渐增加）

void* ThreadCache::FetchFromCentralCache(size_t index, size_t byte)
{
	FreeList* freelist = &_freelist[index];
	size_t num_to_move = ClassSize::BytesMoveNum(byte);

	num_to_move = min(num_to_move,freelist->MaxSize());	//小对象申请数量多，移动多，8字节，也就移动512，大对象申请少，移动小，1024也就移动64个
	void* start  = nullptr, *end = nullptr;
	size_t fetchnum = CentralCache::GetInstance()->FetchRangeObj(start, end, num_to_move, byte);
	if (fetchnum >1)
		freelist->PushRange(NEXT_OBJ(start), end, fetchnum - 1);

	//申请需求越频繁，需要越大，那么就逐渐增大申请个数
	//并且达到512，控制增长（停止）
	if (num_to_move <= freelist->MaxSize())
	{
		freelist->SetMaxSize(num_to_move+1);
	}

	return start;
}

void* ThreadCache::Allocate(size_t byte)
{
	assert(byte <= MAXBYTES);

	//建立自定义的对齐规则
	// 根据对齐规则：size向上取整
	byte = ClassSize::Roundup(byte);
	//获取对齐数对应的下标
	size_t index = ClassSize::Index(byte);
	FreeList* freelist = &_freelist[index];
	if (!freelist->Empty())
	{
		//链表头删一个节点并返回
		return freelist->Pop();
	}
	else
	{
		//链表为空，从Centralache中获取对象，
		//取一个不合理，每次都加锁增加消耗，可以多取一点，多的挂在链表上，以备下次使用
		return FetchFromCentralCache(index, byte);
	}
}

void ThreadCache::ListTooLong(FreeList* freelist, size_t byte)
{
	//粗暴解决，直接消除该链表,t头指针交给中心cache
	void* start = freelist->Clear();
	CentralCache::GetInstance()->ReleaseListToSpans(start,byte);
	

}
void ThreadCache::Deallocate(void* ptr, size_t byte)
{
	assert(byte <= MAXBYTES);
	size_t index = ClassSize::Index(byte);
	FreeList* freelist = &_freelist[index];
	freelist->Push(ptr);

	//当自由链表对象数量超过一次批量从中心缓存移动（申请）的数量时，开始进行回收对象到中心cache
	if (freelist->Size() >= freelist->MaxSize())
	{
		//粗暴，全部回收:缺陷：在次用就要再次申请，竞争锁，增加开销，但是又为后期的合并内存做好了铺垫，即减少内存碎片
		ListTooLong(freelist, byte);
	}

	//项目不足：释放的逻辑还可以更强一些就是threadcache的链表的总的字节数超过2M，则开始释放
}