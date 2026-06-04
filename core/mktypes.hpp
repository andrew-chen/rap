#pragma once
#include <cstdint>

// Forward declarations for all types in the RAP codebase.
// Every header includes this first.

enum class TermTag    : std::uint8_t;
enum class GoalTag    : std::uint8_t;
enum class Outcome    : std::uint8_t;
enum class ClientId   : std::uint32_t;  // used in Stage 0B
enum class StepResult : std::uint8_t;

struct Arena;
struct SymEntry;
struct Intern;
struct Term;
struct PairNode;
struct RelNode;        // Stage 0A
struct Goal;
struct GoalCall;       // Stage 0A
struct State;
struct Binding;
struct EnvFrame;
struct WorkQueue;
struct Work;
struct Kont;
struct OutcomeSyms;
struct ClientRegion;   // Stage 0B
struct RelEnv;         // Stage 0A
struct RelEnvEntry;    // Stage 0A
struct GlobalBind;
struct BoundBind;

// STAGE_ARITH: extended constraint store
enum class ConstraintRel : std::uint8_t;
struct Constraint;
