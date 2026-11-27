// Styles from JSON, the Settlement style guesses and programs mapped onto them, specs and edits as JSON.

#include "Buildings.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace buildings
{
namespace
{
template <class T> void read(const json& j, const char* key, T& out)
{
    if (j.contains(key) && !j[key].is_null())
    {
        try
        {
            out = j[key].get<T>();
        }
        catch (const std::exception&)
        {
        }
    }
}

std::vector<glm::vec3> colours(const json& j, const char* key)
{
    std::vector<glm::vec3> out;
    if (j.contains(key) && j[key].is_array())
        for (const json& c : j[key])
            if (c.is_string())
                out.push_back(hexColour(c.get<std::string>(), glm::vec3(0.7f)));
    return out;
}

json hexes(const std::vector<glm::vec3>& cs)
{
    json out = json::array();
    for (const glm::vec3& c : cs)
        out.push_back(colourHex(c));
    return out;
}

Ring ringOf(const json& j)
{
    Ring r;
    if (j.is_array())
        for (const json& p : j)
            if (p.is_array() && p.size() >= 2)
                r.push_back({p[0].get<double>(), p[p.size() - 1].get<double>()});
    return r;
}

json ringJson(const Ring& r)
{
    json out = json::array();
    for (const Vec2& p : r)
        out.push_back({p.x, p.y});
    return out;
}
} // namespace

Style Style::fromJson(const json& j, std::string* error)
{
    Style s;
    s.raw = j;
    if (!j.is_object())
    {
        if (error)
            *error = "a style is a JSON object";
        return s;
    }
    read(j, "id", s.id);
    read(j, "label", s.label);
    read(j, "description", s.description);
    read(j, "tags", s.tags);
    read(j, "replaces", s.replaces);
    if (s.label.empty())
        s.label = s.id;
    const json fl = j.value("floors", json::object());
    read(fl, "min", s.minFloors);
    read(fl, "max", s.maxFloors);
    read(fl, "groundHeight", s.groundHeight);
    read(fl, "height", s.floorHeight);
    read(fl, "plinth", s.plinth);
    const json b = j.value("bays", json::object());
    read(b, "width", s.bayWidth);
    read(b, "min", s.minBay);
    read(b, "cornerWidth", s.cornerWidth);
    read(b, "rhythm", s.rhythm);
    read(b, "blank", s.blankBays);
    read(b, "side", s.sideOpenings);
    read(b, "blankWithin", s.blankWithin);
    read(b, "sparseWithin", s.sparseWithin);
    const json op = j.value("openings", json::object());
    const json w = op.value("window", json::object()), d = op.value("door", json::object()), sh = op.value("shopfront", json::object());
    read(w, "width", s.windowW);
    read(w, "height", s.windowH);
    read(w, "sill", s.windowSill);
    read(w, "kind", s.windowKind);
    read(d, "width", s.doorW);
    read(d, "height", s.doorH);
    read(d, "kind", s.doorKind);
    read(sh, "width", s.shopW);
    read(sh, "height", s.shopH);
    read(op, "reveal", s.reveal);
    read(op, "glassFacade", s.glassFacade);
    const json g = j.value("groundFloor", json::object());
    read(g, "street", s.groundStreet);
    read(g, "arcade", s.arcade);
    read(g, "arcadeDepth", s.arcadeDepth);
    read(g, "shopfronts", s.shopfronts);
    read(g, "shutters", s.shutters);
    read(g, "awnings", s.awnings);
    const json m = j.value("modules", json::object());
    read(m, "balconies", s.balconies);
    read(m, "balconyKind", s.balconyKind);
    read(m, "grilles", s.grilles);
    read(m, "acUnits", s.acUnits);
    read(m, "pilasters", s.pilasters);
    read(m, "cornice", s.cornice);
    read(m, "bands", s.bands);
    read(m, "corner", s.corner);
    read(m, "drainpipes", s.drainpipes);
    read(m, "sign", s.signKind);
    const json r = j.value("roof", json::object());
    read(r, "kind", s.roof);
    read(r, "material", s.roofMaterial);
    read(r, "parapet", s.parapet);
    read(r, "overhang", s.overhang);
    read(r, "pitch", s.pitch);
    read(r, "waterTanks", s.waterTanks);
    read(r, "clutter", s.clutter);
    read(r, "rebar", s.rebar);
    const json ms = j.value("massing", json::object());
    read(ms, "podium", s.podium);
    read(ms, "towerInset", s.towerInset);
    read(ms, "stepEvery", s.stepEvery);
    read(ms, "stepInset", s.stepInset);
    read(ms, "wallThickness", s.wallThickness);
    const json e = j.value("extras", json::object());
    read(e, "columns", s.columns);
    read(e, "domes", s.domes);
    read(e, "perimeterWall", s.perimeterWall);
    read(e, "gate", s.gate);
    read(e, "pool", s.pool);
    read(e, "bellTowers", s.bellTowers);
    read(e, "enterable", s.enterable);
    if (const json t = j.value("moduleTones", json::object()); t.is_object())
        for (auto it = t.begin(); it != t.end(); ++it)
            if (it->is_array() && it->size() == 2)
                s.moduleTones[it.key()] = {(*it)[0].is_string() ? (*it)[0].get<std::string>() : std::string(),
                                           (*it)[1].is_string() ? (*it)[1].get<std::string>() : std::string()};
    const json p = j.value("palette", json::object());
    s.renders = colours(p, "render");
    s.trims = colours(p, "trim");
    s.roofs = colours(p, "roof");
    s.bases = colours(p, "base");
    if (p.contains("dirt") && p["dirt"].is_array() && p["dirt"].size() == 2)
        s.dirt = {p["dirt"][0].get<float>(), p["dirt"][1].get<float>()};
    if (s.renders.empty())
        s.renders = {glm::vec3(0.7f)};
    if (s.trims.empty())
        s.trims = {glm::vec3(0.85f)};
    if (s.roofs.empty())
        s.roofs = {glm::vec3(0.35f, 0.2f, 0.15f)};
    if (s.bases.empty())
        s.bases = {glm::vec3(0.35f)};
    s.minFloors = std::max(1, s.minFloors);
    s.maxFloors = std::max(s.minFloors, s.maxFloors);
    s.groundHeight = std::clamp(s.groundHeight, 200.0, 1500.0);
    s.floorHeight = std::clamp(s.floorHeight, 200.0, 1200.0);
    s.bayWidth = std::clamp(s.bayWidth, 120.0, 3000.0);
    s.minBay = std::clamp(s.minBay, 80.0, s.bayWidth);
    if (s.id.empty() && error)
        *error = "the style has no id";
    return s;
}

json Style::toJson() const
{
    json j = raw.is_object() ? raw : json::object();
    j["id"] = id;
    j["label"] = label;
    j["description"] = description;
    j["tags"] = tags;
    j["floors"] = {{"min", minFloors}, {"max", maxFloors}, {"groundHeight", groundHeight}, {"height", floorHeight}, {"plinth", plinth}};
    j["bays"] = {{"width", bayWidth}, {"min", minBay}, {"cornerWidth", cornerWidth}, {"rhythm", rhythm}, {"blank", blankBays}, {"side", sideOpenings},
                 {"blankWithin", blankWithin}, {"sparseWithin", sparseWithin}};
    j["openings"] = {{"window", {{"width", windowW}, {"height", windowH}, {"sill", windowSill}, {"kind", windowKind}}},
                     {"door", {{"width", doorW}, {"height", doorH}, {"kind", doorKind}}},
                     {"shopfront", {{"width", shopW}, {"height", shopH}}},
                     {"reveal", reveal},
                     {"glassFacade", glassFacade}};
    j["groundFloor"] = {{"street", groundStreet}, {"arcade", arcade}, {"arcadeDepth", arcadeDepth}, {"shopfronts", shopfronts},
                        {"shutters", shutters}, {"awnings", awnings}};
    j["modules"] = {{"balconies", balconies}, {"balconyKind", balconyKind}, {"grilles", grilles}, {"acUnits", acUnits},
                    {"pilasters", pilasters}, {"cornice", cornice}, {"bands", bands}, {"corner", corner}, {"drainpipes", drainpipes},
                    {"sign", signKind}};
    j["roof"] = {{"kind", roof}, {"material", roofMaterial}, {"parapet", parapet}, {"overhang", overhang}, {"pitch", pitch},
                 {"waterTanks", waterTanks}, {"clutter", clutter}, {"rebar", rebar}};
    j["massing"] = {{"podium", podium}, {"towerInset", towerInset}, {"stepEvery", stepEvery}, {"stepInset", stepInset}, {"wallThickness", wallThickness}};
    j["extras"] = {{"columns", columns}, {"domes", domes}, {"perimeterWall", perimeterWall}, {"gate", gate}, {"pool", pool},
                   {"bellTowers", bellTowers}, {"enterable", enterable}};
    j["palette"] = {{"render", hexes(renders)}, {"trim", hexes(trims)}, {"roof", hexes(roofs)}, {"base", hexes(bases)}, {"dirt", {dirt.x, dirt.y}}};
    if (!moduleTones.empty())
    {
        json t = json::object();
        for (const auto& [kind, tones] : moduleTones)
            t[kind] = {tones[0].empty() ? json() : json(tones[0]), tones[1].empty() ? json() : json(tones[1])};
        j["moduleTones"] = t;
    }
    else
        j.erase("moduleTones");
    return j;
}

std::array<glm::vec4, 2> moduleTones(const Style& st, const Palette& p, const std::string& kind)
{
    static const std::set<std::string> kRender = {"wall", "pilaster", "cornice", "corner", "band", "plinth", "dome"};
    static const std::set<std::string> kFramed = {"window", "door", "shopfront", "balcony"};
    auto entry = [&](const std::string& name) -> glm::vec4 {
        if (name == "render")
            return {p.render, 1.0f};
        if (name == "trim")
            return {p.trim, 1.0f};
        if (name == "roof")
            return {p.roof, 1.0f};
        if (name == "base")
            return {p.base, 1.0f};
        if (name == "frame")
            return {p.frame, 1.0f};
        if (name == "door")
            return {p.door, 1.0f};
        if (name == "awning")
            return {p.awning, 1.0f};
        return glm::vec4(0.0f);
    };
    const std::string main = kind == "roof" ? "roof" : kRender.contains(kind) ? "render" : "trim";
    std::array<std::string, 2> names = {main, kFramed.contains(kind) ? std::string("none") : main};
    if (auto it = st.moduleTones.find(kind); it != st.moduleTones.end())
        for (int k = 0; k < 2; ++k)
            if (!it->second[size_t(k)].empty())
                names[size_t(k)] = it->second[size_t(k)];
    return {entry(names[0]), entry(names[1])};
}

const std::vector<std::string>& shippedStyleIds()
{
    static const std::vector<std::string> ids = {"colonial",  "seafront",     "resortTower", "modernCommercial", "residential",
                                                 "mansion",   "civic",        "policeStation", "church",         "mausoleum",
                                                 "warehouse", "packingPlant", "portShed",    "hangar",           "adobe",
                                                 "ruralShack", "terminal",    "lighthouse"};
    return ids;
}

std::string styleFor(std::string_view settlementStyle, std::string_view program, std::string_view use)
{
    const std::string s(settlementStyle), p(program), u(use);
    if (p == "police" || p == "fire_station" || p == "prison")
        return "policeStation";
    for (const std::string& id : shippedStyleIds())
        if (s == id)
            return id;
    // A user or workspace style named by the settlement; the node falls back with a warning when it is missing.
    if (!s.empty())
        return s;
    if (p == "church" || p == "cathedral" || p == "chapel" || p == "place_of_worship")
        return "church";
    if (p == "townhall" || p == "courthouse" || p == "government" || p == "public")
        return "civic";
    if (p == "hangar")
        return "hangar";
    if (p == "warehouse" || p == "industrial" || p == "factory" || u == "industrial")
        return "warehouse";
    if (p == "hotel" || p == "apartments")
        return "resortTower";
    if (u == "commercial" || p == "shop" || p == "retail" || p == "office" || p == "supermarket")
        return "modernCommercial";
    if (u == "civic")
        return "civic";
    if (u == "farm" || u == "rural")
        return "adobe";
    return "residential";
}

// Specs -------------------------------------------------------------------------------------------------------------------

json Spec::toJson() const
{
    json tiersJ = json::array(), holesJ = json::array(), biz = json::array(), doors = json::array();
    for (const Tier& t : tiers)
        tiersJ.push_back({{"footprint", ringJson(t.footprint)}, {"bottom", t.bottom}, {"top", t.top}});
    for (const Ring& h : holes)
        holesJ.push_back(ringJson(h));
    for (const Business& b : businesses)
    {
        json e = {{"name", b.name}, {"program", b.program}};
        if (b.hasAt)
            e["at"] = {b.at.x, b.at.y};
        biz.push_back(std::move(e));
    }
    for (const Vec2& d : entrances)
        doors.push_back({d.x, d.y});
    json j = {{"id", id},
              {"key", hex64(key)},
              {"source", source},
              {"footprint", ringJson(footprint)},
              {"sides", sides},
              {"holes", holesJ},
              {"tiers", tiersJ},
              {"ground", ground},
              {"highGround", highGround},
              {"top", top},
              {"roofHeight", roofHeight},
              {"floors", floors},
              {"roof", roof},
              {"front", {front.x, front.y}},
              {"ridge", {ridge.x, ridge.y}},
              {"entrances", doors},
              {"program", program},
              {"use", use},
              {"groundUse", groundUse},
              {"style", style},
              {"styleFrom", styleFrom},
              {"name", name},
              {"corner", corner},
              {"landmark", landmark},
              {"era", era},
              {"businesses", biz},
              {"wealth", wealth},
              {"random", random},
              {"regions", regions},
              {"setbackFront", setbackFront},
              {"setbackSide", setbackSide}};
    if (!cornerGround.empty())
        j["cornerGround"] = cornerGround;
    if (!clearance.empty())
        j["clearance"] = clearance;
    for (auto [k, v] : {std::pair{"colour", &colour}, std::pair{"roofColour", &roofColour}, std::pair{"material", &material},
                        std::pair{"roofMaterial", &roofMaterial}, std::pair{"osmId", &osmId}})
        if (!v->empty())
            j[k] = *v;
    return j;
}

Spec Spec::fromJson(const json& j)
{
    Spec s;
    read(j, "id", s.id);
    if (j.contains("key") && j["key"].is_string())
        s.key = std::strtoull(j["key"].get<std::string>().c_str(), nullptr, 16);
    else
        read(j, "key", s.key);
    read(j, "source", s.source);
    s.footprint = ringOf(j.value("footprint", json::array()));
    read(j, "sides", s.sides);
    read(j, "clearance", s.clearance);
    for (const json& h : j.value("holes", json::array()))
        s.holes.push_back(ringOf(h));
    for (const json& t : j.value("tiers", json::array()))
        s.tiers.push_back({ringOf(t.value("footprint", json::array())), t.value("bottom", 0.0), t.value("top", 0.0)});
    read(j, "ground", s.ground);
    s.highGround = j.value("highGround", s.ground);
    read(j, "cornerGround", s.cornerGround);
    read(j, "top", s.top);
    read(j, "roofHeight", s.roofHeight);
    read(j, "floors", s.floors);
    read(j, "roof", s.roof);
    if (j.contains("front") && j["front"].is_array() && j["front"].size() >= 2)
        s.front = {j["front"][0].get<double>(), j["front"][1].get<double>()};
    if (j.contains("ridge") && j["ridge"].is_array() && j["ridge"].size() >= 2)
        s.ridge = {j["ridge"][0].get<double>(), j["ridge"][1].get<double>()};
    s.entrances = ringOf(j.value("entrances", json::array()));
    for (auto [k, v] : {std::pair{"program", &s.program}, std::pair{"use", &s.use}, std::pair{"groundUse", &s.groundUse},
                        std::pair{"style", &s.style}, std::pair{"styleFrom", &s.styleFrom}, std::pair{"name", &s.name},
                        std::pair{"corner", &s.corner}, std::pair{"landmark", &s.landmark}, std::pair{"era", &s.era},
                        std::pair{"colour", &s.colour}, std::pair{"roofColour", &s.roofColour}, std::pair{"material", &s.material},
                        std::pair{"roofMaterial", &s.roofMaterial}, std::pair{"osmId", &s.osmId}})
        read(j, k, *v);
    for (const json& b : j.value("businesses", json::array()))
    {
        Business x;
        read(b, "name", x.name);
        read(b, "program", x.program);
        if (b.contains("at") && b["at"].is_array() && b["at"].size() >= 2)
        {
            x.at = {b["at"][0].get<double>(), b["at"][b["at"].size() - 1].get<double>()};
            x.hasAt = true;
        }
        s.businesses.push_back(std::move(x));
    }
    read(j, "wealth", s.wealth);
    read(j, "random", s.random);
    read(j, "regions", s.regions);
    read(j, "setbackFront", s.setbackFront);
    read(j, "setbackSide", s.setbackSide);
    return s;
}

Spec Spec::fromEnvelope(const json& env, const std::string& settlement)
{
    Spec s;
    s.source = "settlement";
    uint64_t key = env.value("lotId", uint64_t(0));
    // Site kit envelopes carry no lot: their stable id keys them instead.
    if (key == 0 && env.contains("id") && env["id"].is_string())
        key = fnv(env["id"].get<std::string>());
    if (key == 0)
        key = fnv(settlement + "/" + std::to_string(env.value("lot", 0)));
    s.key = key;
    s.id = "building:" + hex64(key);
    Ring fp = ringOf(env.value("footprint", json::array()));
    std::vector<int> sides;
    for (const json& t : env.value("sides", json::array()))
    {
        const std::string n = t.is_string() ? t.get<std::string>() : std::string();
        sides.push_back(n == "street" ? 0 : n == "party" ? 1 : 2);
    }
    sides.resize(fp.size(), 2);
    // Sides follow the edges, so flip them with the ring when it is clockwise.
    if (signedArea(fp) < 0.0)
    {
        std::reverse(fp.begin(), fp.end());
        // Edge i (p_i -> p_i+1) becomes edge n-2-i after reversing; the closing edge stays last.
        std::vector<int> flipped(sides.size(), 2);
        const size_t n = sides.size();
        for (size_t i = 0; i < n; ++i)
            flipped[(2 * n - 2 - i) % n] = sides[i];
        sides = flipped;
    }
    s.footprint = fp;
    s.sides = sides;
    for (const json& h : env.value("holes", json::array()))
        s.holes.push_back(ringOf(h));
    for (const json& t : env.value("tiers", json::array()))
        s.tiers.push_back({ringOf(t.value("footprint", json::array())), t.value("bottom", 0.0), t.value("top", 0.0)});
    s.ground = env.value("ground", 0.0);
    s.top = env.value("top", 0.0);
    const double height = env.value("height", 0.0);
    s.highGround = std::max(s.ground, s.top - height);
    s.roofHeight = env.value("roofHeight", 0.0);
    s.floors = std::max(1, env.value("floors", 1));
    s.roof = env.value("roof", std::string("flat"));
    if (env.contains("front") && env["front"].is_array() && env["front"].size() >= 2)
        s.front = {env["front"][0].get<double>(), env["front"][env["front"].size() - 1].get<double>()};
    if (env.contains("ridge") && env["ridge"].is_array() && env["ridge"].size() >= 2)
        s.ridge = {env["ridge"][0].get<double>(), env["ridge"][env["ridge"].size() - 1].get<double>()};
    s.entrances = ringOf(env.value("entrances", json::array()));
    s.program = env.value("program", std::string());
    s.use = env.value("use", std::string());
    s.groundUse = env.value("groundUse", std::string());
    s.style = env.value("style", std::string());
    s.styleFrom = env.value("styleFrom", std::string());
    s.name = env.value("name", std::string());
    s.corner = env.value("corner", std::string("none"));
    s.landmark = env.value("landmark", std::string());
    s.era = env.value("eraName", std::string());
    s.wealth = env.value("wealth", 1);
    s.random = env.value("random", 0.0f);
    s.setbackFront = env.value("frontSetback", 0.0);
    s.setbackSide = env.value("sideSetback", 0.0);
    for (const json& b : env.value("businesses", json::array()))
    {
        Business x;
        x.name = b.value("name", std::string());
        x.program = b.value("program", std::string());
        if (b.contains("at") && b["at"].is_array() && b["at"].size() >= 2)
        {
            x.at = {b["at"][0].get<double>(), b["at"][b["at"].size() - 1].get<double>()};
            x.hasAt = true;
        }
        s.businesses.push_back(std::move(x));
    }
    for (auto [k, v] : {std::pair{"colour", &s.colour}, std::pair{"roofColour", &s.roofColour}, std::pair{"material", &s.material},
                        std::pair{"roofMaterial", &s.roofMaterial}, std::pair{"osmId", &s.osmId}})
        *v = env.value(k, std::string());
    if (!settlement.empty())
        s.regions.push_back("settlement:" + settlement);
    if (env.contains("district") && env["district"].is_number_integer())
        s.regions.push_back("district:" + std::to_string(env["district"].get<int>()));
    return s;
}

double Spec::groundAt(Vec2 p) const
{
    if (cornerGround.size() != footprint.size() || footprint.size() < 3)
        return ground;
    // Inverse distance over the sampled corners: smooth, exact at each corner.
    double sw = 0.0, sh = 0.0;
    for (size_t k = 0; k < footprint.size(); ++k)
    {
        const double d = glm::length(p - footprint[k]);
        if (d < 1.0)
            return cornerGround[k];
        const double w = 1.0 / (d * d);
        sw += w;
        sh += w * cornerGround[k];
    }
    return sh / sw;
}

std::pair<double, double> Spec::groundRange() const
{
    if (cornerGround.empty())
        return {ground, std::max(highGround, ground)};
    return {*std::min_element(cornerGround.begin(), cornerGround.end()), *std::max_element(cornerGround.begin(), cornerGround.end())};
}

bool Spec::sloped() const
{
    const auto [lo, hi] = groundRange();
    return !cornerGround.empty() && hi - lo > 60.0;
}

double Spec::steppedLevel(Vec2 p, double plinth, double floor0) const
{
    const double lo = groundRange().first;
    const double stepped = lo + std::floor((groundAt(p) + plinth - lo) / 30.0 + 0.5) * 30.0;
    return std::clamp(stepped, lo + 5.0, std::max(floor0, lo + 5.0));
}

// Edits -------------------------------------------------------------------------------------------------------------------

bool Edits::empty() const
{
    return style.empty() && !floors && !seed && !bayWidth && !floorHeight && !roof && !enterable && palette.empty() && bays.empty() &&
           toggles.empty() && facadeBays.empty() && (!interior.is_object() || interior.empty());
}

json Edits::toJson() const
{
    json j = json::object();
    if (!style.empty())
        j["style"] = style;
    if (floors)
        j["floors"] = *floors;
    if (seed)
        j["seed"] = *seed;
    if (bayWidth)
        j["bayWidth"] = *bayWidth;
    if (floorHeight)
        j["floorHeight"] = *floorHeight;
    if (roof)
        j["roof"] = *roof;
    if (enterable)
        j["enterable"] = *enterable;
    if (!palette.empty())
        j["palette"] = palette;
    if (!bays.empty())
        j["bays"] = bays;
    if (!toggles.empty())
        j["toggles"] = toggles;
    if (!facadeBays.empty())
        j["facadeBays"] = facadeBays;
    if (interior.is_object() && !interior.empty())
        j["interior"] = interior;
    return j;
}

Edits Edits::fromJson(const json& j)
{
    Edits e;
    if (!j.is_object())
        return e;
    read(j, "style", e.style);
    if (j.contains("floors") && j["floors"].is_number())
        e.floors = j["floors"].get<int>();
    if (j.contains("seed") && j["seed"].is_number())
        e.seed = j["seed"].get<int>();
    if (j.contains("bayWidth") && j["bayWidth"].is_number())
        e.bayWidth = j["bayWidth"].get<double>();
    if (j.contains("floorHeight") && j["floorHeight"].is_number())
        e.floorHeight = j["floorHeight"].get<double>();
    if (j.contains("roof") && j["roof"].is_string())
        e.roof = j["roof"].get<std::string>();
    if (j.contains("enterable") && j["enterable"].is_boolean())
        e.enterable = j["enterable"].get<bool>();
    if (j.contains("palette") && j["palette"].is_object())
        e.palette = j["palette"];
    read(j, "bays", e.bays);
    read(j, "toggles", e.toggles);
    read(j, "facadeBays", e.facadeBays);
    if (j.contains("interior") && j["interior"].is_object())
        e.interior = j["interior"];
    return e;
}
} // namespace buildings
