#include <string>
#include <vector>
#include <chrono>
#include "../core/sexp_parser.hpp"
#include <cstdio>

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
    
	Result check(const std::string& query_str) {
	    std::printf("Query: %s\n", query_str.c_str());
	    ParsedQuery pq = parse_query(arena, query_str.c_str());
	    print_query(pq);
	    if (!pq.goal) {
		arena.reset();
		return Result::Error;
	    }
	    
	    Result result = Result::Violation;  // default: no solution = violation
	    
	    runN(arena, pq.n, pq.goal, pq.qvar, pq.vars_used, pq.outcome_syms,
		[&](Term ans, State) {
		    result = Result::Permitted;  // callback fired = permitted
		});
	    
	    arena.reset();
	    return result;
	}
};

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

    // --- Case Study 1: ACL ---
    std::printf("=== Case Study 1: Access Control ===\n\n");

    std::vector<Event> acl_events = {
        // Normal behavior
        {Event::Type::AccessControl, "sensor-node-7",      "read",  "sensor-data"},
        {Event::Type::AccessControl, "operator-console-2", "write", "config-data"},
        {Event::Type::AccessControl, "gateway-node-1",     "read",  "sensor-data"},
        // Violations
        {Event::Type::AccessControl, "sensor-node-7",  "write", "firmware"},
        {Event::Type::AccessControl, "guest-device-1", "write", "config-data"},
        {Event::Type::AccessControl, "gateway-node-1", "write", "firmware"}
    };

    for (auto& event : acl_events) {
        std::string query = build_acl_query(facts, event);
	auto start = std::chrono::high_resolution_clock::now();
	auto result = checker.check(query);
	auto end = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
	std::printf("  (%.1f µs)\n", (double)duration.count());

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
	auto start = std::chrono::high_resolution_clock::now();
	auto result = checker.check(query);
	auto end = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
	std::printf("  (%.1f µs)\n", (double)duration.count());

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

    return 0;
}
