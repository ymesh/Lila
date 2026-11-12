// Roadmap votes for lila-3d.github.io/roadmap.html. One vote per card per voter, where the voter is a salted
// hash of the IP (VOTE_SALT secret), so no address is stored. Card ids come from the site's roadmap.json.
// Bindings: CREDITS_DB (D1, roadmap_votes table), VOTE_LIMIT (per-minute rate limit, optional).

import { sha256 } from "./issues.js";

export const ROADMAP_URL = "https://lila-3d.github.io/roadmap.json";
const CARDS_TTL_MS = 10 * 60 * 1000;
const MAX_CARDS = 500;

let cardsCache = { at: 0, ids: null, url: "" };

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type",
};

function json(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json", "Cache-Control": "no-store", ...CORS },
  });
}

/** Ids of every card in roadmap.json, cached; a stale list is kept when the site can't be reached. */
export async function cardIds(env, now = Date.now()) {
  const url = env.ROADMAP_URL || ROADMAP_URL;
  if (cardsCache.ids && cardsCache.url === url && now - cardsCache.at < CARDS_TTL_MS) return cardsCache.ids;
  try {
    const res = await fetch(url, { cf: { cacheTtl: 300 } });
    if (!res.ok) throw new Error(String(res.status));
    const data = await res.json();
    const ids = new Set();
    for (const section of data.sections || [])
      for (const card of section.cards || [])
        if (typeof card.id === "string" && ids.size < MAX_CARDS) ids.add(card.id);
    cardsCache = { at: now, ids, url };
  } catch {
    if (!cardsCache.ids || cardsCache.url !== url) return null;
  }
  return cardsCache.ids;
}

export function resetCardCache() {
  cardsCache = { at: 0, ids: null, url: "" };
}

async function voterOf(request, env) {
  return sha256((request.headers.get("CF-Connecting-IP") || "unknown") + "|" + env.VOTE_SALT);
}

async function tally(env, voter) {
  const counts = {};
  for (const r of (await env.CREDITS_DB.prepare("SELECT card, COUNT(*) AS n FROM roadmap_votes GROUP BY card").all()).results)
    counts[r.card] = r.n;
  const mine = (await env.CREDITS_DB.prepare("SELECT card FROM roadmap_votes WHERE voter = ?").bind(voter).all()).results.map((r) => r.card);
  return { counts, mine };
}

async function vote(request, env) {
  if (env.VOTE_LIMIT) {
    const { success } = await env.VOTE_LIMIT.limit({ key: request.headers.get("CF-Connecting-IP") || "unknown" });
    if (!success) return json({ error: "too many votes, try again in a minute" }, 429);
  }
  let body;
  try {
    body = await request.json();
  } catch {
    return json({ error: "bad json" }, 400);
  }
  const card = typeof body?.card === "string" ? body.card : "";
  const ids = await cardIds(env);
  if (!ids) return json({ error: "roadmap unavailable" }, 503);
  if (!ids.has(card)) return json({ error: "unknown card" }, 404);
  const voter = await voterOf(request, env);
  if (body.on === false)
    await env.CREDITS_DB.prepare("DELETE FROM roadmap_votes WHERE card = ? AND voter = ?").bind(card, voter).run();
  else
    await env.CREDITS_DB.prepare("INSERT OR IGNORE INTO roadmap_votes (card, voter, at) VALUES (?, ?, ?)").bind(card, voter, new Date().toISOString()).run();
  return json(await tally(env, voter));
}

/** The roadmap routes, or null when the path isn't one of them. */
export async function handleRoadmap(request, env) {
  const path = new URL(request.url).pathname;
  if (path !== "/roadmap/votes" && path !== "/roadmap/vote") return null;
  if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });
  if (!env.CREDITS_DB || !env.VOTE_SALT) return json({ error: "votes not configured" }, 503);
  if (path === "/roadmap/votes" && request.method === "GET") return json(await tally(env, await voterOf(request, env)));
  if (path === "/roadmap/vote" && request.method === "POST") return vote(request, env);
  return json({ error: "not found" }, 404);
}
