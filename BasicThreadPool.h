#include <mutex>
#include <condition_variable>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <queue>

template<typename T>
class ThreadSafeQueue {
private:
	mutable std::mutex theMutex;
	std::queue<T> dataQueue;
	std::condition_variable condVar;
public:
	ThreadSafeQueue() {}

	void push(T newItem) {
		std::lock_guard<std::mutex> lk(theMutex);
		dataQueue.push(std::move(newItem));
		condVar.notify_one();
	}

	void waitAndPop(T& value) {
		std::unique_lock<std::mutex> lk(theMutex);
		condVar.wait(lk,[this]{return !dataQueue.empty();});
		value=std::move(dataQueue.front());
		dataQueue.pop();
	}

	std::shared_ptr<T> waitAndPop() {
		std::unique_lock<std::mutex> lk(theMutex);
		condVar.wait(lk,[this]{return !dataQueue.empty();});
		std::shared_ptr<T> result(
			std::make_shared<T>(std::move(dataQueue.front())));
		dataQueue.pop();
		return result;
	}

	bool tryPop(T& value) {
		std::lock_guard<std::mutex> lk(theMutex);
		if(dataQueue.empty())
			return false;
		value=std::move(dataQueue.front());
		dataQueue.pop();
		return true;
	}

	std::shared_ptr<T> tryPop() {
		std::lock_guard<std::mutex> lk(theMutex);
		if(dataQueue.empty())
			return std::shared_ptr<T>();
		std::shared_ptr<T> result(
			std::make_shared<T>(std::move(dataQueue.front())));
		dataQueue.pop();
		return result;
	}

	bool empty() const {
		std::lock_guard<std::mutex> lk(theMutex);
		return dataQueue.empty();
	}
};

class JoinThreads {
	std::vector<std::thread>& threads;
public:
explicit JoinThreads(std::vector<std::thread>& threadsParam):
	threads(threadsParam) {}
	~JoinThreads() {
		for(unsigned long i=0;i<threads.size();++i) {
			if(threads[i].joinable())
				threads[i].join();
		}
	}
};

class ThreadPool {
	std::atomic_bool done;
	ThreadSafeQueue<std::function<void()> > workQueue;
	std::vector<std::thread> threads;
	JoinThreads joiner;

	void workerThread() {
		while (!done) {
			std::function<void()> task;
			if (workQueue.tryPop(task)) {
				task();
			} else {
				std::this_thread::yield();
			}
		}
	}
public:
	ThreadPool() : done(false), joiner(threads) {
        unsigned const threadCount = std::thread::hardware_concurrency();
		try {
			for (unsigned i = 0; i < threadCount; ++i) {
				threads.push_back(
					std::thread(&ThreadPool::workerThread, this));
			}
		} catch(...) {
			done = true;
			throw;
		}
	}

	~ThreadPool() {
		done = true;
	}

	template<typename FunctionType>
	void submitTask(FunctionType f) {
		workQueue.push(std::function<void()>(f));
	}
};

