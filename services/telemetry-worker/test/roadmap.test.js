import test from "node:test";
import assert from "node:assert/strict";

import { handleRoadmap, resetCardCache } from "../src/roadmap.js";
import { fakeD1 } from "./d1.js";

const ROADMAP = { sections: [{ id: "alpha", cards: [{ id: "modeling" }, { id: "uvs" }] }] };

function env() {
  return { CREDITS_DB: fakeD1(), VOTE_SALT: "salt", ROADMAP_URL: "https://site.test/roadmap.json" };
}

function req(path, ip, body) {
  return new Request("https://w.test" + path, {
    method: body ? "POST" : "GET",
    headers: { "CF-Connecting-IP": ip, "Content-Type": "application/json" },
    body: body ? JSON.stringify(body) : undefined,
  });
}

test.beforeEach(() => {
  resetCardCache();
  globalThis.fetch = async () => new Response(JSON.stringify(ROADMAP));
});

test("one vote per card per address, and it can be taken back", async () => {
  const e = env();
  await handleRoadmap(req("/roadmap/vote", "1.1.1.1", { card: "uvs" }), e);
  await handleRoadmap(req("/roadmap/vote", "1.1.1.1", { card: "uvs" }), e);
  let r = await (await handleRoadmap(req("/roadmap/vote", "2.2.2.2", { card: "uvs" }), e)).json();
  assert.equal(r.counts.uvs, 2);
  assert.deepEqual(r.mine, ["uvs"]);

  r = await (await handleRoadmap(req("/roadmap/vote", "1.1.1.1", { card: "uvs", on: false }), e)).json();
  assert.equal(r.counts.uvs, 1);
  assert.deepEqual(r.mine, []);

  r = await (await handleRoadmap(req("/roadmap/votes", "2.2.2.2"), e)).json();
  assert.deepEqual(r, { counts: { uvs: 1 }, mine: ["uvs"] });
});

test("cards not on the roadmap are refused", async () => {
  const res = await handleRoadmap(req("/roadmap/vote", "1.1.1.1", { card: "made-up" }), env());
  assert.equal(res.status, 404);
});

test("the address itself is never stored", async () => {
  const e = env();
  await handleRoadmap(req("/roadmap/vote", "9.8.7.6", { card: "modeling" }), e);
  const rows = (await e.CREDITS_DB.prepare("SELECT voter FROM roadmap_votes").all()).results;
  assert.equal(rows.length, 1);
  assert.ok(!rows[0].voter.includes("9.8.7.6"));
});

test("votes are off without the salt, and other paths fall through", async () => {
  const e = env();
  delete e.VOTE_SALT;
  assert.equal((await handleRoadmap(req("/roadmap/votes", "1.1.1.1"), e)).status, 503);
  assert.equal(await handleRoadmap(req("/credits.json", "1.1.1.1"), env()), null);
});
