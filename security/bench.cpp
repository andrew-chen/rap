// security/bench.cpp — performance benchmark for security case studies
// Runs all 10 policy checks 100 times, reports median and range.
// Compile:
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror \
//           -o security/bench security/bench.cpp
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include "../core/sexp_parser.hpp"

// ---- Types duplicated from security_test.cpp (cannot include that file) ----

struct RoleFact    { std::string device; std::string role; };
struct AllowFact   { std::string role;   std::string action; std::string resource; };
struct NetworkAllowFact { std::string source; std::string destination; std::string port; };

struct FactBase {
    std::vector<RoleFact>       roles;
    std::vector<AllowFact>      allows;
    std::vector<NetworkAllowFact> network_allows;
};

struct Event {
    enum class Type { AccessControl, NetworkPolicy };
    Type        type;
    std::string device, action, resource;   // ACL fields
    std::string source, destination, port;  // Network fields
};

struct PolicyChecker {
    alignas(64) unsigned char mem[1 << 16];
    Arena arena;
    PolicyChecker() : arena(mem, sizeof(mem)) {}

    enum class Result { Permitted, Violation, Error };

    Result check(const std::string& query_str) {
        ParsedQuery pq = parse_query(arena, query_str.c_str());
        if (!pq.goal) { arena.reset(); return Result::Error; }
        Result result = Result::Violation;
        runN(arena, pq.n, pq.goal, pq.qvar, pq.vars_used, pq.outcome_syms,
             [&](Term, State) { result = Result::Permitted; });
        arena.reset();
        return result;
    }
};

static std::string build_acl_query(const FactBase& facts, const Event& ev) {
    std::string q = "(run 1 (q) (probe (fresh (r) (conj (disj ";
    for (auto& rf : facts.roles)
        q += "(== (list role " + rf.device + " " + rf.role + ") "
           + "(list role " + ev.device + " r)) ";
    q += ") (disj ";
    for (auto& af : facts.allows)
        q += "(== (list allow " + af.role + " " + af.action + " " + af.resource + ") "
           + "(list allow r " + ev.action + " " + ev.resource + ")) ";
    q += ") (== q r))) true 100 true false))";
    return q;
}

static std::string build_network_query(const FactBase& facts, const Event& ev) {
    std::string q = "(run 1 (q) (probe (disj ";
    for (auto& nf : facts.network_allows)
        q += "(== (list allow " + nf.source + " " + nf.destination + " " + nf.port + ") "
           + "(list allow " + ev.source + " " + ev.destination + " " + ev.port + ")) ";
    q += ") true 100 true false))";
    return q;
}

// ---- Timing helpers ----
using Clock = std::chrono::high_resolution_clock;
using NS    = std::chrono::nanoseconds;

static double median_us(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    std::size_t n = v.size();
    return (n % 2 == 0) ? (v[n/2 - 1] + v[n/2]) / 2.0 : v[n/2];
}

int main() {
    // ---- Fact base ----
    FactBase facts;
    facts.roles = {
        {"sensor-node-7","sensor"}, {"operator-console-2","operator"},
        {"gateway-node-1","gateway"}, {"guest-device-1","guest"}
    };
    facts.allows = {
        {"sensor","read","sensor-data"}, {"operator","read","config-data"},
        {"operator","write","config-data"}, {"gateway","read","sensor-data"},
        {"guest","read","public-data"}
    };
    facts.network_allows = {
        {"sensor-node-7","gateway-node-1","5683"},
        {"gateway-node-1","cloud-service-A","443"}
    };

    std::vector<Event> acl_events = {
        {Event::Type::AccessControl, "sensor-node-7",      "read",  "sensor-data", "","",""},
        {Event::Type::AccessControl, "operator-console-2", "write", "config-data", "","",""},
        {Event::Type::AccessControl, "gateway-node-1",     "read",  "sensor-data", "","",""},
        {Event::Type::AccessControl, "sensor-node-7",      "write", "firmware",    "","",""},
        {Event::Type::AccessControl, "guest-device-1",     "write", "config-data", "","",""},
        {Event::Type::AccessControl, "gateway-node-1",     "write", "firmware",    "","",""}
    };
    std::vector<Event> net_events = {
        {Event::Type::NetworkPolicy, "","","", "sensor-node-7",    "gateway-node-1",  "5683"},
        {Event::Type::NetworkPolicy, "","","", "gateway-node-1",   "cloud-service-A", "443"},
        {Event::Type::NetworkPolicy, "","","", "sensor-node-7",    "cloud-service-A", "443"},
        {Event::Type::NetworkPolicy, "","","", "external-device-9","sensor-node-7",   "22"}
    };

    // Pre-build query strings (outside timing loop)
    std::vector<std::string> acl_queries, net_queries;
    for (auto& e : acl_events) acl_queries.push_back(build_acl_query(facts, e));
    for (auto& e : net_events) net_queries.push_back(build_network_query(facts, e));

    constexpr int WARMUP = 10;
    constexpr int ITERS  = 100;

    std::vector<double> acl_times(ITERS), net_times(ITERS), all_times(ITERS);

    PolicyChecker checker;

    // Warmup
    for (int i = 0; i < WARMUP; ++i) {
        for (auto& q : acl_queries) checker.check(q);
        for (auto& q : net_queries) checker.check(q);
    }

    // Measured iterations
    for (int i = 0; i < ITERS; ++i) {
        auto t0 = Clock::now();
        for (auto& q : acl_queries) checker.check(q);
        auto t1 = Clock::now();
        for (auto& q : net_queries) checker.check(q);
        auto t2 = Clock::now();

        double acl_us = std::chrono::duration_cast<NS>(t1 - t0).count() / 1000.0;
        double net_us = std::chrono::duration_cast<NS>(t2 - t1).count() / 1000.0;
        acl_times[i] = acl_us;
        net_times[i] = net_us;
        all_times[i] = acl_us + net_us;
    }

    std::sort(acl_times.begin(), acl_times.end());
    std::sort(net_times.begin(), net_times.end());
    std::sort(all_times.begin(), all_times.end());

    double acl_med  = median_us(acl_times);
    double net_med  = median_us(net_times);
    double all_med  = median_us(all_times);

    std::printf("BENCH_ACL_MED=%.1f MIN=%.1f MAX=%.1f\n",
                acl_med, acl_times.front(), acl_times.back());
    std::printf("BENCH_NET_MED=%.1f MIN=%.1f MAX=%.1f\n",
                net_med, net_times.front(), net_times.back());
    std::printf("BENCH_ALL_MED=%.1f MIN=%.1f MAX=%.1f\n",
                all_med, all_times.front(), all_times.back());

    return 0;
}
