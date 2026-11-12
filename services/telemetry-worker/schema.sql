-- Credits database (D1 binding CREDITS_DB). Apply with:
--   wrangler d1 execute lila-credits --remote --file=schema.sql

-- One row per platform account: github:<login>, kofi:<sha256 of email>, steam:<steamid>, manual:<id>.
CREATE TABLE IF NOT EXISTS donors (
  id TEXT PRIMARY KEY,
  person_id TEXT NOT NULL,
  name TEXT,
  public INTEGER NOT NULL DEFAULT 0,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS payments (
  id TEXT PRIMARY KEY,
  donor_id TEXT NOT NULL,
  cents INTEGER,
  currency TEXT,
  at TEXT NOT NULL,
  reversed INTEGER NOT NULL DEFAULT 0
);

-- Linked donor accounts share one person. Display name and number are fixed here once set.
CREATE TABLE IF NOT EXISTS people (
  id TEXT PRIMARY KEY,
  name TEXT,
  hidden INTEGER NOT NULL DEFAULT 0,
  tier TEXT,
  number INTEGER,
  founder INTEGER NOT NULL DEFAULT 0
);

-- owner: person:<id>, contributor:<name> or corporate:<name>.
CREATE TABLE IF NOT EXISTS links (
  owner TEXT PRIMARY KEY,
  url TEXT NOT NULL,
  host TEXT NOT NULL,
  status TEXT NOT NULL,
  reason TEXT,
  approved_at TEXT,
  changed_at TEXT,
  checked_at TEXT,
  dead_since TEXT,
  strikes INTEGER NOT NULL DEFAULT 0,
  banned INTEGER NOT NULL DEFAULT 0
);

-- Hand-kept JSON: corporate, pastSponsors, contributors, testers, hide, allowlist, fx, steamDlc, founderOpen.
CREATE TABLE IF NOT EXISTS settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS reports (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  owner TEXT,
  name TEXT,
  url TEXT,
  reason TEXT,
  at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS alerts (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  at TEXT NOT NULL,
  message TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS published (
  key TEXT PRIMARY KEY,
  body TEXT NOT NULL,
  updated TEXT NOT NULL
);

-- Roadmap votes: voter is sha256(IP | VOTE_SALT), never the address itself.
CREATE TABLE IF NOT EXISTS roadmap_votes (
  card TEXT NOT NULL,
  voter TEXT NOT NULL,
  at TEXT NOT NULL,
  PRIMARY KEY (card, voter)
);
