import express from "express";
import { MongoClient } from "mongodb";
import path from "path";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const app = express();

// Serve static admin UI (this must be AFTER __dirname exists)
app.use(express.static(path.join(__dirname, "public")));

// optional: make "/" go to admin
app.get("/", (req, res) => res.redirect("/admin.html"));


// If you put this behind nginx / cloudflare later, this allows req.ip to work correctly
app.set("trust proxy", true);

// ---- Env ----
const PORT = parseInt(process.env.PORT || "8082", 10);

// IMPORTANT: in Docker, use service name "mongodb", not localhost
const MONGO_URL = process.env.MONGO_URL || "mongodb://mongodb:27017/trigdb";
const DB_NAME = process.env.DB_NAME || "trigdb";

// Simple shared token (later we can move to per-device token)
const AUTH_TOKEN = process.env.AUTH_TOKEN || "";
const REQUIRE_AUTH = (process.env.REQUIRE_AUTH || "1") === "1";

// Device considered online if seen within this many seconds
const ONLINE_WINDOW_SEC = parseInt(process.env.ONLINE_WINDOW_SEC || "180", 10); // 3 minutes default

// Background offline marking interval
const OFFLINE_SWEEP_SEC = parseInt(process.env.OFFLINE_SWEEP_SEC || "30", 10);

// Body size limit
app.use(express.json({ limit: "256kb" }));

// ---- Auth middleware ----
function requireToken(req, res, next) {
  if (!REQUIRE_AUTH) return next();
  if (!AUTH_TOKEN) return res.status(500).json({ ok: false, error: "AUTH_TOKEN not set on server" });

  const token = req.header("X-Auth-Token");
  if (!token || token !== AUTH_TOKEN) {
    return res.status(401).json({ ok: false, error: "Unauthorized" });
  }
  next();
}

// ---- Mongo setup ----
let mongoClient;
let db;
let colDevices;
let colConfigs;

async function initMongo() {
  mongoClient = new MongoClient(MONGO_URL, { ignoreUndefined: true });
  await mongoClient.connect();
  db = mongoClient.db(DB_NAME);

  colDevices = db.collection("devices");
  colConfigs = db.collection("device_configs");

  // Indexes
  await colDevices.createIndex({ deviceId: 1 }, { unique: true });
  await colDevices.createIndex({ lastSeenAt: -1 });
  await colDevices.createIndex({ deviceArea: 1 }, { sparse: true });
  await colDevices.createIndex({ deviceAreaCode: 1 }, { sparse: true });

  await colConfigs.createIndex({ deviceId: 1 }, { unique: true });

  console.log("Mongo connected:", MONGO_URL, "DB:", DB_NAME);
}

// ---- Helpers ----
function nowIso() {
  return new Date().toISOString();
}

function clampInt(v, min, max, fallback) {
  const n = parseInt(v, 10);
  if (Number.isNaN(n)) return fallback;
  return Math.max(min, Math.min(max, n));
}

function computeOnline(lastSeenAt) {
  if (!lastSeenAt) return false;
  const ts = new Date(lastSeenAt).getTime();
  if (Number.isNaN(ts)) return false;
  return (Date.now() - ts) <= ONLINE_WINDOW_SEC * 1000;
}

// Try to capture real client IP even behind proxy/CDN
function getClientIp(req) {
  const xf = req.headers["x-forwarded-for"];
  if (xf && typeof xf === "string") return xf.split(",")[0].trim();
  const xr = req.headers["x-real-ip"];
  if (xr && typeof xr === "string") return xr.trim();
  return (req.ip || "").replace("::ffff:", "");
}

// ---- Routes ----
app.get("/health", async (req, res) => {
  let mongoOk = false;
  try {
    await db.command({ ping: 1 });
    mongoOk = true;
  } catch {
    mongoOk = false;
  }

  res.json({
    ok: true,
    time: nowIso(),
    mongoOk,
    onlineWindowSec: ONLINE_WINDOW_SEC
  });
});

// Quick overview for dashboards
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
    onlineWindowSec: ONLINE_WINDOW_SEC
  });
});

/**
 * ESP32 Heartbeat
 * POST /api/heartbeat
 * Headers: X-Auth-Token: <token>
 *
 * Body example:
 * {
 *   "deviceId": "trig-a4cf12bc9034",
 *   "deviceArea": "ภาคกลาง พระนครศรีอยุธยา",
 *   "deviceAreaCode": "TH-14",
 *   "deviceAreaTags": ["พระนครศรีอยุธยา","ภาคกลาง"],
 *   "ethIp": "192.168.1.250",
 *   "ethLink": true,
 *   "playerIp": "192.168.1.166",
 *   "playerOk": true,
 *   "fwVersion": "1.0.3",
 *   "appliedConfigVersion": 12,
 *   "lastAlertId": "NDWC20260129090948_2",
 *   "lastEvent": "Tsunami",
 *   "lastSeverity": "Moderate"
 * }
 *
 * Response may include config update if newer version exists.
 */
app.post("/api/heartbeat", requireToken, async (req, res) => {
  const now = new Date();
  const body = req.body || {};

  const deviceId = String(body.deviceId || "").trim();
  if (!deviceId) return res.status(400).json({ ok: false, error: "deviceId required" });

  const clientIp = getClientIp(req);
  const ua = String(req.headers["user-agent"] || "").trim();

  const deviceArea = String(body.deviceArea || "").trim();
  const deviceAreaCode = String(body.deviceAreaCode || "").trim();

  const tags = Array.isArray(body.deviceAreaTags)
    ? body.deviceAreaTags.map(x => String(x || "").trim()).filter(Boolean).slice(0, 20)
    : undefined;

  const appliedV = body.appliedConfigVersion === undefined
    ? undefined
    : clampInt(body.appliedConfigVersion, 0, 1_000_000, 0);

  // Optional: device can include a full config snapshot under body.config.
  // We store it so the admin UI can pull current device settings.
  let configSnapshot = undefined;
  if (body && body.config && typeof body.config === 'object' && !Array.isArray(body.config)) {
    try {
      const raw = JSON.stringify(body.config);
      // Prevent accidental giant payloads (polygons etc.) from being stored.
      if (raw.length <= 20000) {
        configSnapshot = JSON.parse(raw);
      }
    } catch {
      configSnapshot = undefined;
    }
  }

  const update = {
    deviceId,

    // Device self-reported area / matching keys
    deviceArea: deviceArea || undefined,
    deviceAreaCode: deviceAreaCode || undefined,
    deviceAreaTags: tags && tags.length ? tags : undefined,

    // Ethernet status from ESP32
    ethIp: String(body.ethIp || "").trim() || undefined,
    ethLink: body.ethLink === undefined ? undefined : !!body.ethLink,

    // Player connectivity test from ESP32
    playerIp: String(body.playerIp || "").trim() || undefined,
    playerOk: body.playerOk === undefined ? undefined : !!body.playerOk,

    // Firmware / status
    fwVersion: String(body.fwVersion || "").trim() || undefined,
    appliedConfigVersion: appliedV,

    // Latest config snapshot reported by the device (if provided)
    configSnapshot: configSnapshot,
    configSnapshotAt: configSnapshot ? now.toISOString() : undefined,

    lastAlertId: String(body.lastAlertId || "").trim() || undefined,
    lastEvent: String(body.lastEvent || "").trim() || undefined,
    lastSeverity: String(body.lastSeverity || "").trim() || undefined,

    // Network info
    lastSeenAt: nowIso(),
    lastSeenIp: clientIp || undefined,
    lastUserAgent: ua || undefined
  };

  // Optional stored boolean for dashboards (we still compute dynamically too)
  update.online = true;

  await colDevices.updateOne(
    { deviceId },
    {
      $set: update,
      $setOnInsert: {
        createdAt: nowIso()
      }
    },
    { upsert: true }
  );

  // If config update exists, respond with it (ESP32 can apply immediately)
  const currentVersion = clampInt(body.appliedConfigVersion, 0, 1_000_000, 0);
  const cfg = await colConfigs.findOne({ deviceId });

  let configResponse = { update: false };
  if (cfg && typeof cfg.version === "number" && cfg.version > currentVersion && cfg.desired) {
    configResponse = { update: true, version: cfg.version, desired: cfg.desired };
  }

  res.json({
    ok: true,
    serverTime: nowIso(),
    deviceId,
    online: true,
    ...configResponse
  });
});

/**
 * ESP32 pulls config updates (versioned) - Option A
 * GET /api/config/:deviceId?currentVersion=12
 */
app.get("/api/config/:deviceId", requireToken, async (req, res) => {
  const deviceId = String(req.params.deviceId || "").trim();
  if (!deviceId) return res.status(400).json({ ok: false, error: "deviceId required" });

  const currentVersion = clampInt(req.query.currentVersion, 0, 1_000_000, 0);
  const cfg = await colConfigs.findOne({ deviceId });

  if (!cfg) return res.json({ ok: true, update: false, reason: "no config record" });
  if (typeof cfg.version !== "number") return res.json({ ok: true, update: false, reason: "invalid config version" });
  if (cfg.version <= currentVersion) return res.json({ ok: true, update: false, version: cfg.version });

  return res.json({ ok: true, update: true, version: cfg.version, desired: cfg.desired || {} });
});

/**
 * ESP32 pulls config updates - Option B (no path param)
 * GET /api/config?deviceId=xxx&currentVersion=12
 */
app.get("/api/config", requireToken, async (req, res) => {
  const deviceId = String(req.query.deviceId || "").trim();
  if (!deviceId) return res.status(400).json({ ok: false, error: "deviceId required" });

  const currentVersion = clampInt(req.query.currentVersion, 0, 1_000_000, 0);
  const cfg = await colConfigs.findOne({ deviceId });

  if (!cfg) return res.json({ ok: true, update: false, reason: "no config record" });
  if (typeof cfg.version !== "number") return res.json({ ok: true, update: false, reason: "invalid config version" });
  if (cfg.version <= currentVersion) return res.json({ ok: true, update: false, version: cfg.version });

  return res.json({ ok: true, update: true, version: cfg.version, desired: cfg.desired || {} });
});

/**
 * Admin sets desired config for a device (increments version)
 * POST /api/config/:deviceId
 *
 * Supports your new requirement:
 * - countryCapUrl: national alerts
 * - regionCapUrl : region/district alerts (matched with areaDesc against deviceArea / tags / codes)
 *
 * Body:
 * {
 *   "desired": {
 *     "playerIp": "192.168.1.166",
 *     "normalPlaylist": "Station-CC02",
 *     "pollSec": 30,
 *     "cycles": 1,
 *     "eventPlaySec": 60,
 *     "normalHoldSec": 30,
 *
 *     "countryCapUrl": "http://x.x.x.x:8081/country/latest.json",
 *     "regionCapUrl":  "http://x.x.x.x:8081/region/latest.json",
 *
 *     "deviceArea": "ภาคกลาง พระนครศรีอยุธยา",
 *     "deviceAreaCode": "TH-14",
 *     "deviceAreaTags": ["พระนครศรีอยุธยา","ภาคกลาง"]
 *   }
 * }
 */
app.post("/api/config/:deviceId", requireToken, async (req, res) => {
  const deviceId = String(req.params.deviceId || "").trim();
  if (!deviceId) return res.status(400).json({ ok: false, error: "deviceId required" });

  const desired = req.body?.desired;
  if (!desired || typeof desired !== "object") {
    return res.status(400).json({ ok: false, error: "desired object required" });
  }

  const existing = await colConfigs.findOne({ deviceId });
  const nextVersion = (existing?.version && Number.isInteger(existing.version)) ? existing.version + 1 : 1;

  await colConfigs.updateOne(
    { deviceId },
    {
      $set: {
        deviceId,
        version: nextVersion,
        desired,
        updatedAt: nowIso()
      },
      $setOnInsert: { createdAt: nowIso() }
    },
    { upsert: true }
  );

  res.json({ ok: true, deviceId, version: nextVersion });
});

/**
 * Admin: list devices
 * GET /api/devices?limit=200&area=...&areaCode=...&online=1
 */
app.get("/api/devices", requireToken, async (req, res) => {
  const limit = clampInt(req.query.limit, 1, 2000, 200);
  const area = String(req.query.area || "").trim();
  const areaCode = String(req.query.areaCode || "").trim();
  const onlineFilter = String(req.query.online || "").trim(); // "1" or "0"

  const q = {};
  if (area) q.deviceArea = area;
  if (areaCode) q.deviceAreaCode = areaCode;

  const docs = await colDevices.find(q).sort({ lastSeenAt: -1 }).limit(limit).toArray();

  const rows = docs.map(d => {
    const online = computeOnline(d.lastSeenAt);
    return {
      deviceId: d.deviceId,
      deviceArea: d.deviceArea || null,
      deviceAreaCode: d.deviceAreaCode || null,
      deviceAreaTags: d.deviceAreaTags || null,

      ethIp: d.ethIp || null,
      ethLink: d.ethLink === undefined ? null : !!d.ethLink,

      playerIp: d.playerIp || null,
      playerOk: d.playerOk === undefined ? null : !!d.playerOk,

      fwVersion: d.fwVersion || null,
      appliedConfigVersion: d.appliedConfigVersion ?? null,

      lastAlertId: d.lastAlertId || null,
      lastEvent: d.lastEvent || null,
      lastSeverity: d.lastSeverity || null,

      lastSeenIp: d.lastSeenIp || null,
      lastSeenAt: d.lastSeenAt || null,
      online
    };
  });

  let filtered = rows;
  if (onlineFilter === "1") filtered = rows.filter(r => r.online);
  if (onlineFilter === "0") filtered = rows.filter(r => !r.online);

  res.json({
    ok: true,
    count: filtered.length,
    serverTime: nowIso(),
    onlineWindowSec: ONLINE_WINDOW_SEC,
    devices: filtered
  });
});

/**
 * Admin: device details + desired config
 * GET /api/device/:deviceId
 */
app.get("/api/device/:deviceId", requireToken, async (req, res) => {
  const deviceId = String(req.params.deviceId || "").trim();
  if (!deviceId) return res.status(400).json({ ok: false, error: "deviceId required" });

  const dev = await colDevices.findOne({ deviceId });
  const cfg = await colConfigs.findOne({ deviceId });

  if (!dev) return res.status(404).json({ ok: false, error: "not found" });

  res.json({
    ok: true,
    device: {
      ...dev,
      online: computeOnline(dev.lastSeenAt)
    },
    desiredConfig: cfg ? { version: cfg.version, desired: cfg.desired, updatedAt: cfg.updatedAt } : null
  });
});

app.get("/", (req, res) => res.redirect("/admin.html"));


// ---- Background offline sweep (optional) ----
function startOfflineSweep() {
  setInterval(async () => {
    try {
      const cutoffIso = new Date(Date.now() - ONLINE_WINDOW_SEC * 1000).toISOString();
      await colDevices.updateMany(
        { lastSeenAt: { $lt: cutoffIso }, online: true },
        { $set: { online: false, lastOfflineAt: nowIso() } }
      );
    } catch (e) {
      console.error("offline sweep error:", e?.message || e);
    }
  }, OFFLINE_SWEEP_SEC * 1000);
}

// ---- Start ----
await initMongo();
startOfflineSweep();

app.listen(PORT, "0.0.0.0", () => {
  console.log(`TRIG API listening on http://0.0.0.0:${PORT}`);
  console.log(`MONGO_URL=${MONGO_URL}`);
  console.log(`ONLINE_WINDOW_SEC=${ONLINE_WINDOW_SEC}, REQUIRE_AUTH=${REQUIRE_AUTH ? "1" : "0"}`);
});
