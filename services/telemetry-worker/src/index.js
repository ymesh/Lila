// Receives Lila's telemetry batches, adds the country, drops the sender's IP, and forwards them to PostHog.
// The PostHog project token is the POSTHOG_KEY secret (wrangler secret put POSTHOG_KEY).
// Also serves the credits (creditsService.js), issue reports (issues.js) and roadmap votes (roadmap.js).

import { handleCredits, nightly } from "./creditsService.js";
import { handleIssues } from "./issues.js";
import { handleRoadmap } from "./roadmap.js";

const POSTHOG_BATCH = "https://eu.i.posthog.com/batch/";
const EVENTS = new Set(["session_start", "session_end", "onboarding"]);
const MAX_BYTES = 512 * 1024;
const MAX_EVENTS = 500;

export default {
  async fetch(request, env) {
    if (new URL(request.url).pathname !== "/e")
      return (await handleIssues(request, env)) || (await handleRoadmap(request, env)) || (await handleCredits(request, env)) || new Response("not found", { status: 404 });
    if (request.method !== "POST")
      return new Response("not found", { status: 404 });
    const body = await request.text();
    if (body.length > MAX_BYTES)
      return new Response("too large", { status: 413 });

    let batch;
    try {
      batch = JSON.parse(body).batch;
    } catch {
      return new Response("bad json", { status: 400 });
    }
    if (!Array.isArray(batch) || batch.length === 0 || batch.length > MAX_EVENTS)
      return new Response("bad batch", { status: 400 });

    const country = request.cf?.country;
    const events = [];
    for (const e of batch) {
      if (!e || typeof e !== "object" || !EVENTS.has(e.event) || typeof e.distinct_id !== "string" || e.distinct_id.length > 64)
        continue;
      const properties = e.properties && typeof e.properties === "object" ? e.properties : {};
      delete properties.$ip;
      properties.$geoip_disable = true;
      if (country && country !== "XX" && country !== "T1")
        properties.$geoip_country_code = country;
      events.push({ event: e.event, distinct_id: e.distinct_id, timestamp: e.timestamp, properties });
    }
    // Malformed events are dropped rather than retried forever by the client.
    if (events.length === 0)
      return new Response("ok", { status: 200 });

    const forwarded = await fetch(POSTHOG_BATCH, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ api_key: env.POSTHOG_KEY, batch: events }),
    });
    return new Response(forwarded.ok ? "ok" : "upstream error", { status: forwarded.ok ? 200 : 502 });
  },

  async scheduled(event, env, ctx) {
    if (env.CREDITS_DB)
      ctx.waitUntil(nightly(env));
  },
};
