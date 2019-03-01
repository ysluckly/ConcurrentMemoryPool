#include "PageCache.h"

PageCache PageCache::_inst;

Span* PageCache::_NewSpan(size_t npage)
{
	//std::unique_lock<std::mutex> lock(_mutex);//死锁，递归调用自己的时候已经锁住了自己，解决方案，重开一个子接口

	if (!_pagelist[npage].Empty())
	{
		return _pagelist[npage].PopFront();
	}

	for (size_t i = npage + 1; i < NPAGES; ++i)
	{
		SpanList* pagelist = &_pagelist[i];
		if (!pagelist->Empty())
		{
			Span* span = pagelist->PopFront();
			Span* split = new Span;
			split->_pageid = span->_pageid + span->_npage - npage;	//尾切
			split->_npage = npage;
			span->_npage -= npage;

			_pagelist[span->_npage].PushFront(span);
			
			//建立映射,便于回收
			for (size_t i = 0; i < split->_npage; ++i)
			{
				_id_span_map[split->_pageid + i] = split;
			}

			return split;
		}
	}

	//内存不足
	void* ptr = SystemAlloc(NPAGES-1);

	Span* largespan = new Span;
	largespan->_pageid = (PageID)ptr >> PAGE_SHIFT;
	largespan->_npage = NPAGES - 1;

	_pagelist[NPAGES - 1].PushFront(largespan);

	//建立映射,便于回收
	for (size_t i = 0; i < largespan->_npage; ++i)
	{
		_id_span_map[largespan->_pageid+i] = largespan;
	}

	Span* span = _NewSpan(npage);
	span->_objsize = npage << PAGE_SHIFT;
	return span;
}

Span* PageCache::NewSpan(size_t npage)
{
	std::unique_lock<std::mutex> lock(_mutex);//原来可能死锁，递归调用自己的时候已经锁住了自己，现在得以改进
	
	if (npage >= NPAGES)
	{
		void* ptr = SystemAlloc(npage);

		//不能直接返回，找的时候找不到，返回内存的时候不知道多大，
		Span* span = new Span;
		span->_pageid = (PageID)ptr >> PAGE_SHIFT;
		span->_npage = npage;

		span->_objsize = npage << PAGE_SHIFT;  //对象总的字节数

		_id_span_map[span->_pageid] = span;
		return span;
	}
	
	return _NewSpan(npage);
}


//获取从对象到span的映射
Span* PageCache::MapObjectToSpan(void* obj)
{
	PageID pageid = (PageID)obj >> PAGE_SHIFT;
	auto it = _id_span_map.find(pageid);
	assert(it != _id_span_map.end());  //严格说必须找到，否则出问题

	return it->second;
}

// 释放空闲span回到Pagecache，并合并相邻的span
void PageCache::ReleaseSpanToPageCahce(Span* span)
{
	std::unique_lock<std::mutex> lock(_mutex);

	if (span->_npage >= NPAGES)
	{
		void* ptr = (void*)(span->_pageid << PAGE_SHIFT);
		_id_span_map.erase(span->_pageid);   //除去映射
		SystemFree(ptr);
		delete span;
		return;
	}

	auto previt = _id_span_map.find(span->_pageid - 1);
	while (previt != _id_span_map.end())
	{
		Span* prevspan = previt->second;
		//不空闲
		if (prevspan->_usecount != 0)
			break;

		//如果合出了NPAGES，则不合并，否则没办法管理,可能是一个小缺陷
		if (prevspan->_npage + span->_npage >= NPAGES)
			break;

		_pagelist[prevspan->_npage].Erase(prevspan);
		prevspan->_npage += span->_npage;
		delete span;
		span = prevspan;

		previt = _id_span_map.find(span->_pageid - 1);
	}

	auto nextit = _id_span_map.find(span->_pageid + span->_npage);
	while (nextit != _id_span_map.end())
	{
		Span* nextspan = nextit->second;
		if (nextspan->_usecount != 0)
			break;

		//如果合出了NPAGES，则不合并，否则没办法管理,可能是一个小缺陷
		if (nextspan->_npage + span->_npage >= NPAGES)
			break;

		_pagelist[nextspan->_npage].Erase(nextspan);
		span->_npage += nextspan->_npage;
		delete nextspan;

		nextit = _id_span_map.find(span->_pageid + span->_npage);
	}

	//重新映射
	for (size_t i = 0; i < span->_npage; ++i)
	{
		_id_span_map[span->_pageid + i] = span;
	}

	_pagelist[span->_npage].PushFront(span);
}