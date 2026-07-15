#include <string>
#include <vector>
#include <chrono>
#include "../core/sexp_parser.hpp"
#include <cstdio>
#include <cstdlib>

// A single role fact: role(device, role_name)
struct RoleFact {
    std::string device;
    std::string role;
};

// A single allow fact: allow(role, action, resource)
struct AllowFact {
    std::string role;
    std::string action;
    std::string resource;
};

// A single network allow fact: allow(source, destination, port)
struct NetworkAllowFact {
    std::string source;
    std::string destination;
    std::string port;  // string to match s-expression term representation, even though we know it's actually an int
};

// The full fact base for both case studies
struct FactBase {
    std::vector<RoleFact> roles;
    std::vector<AllowFact> allows;
    std::vector<NetworkAllowFact> network_allows;
};

// An incoming runtime event — covers both case studies
struct Event {
    enum class Type { AccessControl, NetworkPolicy };

    Type type;

    // ACL fields
    std::string device;
    std::string action;
    std::string resource;

    // Network policy fields
    std::string source;
    std::string destination;
    std::string port;  // string for same reason as above
};

// No Engine class — work directly with arena and free functions
struct PolicyChecker {
    alignas(64) unsigned char mem[1 << 16];
    Arena arena;

    PolicyChecker() : arena(mem, sizeof(mem)) {}

    enum class Result { Permitted, Violation, Error };

    // Run one check, accumulating parse and evaluation nanoseconds into the
    // provided accumulators. arena.reset() is called before returning so the
    // next iteration starts clean, but the reset time is not included in
    // either accumulator.
    Result check_timed(const std::string& query_str,
                       long long& parse_ns_acc,
                       long long& eval_ns_acc) {
        using clock = std::chrono::high_resolution_clock;

        auto t0 = clock::now();
        ParsedQuery pq = parse_query(arena, query_str.c_str());
        auto t1 = clock::now();
        parse_ns_acc += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

        if (!pq.goal) {
            arena.reset();
            return Result::Error;
        }

        Result result = Result::Violation;

        auto t2 = clock::now();
        runN(arena, pq.n, pq.goal, pq.qvar, pq.vars_used, pq.outcome_syms,
            [&](Term, State) { result = Result::Permitted; });
        auto t3 = clock::now();
        eval_ns_acc += std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();

        arena.reset();
        return result;
    }
};

// Run 5 warmup + 100 measured iterations for a single query string.
// Prints "  (parse: X.X µs, eval: Y.Y µs, total: Z.Z µs)" and returns
// the result (all 100 iterations must agree; aborts on inconsistency).
static PolicyChecker::Result benchmark_query(PolicyChecker& checker,
                                             const std::string& query) {
    // Warmup: 5 iterations, timing discarded.
    for (int i = 0; i < 5; ++i) {
        long long dummy_p = 0, dummy_e = 0;
        checker.check_timed(query, dummy_p, dummy_e);
    }

    // Measurement: 100 iterations.
    long long total_parse_ns = 0, total_eval_ns = 0;
    PolicyChecker::Result first_result = PolicyChecker::Result::Error;

    for (int i = 0; i < 100; ++i) {
        long long p = 0, e = 0;
        PolicyChecker::Result r = checker.check_timed(query, p, e);
        if (i == 0) {
            first_result = r;
        } else if (r != first_result) {
            std::printf("  ERROR: result changed on iteration %d — aborting\n", i + 1);
            std::exit(1);
        }
        total_parse_ns += p;
        total_eval_ns  += e;
    }

    double parse_us = total_parse_ns / 100.0 / 1000.0;
    double eval_us  = total_eval_ns  / 100.0 / 1000.0;
    double total_us = parse_us + eval_us;
    std::printf("  (parse: %.1f \xc2\xb5s, eval: %.1f \xc2\xb5s, total: %.1f \xc2\xb5s)\n",
                parse_us, eval_us, total_us);

    return first_result;
}

std::string build_acl_query(const FactBase& facts, const Event& event) {
    std::string q = "(run 1 (q) (probe (fresh (r) (conj ";

    // role facts disj
    q += "(disj ";
    for (auto& rf : facts.roles) {
        q += "(== (list role " + rf.device + " " + rf.role + ") "
           + "(list role " + event.device + " r)) ";
    }
    q += ") ";

    // allow facts disj
    q += "(disj ";
    for (auto& af : facts.allows) {
        q += "(== (list allow " + af.role + " " + af.action + " " + af.resource + ") "
           + "(list allow r " + event.action + " " + event.resource + ")) ";
    }
    q += ") ";

    q += "(== q r))) true 100 true false))";
    return q;
}

std::string build_network_query(const FactBase& facts, const Event& event) {
    std::string q = "(run 1 (q) (probe (disj ";

    for (auto& nf : facts.network_allows) {
        q += "(== (list allow " + nf.source + " " + nf.destination + " " + nf.port + ") "
           + "(list allow " + event.source + " " + event.destination + " " + event.port + ")) ";
    }

    q += ") true 100 true false))";
    return q;
}


int main() {
    // Set up fact base
    FactBase facts;

    // Role facts
    facts.roles = {
        {"sensor-node-7",      "sensor"},
        {"operator-console-2", "operator"},
        {"gateway-node-1",     "gateway"},
        {"guest-device-1",     "guest"}
    };

    // ACL allow facts
    facts.allows = {
        {"sensor",   "read",  "sensor-data"},
        {"operator", "read",  "config-data"},
        {"operator", "write", "config-data"},
        {"gateway",  "read",  "sensor-data"},
        {"guest",    "read",  "public-data"}
    };

    // Network allow facts
    // port as string to avoid to_string conversion in query builder
    facts.network_allows = {
        {"sensor-node-7",  "gateway-node-1",  "5683"},
        {"gateway-node-1", "cloud-service-A", "443"}
    };

    PolicyChecker checker;

    // Each event is run for 5 warmup + 100 measured iterations.
    // Timing line shows average parse, eval, and total per iteration.
    // The PERMITTED/VIOLATION line uses the result from the measured run
    // (all 100 iterations must agree).

    // --- Case Study 1: ACL ---
    std::printf("=== Case Study 1: Access Control ===\n\n");

    std::vector<Event> acl_events = {
        // Normal behavior
        {Event::Type::AccessControl, "sensor-node-7",      "read",  "sensor-data", "", "", ""},
        {Event::Type::AccessControl, "operator-console-2", "write", "config-data", "", "", ""},
        {Event::Type::AccessControl, "gateway-node-1",     "read",  "sensor-data", "", "", ""},
        // Violations
        {Event::Type::AccessControl, "sensor-node-7",  "write", "firmware",    "", "", ""},
        {Event::Type::AccessControl, "guest-device-1", "write", "config-data", "", "", ""},
        {Event::Type::AccessControl, "gateway-node-1", "write", "firmware",    "", "", ""}
    };

    for (auto& event : acl_events) {
        std::string query = build_acl_query(facts, event);
        auto result = benchmark_query(checker, query);

        std::printf("request(%s, %s, %s) => ",
            event.device.c_str(),
            event.action.c_str(),
            event.resource.c_str());

        switch (result) {
            case PolicyChecker::Result::Permitted:
                std::printf("PERMITTED\n");
                break;
            case PolicyChecker::Result::Violation:
                std::printf("VIOLATION -- device: %s, action: %s, resource: %s\n",
                    event.device.c_str(),
                    event.action.c_str(),
                    event.resource.c_str());
                break;
            case PolicyChecker::Result::Error:
                std::printf("ERROR (malformed query or indeterminate result)\n");
                break;
        }
    }

    // --- Case Study 2: Network Policy ---
    std::printf("\n=== Case Study 2: Network Policy ===\n\n");

    std::vector<Event> net_events = {
        // Normal behavior
        {Event::Type::NetworkPolicy, "", "", "", "sensor-node-7",  "gateway-node-1",  "5683"},
        {Event::Type::NetworkPolicy, "", "", "", "gateway-node-1", "cloud-service-A", "443"},
        // Violations
        {Event::Type::NetworkPolicy, "", "", "", "sensor-node-7",   "cloud-service-A", "443"},
        {Event::Type::NetworkPolicy, "", "", "", "external-device-9", "sensor-node-7", "22"}
    };

    for (auto& event : net_events) {
        std::string query = build_network_query(facts, event);
        auto result = benchmark_query(checker, query);

        std::printf("send(%s, %s, %s) => ",
            event.source.c_str(),
            event.destination.c_str(),
            event.port.c_str());

        switch (result) {
            case PolicyChecker::Result::Permitted:
                std::printf("PERMITTED\n");
                break;
            case PolicyChecker::Result::Violation:
                std::printf("VIOLATION -- source: %s, destination: %s, port: %s\n",
                    event.source.c_str(),
                    event.destination.c_str(),
                    event.port.c_str());
                break;
            case PolicyChecker::Result::Error:
                std::printf("ERROR (malformed query or indeterminate result)\n");
                break;
        }
    }

    // --- Case Study 3: Audit Query ---
    std::printf("\n=== Audit Query: Devices with read access to sensor-data ===\n\n");

    // Multi-solution query: find all devices whose role permits read on sensor-data.
    // Uses the same fact base already in memory; no dynamic string construction needed.
    // Single run — correctness demonstration, not benchmarking.
    const char* audit_query =
        "(run 4 (q)"
        "  (fresh (r)"
        "    (disj"
        "      (== (list role sensor-node-7         sensor)   (list role q r))"
        "      (== (list role operator-console-2    operator) (list role q r))"
        "      (== (list role gateway-node-1        gateway)  (list role q r))"
        "      (== (list role guest-device-1        guest)    (list role q r)))"
        "    (disj"
        "      (== (list allow sensor   read  sensor-data)    (list allow r read sensor-data))"
        "      (== (list allow operator read  config-data)    (list allow r read sensor-data))"
        "      (== (list allow operator write config-data)    (list allow r read sensor-data))"
        "      (== (list allow gateway  read  sensor-data)    (list allow r read sensor-data))"
        "      (== (list allow guest    read  public-data)    (list allow r read sensor-data)))))";

    ParsedQuery pq = parse_query(checker.arena, audit_query);
    if (!pq.goal) {
        std::printf("ERROR: audit query failed to parse\n");
        return 1;
    }

    std::vector<Term> answers;
    runN(checker.arena, pq.n, pq.goal, pq.qvar, pq.vars_used, pq.outcome_syms,
        [&](Term ans, State) { answers.push_back(ans); });

    std::printf("Results: ");
    for (std::size_t i = 0; i < answers.size(); ++i) {
        if (i > 0) std::printf(", ");
        print_term(answers[i]);
    }
    std::printf("\n");

    bool found_sensor  = false;
    bool found_gateway = false;
    for (const auto& t : answers) {
        if (t.tag == TermTag::Sym && t.sym) {
            if (sym_lit_eq(t.sym, "sensor-node-7"))  found_sensor  = true;
            if (sym_lit_eq(t.sym, "gateway-node-1")) found_gateway = true;
        }
    }

    if (answers.size() == 2 && found_sensor && found_gateway) {
        std::printf("PASS: 2 devices found as expected\n");
    } else {
        std::printf("FAIL: unexpected results\n");
        std::printf("  got %zu answer(s):", answers.size());
        for (const auto& t : answers) { std::printf(" "); print_term(t); }
        std::printf("\n");
        checker.arena.reset();
        return 1;
    }

    checker.arena.reset();
    return 0;
}
