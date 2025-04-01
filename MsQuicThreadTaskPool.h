#pragma once
#include <iostream>
#include <queue>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <functional>
class MsQuicThreadTaskPool
{

public:

	MsQuicThreadTaskPool(size_t size = 8);

	~MsQuicThreadTaskPool();

	template<class T, class... Args>
	void runTask(T&& t, Args&&... args) {

		std::shared_ptr<std::function<void()>> task = std::make_shared<std::function<void()>>(std::bind(std::forward<T>(t), std::forward<Args>(args)...));

		std::unique_lock<std::mutex> lock(taskMutexs);

		if (!isRunning.load()) return;

		tasks.emplace(task);

		condition.notify_one();
	}

	static MsQuicThreadTaskPool* getInstannce();

private:

	std::queue <std::shared_ptr<std::function<void()>>> tasks;

	std::mutex taskMutexs;

	std::condition_variable condition;

	std::atomic<bool> isRunning;

	std::vector<std::thread> taskThreads;

	std::atomic<int> taskThreadCount;

	size_t threadCount;

	void stop();
};

