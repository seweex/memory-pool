#ifndef SEWEEX_MEMORY_POOL
#define SEWEEX_MEMORY_POOL

#include <bit>
#include <list>
#include <map>
#include <memory>
#include <array>
#include <limits>
#include <thread>
#include <variant>
#include <mutex>
#include <atomic>
#include <shared_mutex>

namespace Seweex
{
	namespace Detail
	{
		class PageBlockInfo final
		{
		public:
			constexpr PageBlockInfo() noexcept = default;

			PageBlockInfo(PageBlockInfo&&)	    = delete;
			PageBlockInfo(PageBlockInfo const&) = delete;

			PageBlockInfo& operator=(PageBlockInfo&&)      = delete;
			PageBlockInfo& operator=(PageBlockInfo const&) = delete;

			void make_head(bool free, size_t size) noexcept {
				mySize = free ? static_cast<ptrdiff_t>(size) : -static_cast<ptrdiff_t>(size);
			}
			void remove_head() noexcept {
				mySize = 0;
				myPrev = nullptr;
			}

			void prev(PageBlockInfo& info) noexcept {
				myPrev = std::addressof(info);
			}
			void prev(PageBlockInfo* info) noexcept {
				myPrev = info;
			}

			_NODISCARD PageBlockInfo* prev() const noexcept {
				return myPrev;
			}

			_NODISCARD size_t size() const noexcept {
				return mySize >= 0 ? mySize : -mySize;
			}

			_NODISCARD bool is_free() const noexcept {
				return mySize > 0;
			}

		private:
			PageBlockInfo* myPrev = nullptr;
			ptrdiff_t      mySize = 0;
		};

		template <class _Ty>
		concept Allocator = requires (
			_Ty _val, 
			typename std::allocator_traits<_Ty>::size_type size,
			typename std::allocator_traits<_Ty>::pointer data
		) {
			{ _val.allocate(size) } -> std::same_as<typename std::allocator_traits<_Ty>::pointer>;
			{ _val.deallocate(data, size) };

			typename _Ty::value_type;
		};
	}

	namespace Memory
	{
		template <size_t _Size, size_t _Alignment>
		requires (
			std::has_single_bit(_Alignment) &&
			_Size % _Alignment == 0 &&
			_Size > 0
		)
		class alignas(_Alignment) Page final
		{
			static constexpr size_t blocks_count = _Size / _Alignment;

			using storage_type = std::array<unsigned char, _Size>;
			using info_type    = std::array<Detail::PageBlockInfo, blocks_count>;

		public:
			class Hint final
			{
			private:
				friend class Page;

				using iterator = typename info_type::const_iterator;

				constexpr Hint (
					iterator iter,
					iterator end
				) noexcept :
					myIter (iter),
					myEnd  (end)
				{}

				constexpr Hint (iterator end) noexcept :
					myIter (end),
					myEnd  (end)
				{}

			public:
				constexpr Hint() noexcept = default;

				constexpr Hint(Hint&&)      noexcept = default;
				constexpr Hint(Hint const&) noexcept = default;

				constexpr Hint& operator=(Hint&&)      noexcept = default;
				constexpr Hint& operator=(Hint const&) noexcept = default;

				_NODISCARD constexpr bool is_valid() const noexcept {
					return std::holds_alternative<iterator>(myEnd) 
						&& myIter != std::get<iterator>(myEnd);
				}

				_NODISCARD constexpr bool is_valid(iterator const& trueEnd) const noexcept {
					if (std::holds_alternative<iterator>(myEnd)) {
						auto const& end = std::get<iterator>(myEnd);
						return end == trueEnd && myIter != end;
					}
					else 
						return false;
				}

				_NODISCARD constexpr operator bool() const noexcept {
					return is_valid();
				}

			private:
				iterator myIter;
				std::variant<std::monostate, iterator> myEnd;
			};

		private:
			template <class _Ty>
			_NODISCARD static constexpr size_t size_in_blocks(size_t count) noexcept {
				return (sizeof(_Ty) * count + _Alignment - 1) & ~(_Alignment - 1);
			}

			_NODISCARD constexpr typename info_type::iterator from_hint(Hint const& hint) noexcept 
			{
				return hint.is_valid(myInfo.cend()) ?
					   myInfo.begin() + (hint.myIter - myInfo.cbegin()) :
					   myInfo.end();
			}

		public:
			constexpr Page() noexcept {
				myInfo.front().make_head(true, blocks_count);
			}

			Page(Page&&)	  = delete;
			Page(Page const&) = delete;

			Page& operator=(Page&&)      = delete;
			Page& operator=(Page const&) = delete;

			template <class _Ty>
			_NODISCARD static constexpr float load_of(size_t count) noexcept {
				constexpr float step = sizeof(_Ty) / static_cast<float>(_Size);
				return step * count;
			}

			_NODISCARD static constexpr float max_load() noexcept {
				return 1;
			}

			_NODISCARD float load() const noexcept {
				return myLoad;
			}

			template <class _Ty>
			_NODISCARD constexpr Hint fit(size_t count) const noexcept 
			{
				if constexpr (alignof(_Ty) <= _Alignment)
				{
					auto const blocks = size_in_blocks<_Ty>(count);
					auto	   iter   = myInfo.begin();

					while (iter != myInfo.end()) {
						auto const size = iter->size();

						if (iter->is_free() && size >= blocks)
							return { iter, myInfo.end() };

						iter += size;
					}
				}
				
				return { myInfo.end() };
			}

			template <class _Ty>
			_NODISCARD constexpr Hint contains(_Ty* data, size_t count) const noexcept 
			{
				if constexpr (alignof(_Ty) <= _Alignment)
				{
					auto const storageBegin = reinterpret_cast<uintptr_t>(myData.data());
					auto const storageEnd   = storageBegin + _Size;

					auto const dataBegin = reinterpret_cast<uintptr_t>(data);
					auto const dataEnd   = reinterpret_cast<uintptr_t>(data + count);

					if (dataBegin >= storageBegin && 
						dataEnd <= storageEnd &&
						dataBegin % _Alignment == 0
					) {
						auto const offset = (dataBegin - storageBegin) / _Alignment;
						auto const blocks = size_in_blocks<_Ty>(count);
						auto	   iter	  = myInfo.begin() + offset;

						if (!iter->is_free() && iter->size() == blocks)
							return { iter, myInfo.end() };
					}
				}

				return { myInfo.end() };
			}

			template <class _Ty>
			_NODISCARD constexpr _Ty* try_occupy(size_t count) noexcept {
				return try_occupy<_Ty>(count, fit<_Ty>(count));
			}

			template <class _Ty>
			_NODISCARD constexpr _Ty* try_occupy(size_t count, Hint const& hint) noexcept
			{
				auto const blocks = size_in_blocks<_Ty>(count);
				auto	   iter   = from_hint(hint);

				if (iter != myInfo.end())
				{
					auto const size = iter->size();

					if (size > blocks) {
						auto next = iter + blocks;

						if (next != myInfo.end()) {
							next->make_head(true, size - blocks);
							next->prev(*iter);
						}
					}

					myLoad += static_cast<float>(blocks) / blocks_count;
					iter->make_head(false, blocks);

					auto const offset  = iter - myInfo.begin();
					auto const storage = reinterpret_cast<_Ty*>(std::addressof(myData[offset * _Alignment]));

					return std::assume_aligned<_Alignment>(storage);
				}

				return nullptr;
			}

			template <class _Ty>
			constexpr bool release(_Ty* ptr, size_t count) noexcept {
				return release(contains(ptr, count));
			}

			constexpr bool release(Hint const& hint) noexcept
			{
				auto iter = from_hint(hint);

				if (iter != myInfo.end())
				{
					auto size    = iter->size();
					auto prev    = iter->prev();
					auto next    = iter + size;
					auto farNext = myInfo.end();

					myLoad -= static_cast<float>(size) / blocks_count;
					iter->make_head(true, size);

					if (next != myInfo.end() && next->is_free()) 
					{
						auto const nextSize = next->size();
						farNext = next + nextSize;

						if (farNext != myInfo.end())
							farNext->prev(*iter);

						iter->make_head(true, size += nextSize);
						next->remove_head();
					}

					if (prev != nullptr && prev->is_free()) 
					{
						prev->make_head(true, size + prev->size());
						iter->remove_head();

						if (farNext != myInfo.end())
							farNext->prev(prev);
					}

					return true;
				}

				return false;
			}

		private:
			alignas(_Alignment) storage_type myData = {};
							    info_type	 myInfo = {};
								float		 myLoad = 0;
		};

		template <
			size_t _Size,
			size_t _Alignment,
			Detail::Allocator _AllocTy = std::allocator <Page<_Size, _Alignment>>
		>
		class Pool final
		{
			using page_type = Page<_Size, _Alignment>;

		public:
			using allocator_type = typename std::allocator_traits <_AllocTy>::
								   template rebind_alloc<std::pair<float const, std::unique_ptr<page_type>>>;

		private:
			void pages_allocating_proc(std::stop_token stop) 
			{
				while (!stop.stop_requested())
				{
					constexpr auto maxLoad = page_type::max_load();

					float maxPageLoad;
					float averageLoad;

					{
						decltype(myPages.begin()) first;

						std::shared_lock lock{ myAllocateMutex };

						first = myPages.begin();
						maxPageLoad = first != myPages.end() ? first->first : maxLoad;
					}

					{
						std::shared_lock lock{ myReserveMutex };
						averageLoad = myAverageLoadRequest;
					}

					if (maxPageLoad + averageLoad >= maxLoad)
						make_pages(1);
					else
						std::this_thread::yield();
				}
			}

		public:
			constexpr Pool (allocator_type const& alloc) noexcept :
				myPages  (alloc),
				myThread (pages_allocating_proc, this)
			{}

			constexpr ~Pool() noexcept {
				myThread.request_stop();
				myThread.join();
			}

			void make_pages(size_t count) 
			{
				for (size_t i = 0; i < count; ++i)
				{
					auto	   page = std::make_unique<page_type>();
					auto const load = page->load();

					std::unique_lock lock{ myAllocateMutex };
					
					myPages.emplace_hint(myPages.begin(), load, std::move(page));
				}
			}

			template <class _Ty>
			_NODISCARD _Ty* occupy(size_t count) noexcept 
			{
				auto const load = page_type::load_of<_Ty>(count);
				_Ty* ptr;

				{
					std::unique_lock lock{ myAllocateMutex };

					if (!myPages.empty()) 
					{
						auto iter = myPages.upper_bound(page_type::max_load() - load);

						if (iter == myPages.begin())
							ptr = nullptr;
						
						else {
							auto hint = iter;
							auto node = myPages.extract(--iter);

							auto&	   page = node.mapped();
							auto const load = page->load();

							ptr = page->try_occupy<_Ty>(count);

							myPages.emplace_hint(++hint, load, std::move(page));
						}
					}
					else
						ptr = nullptr;
				}

				{
					std::unique_lock lock{ myReserveMutex };
					myAverageLoadRequest = (myAverageLoadRequest * myRequestsCount + load) / ++myRequestsCount;
				}

				return ptr;
			}

			// some methods hasn't been realized yet ...

		private:
			std::multimap <float, std::unique_ptr<page_type>> myPages;

			std::jthread      myThread;
			std::shared_mutex myAllocateMutex;
			std::shared_mutex myReserveMutex;

			float  myAverageLoadRequest = 0;
			size_t myRequestsCount		= 0;
		};
	}
}

#endif 