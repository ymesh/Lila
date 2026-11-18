"""Paintbox: twelve painterly looks for the Painterly shading model.

Each look is a palette, a stroke sheet and stroke settings tuned together. meta.shading holds the material node settings
that drive the live Painterly model; the graph bakes the same look into plain channels (palette into base colour, the
sheet's stroke tilt into the normal, its shade into height) for targets that want ordinary PBR textures.
"""

from common import lin, material, std
from matgraph import col, f

SHEETS = {"soft": "strokes_soft", "bristle": "strokes_bristle", "dabs": "strokes_dabs"}


def look(ident, name, description, tags, sheet, tiles, palette, shadow, light, accent, contrast=0.5, spread=0.35, bands=5.0,
         relief=0.3, tilt=0.6, rough=0.8, layer_ratio=2.7, broken=0.0, extra=None):
    @material(ident, name, f"Paintbox/{name}", "stylised", 50, description, ["paintbox", "brushwork", "painterly", "stylised", "strokes"] + tags)
    def build(m):
        P = std(m, wear=relief, age=0.0, var=0.6, wear_doc="Stroke relief: how thick the paint stands.", age_doc="Aged varnish: warmer, duller.")
        sheet_file = f"strokes/{SHEETS[sheet]}.png"
        shading = {
            "shadingModel": "painterly",
            "strokeMap": SHEETS[sheet],
            "strokeScale": 50.0 / tiles,
            "strokeNormal": tilt,
            "strokeLayerRatio": layer_ratio,
            "paletteCount": 3,
            "paletteShadow": lin(shadow),
            "paletteLight": lin(light),
            "paletteAccent": lin(accent),
            "paletteContrast": contrast,
            "paletteSpread": spread,
            "specularTint": 0.6,
        }
        if extra:
            shading.update(extra)
        m.meta["shadingModel"] = "painterly"
        m.meta["shading"] = shading
        st = m.add("image", name="Strokes", file=sheet_file, tiles=tiles)
        st2 = m.add("image", name="FineStrokes", file=sheet_file, tiles=int(round(tiles * layer_ratio)))
        light_field = m.add("clouds", name="LightField", scale=2, octaves=4, persistence=0.5)
        c0, c1, c2, c3 = (col(c) for c in palette)
        base = m.px(f"""
float4 s = a.sample(uv);
float4 s2 = b.sample(uv);
float t = texToFloat(c.sample(uv));
t = saturate(0.5 + (t - 0.5) * 1.6 + (s.a - 0.5) * {f(spread)} * (0.4 + p.z) + (s.b - 0.5) * 0.3 + (s2.b - 0.5) * 0.15);
float k = t * {f(bands)};
t = (floor(k) + smoothstep(0.5 - 0.5 * {f(1.0 - contrast)}, 0.5 + 0.5 * {f(1.0 - contrast)}, frac(k))) / {f(bands)};
float3 cc = t < 0.34 ? lerp({c0}, {c1}, t / 0.34) : (t < 0.67 ? lerp({c1}, {c2}, (t - 0.34) / 0.33) : lerp({c2}, {c3}, (t - 0.67) / 0.33));
float gap = step(s.b, {f(broken)} * 0.6) * {f(min(broken * 2.0, 1.0))};
cc = lerp(cc, {c0} * 0.85, gap);
cc = lerp(cc, cc * {col("#e8d2a0")}, p.y * 0.4);
return float4(cc, 1.0);""", name="Palette", a=st, b=st2, c=light_field, z=P["var"], y=P["age"])
        tiltN = m.px(f"""
float4 s = a.sample(uv);
float4 s2 = b.sample(uv);
float2 t = (s.rg - 0.5) * 2.0 + (s2.rg - 0.5) * 0.8;
float3 n = normalize(float3(t * {f(tilt)}, 1.0));
return float4(n * 0.5 + 0.5, 1.0);""", name="StrokeTilt", a=st, b=st2)
        h = m.gray("0.5 + (a.sample(uv).b - 0.5) * p.x * 0.2 + (b.sample(uv).b - 0.5) * p.x * 0.08", name="Height", a=st, b=st2, x=P["wear"])
        r = m.gray(f"{f(rough)} + (a.sample(uv).b - 0.5) * 0.1 + p.y * 0.1", name="Roughness", a=st, y=P["age"])
        m.finish(base, r, 0.0, h, normal=0.6, ao=(0.01, 0.03, 0.4), hue=P["hue"], detail=tiltN)

    return build


look("GouacheMorning", "Gouache Morning", "Soft opaque gouache in early light: cool lilac shadows, peach mid-tones, creamy highlights, gentle round strokes.",
     ["gouache", "soft", "pastel", "morning"], "soft", 5, ["#6e6a9a", "#c89a9a", "#f0c8a0", "#fbecd2"], "#5a5a90", "#ffe8c8", "#f0a890",
     contrast=0.45, spread=0.3, relief=0.15, tilt=0.45, rough=0.9)

look("ImpastoEmber", "Impasto Ember", "Thick loaded oil paint in ember reds and burning oranges, every stroke standing proud with bristle ridges.",
     ["impasto", "oil", "thick", "warm", "fire"], "bristle", 4, ["#3a1418", "#8a2a1e", "#d8642a", "#ffc070"], "#3a1020", "#ffc878", "#e04020",
     contrast=0.6, spread=0.4, relief=0.8, tilt=1.0, rough=0.55, extra={"specularTint": 0.8})

look("DryBrushDusk", "Dry Brush Dusk", "Scumbled dry-brush strokes at dusk: violet ground breaking through broken orange and rose strokes.",
     ["dry brush", "scumble", "dusk", "violet", "broken"], "bristle", 5, ["#2e2448", "#6a3a6a", "#d0705a", "#f8c08a"], "#2a2050", "#ffb880", "#c05080",
     contrast=0.55, spread=0.45, relief=0.4, tilt=0.8, rough=0.85, broken=0.45)

look("WashAndBloom", "Wash and Bloom", "Watercolour-like washes: pale blue-green pools with soft blooms, few value steps, the paper glowing through.",
     ["watercolour", "watercolor", "wash", "pale", "soft"], "soft", 3, ["#7a9ab0", "#a8c8c0", "#dce8d8", "#f6f4ea"], "#7090b0", "#fff8e8", "#a8d0c0",
     contrast=0.25, spread=0.5, bands=3.0, relief=0.0, tilt=0.25, rough=0.95)

look("PaletteKnifeSlate", "Palette Knife Slate", "Flat palette-knife planes in cool slate greys and blue, hard-edged steps between values.",
     ["palette knife", "flat", "grey", "cool", "hard edge"], "dabs", 4, ["#2a3038", "#5a6470", "#9aa4ac", "#dfe2e0"], "#283040", "#e8ecec", "#6a88a8",
     contrast=0.9, spread=0.35, bands=4.0, relief=0.35, tilt=0.7, rough=0.7)

look("PointillistNoon", "Pointillist Noon", "Small separate dabs of pure colour at midday: yellows against violets, oranges against blues, mixing in the eye.",
     ["pointillism", "dots", "dabs", "bright", "complementary"], "dabs", 7, ["#3a4aa0", "#8a5ab0", "#f0a040", "#fff07a"], "#4050b0", "#fff0a0", "#ff8040",
     contrast=0.8, spread=0.9, bands=4.0, relief=0.25, tilt=0.6, rough=0.85, layer_ratio=2.0)

look("CharcoalStudy", "Charcoal Study", "A tonal study in charcoal and chalk on warm paper: soft smudged darks, crisp chalk lights, no colour.",
     ["charcoal", "sketch", "monochrome", "drawing", "study"], "soft", 5, ["#2a2826", "#6a6662", "#b8b2a6", "#ece6d8"], "#2a2826", "#f4eee0", "#8a8478",
     contrast=0.75, spread=0.3, bands=4.0, relief=0.1, tilt=0.35, rough=0.95)

look("PleinAirVerdure", "Plein Air Verdure", "Quick outdoor oil sketch in greens: blue-green shade, sap and olive mid-tones, sunlit yellow strokes.",
     ["plein air", "green", "landscape", "oil", "foliage"], "bristle", 5, ["#1e3a3a", "#3a6a3a", "#8aaa3e", "#e8e08a"], "#1e3a40", "#f8f0a0", "#a8c040",
     contrast=0.55, spread=0.4, relief=0.45, tilt=0.8, rough=0.75)

look("MoonlitNocturne", "Moonlit Nocturne", "A night scene in deep ultramarine and indigo, silver-blue moonlit strokes and a rare warm window glow.",
     ["nocturne", "night", "blue", "moonlight", "dark"], "soft", 4, ["#0e1430", "#20305e", "#4a6a9a", "#b8c8e0"], "#0c1028", "#c8d8f0", "#e0a050",
     contrast=0.5, spread=0.35, relief=0.2, tilt=0.5, rough=0.8)

look("AutumnGlaze", "Autumn Glaze", "Glazed oils in autumn: umber depths under translucent orange and gold, a soft varnish sheen.",
     ["glaze", "autumn", "orange", "gold", "varnish"], "soft", 4, ["#3a2014", "#8a4220", "#d88a30", "#f6d07a"], "#3a2018", "#ffd890", "#c84020",
     contrast=0.4, spread=0.35, relief=0.2, tilt=0.5, rough=0.35, extra={"specularTint": 0.7})

look("FrescoChalk", "Fresco Chalk", "Chalky fresco pigments on lime: terracotta, ochre and faded teal in soft square dabs, very matte.",
     ["fresco", "chalk", "pastel", "matte", "terracotta"], "dabs", 5, ["#7a5a5a", "#c07a5a", "#d8b07a", "#8ab0a8"], "#6a5a68", "#f8e8d0", "#7aa8a0",
     contrast=0.4, spread=0.6, relief=0.1, tilt=0.4, rough=0.97, extra={"specularTint": 0.1})

look("StormlightOil", "Stormlight Oil", "Dramatic oils under a breaking storm: teal-black shadows, slate mid-tones and shafts of gold light.",
     ["storm", "dramatic", "teal", "gold", "oil"], "bristle", 4, ["#14262a", "#3a5258", "#8a8a6a", "#f0c860"], "#10242a", "#ffd070", "#4a8a8a",
     contrast=0.65, spread=0.4, relief=0.5, tilt=0.85, rough=0.7)
