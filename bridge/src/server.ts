import { createHash, timingSafeEqual } from "node:crypto";
import { Readable } from "node:stream";
import { pathToFileURL } from "node:url";
import Fastify, { type FastifyInstance, type FastifyReply, type FastifyRequest } from "fastify";
import multipart from "@fastify/multipart";
import rateLimit from "@fastify/rate-limit";
import websocket from "@fastify/websocket";
import { Pool } from "pg";
import { DeleteObjectCommand, GetObjectCommand, PutObjectCommand, S3Client } from "@aws-sdk/client-s3";

export type EventInput = {
  kind: "98" | "99";
  callId: string;
  line?: string;
  from?: string;
  to?: string;
  status?: string;
  occurredAt: string;
  raw: Record<string, string>;
};

export type Recording = { id: string; callId: string; key: string; mime: string; size: number; metadata: Record<string, string>; notes?: string; retainUntil?: string | null };

export interface Store {
  addEvent(event: EventInput): Promise<boolean>;
  events(limit: number): Promise<EventInput[]>;
  putIvr(slot: number, script: string): Promise<void>;
  getIvr(slot: number): Promise<string | undefined>;
  addRecording(recording: Recording): Promise<void>;
  recordings(): Promise<unknown[]>;
  recording(id: string): Promise<Recording | undefined>;
  updateRecording(id: string, notes: string, retainUntil: string | null): Promise<boolean>;
  deleteRecording(id: string): Promise<Recording | undefined>;
  expiredRecordings(now: string): Promise<Recording[]>;
}

export interface Objects {
  put(key: string, body: Readable, mime: string): Promise<void>;
  get(key: string): Promise<Readable | undefined>;
  delete(key: string): Promise<void>;
}

export type Dependencies = { store: Store; objects: Objects; ingressToken: string; clientToken: string };

const safeEqual = (actual: string, expected: string) => {
  const a = Buffer.from(actual);
  const b = Buffer.from(expected);
  return a.length === b.length && timingSafeEqual(a, b);
};

const value = (input: unknown): string => Array.isArray(input) ? String(input[0] ?? "") : String(input ?? "");

export function validateIvr(script: string): string {
  const commands = /^(answer|hangup|play:(?:https?:\/\/\S+|\d{1,3})|play2:(?:https?:\/\/\S+|\d{1,3})|playnumber:\d{1,6}|dial:\S+|setclip:\+?\d+|uri:https?:\/\/\S+)$/;
  const lines = script.split(/\r?\n/).map(line => line.trim()).filter(Boolean);
  if (!lines.length || lines.length > 100 || lines.some(line => !commands.test(line))) throw new Error("invalid IVR script");
  return lines.join("\n");
}

export async function createApp(deps: Dependencies): Promise<FastifyInstance> {
  const app = Fastify({ logger: false, bodyLimit: 10 * 1024 * 1024 });
  await app.register(rateLimit, { max: 120, timeWindow: "1 minute" });
  await app.register(multipart, { limits: { files: 1, fileSize: 10 * 1024 * 1024, fields: 20 } });
  await app.register(websocket);

  const ingress = async (request: FastifyRequest, reply: FastifyReply) => {
    const query = (request.query ?? {}) as Record<string, unknown>;
    const token = value(request.headers["x-thsip-token"] ?? query.token);
    if (!safeEqual(token, deps.ingressToken)) return reply.code(401).send({ error: "unauthorized" });
  };
  const client = async (request: FastifyRequest, reply: FastifyReply) => {
    const token = value(request.headers.authorization).replace(/^Bearer\s+/i, "");
    if (!safeEqual(token, deps.clientToken)) return reply.code(401).send({ error: "unauthorized" });
  };

  for (const kind of ["98", "99"] as const) {
    app.route({ method: ["GET", "POST"], url: `/odorik/hooks/${kind}`, preHandler: ingress, handler: async (request, reply) => {
      const raw = Object.fromEntries(Object.entries({ ...(request.query as object), ...(request.body as object) }).map(([k, v]) => [k, value(v)]));
      const callId = raw.sip_in_callid || raw.callid || raw.call_id;
      if (!callId) return reply.code(400).send({ error: "missing Call-ID" });
      const occurredAt = raw.time || new Date().toISOString();
      const inserted = await deps.store.addEvent({ kind, callId, line: raw.line, from: raw.from, to: raw.to, status: raw.status, occurredAt, raw });
      return { ok: true, duplicate: !inserted };
    }});
  }

  app.route({ method: ["GET", "POST"], url: "/odorik/ivr/:slot", preHandler: ingress, handler: async request => {
    const slot = Number((request.params as { slot: string }).slot);
    if (!Number.isInteger(slot) || slot < 1 || slot > 99) return "hangup";
    return (await deps.store.getIvr(slot)) ?? "hangup";
  }});

  app.post("/odorik/recordings", { preHandler: ingress }, async (request, reply) => {
    const fields: Record<string, string> = {};
    let file: { filename: string; mimetype: string; body: Buffer } | undefined;
    for await (const part of request.parts()) {
      if (part.type === "field") { fields[part.fieldname] = value(part.value); continue; }
      if (file) return reply.code(400).send({ error: "one recording required" });
      const chunks: Buffer[] = [];
      for await (const chunk of part.file) chunks.push(Buffer.from(chunk));
      file = { filename: part.filename, mimetype: part.mimetype, body: Buffer.concat(chunks) };
    }
    if (!file || !["audio/wav", "audio/mpeg", "audio/ogg", "audio/opus", "application/ogg"].includes(file.mimetype)) return reply.code(415).send({ error: "unsupported recording" });
    const callId = fields.callid || fields.sip_in_callid;
    if (!callId) return reply.code(400).send({ error: "missing Call-ID" });
    const id = createHash("sha256").update(`${callId}:${fields.time ?? ""}:${file.filename}`).digest("hex");
    const key = `recordings/${id}/${file.filename.replace(/[^a-zA-Z0-9._-]/g, "_")}`;
    await deps.objects.put(key, Readable.from(file.body), file.mimetype);
    await deps.store.addRecording({ id, callId, key, mime: file.mimetype, size: file.body.length, metadata: fields });
    return reply.code(201).send({ id });
  });

  app.get("/v1/events", { preHandler: client }, async request => deps.store.events(Math.min(Number((request.query as { limit?: string }).limit ?? 100), 1000)));
  app.get("/v1/recordings", { preHandler: client }, async () => deps.store.recordings());
  app.get("/v1/recordings/:id", { preHandler: client }, async (request, reply) => {
    const id = (request.params as { id: string }).id;
    if (!/^[a-f0-9]{64}$/.test(id)) return reply.code(400).send({ error: "invalid recording id" });
    const recording = await deps.store.recording(id);
    if (!recording) return reply.code(404).send({ error: "recording not found" });
    const body = await deps.objects.get(recording.key);
    if (!body) return reply.code(404).send({ error: "recording object not found" });
    return reply.type(recording.mime).header("content-length", recording.size).send(body);
  });
  app.patch("/v1/recordings/:id", { preHandler: client }, async (request, reply) => {
    const id = (request.params as { id: string }).id;
    if (!/^[a-f0-9]{64}$/.test(id)) return reply.code(400).send({ error: "invalid recording id" });
    const input = request.body as { notes?: unknown; retainUntil?: unknown };
    const notes = value(input?.notes);
    if (notes.length > 10_000) return reply.code(400).send({ error: "notes too long" });
    const retainUntil = input?.retainUntil == null || input.retainUntil === "" ? null : value(input.retainUntil);
    if (retainUntil && (!Number.isFinite(Date.parse(retainUntil)) || Date.parse(retainUntil) <= Date.now())) return reply.code(400).send({ error: "retainUntil must be future ISO date" });
    if (!await deps.store.updateRecording(id, notes, retainUntil)) return reply.code(404).send({ error: "recording not found" });
    return { ok: true };
  });
  app.delete("/v1/recordings/:id", { preHandler: client }, async (request, reply) => {
    if (request.headers["x-confirm-delete"] !== "recording") return reply.code(409).send({ error: "confirmation required" });
    const id = (request.params as { id: string }).id;
    if (!/^[a-f0-9]{64}$/.test(id)) return reply.code(400).send({ error: "invalid recording id" });
    const recording = await deps.store.deleteRecording(id);
    if (!recording) return reply.code(404).send({ error: "recording not found" });
    await deps.objects.delete(recording.key);
    return reply.code(204).send();
  });
  app.put("/v1/ivr/:slot", { preHandler: client }, async (request, reply) => {
    const slot = Number((request.params as { slot: string }).slot);
    if (!Number.isInteger(slot) || slot < 1 || slot > 99) return reply.code(400).send({ error: "invalid slot" });
    try { await deps.store.putIvr(slot, validateIvr((request.body as { script?: string }).script ?? "")); }
    catch { return reply.code(400).send({ error: "invalid IVR script" }); }
    return { ok: true };
  });
  app.get("/v1/ivr/:slot", { preHandler: client }, async request => ({ script: await deps.store.getIvr(Number((request.params as { slot: string }).slot)) }));
  app.get("/v1/stream", { websocket: true, preValidation: client }, socket => {
    const timer = setInterval(() => socket.send(JSON.stringify({ type: "ping", at: new Date().toISOString() })), 25_000);
    socket.on("close", () => clearInterval(timer));
  });
  app.get("/health", async () => ({ ok: true }));
  return app;
}

class PostgresStore implements Store {
  constructor(private readonly pool: Pool) {}
  async addEvent(event: EventInput) {
    const result = await this.pool.query("INSERT INTO webhook_events(kind,call_id,line,from_number,to_number,status,occurred_at,raw) VALUES($1,$2,$3,$4,$5,$6,$7,$8) ON CONFLICT(kind,call_id,occurred_at) DO NOTHING", [event.kind,event.callId,event.line,event.from,event.to,event.status,event.occurredAt,event.raw]);
    return result.rowCount === 1;
  }
  async events(limit: number) { return (await this.pool.query("SELECT kind,call_id AS \"callId\",line,from_number AS \"from\",to_number AS \"to\",status,occurred_at AS \"occurredAt\",raw FROM webhook_events ORDER BY occurred_at DESC LIMIT $1", [limit])).rows; }
  async putIvr(slot: number, script: string) { await this.pool.query("INSERT INTO ivr_scripts(slot,script) VALUES($1,$2) ON CONFLICT(slot) DO UPDATE SET script=$2,updated_at=now()", [slot, script]); }
  async getIvr(slot: number) { return (await this.pool.query("SELECT script FROM ivr_scripts WHERE slot=$1", [slot])).rows[0]?.script; }
  async addRecording(r: Recording) { await this.pool.query("INSERT INTO recordings(id,call_id,object_key,mime,size,metadata) VALUES($1,$2,$3,$4,$5,$6) ON CONFLICT(id) DO NOTHING", [r.id,r.callId,r.key,r.mime,r.size,r.metadata]); }
  async recordings() { return (await this.pool.query("SELECT id,call_id AS \"callId\",mime,size,metadata,notes,retain_until AS \"retainUntil\",created_at AS \"createdAt\" FROM recordings ORDER BY created_at DESC LIMIT 1000")).rows; }
  async recording(id: string) { return (await this.pool.query("SELECT id,call_id AS \"callId\",object_key AS key,mime,size,metadata,notes,retain_until AS \"retainUntil\" FROM recordings WHERE id=$1", [id])).rows[0]; }
  async updateRecording(id: string, notes: string, retainUntil: string | null) { return (await this.pool.query("UPDATE recordings SET notes=$2,retain_until=$3 WHERE id=$1", [id,notes,retainUntil])).rowCount === 1; }
  async deleteRecording(id: string) { const result = await this.pool.query("DELETE FROM recordings WHERE id=$1 RETURNING id,call_id AS \"callId\",object_key AS key,mime,size,metadata,notes,retain_until AS \"retainUntil\"", [id]); return result.rows[0]; }
  async expiredRecordings(now: string) { return (await this.pool.query("SELECT id,call_id AS \"callId\",object_key AS key,mime,size,metadata FROM recordings WHERE retain_until IS NOT NULL AND retain_until <= $1", [now])).rows; }
}

class S3Objects implements Objects {
  constructor(private readonly client: S3Client, private readonly bucket: string) {}
  async put(key: string, body: Readable, mime: string) { await this.client.send(new PutObjectCommand({ Bucket: this.bucket, Key: key, Body: body, ContentType: mime, ServerSideEncryption: "AES256" })); }
  async get(key: string) { return (await this.client.send(new GetObjectCommand({ Bucket: this.bucket, Key: key }))).Body as Readable | undefined; }
  async delete(key: string) { await this.client.send(new DeleteObjectCommand({ Bucket: this.bucket, Key: key })); }
}

async function main() {
  const required = ["DATABASE_URL", "S3_BUCKET", "INGRESS_TOKEN", "CLIENT_TOKEN"] as const;
  for (const name of required) if (!process.env[name]) throw new Error(`${name} required`);
  const pool = new Pool({ connectionString: process.env.DATABASE_URL, ssl: process.env.PGSSL === "disable" ? false : { rejectUnauthorized: true } });
  const s3 = new S3Client({ region: process.env.S3_REGION ?? "eu-central-1", endpoint: process.env.S3_ENDPOINT, forcePathStyle: Boolean(process.env.S3_ENDPOINT) });
  const app = await createApp({ store: new PostgresStore(pool), objects: new S3Objects(s3, process.env.S3_BUCKET!), ingressToken: process.env.INGRESS_TOKEN!, clientToken: process.env.CLIENT_TOKEN! });
  await app.listen({ host: process.env.HOST ?? "0.0.0.0", port: Number(process.env.PORT ?? 3000) });
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) main().catch(error => { console.error(error); process.exit(1); });
