#include "CentralCache.h"
#include "PageCache.h"

CentralCache CentralCache::_inst;

//// 打桩	(伪数据测试)
//// 从中心缓存获取一定数量的对象给thread cache
//size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t n, size_t bytes)
//{	 //验证threadcache在中心获取内存
//	// 通过伪数据测试功能：单元测试中叫做打桩
//	start = malloc(n*bytes);
//	end = (char*)start + (n - 1)*bytes;
//	//链接成链表
//	void* cur = start;
//	while (cur != end)
//	{
//		void* next = (char*)cur + bytes;
//		NEXT_OBJ(cur) = next;
//		cur = next;
//	}
//	NEXT_OBJ(end) = nullptr;
//	return n;
//}

//获取不为空的span，至少有一个
Span* CentralCache::GetOneSpan(SpanList* spanlist, size_t bytes)
{
	Span* span = spanlist->begin();
	while (span != spanlist->end())
	{
		if (span->_objlist != nullptr)
			return span;

		span = span->_next;
	}

	//通过bytes计算出npage
	size_t npage = ClassSize::BytesMovePage(bytes);
	//全空span， 向pagecache申请一个新的合适大小的span
	Span* newspan = PageCache::GetInstance()->NewSpan(npage);

	// 将span的内存切割成一个个bytes大小的对象挂起来
	char* start = (char*)(newspan->_pageid << PAGE_SHIFT);
	char* end = start + (newspan->_npage << PAGE_SHIFT);
	char* cur = start;
	char* next = cur + bytes;
	while (next < end)
	{
		NEXT_OBJ(cur) = next;
		cur = next;
		next = cur + bytes;
	}
	NEXT_OBJ(cur) = nullptr;
	newspan->_objlist = start;
	newspan->_objsize = bytes;
	newspan->_usecount = 0;

	// 将newspan插入到spanlist
	spanlist->PushFront(newspan);
	return newspan;
}

// 从中心缓存获取一定数量的对象给 central->thread cache
size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t num, size_t bytes)
{
	size_t index = ClassSize::Index(bytes);
	SpanList* spanlist = &_spanlist[index];

	//对桶加锁

	//spanlist->_mutex.lock() 
	//可能内存不足，逐级向上申请，但是如果virtualallov申请不成功就抛异常
	//就需要捕获异常，并且不会释放锁，就产生死锁问题，对于性能问题大大降低
	//所以运用RALL
	std::unique_lock<std::mutex> lock(spanlist->_mutex);

	Span* span = GetOneSpan(spanlist, bytes);  

	void* cur = span->_objlist;
	void* prev = cur;
	size_t fetchnum = 0;
	while (cur != nullptr && fetchnum < num)
	{
		prev = cur;
		cur = NEXT_OBJ(cur);
		++fetchnum;
	}

	start = span->_objlist;
	end = prev;
	NEXT_OBJ(end) = nullptr;

	span->_objlist = cur;
	span->_usecount += fetchnum;

	//当第一个span为空，移到尾上，减少下次遍历空span
	if (span->_objlist == nullptr)
	{
		spanlist->Erase(span);
		spanlist->PushBack(span);
	}

	return fetchnum;
}

//thread->central
void CentralCache::ReleaseListToSpans(void* start, size_t byte)
{
	//释放对桶加锁
	size_t index = ClassSize::Index(byte);
	SpanList* spanlist = &_spanlist[index];
	std::unique_lock<std::mutex> lock(spanlist->_mutex);

	//找到合适的span头删
	while (start)
	{
		void* next = NEXT_OBJ(start);
		Span* span = PageCache::GetInstance()->MapObjectToSpan(start);

		//当释放的对象回到空的span，把span移到头上，减少遍历空span
		if (span->_objlist == nullptr)
		{
			spanlist->Erase(span);
			spanlist->PushFront(span);
		}

		NEXT_OBJ(start) = span->_objlist;
		span->_objlist = start;

		//usecount == 0，表示空闲，即释放的归还了，回收到pagecache进行合并
		if (--span->_usecount == 0)
		{
			_spanlist[index].Erase(span);
			span->_objlist = nullptr;
			span->_objsize = 0;
			span->_next = span->_prev = nullptr;

			PageCache::GetInstance()->ReleaseSpanToPageCahce(span);
		}

		start = next;
	}

}