#ifndef __PROGTEST__

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <climits>
#include <cfloat>
#include <cassert>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <string>
#include <utility>
#include <vector>
#include <array>
#include <iterator>
#include <set>
#include <list>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <compare>
#include <queue>
#include <stack>
#include <deque>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <condition_variable>
#include <pthread.h>
#include <semaphore.h>
#include "progtest_solver.h"
#include "sample_tester.h"

using namespace std;
#endif /* __PROGTEST__ */

//-------------------------------------------------------------------------------------------------------------------------------------------------------------

struct ProblemPackWrapper;

struct CompanyWrapper {
    ACompany company;
    long unsigned int id = 0;
    shared_ptr<std::condition_variable> cv;
    std::unordered_map<size_t, std::shared_ptr<ProblemPackWrapper>> solvedProblems; // order and pack
    shared_ptr<std::mutex> mtx;
    std::set<size_t> solvedOrders;

    explicit CompanyWrapper(ACompany company, const long unsigned int id)
        : company(std::move(company)), id(id), cv(std::make_shared<std::condition_variable>()), mtx(std::make_shared<std::mutex>()) {
    }
};

struct ProblemPackWrapper {
    size_t order;
    CompanyWrapper *companyWrapper;
    AProblemPack problemPack;
    size_t solved = 0;

    bool isSolved() const {
        return problemPack->m_ProblemsMin.size() + problemPack->m_ProblemsCnt.size() == solved;
    }

    ProblemPackWrapper(const size_t order, CompanyWrapper *companyWrapper, AProblemPack problemPack)
        : order(order), companyWrapper(companyWrapper), problemPack(std::move(problemPack)) {
    }
};

struct SolverWrapper {
    std::string type;
    AProgtestSolver solver;
    std::set<shared_ptr<ProblemPackWrapper>> inSolver;

    SolverWrapper(std::string type, AProgtestSolver solver) : type(std::move(type)), solver(std::move(solver)){
    }

    void solveWrapper() const {
        solver->solve();

        for (const std::shared_ptr<ProblemPackWrapper> &x : inSolver) {
            std::lock_guard<std::mutex> lock(*x->companyWrapper->mtx);

            if (x->companyWrapper->solvedProblems.contains(x->order))
                x->companyWrapper->solvedProblems[x->order]->solved += x->solved;
            else
                x->companyWrapper->solvedProblems[x->order] = x;

            if (x->companyWrapper->solvedProblems[x->order]->isSolved()) {
                x->companyWrapper->solvedOrders.insert(x->order);
                x->companyWrapper->cv->notify_all();
            }
        }
    }
};

class COptimizer {
public:
    static bool usingProgtestSolver() {
        return true;
    }
    static void checkAlgorithmMin(APolygon p) {
    }
    static void checkAlgorithmCnt(APolygon p) {
    }

    void addCompany(ACompany company) {
        companies.emplace_back(company, companies.size());
    }
    void start(int workThreads) {
        activeReceivers = (int)companies.size();
        activeWorkers = workThreads;

        for (int i = 0; i < workThreads; i++)
            workerThreads.emplace_back(&COptimizer::workerFunction, this, i);

        for (auto &companyWrapper : companies) {
            receiverThreads.emplace_back(&COptimizer::receiverFunction, this, std::ref(companyWrapper));
            senderThreads.emplace_back(&COptimizer::senderFunction, this, std::ref(companyWrapper));
        }
    }
    void stop() {
        for (auto &thread : receiverThreads)
            thread.join();
        for (auto &thread : senderThreads)
            thread.join();
        for (auto &thread : workerThreads)
            thread.join();

        receiverThreads.clear();
        senderThreads.clear();
        workerThreads.clear();
    }

private:
    std::atomic<int> activeReceivers;
    std::atomic<int> activeWorkers;
    std::condition_variable cv;
    std::vector<CompanyWrapper> companies;
    std::vector<std::thread> workerThreads;
    std::vector<std::thread> receiverThreads;
    std::vector<std::thread> senderThreads;
    std::mutex queueMtx;
    std::queue<shared_ptr<SolverWrapper>> solvers;

    shared_ptr<SolverWrapper> solver_min = std::make_shared<SolverWrapper>("min", createProgtestMinSolver());
    shared_ptr<SolverWrapper> solver_cnt = std::make_shared<SolverWrapper>("cnt", createProgtestCntSolver());

    void processProblems(const std::vector<APolygon>& problems,
                        shared_ptr<SolverWrapper>& solver,
                        const AProblemPack& problemPack,
                        const size_t order,
                        CompanyWrapper& companyWrapper,
                        const string& type) {
        size_t count = 0;
        for (const auto &polygon : problems) {
            if (solver) {
                solver->solver->addPolygon(polygon);
                count++;
                if (!solver->solver->hasFreeCapacity()) {
                    auto pack = std::make_shared<ProblemPackWrapper>(order, &companyWrapper, problemPack);
                    solver->inSolver.insert(pack);
                    pack->solved = count;
                    count = 0;
                    solvers.push(std::move(solver));
                    cv.notify_all();
                    if (type == "min")
                        solver = std::make_shared<SolverWrapper>("min", createProgtestMinSolver());
                    else
                        solver = std::make_shared<SolverWrapper>("cnt", createProgtestCntSolver());
                }
            }
        }
        if (count > 0) {
            const auto pack = std::make_shared<ProblemPackWrapper>(order, &companyWrapper, problemPack);
            solver->inSolver.insert(pack);
            pack->solved = count;
        }
    }

    void receiverFunction(CompanyWrapper &companyWrapper) {
        size_t order = 0;
        while (true) {
            AProblemPack problemPack = companyWrapper.company->waitForPack();
            if (!problemPack) {
                --activeReceivers;
                if (!activeReceivers) {
                    std::lock_guard<std::mutex> lock(queueMtx);
                    if (solver_min)
                        solvers.push(std::move(solver_min));

                    if (solver_cnt)
                        solvers.push(std::move(solver_cnt));
                }
                cv.notify_all();
                return;
            }

            std::lock_guard<std::mutex> lock(queueMtx);
            if (!problemPack->m_ProblemsMin.empty())
                processProblems(problemPack->m_ProblemsMin, solver_min, problemPack, order, companyWrapper, "min");
            if (!problemPack->m_ProblemsCnt.empty())
                processProblems(problemPack->m_ProblemsCnt, solver_cnt, problemPack, order, companyWrapper, "cnt");

            order++;
        }
    }

    void workerFunction(const int id) {
        while (true) {
            std::unique_lock<std::mutex> lock(queueMtx);
            cv.wait(lock, [this](){ return !activeReceivers || !solvers.empty() || !activeWorkers; });

            if (!activeReceivers && solvers.empty()) {
                --activeWorkers;
                for (const auto &x : companies)
                    x.cv->notify_all();
                return;
            }

            if (!solvers.empty()) {
                const auto solver = solvers.front();
                solvers.pop();
                lock.unlock();
                solver->solveWrapper();
            }
        }
    }

    void senderFunction(CompanyWrapper &companyWrapper) const {
        size_t order = 0;
        while (true) {
            std::unique_lock<std::mutex> lock(*companyWrapper.mtx);
            companyWrapper.cv->wait(lock, [&](){ return !activeWorkers || companyWrapper.solvedOrders.contains(order); });

            if (!activeWorkers && companyWrapper.solvedProblems.empty())
                return;

            if (companyWrapper.solvedOrders.contains(order)) {
                companyWrapper.company->solvedPack(companyWrapper.solvedProblems[order]->problemPack);
                companyWrapper.solvedProblems.erase(order);
                ++order;
            }
        }
    }
};

//-------------------------------------------------------------------------------------------------------------------------------------------------------------
#ifndef __PROGTEST__

int main() {
    COptimizer optimizer;

    int companyNum = 90;
    std::vector<ACompanyTest> companies;
    companies.reserve(companyNum + 1);
    for (int x = 0; x < companyNum; x++)
        companies.push_back(std::make_shared<CCompanyTest>());
    for (auto &x : companies)
        optimizer.addCompany(x);

    optimizer.start(5);
    optimizer.stop();

    int cnt = 0;
    for (const auto &x : companies) {
        if (!x->allProcessed()) {
            printf("Company %d err\n", cnt);
            throw std::logic_error("Some problems were not correctly processsed");
        }
        cnt++;
    }
    printf("All companies processed\n");
    return 0;
}

#endif /* __PROGTEST__ */