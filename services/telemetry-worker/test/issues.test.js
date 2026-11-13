import test from "node:test";
import assert from "node:assert/strict";
import { generateKeyPairSync } from "node:crypto";

import { allowedHost, sanitizeText, stripPaths, LINK_HOSTS } from "../src/issues.js";
import { appJwt, attachmentName, buildCommit, UNOFFICIAL_BUILD, handleIssues, MAX_BYTES_PER_DAY, MAX_REPLIES_PER_IP_DAY, MAX_REPORTS_PER_IP_DAY, issueBody, parseReport, replyAttachmentName, reportStatus, sha256 } from "../src/issues.js";

/** An R2 stand-in holding bytes in a Map. */
function fakeR2() {
  const items = new Map();
  return {
    items,
    async put(key, value, opts) {
      const bytes = typeof value === "string" ? new TextEncoder().encode(value) : new Uint8Array(await new Response(value).arrayBuffer());
      items.set(key, { bytes, httpMetadata: opts?.httpMetadata });
    },
    async get(key) {
      const it = items.get(key);
      if (!it) return null;
      return {
        body: new Response(it.bytes).body,
        httpMetadata: it.httpMetadata,
        json: async () => JSON.parse(new TextDecoder().decode(it.bytes)),
      };
    },
    async delete(key) {
      items.delete(key);
    },
  };
}

/** A GitHub stand-in for the calls the Worker makes. */
function fakeGitHub() {
  const issues = new Map();
  const calls = [];
  const fetcher = async (url, init = {}) => {
    const path = new URL(url).pathname;
    calls.push({ method: init.method || "GET", path, auth: init.headers?.Authorization });
    if (init.method === "POST" && path === "/repos/lila-3d/issues/issues") {
      const body = JSON.parse(init.body);
      const number = issues.size + 1;
      issues.set(number, { ...body, number, html_url: `https://github.com/lila-3d/issues/issues/${number}`, state: "open", comments: 0, labels: body.labels.map((name) => ({ name })) });
      return Response.json(issues.get(number), { status: 201 });
    }
    const m = path.match(/^\/repos\/lila-3d\/issues\/issues\/(\d+)(\/comments|\/labels)?$/);
    if (m && issues.has(Number(m[1]))) {
      const issue = issues.get(Number(m[1]));
      if (init.method === "POST" && m[2] === "/comments") {
        const c = { user: { login: "lila-reports[bot]" }, author_association: "NONE", created_at: "now", body: JSON.parse(init.body).body };
        issue.commentList = [...(issue.commentList || []), c];
        issue.comments = issue.commentList.length;
        return Response.json(c, { status: 201 });
      }
      if (init.method === "PUT" && m[2] === "/labels") {
        issue.labels = JSON.parse(init.body).labels.map((name) => ({ name }));
        return Response.json(issue.labels);
      }
      return Response.json(m[2] ? issue.commentList || [] : issue);
    }
    const cmp = path.match(/^\/repos\/lila-3d\/Lila\/compare\/([0-9a-f]+)\.\.\.main$/);
    if (cmp && COMMITS[cmp[1]]) return Response.json({ status: COMMITS[cmp[1]] });
    return new Response("nope", { status: 404 });
  };
  return { issues, calls, fetcher };
}

/** Commits the fake GitHub knows, by how main compares to them. */
const COMMITS = { "9641744ab1": "ahead", aaaaaaaaaa: "identical", f0f0f0f0f0: "diverged" };

const env = (extra = {}) => ({ REPORTS: fakeR2(), REPORTS_GITHUB_TOKEN: "t0ken", ADMIN_TOKEN: "admin", ...extra });

function reportRequest(meta, files = {}) {
  const form = new FormData();
  form.set("report", JSON.stringify(meta));
  for (const [k, v] of Object.entries(files)) form.set(k, v);
  return new Request("https://w.example/issues/new", { method: "POST", body: form });
}

const META = {
  title: "Bevel crashes",
  sections: [{ heading: "What happened?", text: "Bevel on a cube crashes." }],
  kind: "Bug",
  version: "0.9.1+9641744ab1 (build 202611120000-9641744ab1)",
  os: "Windows 11",
  gpu: "RTX 4090",
};

test("reports need a title and at least one answer", () => {
  assert.equal(parseReport({ ...META, title: "  " }).error, "title missing");
  assert.equal(parseReport({ ...META, sections: [] }).error, "description missing");
  assert.equal(parseReport({ ...META, sections: [{ heading: "What happened?", text: "  " }] }).error, "description missing");
  assert.equal(parseReport({ ...META, kind: "Rant" }).kind, "Bug");
  assert.equal(parseReport({ ...META, kind: "Crash" }).kind, "Crash");
  assert.equal(parseReport({ ...META, github: "@octo-cat" }).github, "octo-cat");
  assert.ok(parseReport({ ...META, github: "bad name!" }).error);
  assert.equal(parseReport({ ...META, title: "a\nb" }).title, "a b");
  const many = Array.from({ length: 20 }, (_, i) => ({ heading: `h${i}`, text: "t" }));
  assert.equal(parseReport({ ...META, sections: many }).sections.length, 10);
});

test("the issue body has a heading per answer, the environment and only lists private attachments", () => {
  const report = parseReport({
    ...META,
    github: "octocat",
    gpu: "A | B",
    sections: [
      { heading: "What were you doing?", text: "Beveling" },
      { heading: "Steps to reproduce", text: "1. Cube\n2. Bevel" },
      { heading: "Empty", text: "" },
    ],
  });
  const body = issueBody(report, "id-1", [{ label: "support bundle", size: 2 * 1024 * 1024 }, { label: "performance trace", size: 4096 }]);
  assert.ok(body.startsWith("### What were you doing?\n\nBeveling\n\n### Steps to reproduce\n\n1. Cube\n2. Bevel\n\n"));
  assert.doesNotMatch(body, /Empty/);
  assert.match(body, /\| GPU \| A {3}B \|/);
  assert.match(body, /\| Reporter \| @octocat \|/);
  assert.match(body, /Private attachments: support bundle \(2\.0 MB\), performance trace \(4 KB\)\./);
  assert.match(body, /<!-- lila-report id-1 -->/);
});

test("status keeps maintainer replies only", () => {
  const s = reportStatus({ number: 3, html_url: "u", title: "t", state: "closed", state_reason: "completed", labels: [{ name: "Solved" }], updated_at: "x" }, [
    { author_association: "OWNER", created_at: "a", body: "Fixed in the next update." },
    { author_association: "NONE", created_at: "b", body: "me too" },
    { author_association: "NONE", user: { login: "lila-reports[bot]" }, created_at: "c", body: "Which menu was open?" },
  ]);
  assert.deepEqual(s.labels, ["Solved"]);
  assert.deepEqual(s.replies, [
    { at: "a", body: "Fixed in the next update." },
    { at: "c", body: "Which menu was open?" },
  ]);
});

test("a report files an issue, stores attachments privately and answers status with its token", async () => {
  const e = env();
  const gh = fakeGitHub();
  const res = await handleIssues(
    reportRequest(META, { bundle: new File([new Uint8Array(10)], "b.zip"), screenshot: new File([new Uint8Array(5)], "s.png") }),
    e,
    gh.fetcher,
  );
  assert.equal(res.status, 200);
  const { number, token, url } = await res.json();
  assert.equal(number, 1);
  assert.match(url, /issues\/1$/);
  const issue = gh.issues.get(1);
  assert.deepEqual(issue.labels.map((l) => l.name), ["Bug", "Pending"]);
  assert.equal(gh.calls[0].auth, "Bearer t0ken");

  const rec = await (await e.REPORTS.get("issues/1.json")).json();
  assert.equal(rec.tokenHash, await sha256(token));
  assert.deepEqual(rec.files, ["bundle.zip", "screenshot.png"]);
  assert.equal(e.REPORTS.items.get(`files/${rec.id}/bundle.zip`).bytes.length, 10);
  assert.ok(!issue.body.includes(token));

  issue.comments = 1;
  issue.commentList = [{ author_association: "OWNER", created_at: "now", body: "Thanks, fixed." }];
  const st = await handleIssues(
    new Request("https://w.example/issues/status", { method: "POST", body: JSON.stringify({ reports: [{ number, token }, { number, token: "wrong" }] }) }),
    e,
    gh.fetcher,
  );
  const { reports } = await st.json();
  assert.equal(reports[0].replies[0].body, "Thanks, fixed.");
  assert.equal(reports[1].error, "unknown report");
});

test("extra attachments keep a safe name of their own", async () => {
  assert.equal(attachmentName("trace 2026-11-04.zip"), "file-trace_2026-11-04.zip");
  assert.equal(attachmentName("../../bundle.zip"), "file-_.._bundle.zip");
  assert.equal(attachmentName(""), "file-attachment");
  const e = env();
  await handleIssues(reportRequest(META, { file: new File([new Uint8Array(4)], "perf trace.zip") }), e, fakeGitHub().fetcher);
  const rec = await (await e.REPORTS.get("issues/1.json")).json();
  assert.deepEqual(rec.files, ["file-perf_trace.zip"]);
});

test("attachments download only with the admin token", async () => {
  const e = env();
  const gh = fakeGitHub();
  await handleIssues(reportRequest(META, { bundle: new File([new Uint8Array([1, 2, 3])], "b.zip") }), e, gh.fetcher);
  const get = (auth, q) => handleIssues(new Request(`https://w.example/issues/file?number=1${q}`, { headers: auth ? { Authorization: auth } : {} }), e, gh.fetcher);
  assert.equal((await get(null, "&name=bundle.zip")).status, 401);
  assert.deepEqual((await (await get("Bearer admin", "")).json()).files, ["bundle.zip"]);
  const file = await get("Bearer admin", "&name=bundle.zip");
  assert.deepEqual([...new Uint8Array(await file.arrayBuffer())], [1, 2, 3]);
  assert.equal((await get("Bearer admin", "&name=../x")).status, 404);
});

function replyRequest(reply, file) {
  const form = new FormData();
  form.set("reply", JSON.stringify(reply));
  if (file) form.set("file", file);
  return new Request("https://w.example/issues/reply", { method: "POST", body: form });
}

test("a reporter's reply is posted by the bot, goes back to Pending and shows as theirs in status", async () => {
  const e = env();
  const gh = fakeGitHub();
  const { number, token } = await (await handleIssues(reportRequest(META, { bundle: new File([new Uint8Array(3)], "b.zip") }), e, gh.fetcher)).json();
  const issue = gh.issues.get(number);
  issue.labels = [{ name: "Bug" }, { name: "Needs Info" }];

  assert.equal((await handleIssues(replyRequest({ number, token: "wrong", text: "hi" }), e, gh.fetcher)).status, 404);
  assert.equal((await handleIssues(replyRequest({ number, token, text: "  " }), e, gh.fetcher)).status, 400);

  const res = await handleIssues(replyRequest({ number, token, text: "It was the UV menu. @someone <b>x</b>" }, new File([new Uint8Array(4)], "bundle.zip")), e, gh.fetcher);
  assert.equal(res.status, 200);
  const sent = await res.json();
  assert.equal(sent.from, "you");
  assert.match(sent.body, /^It was the UV menu\. `@someone` &lt;b>x&lt;\/b>/);
  assert.deepEqual(issue.labels.map((l) => l.name), ["Bug", "Pending"]);
  assert.match(issue.commentList[0].body, /<!-- lila-reporter -->$/);

  const rec = await (await e.REPORTS.get(`issues/${number}.json`)).json();
  assert.deepEqual(rec.files, ["bundle.zip", "file-bundle.zip"]);

  const st = await handleIssues(new Request("https://w.example/issues/status", { method: "POST", body: JSON.stringify({ reports: [{ number, token }] }) }), e, gh.fetcher);
  const [r] = (await st.json()).reports;
  assert.equal(r.replies.length, 1);
  assert.equal(r.replies[0].from, "you");
  assert.ok(!r.replies[0].body.includes("lila-reporter"));
  assert.ok(!r.replies[0].body.startsWith("**Reporter"));

  issue.state = "closed";
  assert.equal((await handleIssues(replyRequest({ number, token, text: "still broken" }), e, gh.fetcher)).status, 409);
});

test("reply attachments never replace one already on the report", () => {
  assert.equal(replyAttachmentName("x.zip", ["bundle.zip"]), "file-x.zip");
  assert.equal(replyAttachmentName("x.zip", ["file-x.zip"]), "file-2-x.zip");
  assert.equal(replyAttachmentName("x.zip", ["file-x.zip", "file-2-x.zip"]), "file-3-x.zip");
});

test("a sender gets a set number of replies a day", async () => {
  const e = env();
  const gh = fakeGitHub();
  const { number, token } = await (await handleIssues(reportRequest(META), e, gh.fetcher)).json();
  for (let i = 0; i < MAX_REPLIES_PER_IP_DAY; ++i) assert.equal((await handleIssues(replyRequest({ number, token, text: `r${i}` }), e, gh.fetcher)).status, 200);
  assert.equal((await handleIssues(replyRequest({ number, token, text: "one more" }), e, gh.fetcher)).status, 429);
});

test("oversize attachments and missing storage are refused", async () => {
  const gh = fakeGitHub();
  const big = new File([new Uint8Array(8 * 1024 * 1024 + 1)], "s.png");
  assert.equal((await handleIssues(reportRequest(META, { screenshot: big }), env(), gh.fetcher)).status, 413);
  assert.equal((await handleIssues(reportRequest(META), { }, gh.fetcher)).status, 503);
  assert.equal(await handleIssues(new Request("https://w.example/credits.json"), env(), gh.fetcher), null);
});

test("links outside the allowlist, images, HTML and mentions don't render", () => {
  assert.equal(sanitizeText("see https://evil.example/x.exe."), "see `https://evil.example/x.exe`.");
  assert.equal(sanitizeText("see https://youtu.be/abc, ok"), "see https://youtu.be/abc, ok");
  assert.equal(sanitizeText("www.evil.example"), "`www.evil.example`");
  assert.equal(sanitizeText("[click](https://evil.example)"), "click (`https://evil.example`)");
  assert.equal(sanitizeText("[docs](https://lila-3d.github.io/docs/)"), "[docs](https://lila-3d.github.io/docs/)");
  assert.equal(sanitizeText("![x](https://github.com/a.png)"), "x (`https://github.com/a.png`)");
  assert.equal(sanitizeText("<img src=x onerror=alert(1)>"), "&lt;img src=x onerror=alert(1)>");
  assert.equal(sanitizeText("thanks @octocat, mail a@b.com"), "thanks `@octocat`, mail a@b.com");
  assert.equal(sanitizeText("`https://evil.example` stays"), "`https://evil.example` stays");
  assert.equal(sanitizeText("```\n<b>@x https://evil.example</b>\n```"), "```\n<b>@x https://evil.example</b>\n```");
  assert.equal(allowedHost("https://m.youtube.com/watch?v=1", LINK_HOSTS), "m.youtube.com");
  assert.equal(allowedHost("https://youtube.com.evil.example/", LINK_HOSTS), null);
  assert.equal(allowedHost("javascript:alert(1)", LINK_HOSTS), null);
});

test("video links must be on a video host, and crash paths lose their folders", () => {
  const video = (text) => parseReport({ ...META, sections: [...META.sections, { key: "video", heading: "Video link", text }] });
  assert.ok(video("https://evil.example/v.mp4").error);
  assert.ok(video("https://youtu.be/abc extra").error);
  assert.equal(video("https://www.youtube.com/watch?v=abc").sections[1].text, "https://www.youtube.com/watch?v=abc");
  assert.equal(stripPaths("in Qt6Widgets.dll, at f (X:\\Lila\\vcpkg\\qdockarealayout.cpp:119)"), "in Qt6Widgets.dll, at f (qdockarealayout.cpp:119)");
  const crash = parseReport({ ...META, sections: [{ key: "crash", heading: "Crash", text: "at f (C:\\Program Files\\VS\\memory:1181)" }] });
  assert.equal(crash.sections[0].text, "at f (memory:1181)");
});

test("rate-limited senders get 429", async () => {
  const e = env({ REPORT_LIMIT: { limit: async () => ({ success: false }) } });
  assert.equal((await handleIssues(reportRequest(META), e, fakeGitHub().fetcher)).status, 429);
});

const fromIp = (ip, files) => {
  const r = reportRequest(META, files);
  const headers = new Headers(r.headers);
  headers.set("CF-Connecting-IP", ip);
  return new Request(r, { headers });
};

test("a sender gets a set number of reports a day", async () => {
  const e = env();
  const gh = fakeGitHub();
  for (let i = 0; i < MAX_REPORTS_PER_IP_DAY; ++i) assert.equal((await handleIssues(fromIp("1.2.3.4"), e, gh.fetcher)).status, 200);
  assert.equal((await handleIssues(fromIp("1.2.3.4"), e, gh.fetcher)).status, 429);
  assert.equal((await handleIssues(fromIp("5.6.7.8"), e, gh.fetcher)).status, 200);
  assert.ok(![...e.REPORTS.items.keys()].some((k) => k.includes("1.2.3.4")));
});

test("past the daily byte limit a report is filed without its attachments", async () => {
  const e = env();
  const gh = fakeGitHub();
  const day = new Date().toISOString().slice(0, 10);
  await e.REPORTS.put(`limits/${day}/total.json`, JSON.stringify({ bytes: MAX_BYTES_PER_DAY - 5 }));
  const res = await handleIssues(fromIp("1.2.3.4", { bundle: new File([new Uint8Array(10)], "b.zip") }), e, gh.fetcher);
  assert.equal(res.status, 200);
  assert.equal((await res.json()).dropped, true);
  assert.match(gh.issues.get(1).body, /Attachments not kept: over the daily upload limit\./);
  assert.ok(![...e.REPORTS.items.keys()].some((k) => k.startsWith("files/")));
  const small = await handleIssues(fromIp("1.2.3.4", { bundle: new File([new Uint8Array(4)], "b.zip") }), e, gh.fetcher);
  assert.equal((await small.json()).dropped, undefined);
});

test("the App JWT is RS256 and signed by the key", async () => {
  const { privateKey, publicKey } = generateKeyPairSync("rsa", { modulusLength: 2048 });
  const pem = privateKey.export({ type: "pkcs8", format: "pem" });
  const jwt = await appJwt(123, pem, Date.parse("2026-11-04T00:00:00Z"));
  const [h, c, s] = jwt.split(".");
  const claims = JSON.parse(Buffer.from(c, "base64url").toString());
  assert.equal(claims.iss, "123");
  assert.equal(claims.exp - claims.iat, 540);
  const { createVerify } = await import("node:crypto");
  assert.ok(createVerify("RSA-SHA256").update(`${h}.${c}`).verify(publicKey, Buffer.from(s, "base64url")));
});

test("reports come only from builds of a commit on main", async () => {
  const send = async (version, e = env()) => {
    const res = await handleIssues(reportRequest({ ...META, version }), e, fakeGitHub().fetcher);
    return { status: res.status, body: await res.json() };
  };
  assert.equal(buildCommit("0.1.0+9641744ab1 (build 202610281430-9641744ab1)"), "9641744ab1");
  assert.equal(buildCommit("0.1.0+9641744ab1 (build 202610281430-9641744ab1-dirty)"), null);
  assert.equal(buildCommit("0.1.0"), null);

  assert.equal((await send("0.9.1+aaaaaaaaaa (build 1-aaaaaaaaaa)")).status, 200);
  for (const version of ["0.9.1+f0f0f0f0f0 (build 1-f0f0f0f0f0)", "0.9.1+0123456789 (build 1-0123456789)", "0.9.1+9641744ab1 (build 1-9641744ab1-dirty)", "0.9.1", undefined]) {
    const r = await send(version);
    assert.equal(r.status, 403, String(version));
    assert.equal(r.body.error, UNOFFICIAL_BUILD);
  }

  const e = env();
  assert.equal((await send(META.version, e)).status, 200);
  assert.ok(e.REPORTS.items.has("builds/9641744ab1.json"));
  const down = await handleIssues(reportRequest({ ...META, version: "0.9.1+aaaaaaaaaa (build 1-aaaaaaaaaa)" }), env(), async () => new Response("", { status: 500 }));
  assert.equal(down.status, 502);
});
