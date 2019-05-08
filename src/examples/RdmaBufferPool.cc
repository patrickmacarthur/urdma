/* RdmaBufferPool.cc
 * Patrick MacArthur <patrick@patrickmacarthur.net>
 */

#include "RdmaBufferPool.hpp"

namespace rdma {

BufferPool::BufferPool(unsigned int count, size_t messageSize, int access,
			struct ibv_pd *pd)
	: count(count)
{
	perBufSize = CACHE_LINE_SIZE;
	while (perBufSize < messageSize)
		perBufSize += CACHE_LINE_SIZE;
	if (SIZE_MAX / count < perBufSize) {
		throw std::length_error("overflow calculating total buffer size");
	}
	size_t totalBufSize = count * perBufSize;
	bufferSpace = aligned_alloc(CACHE_LINE_SIZE, totalBufSize);
	if (!bufferSpace) {
		throw std::bad_alloc();
	}
	errno = 0;
	mr = ibv_reg_mr(pd, bufferSpace, count * totalBufSize, access);
	if (!mr) {
		throw std::system_error(errno ? errno : ENOMEM,
				std::system_category());
	}

	for (unsigned int x = 0; x < count; ++x) {
		avail.enqueue(x);
	}
}

BufferPool::~BufferPool()
{
	ibv_dereg_mr(mr);
	free(bufferSpace);
}

}
