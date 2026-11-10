#pragma once

#include "core/Export.h"

#include <memory>
#include <string>
#include <string_view>

/**
 * The expression node: a small script computing outputs from inputs every evaluation, the scalar subset of Maya's MEL
 * expressions. Statements end with ';': assignments to out[n] or to local variables (`float x = in[0] * 2;`, a MEL `$x`
 * works too), `if (...) ... else ...` with blocks in braces. Expressions take numbers, in[n], out[n] already set, time
 * (seconds), frame, + - * / % with parentheses and unary minus, comparisons (== != < <= > >=), && || !, and the functions
 * sin cos tan asin acos atan atan2 abs min max clamp(min, max, value) pow sqrt exp log floor ceil sign deg_to_rad
 * rad_to_deg. Maya's own .I[n] and .O[n] read as in[n] and out[n].
 */
namespace lila::scene::expression
{
inline constexpr int kOutputs = 16;

/** A parsed script, ready to run. */
class LILA_SCENE_API Program
{
public:
    /** Throws std::invalid_argument naming the problem and where. */
    static std::shared_ptr<const Program> parse(std::string_view text);

    struct Env
    {
        const double* inputs = nullptr;
        size_t inputCount = 0;
        double* outputs = nullptr; // kOutputs values, assigned in place
        double time = 0.0;
        double frame = 0.0;
    };
    void run(Env& env) const;

    struct Impl;
    std::shared_ptr<const Impl> impl;
};

/** Whether the text parses; on failure *why says what is wrong. */
LILA_SCENE_API bool valid(std::string_view text, std::string* why = nullptr);

LILA_SCENE_API void registerTypes();
} // namespace lila::scene::expression
