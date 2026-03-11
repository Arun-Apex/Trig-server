import express from "express";
import { MongoClient } from "mongodb";
import path from "path";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const app = express();
app.set("trust proxy", true);
app.use(express.json({ limit: "256kb" }));
app.use(express.static(path.join(__dirname, "public")));
app.get("/", (req, res) => res.redirect("/admin.html"));

const PORT = parseInt(process.env.PORT || "8082", 10);
const MONGO_URL = process.env.MONGO_URL || "mongodb://mongodb:27017/trigdb";
const DB_NAME = process.env.DB_NAME || "trigdb";
const AUTH_TOKEN = process.env.AUTH_TOKEN || "";
const REQUIRE_AUTH = (process.env.REQUIRE_AUTH || "1") === "1";
const ONLINE_WINDOW_SEC = parseInt(process.env.ONLINE_WINDOW_SEC || "180", 10);
const OFFLINE_SWEEP_SEC = parseInt(process.env.OFFLINE_SWEEP_SEC || "30", 10);

let mongoClient;
let db;
let colDevices;
let colConfigs;
let colLogs;

function nowIso() {
  return new Date().toISOString();
}

function clampInt(v, min, max, fallback) {
  const n = parseInt(v, 10);
  if (Number.isNaN(n)) return fallback;
  return Math.max(min, Math.min(max, n));
}

function boolOrUndefined(v) {
  if (v === undefined) return undefined;
  return !!v;
}

function getClientIp(req) {
  const xf = req.headers["x-forwarded-for"];
  if (xf && typeof xf === "string") return xf.split(",")[0].trim();
  const xr = req.headers["x-real-ip"];
  if (xr && typeof xr === "string") return xr.trim();
  return (req.ip || "").replace("::ffff:", "");
}

function computeOnline(lastSeenAt) {
  if (!lastSeenAt) return false;
  const ts = new Date(lastSeenAt).getTime();
  if (Number.isNaN(ts)) return false;
  return (Date.now() - ts) <= ONLINE_WINDOW_SEC * 1000;
}

function requireToken(req, res, next) {
  if (!REQUIRE_AUTH) return next();
  if (!AUTH_TOKEN) {
    return res.status(500).json({ ok: false, error: "AUTH_TOKEN not set on server" });
  }
  const token = req.header("X-Auth-Token");
  if (!token || token !== AUTH_TOKEN) {
    return res.status(401).json({ ok: false, error: "Unauthorized" });
  }
  next();
}

function isHttpUrl(v) {
  return typeof v === "string" && /^http:\/\//i.test(v.trim());
}

function sanitizeString(v, max = 512) {
  return String(v || "").trim().slice(0, max);
}

function sanitizeEvents(events) {
  if (!Array.isArray(events)) return [];
  return events
    .map((row) => ({
      key: sanitizeString(row?.key, 32).toUpperCase(),
      enabled: row?.enabled === undefined ? !!row?.en : !!row?.enabled,
      playlist: sanitizeString(row?.playlist ?? row?.pl, 128),
      minRank: clampInt(row?.minRank ?? row?.min, 0, 4, 0),
    }))
    .filter((row) => row.key);
}

function normalizeDesiredConfig(desired) {
  if (!desired || typeof desired !== "object" || Array.isArray(desired)) {
    return { ok: false, error: "desired object required" };
  }

  const out = {};

  if (desired.playerIp !== undefined) {
    const playerIp = sanitizeString(desired.playerIp, 64);
    if (!playerIp) return { ok: false, error: "playerIp cannot be empty" };
    out.playerIp = playerIp;
  }
  if (desired.basicAuth !== undefined) out.basicAuth = sanitizeString(desired.basicAuth, 256);
  if (desired.normalPlaylist !== undefined) out.normalPlaylist = sanitizeString(desired.normalPlaylist, 128);
  if (desired.capUrl !== undefined) {
    const capUrl = sanitizeString(desired.capUrl, 512);
    if (!isHttpUrl(capUrl)) return { ok: false, error: "capUrl must start with http://" };
    out.capUrl = capUrl;
  }
  if (desired.deviceIso !== undefined) out.deviceIso = sanitizeString(desired.deviceIso, 64).toUpperCase();

  if (desired.pollSec !== undefined) out.pollSec = clampInt(desired.pollSec, 5, 3600, 30);
  if (desired.cycles !== undefined) out.cycles = clampInt(desired.cycles, 1, 10, 1);
  if (desired.eventPlaySec !== undefined) out.eventPlaySec = clampInt(desired.eventPlaySec, 5, 36000, 60);
  if (desired.eventPlayMode !== undefined) out.eventPlayMode = clampInt(desired.eventPlayMode, 0, 1, 0);
  if (desired.normalHoldSec !== undefined) out.normalHoldSec = clampInt(desired.normalHoldSec, 5, 36000, 30);

  if (desired.useIdentifier !== undefined) out.useIdentifier = !!desired.useIdentifier;
  if (desired.allowStaleTrigger !== undefined) out.allowStaleTrigger = !!desired.allowStaleTrigger;

  if (desired.trigUrl !== undefined) {
    const trigUrl = sanitizeString(desired.trigUrl, 512);
    if (!isHttpUrl(trigUrl)) return { ok: false, error: "trigUrl must start with http://" };
    out.trigUrl = trigUrl;
  }
  if (desired.trigToken !== undefined) out.trigToken = sanitizeString(desired.trigToken, 256);
  if (desired.hbSec !== undefined) out.hbSec = clampInt(desired.hbSec, 10, 3600, 30);

  if (desired.events !== undefined) out.events = sanitizeEvents(desired.events);

  return { ok: true, desired: out };
}

async function initMongo() {
  mongoClient = new MongoClient(MONGO_URL, { ignoreUndefined: true });
  await mongoClient.connect();
  db = mongoClient.db(DB_NAME);
  colDevices = db.collection("devices");
  colConfigs = db.collection("device_configs");
  colLogs = db.collection("device_logs");

  await colDevices.createIndex({ deviceId: 1 }, { unique: true });
  await colDevices.createIndex({ lastSeenAt: -1 });
  await colDevices.createIndex({ deviceIso: 1 }, { sparse: true });
  await colConfigs.createIndex({ deviceId: 1 }, { unique: true });
  await colLogs.createIndex({ deviceId: 1, ts: -1 });

  console.log("Mongo connected:", MONGO_URL, "DB:", DB_NAME);
}

app.get("/health", async (req, res) => {
  let mongoOk = false;
  try {
    await db.command({ ping: 1 });
    mongoOk = true;
  } catch {
    mongoOk = false;
  }
  res.json({ ok: true, time: nowIso(), mongoOk, onlineWindowSec: ONLINE_WINDOW_SEC });
});

app.get("/api/summary", requireToken, async (req, res) => {
  const total = await colDevices.countDocuments({});
  const cutoffIso = new Date(Date.now() - ONLINE_WINDOW_SEC * 1000).toISOString();
  const online = await colDevices.countDocuments({ lastSeenAt: { $gte: cutoffIso } });
  res.json({
    ok: true,
    serverTime: nowIso(),
    totalDevices: total,
    onlineDevices: online,
    offlineDevices: Math.max(0, total - online),
    onlineWindowSec: ONLINE_WINDOW_SEC,
  });
});

app.post("/api/heartbeat", requireToken, async (req, res) => {
  const now = new Date();
  const body = req.body || {};
  const deviceId = sanitizeString(body.deviceId, 128);
  if (!deviceId) return res.status(400).json({ ok: false, error: "deviceId required" });

  const clientIp = getClientIp(req);
  const ua = sanitizeString(req.headers["user-agent"], 256);
  const appliedV = body.appliedConfigVersion === undefined ? undefined : clampInt(body.appliedConfigVersion, 0, 1_000_000, 0);

  let configSnapshot;
  if (body.config && typeof body.config === "object" && !Array.isArray(body.config)) {
    try {
      const raw = JSON.stringify(body.config);
      if (raw.length <= 25000) configSnapshot = JSON.parse(raw);
    } catch {
      configSnapshot = undefined;
    }
  }

  const update = {
    deviceId,
    deviceIso: sanitizeString(body.deviceIso || body.deviceAreaCode, 64).toUpperCase() || undefined,
    ethIp: sanitizeString(body.ethIp, 64) || undefined,
    ethLink: boolOrUndefined(body.ethLink),
    playerIp: sanitizeString(body.playerIp, 64) || undefined,
    playerOk: boolOrUndefined(body.playerOk),
    fwVersion: sanitizeString(body.fwVersion, 64) || undefined,
    appliedConfigVersion: appliedV,
    configSnapshot,
    configSnapshotAt: configSnapshot ? now.toISOString() : undefined,
    lastAlertId: sanitizeString(body.lastAlertId, 256) || undefined,
    lastEvent: sanitizeString(body.lastEvent, 128) || undefined,
    lastSeverity: sanitizeString(body.lastSeverity, 64) || undefined,
    lastConfigApplyOk: boolOrUndefined(body.lastConfigApplyOk),
    lastConfigApplyError: sanitizeString(body.lastConfigApplyError, 512) || undefined,
    lastConfigAppliedAt: sanitizeString(body.lastConfigAppliedAt, 64) || undefined,
    lastSeenAt: nowIso(),
    lastSeenIp: clientIp || undefined,
    lastUserAgent: ua || undefined,
    online: true,
  };

  await colDevices.updateOne(
    { deviceId },
    { $set: update, $setOnInsert: { createdAt: nowIso() } },
    { upsert: true },
  );

  const cfg = await colConfigs.findOne({ deviceId });
  let configResponse = { update: false };
  if (cfg && typeof cfg.version === "number" && cfg.version > (appliedV ?? 0) && cfg.desired) {
    configResponse = { update: true, version: cfg.version, desired: cfg.desired };
  }

  res.json({ ok: true, serverTime: nowIso(), deviceId, online: true, ...configResponse });
});

app.get("/api/config/:deviceId", requireToken, async (req, res) => {
  const deviceId = sanitizeString(req.params.deviceId, 128);
  if (!deviceId) return res.status(400).json({ ok: false, error: "deviceId required" });

  const currentVersion = clampInt(req.query.currentVersion, 0, 1_000_000, 0);
  const cfg = await colConfigs.findOne({ deviceId });
  if (!cfg) return res.json({ ok: true, update: false, reason: "no config record" });
  if (typeof cfg.version !== "number") return res.json({ ok: true, update: false, reason: "invalid config version" });
  if (cfg.version <= currentVersion) return res.json({ ok: true, update: false, version: cfg.version });
  return res.json({ ok: true, update: true, version: cfg.version, desired: cfg.desired || {} });
});

app.get("/api/config", requireToken, async (req, res) => {
  const deviceId = sanitizeString(req.query.deviceId, 128);
  if (!deviceId) return res.status(400).json({ ok: false, error: "deviceId required" });

  const currentVersion = clampInt(req.query.currentVersion, 0, 1_000_000, 0);
  const cfg = await colConfigs.findOne({ deviceId });
  if (!cfg) return res.json({ ok: true, update: false, reason: "no config record" });
  if (typeof cfg.version !== "number") return res.json({ ok: true, update: false, reason: "invalid config version" });
  if (cfg.version <= currentVersion) return res.json({ ok: true, update: false, version: cfg.version });
  return res.json({ ok: true, update: true, version: cfg.version, desired: cfg.desired || {} });
});

app.post("/api/config/:deviceId", requireToken, async (req, res) => {
  const deviceId = sanitizeString(req.params.deviceId, 128);
  if (!deviceId) return res.status(400).json({ ok: false, error: "deviceId required" });

  const normalized = normalizeDesiredConfig(req.body?.desired);
  if (!normalized.ok) return res.status(400).json({ ok: false, error: normalized.error });

  const existing = await colConfigs.findOne({ deviceId });
  const nextVersion = Number.isInteger(existing?.version) ? existing.version + 1 : 1;

  await colConfigs.updateOne(
    { deviceId },
    {
      $set: {
        deviceId,
        version: nextVersion,
        desired: normalized.desired,
        updatedAt: nowIso(),
      },
      $setOnInsert: { createdAt: nowIso() },
    },
    { upsert: true },
  );

  res.json({ ok: true, deviceId, version: nextVersion, desired: normalized.desired });
});

app.get("/api/devices", requireToken, async (req, res) => {
  const limit = clampInt(req.query.limit, 1, 2000, 200);
  const deviceIso = sanitizeString(req.query.deviceIso || req.query.areaCode, 64).toUpperCase();
  const onlineFilter = sanitizeString(req.query.online, 8);

  const q = {};
  if (deviceIso) q.deviceIso = deviceIso;

  const docs = await colDevices.find(q).sort({ lastSeenAt: -1 }).limit(limit).toArray();
  let rows = docs.map((d) => ({
    deviceId: d.deviceId,
    deviceIso: d.deviceIso || null,
    ethIp: d.ethIp || null,
    ethLink: d.ethLink === undefined ? null : !!d.ethLink,
    playerIp: d.playerIp || null,
    playerOk: d.playerOk === undefined ? null : !!d.playerOk,
    fwVersion: d.fwVersion || null,
    appliedConfigVersion: d.appliedConfigVersion ?? null,
    lastAlertId: d.lastAlertId || null,
    lastEvent: d.lastEvent || null,
    lastSeverity: d.lastSeverity || null,
    lastConfigApplyOk: d.lastConfigApplyOk ?? null,
    lastConfigApplyError: d.lastConfigApplyError || null,
    lastConfigAppliedAt: d.lastConfigAppliedAt || null,
    lastSeenIp: d.lastSeenIp || null,
    lastSeenAt: d.lastSeenAt || null,
    online: computeOnline(d.lastSeenAt),
  }));

  if (onlineFilter === "1") rows = rows.filter((r) => r.online);
  if (onlineFilter === "0") rows = rows.filter((r) => !r.online);

  res.json({ ok: true, count: rows.length, serverTime: nowIso(), onlineWindowSec: ONLINE_WINDOW_SEC, devices: rows });
});

app.get("/api/device/:deviceId", requireToken, async (req, res) => {
  const deviceId = sanitizeString(req.params.deviceId, 128);
  if (!deviceId) return res.status(400).json({ ok: false, error: "deviceId required" });

  const dev = await colDevices.findOne({ deviceId });
  const cfg = await colConfigs.findOne({ deviceId });
  if (!dev) return res.status(404).json({ ok: false, error: "not found" });

  res.json({
    ok: true,
    device: { ...dev, online: computeOnline(dev.lastSeenAt) },
    desiredConfig: cfg ? { version: cfg.version, desired: cfg.desired, updatedAt: cfg.updatedAt } : null,
  });
});

app.post("/api/device/log", requireToken, async (req, res) => {
  const deviceId = sanitizeString(req.body?.deviceId, 128);
  const msg = sanitizeString(req.body?.msg, 2000);
  const level = sanitizeString(req.body?.level || "INFO", 16).toUpperCase();
  if (!deviceId || !msg) return res.status(400).json({ ok: false, error: "deviceId and msg required" });

  await colLogs.insertOne({
    deviceId,
    level,
    msg,
    ts: new Date().toISOString(),
  });

  res.json({ ok: true });
});

app.get("/api/device/logs", requireToken, async (req, res) => {
  const deviceId = sanitizeString(req.query.deviceId, 128);
  if (!deviceId) return res.status(400).json({ ok: false, error: "deviceId required" });
  const limit = clampInt(req.query.limit, 1, 1000, 200);
  const rows = await colLogs.find({ deviceId }).sort({ ts: -1 }).limit(limit).toArray();
  res.json({ ok: true, rows: rows.reverse() });
});

function startOfflineSweep() {
  setInterval(async () => {
    try {
      const cutoffIso = new Date(Date.now() - ONLINE_WINDOW_SEC * 1000).toISOString();
      await colDevices.updateMany(
        { lastSeenAt: { $lt: cutoffIso }, online: true },
        { $set: { online: false, lastOfflineAt: nowIso() } },
      );
    } catch (e) {
      console.error("offline sweep error:", e?.message || e);
    }
  }, OFFLINE_SWEEP_SEC * 1000);
}

await initMongo();
startOfflineSweep();

app.listen(PORT, "0.0.0.0", () => {
  console.log(`TRIG API listening on http://0.0.0.0:${PORT}`);
  console.log(`MONGO_URL=${MONGO_URL}`);
  console.log(`ONLINE_WINDOW_SEC=${ONLINE_WINDOW_SEC}, REQUIRE_AUTH=${REQUIRE_AUTH ? "1" : "0"}`);
});
