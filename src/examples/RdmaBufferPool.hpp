/* \file RdmaBufferPool.hpp
 * \author Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#include <stdexcept>
#include <queue>

#include <rdma/rdma_verbs.h>

namespace rdma {

class BufferPool {
public:
	BufferPool(unsigned int count, size_t messageSize, int access,
			struct ibv_pd *pd);
	BufferPool(const BufferPool &) = delete;
	~BufferPool();
	BufferPool &operator =(const BufferPool &) = delete;

	template <typename T>
	T *borrow();
	template <typename T>
	void give(T *buf);

private:
	char *bufferSpace;
	struct ibv_mr *mr;
	unsigned int count;
	size_t perBufSize;
	std::queue<unsigned int> avail;
};

template <typename T>
T *BufferPool::borrow()
{
	int x = avail.dequeue();
	return reinterpret_cast<T *>(bufferSpace + x * perBufSize);
}

template <typename T>
void BufferPool::give(T *buf)
{
	auto x = reinterpret_cast<uintptr_t>(buf - reinterpret_cast<uintptr_t>(bufferSpace));
	auto y= x / perBufSize;
	if ((x % perBufSize) || y >= count) {
		throw std::invalid_argument("buf did not come from this pool");
	}
	avail.enqueue(y);
}

} /* namespace rdma */
