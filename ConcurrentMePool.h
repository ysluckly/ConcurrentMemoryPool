#pragma once

#include "Common.h"
#include "ThreadCache.h"
#include "PageCache.h"

//
static void* ConcurrentAlloc(size_t size)
{
	if (size > MAXBYTES)
	{
		//算出对齐数，大数用页对齐，否则浪费严重，并且借助pagecache，所以页对齐
		size_t roundsize = ClassSize::_Roundup(size,1<<PAGE_SHIFT);
		//页数
		size_t npage = roundsize >> PAGE_SHIFT;

		Span* span = PageCache::GetInstance()->NewSpan(npage);
		void* ptr = (void*)(span->_pageid << PAGE_SHIFT);
		return ptr;
	}
	else
	{
		// 通过tls，定义线程独有的全局变量，根据变量获取线程自己的threadcache
		if (tls_threadcache == nullptr)
		{
			tls_threadcache = new ThreadCache;
			//cout << "新创建的线程"<<std::this_thread::get_id() << "->" << tls_threadcache << endl;
		}
		return tls_threadcache->Allocate(size);
	}
}

static void ConcurrentFree(void* ptr)
{
	Span* span = PageCache::GetInstance()->MapObjectToSpan(ptr);
	size_t size = span->_objsize;

	if (size > MAXBYTES)
	{
		PageCache::GetInstance()->ReleaseSpanToPageCahce(span);
	}
	else
	{
		tls_threadcache->Deallocate(ptr, size);
	}
}