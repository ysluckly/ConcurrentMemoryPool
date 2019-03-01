#pragma once

#include "Common.h"
// 1.page cache是一个以页为单位的span自由链表
// 2.为了保证全局只有唯一的page cache，这个类被设计成了单例模式。
class PageCache
{
public:
	static PageCache* GetInstance()
	{
		return &_inst;
	}

	//获取span，页数通过计算得到
	Span* NewSpan(size_t npage);
	//防止死锁，定义新接口
	Span* _NewSpan(size_t npage);

	//获取从对象到span的映射
	Span* MapObjectToSpan(void* obj);

	// 释放空闲span回到Pagecache，并合并相邻的span
	void ReleaseSpanToPageCahce(Span* span);

private:
	SpanList _pagelist[NPAGES];
private:
	PageCache() = default;
	PageCache(const PageCache&) = delete;
	PageCache& operator=(const PageCache&) = delete;

	static PageCache _inst;
	std::mutex _mutex;

	std::unordered_map<PageID, Span*> _id_span_map;
};