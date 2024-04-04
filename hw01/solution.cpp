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
    shared_ptr<std::condition_variable> cv;
    std::queue<shared_ptr<ProblemPackWrapper>> problemPacks;
    shared_ptr<std::mutex> mtx;

    explicit CompanyWrapper(ACompany company)
        : company(std::move(company)), cv(std::make_shared<std::condition_variable>()), mtx(std::make_shared<std::mutex>()) {
    }
};

struct ProblemPackWrapper {
    CompanyWrapper *companyWrapper;
    AProblemPack problemPack;
    atomic<size_t> solved = 0;

    bool isSolved() const {
        return problemPack->m_ProblemsMin.size() + problemPack->m_ProblemsCnt.size() == solved;
    }

    ProblemPackWrapper(CompanyWrapper *companyWrapper, AProblemPack problemPack)
        : companyWrapper(companyWrapper), problemPack(std::move(problemPack)) {
    }
};

struct SolverWrapper {
    std::string type;
    AProgtestSolver solver;
    std::unordered_map<shared_ptr<ProblemPackWrapper>, size_t> inSolver;

    SolverWrapper(std::string type, AProgtestSolver solver) : type(std::move(type)), solver(std::move(solver)){
    }

    void solveWrapper() const {
        solver->solve();

        for (const auto& [x, problems] : inSolver) {
            x->solved += problems;

            if (x->isSolved())
                x->companyWrapper->cv->notify_all();
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
        companies.emplace_back(company);
    }
    void start(int workThreads) {
        activeReceivers = (int)companies.size();
        activeWorkers = workThreads;

        for (int i = 0; i < workThreads; i++)
            workerThreads.emplace_back(&COptimizer::workerFunction, this);

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
    }

private:
    std::atomic<int> activeReceivers, activeWorkers;
    std::condition_variable cv;
    std::vector<CompanyWrapper> companies;
    std::vector<std::thread> workerThreads, receiverThreads, senderThreads;
    std::mutex queueMtx;
    std::queue<shared_ptr<SolverWrapper>> solvers;

    shared_ptr<SolverWrapper> solver_min = std::make_shared<SolverWrapper>("min", createProgtestMinSolver());
    shared_ptr<SolverWrapper> solver_cnt = std::make_shared<SolverWrapper>("cnt", createProgtestCntSolver());

    void processProblems(const std::vector<APolygon>& problems,
                        shared_ptr<SolverWrapper>& solver,
                        const shared_ptr<ProblemPackWrapper>& pack,
                        const string& type) {
        size_t count = 0;
        for (const auto &polygon : problems) {
            if (solver) {
                solver->solver->addPolygon(polygon);
                count++;
                if (!solver->solver->hasFreeCapacity()) {
                    solver->inSolver[pack] = count;
                    count = 0;
                    solvers.push(std::move(solver));
                    cv.notify_all();
                    type == "min" ? solver = std::make_shared<SolverWrapper>("min", createProgtestMinSolver()) :
                                    solver = std::make_shared<SolverWrapper>("cnt", createProgtestCntSolver());
                }
            }
        }
        if (count > 0)
            solver->inSolver[pack] = count;
    }

    void receiverFunction(CompanyWrapper &companyWrapper) {
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
            auto pack = std::make_shared<ProblemPackWrapper>(&companyWrapper, problemPack);
            {
                std::lock_guard<std::mutex> lock(*companyWrapper.mtx);
                companyWrapper.problemPacks.push(pack);
            }

            std::lock_guard<std::mutex> lock(queueMtx);
            processProblems(problemPack->m_ProblemsMin, solver_min, pack,"min");
            processProblems(problemPack->m_ProblemsCnt, solver_cnt, pack, "cnt");
        }
    }

    void workerFunction() {
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
        while (true) {
            std::unique_lock<std::mutex> lock(*companyWrapper.mtx);
            companyWrapper.cv->wait(lock, [&](){ return !activeWorkers ||
                                          (!companyWrapper.problemPacks.empty() && companyWrapper.problemPacks.front()->isSolved()); });

            if (!activeWorkers && companyWrapper.problemPacks.empty())
                return;

            if (!companyWrapper.problemPacks.empty() && companyWrapper.problemPacks.front()->isSolved()) {
                companyWrapper.company->solvedPack(companyWrapper.problemPacks.front()->problemPack);
                companyWrapper.problemPacks.pop();
            }
        }
    }
};

//-------------------------------------------------------------------------------------------------------------------------------------------------------------
#ifndef __PROGTEST__

int main() {
    COptimizer optimizer;

    int companyNum = 200;
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