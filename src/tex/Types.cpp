#include "tex/Types.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace lila::tex
{
namespace
{
constexpr std::array<const char*, 6> kPortNames = {"float", "gray", "color", "vector", "sdf2d", "material"};
constexpr std::array<const char*, 11> kParamNames = {"float", "int", "enum", "bool", "color", "gradient", "curve", "image", "seed", "code", "node"};

bool isScalar(PortType t) { return t == PortType::Float || t == PortType::Gray || t == PortType::Sdf2d; }

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
} // namespace

const char* portTypeName(PortType t) { return kPortNames[size_t(t)]; }

bool parsePortType(std::string_view name, PortType* out)
{
    for (size_t i = 0; i < kPortNames.size(); ++i)
        if (name == kPortNames[i])
        {
            *out = PortType(i);
            return true;
        }
    return false;
}

const char* paramTypeName(ParamType t) { return kParamNames[size_t(t)]; }

bool parseParamType(std::string_view name, ParamType* out)
{
    for (size_t i = 0; i < kParamNames.size(); ++i)
        if (name == kParamNames[i])
        {
            *out = ParamType(i);
            return true;
        }
    return false;
}

bool linkable(PortType from, PortType to)
{
    if (from == to)
        return true;
    if (from == PortType::Material || to == PortType::Material)
        return false;
    if (isScalar(from))
        return true;
    // Vectors have no single scalar reading, so they only go to colours and vectors.
    if (from == PortType::Vector)
        return to == PortType::Color;
    return true;
}

const char* slangType(PortType t)
{
    switch (t)
    {
    case PortType::Color: return "float4";
    case PortType::Vector: return "float3";
    case PortType::Material: return "TexMaterial";
    default: return "float";
    }
}

const char* samplerInterface(PortType t)
{
    switch (t)
    {
    case PortType::Color: return "ITexColor";
    case PortType::Vector: return "ITexVector";
    case PortType::Material: return "ITexMaterial";
    default: return "ITexFloat";
    }
}

int paramSlots(ParamType t)
{
    switch (t)
    {
    case ParamType::Gradient: return 1 + 2 * kMaxGradientStops;
    case ParamType::Curve: return 1 + kMaxCurvePoints / 2;
    default: return 1;
    }
}

bool valueToFloat4(const json& v, PortType t, float out[4])
{
    if (v.is_number() || v.is_boolean())
    {
        float f = v.is_boolean() ? (v.get<bool>() ? 1.0f : 0.0f) : v.get<float>();
        out[0] = out[1] = out[2] = f;
        out[3] = t == PortType::Color ? 1.0f : (t == PortType::Vector ? 0.0f : f);
        return true;
    }
    if (v.is_array() && (v.size() == 2 || v.size() == 3 || v.size() == 4) && std::all_of(v.begin(), v.end(), [](const json& x) { return x.is_number(); }))
    {
        out[0] = v[0].get<float>();
        out[1] = v[1].get<float>();
        out[2] = v.size() > 2 ? v[2].get<float>() : 0.0f;
        out[3] = v.size() > 3 ? v[3].get<float>() : (t == PortType::Vector ? 0.0f : 1.0f);
        return true;
    }
    if (v.is_string())
    {
        const std::string s = v.get<std::string>();
        if (s.size() != 7 && s.size() != 9)
            return false;
        if (s[0] != '#')
            return false;
        float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (size_t i = 0; i + 1 < s.size() - 1; i += 2)
        {
            int hi = hexDigit(s[1 + i]), lo = hexDigit(s[2 + i]);
            if (hi < 0 || lo < 0)
                return false;
            c[i / 2] = float(hi * 16 + lo) / 255.0f;
        }
        std::copy(c, c + 4, out);
        return true;
    }
    return false;
}

namespace
{
bool stopsValid(const json& stops, size_t width)
{
    if (!stops.is_array())
        return false;
    for (const json& s : stops)
    {
        if (!s.is_array() || s.size() != width || !s[0].is_number())
            return false;
        if (width == 2 && !s[1].is_number() && !s[1].is_array() && !s[1].is_string())
            return false;
    }
    return true;
}
} // namespace

bool paramValueValid(ParamType t, const json& v, const std::vector<std::string>& options)
{
    float tmp[4];
    switch (t)
    {
    case ParamType::Float: return v.is_number();
    case ParamType::Int:
    case ParamType::Seed: return v.is_number_integer() || (v.is_number() && v.get<double>() == double(int64_t(v.get<double>())));
    case ParamType::Bool: return v.is_boolean() || v.is_number();
    case ParamType::Enum:
        if (v.is_number_integer())
            return v.get<int64_t>() >= 0 && size_t(v.get<int64_t>()) < options.size();
        return v.is_string() && std::find(options.begin(), options.end(), v.get<std::string>()) != options.end();
    case ParamType::Color: return valueToFloat4(v, PortType::Color, tmp);
    case ParamType::Image:
    case ParamType::Code:
    case ParamType::Node: return v.is_string();
    case ParamType::Gradient:
        return stopsValid(v.is_object() ? v.value("stops", json()) : v, 2);
    case ParamType::Curve:
        return stopsValid(v.is_object() ? v.value("points", json()) : v, 2);
    }
    return false;
}
} // namespace lila::tex
