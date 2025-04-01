#include "MsQuicThreadTaskPool.h"

MsQuicThreadTaskPool::MsQuicThreadTaskPool(size_t size):taskThreads(size),taskThreadCount(size),isRunning(true), threadCount(size)
{
	if (!isRunning) return;

	for (int i = 0; i < size; i++) {
		taskThreads.emplace_back(std::thread([this]() {
			for (;;) {
				
				if (!isRunning) return;

				std::shared_ptr<std::function<void()>> task = nullptr;

				{
					std::unique_lock<std::mutex> lock(taskMutexs);

					condition.wait(lock, [this]() {

						bool isWait = (isRunning && !tasks.empty());

						if (isWait) {

							--taskThreadCount;

						}
						else {

							++taskThreadCount;

						}
						return isWait;

						});

					task = tasks.front();

					tasks.pop();
			
				}

				if (task != nullptr) {

					(*task)();

				}

			}
			}));
	}
}


MsQuicThreadTaskPool::~MsQuicThreadTaskPool()
{
	stop();
}

MsQuicThreadTaskPool* MsQuicThreadTaskPool::getInstannce()
{
	static MsQuicThreadTaskPool msQuicThreadTaskPool;

	return &msQuicThreadTaskPool;
}

void MsQuicThreadTaskPool::stop()
{
	if (isRunning.load() == false) return;

	condition.notify_all();

	for (auto& thread : taskThreads) {

		if (thread.joinable()) {

			thread.join();

		}

	}

}