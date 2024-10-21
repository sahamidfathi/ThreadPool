#include <deque>
#include <future>
#include <memory>
#include <functional>
#include <thread>
#include <vector>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <type_traits>

/* Thread-safe Queue */
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
        condVar.wait(lk, [this] {return !dataQueue.empty(); });
        value = std::move(dataQueue.front());
        dataQueue.pop();
    }

    std::shared_ptr<T> waitAndPop() {
        std::unique_lock<std::mutex> lk(theMutex);
        condVar.wait(lk, [this] {return !dataQueue.empty(); });
        std::shared_ptr<T> result(
            std::make_shared<T>(std::move(dataQueue.front())));
        dataQueue.pop();
        return result;
    }

    bool tryPop(T& value) {
        std::lock_guard<std::mutex> lk(theMutex);
        if (dataQueue.empty())
            return false;
        value = std::move(dataQueue.front());
        dataQueue.pop();
        return true;
    }

    std::shared_ptr<T> tryPop() {
        std::lock_guard<std::mutex> lk(theMutex);
        if (dataQueue.empty())
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
/* End of Thread-safe Queue */

/* Function Wrapper */
class FunctionWrapper {
    struct implBase {
        virtual void call() = 0;
        virtual ~implBase() {}
    };
    std::unique_ptr<implBase> impl;
    template <typename F>
    struct implType : implBase {
        F f;
        implType(F&& fParam) : f(std::move(fParam)) {}
        void call() { std::invoke(f); }
    };
public:
    template <typename F>
    FunctionWrapper(F&& f) :
        impl(new implType<F>(std::move(f))) {}

    void operator()() { impl->call(); }
    FunctionWrapper() = default;

    FunctionWrapper(FunctionWrapper&& other) :
        impl(std::move(other.impl)) {}

    FunctionWrapper& operator=(FunctionWrapper&& other) {
        impl = std::move(other.impl);
        return *this;
    }
    FunctionWrapper(const FunctionWrapper&) = delete;
    FunctionWrapper& operator=(const FunctionWrapper&) = delete;
};
/* End of Function Wrapper */

class JoinThreads {
    std::vector<std::thread>& threads;
public:
    explicit JoinThreads(std::vector<std::thread>& threadsParam) :
        threads(threadsParam) {}
    ~JoinThreads() {
        for (unsigned long i = 0; i < threads.size(); ++i) {
            if (threads[i].joinable())
                threads[i].join();
        }
    }
};

/* Work-stealing Queue */
class WorkStealingQueue {
private:
    typedef FunctionWrapper dataType;
    std::deque<dataType> theQueue;
    mutable std::mutex theMutex;
public:
    WorkStealingQueue() {}

    WorkStealingQueue(const WorkStealingQueue& other) = delete;
    WorkStealingQueue& operator=(const WorkStealingQueue& other) = delete;

    void push(dataType data) {
        std::lock_guard<std::mutex> lock(theMutex);
        theQueue.push_front(std::move(data));
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(theMutex);
        return theQueue.empty();
    }

    bool tryPop(dataType& result) {
        std::lock_guard<std::mutex> lock(theMutex);
        if(theQueue.empty()) {
            return false;
        }

        result = std::move(theQueue.front());
        theQueue.pop_front();
        return true;
    }

    bool trySteal(dataType& result) {
        std::lock_guard<std::mutex> lock(theMutex);
        if(theQueue.empty()) {
            return false;
        }

        result = std::move(theQueue.back());
        theQueue.pop_back();
        return true;
    }
};
/* End of Work-stealing Queue */

class ThreadPool {
    typedef FunctionWrapper TaskType;
    std::atomic_bool done;
    ThreadSafeQueue<TaskType> poolQueue;
    std::vector<std::unique_ptr<WorkStealingQueue>> queues;
    std::vector<std::thread> threads;
    JoinThreads joiner;

    static thread_local WorkStealingQueue* localWorkQueue;
    static thread_local unsigned myIndex;

    void workerThread(unsigned myIndex_) {
        myIndex = myIndex_;
        localWorkQueue = queues[myIndex].get();
        while (!done) {
            runPendingTask();
        }
    }

    bool popTaskFromLocalQueue(TaskType& task) {
        return localWorkQueue && localWorkQueue->tryPop(task);
    }

    bool popTaskFromPoolQueue(TaskType& task) {
        return poolQueue.tryPop(task);
    }

    bool popTaskFromOtherThreadQueue(TaskType& task) {
        for (unsigned i = 0; i < queues.size(); ++i) {
            unsigned const index = (myIndex + i + 1) % queues.size();
            if (queues[index]->trySteal(task)) {
                return true;
            }
        }
        return false;
    }

public:
    ThreadPool() : joiner(threads), done(false) {
        unsigned const threadCount = std::thread::hardware_concurrency();
        // std::cout << "thread pool created and num of threads is: " << threadCount << std::endl;
        try {
            for (unsigned i = 0; i < threadCount; ++i) {
                queues.push_back(std::make_unique<WorkStealingQueue>());
            }
            for (unsigned i = 0; i < threadCount; ++i) {
                threads.push_back(std::thread(&ThreadPool::workerThread, this, i));
            }
        } catch (...) {
            done = true; // Ensure all threads are stopped in case of exception
            throw;
        }
    }

    ~ThreadPool() {
        done = true;
    }

    template<typename FunctionType>
    std::future<typename std::result_of<FunctionType()>::type> submitTask(FunctionType f) {
        using ResultType = typename std::result_of<FunctionType()>::type;
        std::packaged_task<ResultType()> task(std::move(f));
        std::future<ResultType> res = task.get_future();
        if (localWorkQueue) {
            localWorkQueue->push(std::move(task));
        } else {
            poolQueue.push(std::move(task));
        }
        return res;
    }

    void runPendingTask() {
        TaskType task;
        if (popTaskFromLocalQueue(task) ||
            popTaskFromPoolQueue(task) ||
            popTaskFromOtherThreadQueue(task)) {
            task();
        } else {
            std::this_thread::yield();
        }
    }
};

// Static thread_local variable to be initialized in .cpp file. 
//thread_local WorkStealingQueue* ThreadPool::localWorkQueue = nullptr;
//thread_local unsigned ThreadPool::myIndex = 0;



