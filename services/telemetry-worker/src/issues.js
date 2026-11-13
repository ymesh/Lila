// Issue reports from Lila's Help > Report Issue, filed on github.com/lila-3d/issues by the Lila Reports GitHub App.
// The text is public on the issue; the support bundle and screenshot stay private in the REPORTS R2 bucket.
// Secrets: REPORTS_APP_ID, REPORTS_APP_KEY (PKCS#8 PEM), REPORTS_INSTALLATION_ID, ADMIN_TOKEN (attachment downloads).
// Bindings: REPORTS (R2: files/ attachments, expired by a lifecycle rule; issues/ records; limits/ daily counters),
// REPORT_LIMIT (per-minute rate limit for reports and replies, optional). REPORTS_GITHUB_API points GitHub calls elsewhere for local tests.

export const REPO = "lila-3d/issues";
/** Reports are taken only from builds of a commit on this repo's main branch. */
export const SOURCE_REPO = "lila-3d/Lila";
export const UNOFFICIAL_BUILD =
  "Reports are taken from official Lila builds only, and this build isn't one. Download Lila from lila-3d.github.io and report from there.";
export const KINDS = ["Bug", "Crash", "Performance", "Request", "Question"];
export const MAX_SECTIONS = 10;
export const MAX_TITLE = 120;
export const MAX_BODY = 20000;
export const MAX_FIELD = 200;
export const MAX_BUNDLE = 25 * 1024 * 1024;
export const MAX_SCREENSHOT = 8 * 1024 * 1024;
// Each attachment's limit; together they stay under Cloudflare's 100 MB request body cap.
export const MAX_FILE = 25 * 1024 * 1024;
export const SUPPORT_EMAIL = "lila3d.app@gmail.com";
export const MAX_STATUS = 10;
/** Daily limits per sender (by hashed IP) and across everyone; past the byte limits a report is filed without attachments. */
export const MAX_REPORTS_PER_IP_DAY = 10;
export const MAX_BYTES_PER_IP_DAY = 300 * 1024 * 1024;
export const MAX_BYTES_PER_DAY = 2 * 1024 * 1024 * 1024;
export const MAX_REPLIES_PER_IP_DAY = 30;
/** Triage labels; exactly one is on an issue. scripts/issues.py and IssueReport.cpp (statusText) use the same names. */
export const TRIAGE = ["Pending", "Needs Info", "On Hold", "Solved", "Dismissed"];
/** Ends a reporter's reply posted by the bot, so status can tell it from a maintainer's. */
export const REPORTER_MARK = "<!-- lila-reporter -->";
const REPLY_HEADING = "**Reporter replied from Lila:**";
const MAINTAINERS = new Set(["OWNER", "MEMBER", "COLLABORATOR"]);
/** Triage replies are posted by the app itself (scripts/issues.py). */
export const BOT_LOGIN = "lila-reports[bot]";
const GITHUB_API = "https://api.github.com";
const api = (env) => env.REPORTS_GITHUB_API || GITHUB_API;
const USER_AGENT = "lila-reports";

const json = (body, status = 200) => new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } });

const clean = (s, max) => (typeof s === "string" ? s.replace(/\r\n/g, "\n").trim().slice(0, max) : "");

/** One line for a Markdown table cell. */
const cell = (s) => clean(s, MAX_FIELD).replace(/[\n|]/g, " ");

/** Hosts a Video link may point at. Lila's Report Issue dialog (kVideoHosts in IssueReport.cpp) checks the same list. */
export const VIDEO_HOSTS = ["youtube.com", "youtu.be", "vimeo.com", "x.com", "twitter.com", "streamable.com", "loom.com", "medal.tv"];
/** Hosts whose links stay clickable in a report; any other link shows as code, readable but not a link. */
export const LINK_HOSTS = [...VIDEO_HOSTS, "github.com", "lila-3d.github.io"];

/** The link's host if it is http(s) on one of `hosts` (or a subdomain), else null. */
export function allowedHost(url, hosts) {
  let u;
  try {
    u = new URL(/^www\./i.test(url) ? `https://${url}` : url);
  } catch {
    return null;
  }
  if (u.protocol !== "https:" && u.protocol !== "http:") return null;
  const host = u.hostname.toLowerCase().replace(/^www\./, "");
  return hosts.some((h) => host === h || host.endsWith(`.${h}`)) ? host : null;
}

const INLINE = /(`[^`\n]+`)|(!?\[[^\]\n]*\]\([^)\s]*\))|((?:\bhttps?:\/\/|\bwww\.)[^\s<>`]+)|(<)|((?<![\w`@/])@[A-Za-z0-9][A-Za-z0-9-]*)/g;

const defuse = (s) => s.replace(/</g, "&lt;").replace(/(?<![\w`@/])@([A-Za-z0-9][A-Za-z0-9-]*)/g, "`@$1`");

function sanitizeInline(s) {
  return s.replace(INLINE, (m, code, link, url, lt, mention) => {
    if (code) return code;
    if (link) {
      const [, bang, text, href] = link.match(/^(!?)\[([^\]]*)\]\(([^)]*)\)$/);
      if (!bang && allowedHost(href, LINK_HOSTS)) return `[${defuse(text)}](${href})`;
      return `${defuse(text)} (\`${href}\`)`;
    }
    if (url) {
      const [, bare, tail] = url.match(/^(.*?)([.,;:!?)\]'"]*)$/);
      return (allowedHost(bare, LINK_HOSTS) ? bare : `\`${bare}\``) + tail;
    }
    if (lt) return "&lt;";
    return `\`${mention}\``;
  });
}

/**
 * A reporter's text made safe to post as the bot: links to hosts outside LINK_HOSTS, images and raw HTML show as code
 * or text instead of rendering, and @mentions don't notify anyone. Fenced code blocks pass through unchanged.
 */
export function sanitizeText(text) {
  return text
    .split(/(```[\s\S]*?(?:```|$))/)
    .map((part, i) => (i % 2 ? part : sanitizeInline(part)))
    .join("");
}

/** A crash summary's source locations without the build machine's folders: (C:\src\x\memory:1181) becomes (memory:1181). */
export function stripPaths(text) {
  return text.replace(/\(([^()]*[\\/])([^\\/()]+)\)/g, "($2)");
}

const VIDEO_ERROR = "Use a YouTube, Vimeo, X, Streamable, Loom or Medal link for the video.";

/** The report fields from the client, or an error string. */
export function parseReport(meta) {
  if (!meta || typeof meta !== "object") return { error: "bad report" };
  const title = clean(meta.title, MAX_TITLE).replace(/\n/g, " ");
  const raw = (Array.isArray(meta.sections) ? meta.sections : [])
    .slice(0, MAX_SECTIONS)
    .map((x) => ({ key: typeof x?.key === "string" ? x.key : "", heading: clean(x?.heading, 80).replace(/\n/g, " "), text: clean(x?.text, MAX_BODY) }))
    .filter((x) => x.heading && x.text);
  if (!title) return { error: "title missing" };
  if (!raw.length) return { error: "description missing" };
  const video = raw.find((x) => x.key === "video");
  if (video && (/\s/.test(video.text) || !allowedHost(video.text, VIDEO_HOSTS))) return { error: VIDEO_ERROR };
  const sections = raw.map((x) => ({
    heading: sanitizeText(x.heading),
    text: x.key === "video" ? video.text : sanitizeText(x.key === "crash" ? stripPaths(x.text) : x.text),
  }));
  const kind = KINDS.includes(meta.kind) ? meta.kind : "Bug";
  const github = clean(meta.github, 40).replace(/^@/, "");
  if (github && !/^[A-Za-z0-9](?:[A-Za-z0-9]|-(?=[A-Za-z0-9])){0,38}$/.test(github)) return { error: "GitHub username not valid" };
  return { title, sections, kind, github, version: cell(meta.version), os: cell(meta.os), gpu: cell(meta.gpu) };
}

/** The commit a version string names ("0.1.0+9641744ab1 (build 202610281430-9641744ab1)"), or null when it has none or has uncommitted changes. */
export function buildCommit(version) {
  if (typeof version !== "string" || /-dirty\b/.test(version)) return null;
  const m = version.match(/\+([0-9a-f]{7,40})\b/);
  return m ? m[1] : null;
}

/**
 * Whether the build is of a commit on SOURCE_REPO's main: true, false, or null when GitHub can't say. A fork's own commits
 * resolve through the parent repo too, so it has to be an ancestor of main, not just a known commit. Only a yes is cached:
 * a commit not yet pushed is on main later.
 */
async function officialBuild(env, fetcher, version) {
  const sha = buildCommit(version);
  if (!sha) return false;
  const key = `builds/${sha}.json`;
  if (await readJson(env, key)) return true;
  let compare;
  try {
    compare = await github(env, fetcher, "GET", `/repos/${SOURCE_REPO}/compare/${sha}...main`);
  } catch (e) {
    return /: 404$/.test(e.message) ? false : null;
  }
  const official = compare.status === "ahead" || compare.status === "identical";
  if (official) await putJson(env, key, { at: new Date().toISOString() });
  return official;
}

/** A stored name for an extra attachment: its own name made safe, prefixed so it can't replace the bundle or screenshot. */
export function attachmentName(name) {
  const safe = String(name || "").replace(/[^A-Za-z0-9._-]/g, "_").replace(/^\.+/, "").slice(-80);
  return `file-${safe || "attachment"}`;
}

const sizeText = (bytes) => (bytes >= 1024 * 1024 ? `${(bytes / 1024 / 1024).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`);

/** The public issue body: the reporter's text, the environment, and which private attachments exist. */
export function issueBody(report, id, files, dropped = false) {
  const rows = [
    ["Version", report.version],
    ["OS", report.os],
    ["GPU", report.gpu],
    ["Reporter", report.github ? `@${report.github}` : ""],
  ].filter(([, v]) => v);
  const lines = report.sections.flatMap((x) => [`### ${x.heading}`, "", x.text, ""]);
  if (rows.length) lines.push("| | |", "|-|-|", ...rows.map(([k, v]) => `| ${k} | ${v} |`), "");
  const attached = files.map((f) => `${f.label} (${sizeText(f.size)})`);
  const note = dropped ? " Attachments not kept: over the daily upload limit." : attached.length ? ` Private attachments: ${attached.join(", ")}.` : "";
  lines.push(`Sent from Lila.${note}`);
  lines.push(`<!-- lila-report ${id} -->`);
  return lines.join("\n");
}

/** A reporter's reply as the bot posts it. */
export function replyBody(text, file, dropped = false) {
  const lines = [REPLY_HEADING, "", sanitizeText(text)];
  if (file) lines.push("", `Private attachment: ${file.label} (${sizeText(file.size)}).`);
  if (dropped) lines.push("", "Attachment not kept: over the daily upload limit.");
  lines.push(REPORTER_MARK);
  return lines.join("\n");
}

const isReporterReply = (c) => c.user?.login === BOT_LOGIN && (c.body || "").includes(REPORTER_MARK);

/** The reply as the reporter wrote it, without the heading and marker the bot adds. */
function reporterText(body) {
  return body.replace(REPORTER_MARK, "").replace(`${REPLY_HEADING}\n\n`, "").trim();
}

/** What the reporter sees in Lila: state, labels, maintainer replies and their own replies (from: "you"). */
export function reportStatus(issue, comments) {
  return {
    number: issue.number,
    url: issue.html_url,
    title: issue.title,
    state: issue.state,
    reason: issue.state_reason || null,
    labels: (issue.labels || []).map((l) => (typeof l === "string" ? l : l.name)),
    updated: issue.updated_at,
    replies: comments
      .filter((c) => MAINTAINERS.has(c.author_association) || c.user?.login === BOT_LOGIN)
      .map((c) => (isReporterReply(c) ? { at: c.created_at, body: reporterText(c.body), from: "you" } : { at: c.created_at, body: c.body })),
  };
}

const base64url = (bytes) => btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
const textBytes = (s) => new TextEncoder().encode(s);

export async function sha256(s) {
  const digest = await crypto.subtle.digest("SHA-256", textBytes(s));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function randomToken(bytes = 24) {
  return base64url(crypto.getRandomValues(new Uint8Array(bytes)));
}

/** A GitHub App JWT (RS256), valid for nine minutes. */
export async function appJwt(appId, pem, now = Date.now()) {
  const der = Uint8Array.from(atob(pem.replace(/-----[^-]+-----/g, "").replace(/\s+/g, "")), (c) => c.charCodeAt(0));
  const key = await crypto.subtle.importKey("pkcs8", der, { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" }, false, ["sign"]);
  const iat = Math.floor(now / 1000) - 60;
  const head = base64url(textBytes(JSON.stringify({ alg: "RS256", typ: "JWT" })));
  const claims = base64url(textBytes(JSON.stringify({ iat, exp: iat + 540, iss: String(appId) })));
  const sig = await crypto.subtle.sign("RSASSA-PKCS1-v1_5", key, textBytes(`${head}.${claims}`));
  return `${head}.${claims}.${base64url(sig)}`;
}

let cachedToken = null;

async function installationToken(env, fetcher) {
  if (env.REPORTS_GITHUB_TOKEN) return env.REPORTS_GITHUB_TOKEN;
  if (cachedToken && cachedToken.expires - Date.now() > 5 * 60 * 1000) return cachedToken.token;
  const jwt = await appJwt(env.REPORTS_APP_ID, env.REPORTS_APP_KEY);
  const res = await fetcher(`${api(env)}/app/installations/${env.REPORTS_INSTALLATION_ID}/access_tokens`, {
    method: "POST",
    headers: { Authorization: `Bearer ${jwt}`, Accept: "application/vnd.github+json", "User-Agent": USER_AGENT },
  });
  if (!res.ok) throw new Error(`GitHub App token: ${res.status}`);
  const body = await res.json();
  cachedToken = { token: body.token, expires: Date.parse(body.expires_at) };
  return cachedToken.token;
}

async function github(env, fetcher, method, path, body) {
  const token = await installationToken(env, fetcher);
  const res = await fetcher(`${api(env)}${path}`, {
    method,
    headers: {
      Authorization: `Bearer ${token}`,
      Accept: "application/vnd.github+json",
      "User-Agent": USER_AGENT,
      ...(body ? { "Content-Type": "application/json" } : {}),
    },
    body: body ? JSON.stringify(body) : undefined,
  });
  if (!res.ok) throw new Error(`GitHub ${method} ${path}: ${res.status}`);
  return res.json();
}

async function rateLimited(request, env) {
  if (!env.REPORT_LIMIT) return false;
  const { success } = await env.REPORT_LIMIT.limit({ key: request.headers.get("CF-Connecting-IP") || "unknown" });
  return !success;
}

/** Today's counters: this sender's (by hashed IP) and everyone's. */
async function dailyKeys(request) {
  const day = new Date().toISOString().slice(0, 10);
  const ip = request.headers.get("CF-Connecting-IP") || "unknown";
  return { senderKey: `limits/${day}/${await sha256(`${day}:${ip}`)}.json`, totalKey: `limits/${day}/total.json` };
}

async function fileReport(request, env, fetcher) {
  if (await rateLimited(request, env)) return json({ error: "Too many reports from this address. Try again in a minute." }, 429);
  let form;
  try {
    form = await request.formData();
  } catch {
    return json({ error: "bad form" }, 400);
  }
  let meta;
  try {
    meta = JSON.parse(form.get("report"));
  } catch {
    return json({ error: "bad report" }, 400);
  }
  const report = parseReport(meta);
  if (report.error) return json({ error: report.error }, 400);
  const official = await officialBuild(env, fetcher, meta.version);
  if (official === null) return json({ error: "GitHub didn't answer. Try again later." }, 502);
  if (!official) return json({ error: UNOFFICIAL_BUILD }, 403);

  const uploads = [
    { field: "bundle", name: "bundle.zip", label: "support bundle", max: MAX_BUNDLE, type: "application/zip" },
    { field: "screenshot", name: "screenshot.png", label: "screenshot", max: MAX_SCREENSHOT, type: "image/png" },
    { field: "trace", name: "trace.zip", label: "performance trace", max: MAX_FILE, type: "application/zip" },
    { field: "file", label: "file", max: MAX_FILE, type: "application/octet-stream" },
  ];
  const files = [];
  for (const u of uploads) {
    const f = form.get(u.field);
    if (!f || typeof f === "string") continue;
    if (f.size > u.max) return json({ error: `The ${u.label} is over ${sizeText(u.max)}. Email it to ${SUPPORT_EMAIL} instead.` }, 413);
    files.push({ ...u, name: u.name || attachmentName(f.name), file: f, size: f.size });
  }

  const { senderKey, totalKey } = await dailyKeys(request);
  const sender = (await readJson(env, senderKey)) || { reports: 0, bytes: 0 };
  const total = (await readJson(env, totalKey)) || { bytes: 0 };
  if (sender.reports >= MAX_REPORTS_PER_IP_DAY)
    return json({ error: `You've sent ${MAX_REPORTS_PER_IP_DAY} reports today. Try again tomorrow.` }, 429);
  const size = files.reduce((n, f) => n + f.size, 0);
  const dropped = size > 0 && (sender.bytes + size > MAX_BYTES_PER_IP_DAY || total.bytes + size > MAX_BYTES_PER_DAY);
  if (dropped) files.length = 0;

  const id = crypto.randomUUID();
  for (const f of files) await env.REPORTS.put(`files/${id}/${f.name}`, f.file.stream(), { httpMetadata: { contentType: f.type } });

  const labels = [report.kind, "Pending"];
  let issue;
  try {
    issue = await github(env, fetcher, "POST", `/repos/${REPO}/issues`, { title: report.title, body: issueBody(report, id, files, dropped), labels });
  } catch {
    for (const f of files) await env.REPORTS.delete(`files/${id}/${f.name}`);
    return json({ error: "GitHub didn't take the report. Try again later." }, 502);
  }
  const kept = dropped ? 0 : size;
  await putJson(env, senderKey, { ...sender, reports: sender.reports + 1, bytes: sender.bytes + kept });
  if (kept) await putJson(env, totalKey, { bytes: total.bytes + kept });
  const token = randomToken();
  await putJson(env, `issues/${issue.number}.json`, { id, tokenHash: await sha256(token), files: files.map((f) => f.name), at: new Date().toISOString() });
  return json({ number: issue.number, url: issue.html_url, token, ...(dropped ? { dropped: true } : {}) });
}

async function readJson(env, key) {
  const obj = await env.REPORTS.get(key);
  return obj ? obj.json() : null;
}

function putJson(env, key, value) {
  return env.REPORTS.put(key, JSON.stringify(value), { httpMetadata: { contentType: "application/json" } });
}

async function record(env, number) {
  if (!Number.isInteger(number) || number <= 0) return null;
  const obj = await env.REPORTS.get(`issues/${number}.json`);
  return obj ? obj.json() : null;
}

async function status(request, env, fetcher) {
  let reports;
  try {
    reports = (await request.json()).reports;
  } catch {
    return json({ error: "bad json" }, 400);
  }
  if (!Array.isArray(reports) || reports.length > MAX_STATUS) return json({ error: `send 1 to ${MAX_STATUS} reports` }, 400);
  const out = [];
  for (const r of reports) {
    if (!(await authorized(env, r?.number, r?.token))) {
      out.push({ number: r?.number ?? null, error: "unknown report" });
      continue;
    }
    try {
      const issue = await github(env, fetcher, "GET", `/repos/${REPO}/issues/${r.number}`);
      const comments = issue.comments > 0 ? await github(env, fetcher, "GET", `/repos/${REPO}/issues/${r.number}/comments?per_page=100`) : [];
      out.push(reportStatus(issue, comments));
    } catch {
      out.push({ number: r.number, error: "GitHub unavailable" });
    }
  }
  return json({ reports: out });
}

/** The report's record if the token matches it, else null. */
async function authorized(env, number, token) {
  const rec = await record(env, number);
  return rec && typeof token === "string" && (await sha256(token)) === rec.tokenHash ? rec : null;
}

/** A stored name for a reply's attachment that doesn't replace one already on the report. */
export function replyAttachmentName(name, taken) {
  const base = attachmentName(name);
  if (!taken.includes(base)) return base;
  for (let i = 2; ; ++i) if (!taken.includes(`file-${i}-${base.slice(5)}`)) return `file-${i}-${base.slice(5)}`;
}

/** A reporter answers on their report: posted by the bot, which puts the issue back to Pending. */
async function postReply(request, env, fetcher) {
  if (await rateLimited(request, env)) return json({ error: "Too many replies from this address. Try again in a minute." }, 429);
  let form, meta;
  try {
    form = await request.formData();
    meta = JSON.parse(form.get("reply"));
  } catch {
    return json({ error: "bad reply" }, 400);
  }
  const text = clean(meta?.text, MAX_BODY);
  if (!text) return json({ error: "reply missing" }, 400);
  const rec = await authorized(env, meta.number, meta.token);
  if (!rec) return json({ error: "unknown report" }, 404);

  const f = form.get("file");
  const file = f && typeof f !== "string" ? { file: f, size: f.size, label: attachmentName(f.name).slice(5), name: replyAttachmentName(f.name, rec.files) } : null;
  if (file && file.size > MAX_FILE) return json({ error: `The file is over ${sizeText(MAX_FILE)}. Email it to ${SUPPORT_EMAIL} instead.` }, 413);

  const { senderKey, totalKey } = await dailyKeys(request);
  const sender = (await readJson(env, senderKey)) || { reports: 0, bytes: 0 };
  const total = (await readJson(env, totalKey)) || { bytes: 0 };
  if ((sender.replies || 0) >= MAX_REPLIES_PER_IP_DAY) return json({ error: `You've sent ${MAX_REPLIES_PER_IP_DAY} replies today. Try again tomorrow.` }, 429);
  const dropped = !!file && (sender.bytes + file.size > MAX_BYTES_PER_IP_DAY || total.bytes + file.size > MAX_BYTES_PER_DAY);
  const kept = file && !dropped ? file : null;

  let issue;
  try {
    issue = await github(env, fetcher, "GET", `/repos/${REPO}/issues/${meta.number}`);
  } catch {
    return json({ error: "GitHub unavailable. Try again later." }, 502);
  }
  if (issue.state === "closed") return json({ error: "This report is closed. Send a new one from Help > Report Issue." }, 409);

  if (kept) await env.REPORTS.put(`files/${rec.id}/${kept.name}`, kept.file.stream(), { httpMetadata: { contentType: "application/octet-stream" } });
  let posted;
  try {
    posted = await github(env, fetcher, "POST", `/repos/${REPO}/issues/${meta.number}/comments`, { body: replyBody(text, kept, dropped) });
  } catch {
    if (kept) await env.REPORTS.delete(`files/${rec.id}/${kept.name}`);
    return json({ error: "GitHub didn't take the reply. Try again later." }, 502);
  }
  const labels = (issue.labels || []).map((l) => (typeof l === "string" ? l : l.name)).filter((l) => !TRIAGE.includes(l));
  try {
    await github(env, fetcher, "PUT", `/repos/${REPO}/issues/${meta.number}/labels`, { labels: [...labels, "Pending"] });
  } catch {
    // The reply is posted; triage still sees it as the newest comment.
  }
  await putJson(env, senderKey, { ...sender, replies: (sender.replies || 0) + 1, bytes: sender.bytes + (kept ? kept.size : 0) });
  if (kept) {
    await putJson(env, totalKey, { bytes: total.bytes + kept.size });
    await putJson(env, `issues/${meta.number}.json`, { ...rec, files: [...rec.files, kept.name] });
  }
  return json({ at: posted.created_at, body: reporterText(posted.body), from: "you", ...(dropped ? { dropped: true } : {}) });
}

async function attachment(request, env, url) {
  if (!env.ADMIN_TOKEN || request.headers.get("Authorization") !== `Bearer ${env.ADMIN_TOKEN}`) return json({ error: "unauthorized" }, 401);
  const rec = await record(env, Number(url.searchParams.get("number")));
  const name = url.searchParams.get("name");
  if (!rec) return json({ error: "unknown report" }, 404);
  if (!name) return json({ files: rec.files });
  if (!rec.files.includes(name)) return json({ error: "no such file" }, 404);
  const obj = await env.REPORTS.get(`files/${rec.id}/${name}`);
  if (!obj) return json({ error: "file gone (attachments expire after 90 days)" }, 404);
  return new Response(obj.body, { headers: { "Content-Type": obj.httpMetadata?.contentType || "application/octet-stream" } });
}

/** Routes under /issues/, or null for other paths. */
export async function handleIssues(request, env, fetcher = fetch) {
  const url = new URL(request.url);
  if (!url.pathname.startsWith("/issues/")) return null;
  if (!env.REPORTS) return json({ error: "reports not configured" }, 503);
  if (url.pathname === "/issues/new" && request.method === "POST") return fileReport(request, env, fetcher);
  if (url.pathname === "/issues/status" && request.method === "POST") return status(request, env, fetcher);
  if (url.pathname === "/issues/reply" && request.method === "POST") return postReply(request, env, fetcher);
  if (url.pathname === "/issues/file" && request.method === "GET") return attachment(request, env, url);
  return json({ error: "not found" }, 404);
}
