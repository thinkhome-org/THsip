import assert from "node:assert/strict";
import test from "node:test";
import { Readable } from "node:stream";
import WebSocket from "ws";
import { createApp, validateIvr, type EventInput, type Objects, type Recording, type Store } from "../src/server.js";

class MemoryStore implements Store {
  eventList: EventInput[] = [];
  ivr = new Map<number,string>();
  recordingList: Recording[] = [];
  failRecordingOnce = false;
  async addEvent(event: EventInput) { if (this.eventList.some(e => e.kind === event.kind && e.callId === event.callId && e.occurredAt === event.occurredAt)) return false; this.eventList.push(event); return true; }
  async events(limit: number) { return this.eventList.slice(0, limit); }
  async putIvr(slot: number, script: string) { this.ivr.set(slot, script); }
  async getIvr(slot: number) { return this.ivr.get(slot); }
  async addRecording(recording: Recording) {
    if (this.failRecordingOnce) { this.failRecordingOnce = false; throw new Error("temporary DB failure"); }
    if (!this.recordingList.some(item => item.id === recording.id)) this.recordingList.push(recording);
  }
  async recordings() { return this.recordingList; }
  async recording(id: string) { return this.recordingList.find(recording => recording.id === id); }
  async updateRecording(id:string, notes:string, retainUntil:string|null) { const item=this.recordingList.find(recording=>recording.id===id); if(!item)return false; item.notes=notes; item.retainUntil=retainUntil; return true; }
  async deleteRecording(id:string) { const index=this.recordingList.findIndex(recording=>recording.id===id); if(index<0)return undefined; return this.recordingList.splice(index,1)[0]; }
  async expiredRecordings(now:string) { return this.recordingList.filter(recording=>recording.retainUntil && recording.retainUntil<=now); }
}
class MemoryObjects implements Objects {
  values = new Map<string, Buffer>();
  async put(key:string, body:Readable) { const chunks: Buffer[] = []; for await (const chunk of body) chunks.push(Buffer.from(chunk)); this.values.set(key, Buffer.concat(chunks)); }
  async get(key:string) { const value = this.values.get(key); return value ? Readable.from(value) : undefined; }
  async delete(key:string) { this.values.delete(key); }
}

test("auth, webhook idempotence and IVR validation", async () => {
  const store = new MemoryStore();
  const app = await createApp({ store, objects: new MemoryObjects(), ingressToken: "ingress-secret", clientToken: "client-secret" });
  const unauthorized = await app.inject({ method:"POST", url:"/odorik/hooks/98", payload:{} });
  assert.equal(unauthorized.statusCode, 401);
  const url = "/odorik/hooks/98?token=ingress-secret&sip_in_callid=abc&time=2026-07-18T00%3A00%3A00Z";
  assert.deepEqual((await app.inject({ method:"GET", url })).json(), { ok:true, duplicate:false });
  assert.deepEqual((await app.inject({ method:"GET", url })).json(), { ok:true, duplicate:true });
  const save = await app.inject({ method:"PUT", url:"/v1/ivr/7", headers:{authorization:"Bearer client-secret"}, payload:{script:"answer\nplay:1\ndial:*08320+420123456789"} });
  assert.equal(save.statusCode, 200);
  assert.equal(validateIvr("answer\nhangup"), "answer\nhangup");
  assert.throws(() => validateIvr("shell:rm"));
  const id = "a".repeat(64);
  store.recordingList.push({ id, callId:"abc", key:"recordings/test.wav", mime:"audio/wav", size:4, metadata:{} });
  await app.close();

  const objectStore = new MemoryObjects();
  objectStore.values.set("recordings/test.wav", Buffer.from("RIFF"));
  const downloadApp = await createApp({ store, objects:objectStore, ingressToken:"ingress-secret", clientToken:"client-secret" });
  const download = await downloadApp.inject({ method:"GET", url:`/v1/recordings/${id}`, headers:{authorization:"Bearer client-secret"} });
  assert.equal(download.statusCode, 200);
  assert.equal(download.headers["content-type"], "audio/wav");
  assert.equal(download.body, "RIFF");
  assert.equal((await downloadApp.inject({ method:"PATCH", url:`/v1/recordings/${id}`, headers:{authorization:"Bearer client-secret"}, payload:{notes:"důležité",retainUntil:"2099-01-01T00:00:00Z"} })).statusCode, 200);
  assert.equal(store.recordingList[0]?.notes, "důležité");
  assert.equal((await downloadApp.inject({ method:"DELETE", url:`/v1/recordings/${id}`, headers:{authorization:"Bearer client-secret"} })).statusCode, 409);
  assert.equal((await downloadApp.inject({ method:"DELETE", url:`/v1/recordings/${id}`, headers:{authorization:"Bearer client-secret","x-confirm-delete":"recording"} })).statusCode, 204);
  assert.equal(store.recordingList.length, 0);
  assert.equal(objectStore.values.size, 0);
  assert.equal((await downloadApp.inject({ method:"GET", url:"/v1/recordings/not-a-hash", headers:{authorization:"Bearer client-secret"} })).statusCode, 400);
  await downloadApp.close();
});

test("recording accepts fields after file and safely retries", async () => {
  const store = new MemoryStore();
  const objects = new MemoryObjects();
  store.failRecordingOnce = true;
  const app = await createApp({ store, objects, ingressToken:"ingress-secret", clientToken:"client-secret" });
  const boundary = "thsip-test-boundary";
  const payload = Buffer.from([
    `--${boundary}\r\nContent-Disposition: form-data; name="record"; filename="call.wav"\r\nContent-Type: audio/wav\r\n\r\nRIFF\r\n`,
    `--${boundary}\r\nContent-Disposition: form-data; name="callid"\r\n\r\ncall-42\r\n`,
    `--${boundary}\r\nContent-Disposition: form-data; name="time"\r\n\r\n2026-07-18T10:00:00Z\r\n`,
    `--${boundary}--\r\n`
  ].join(""));
  const upload = () => app.inject({ method:"POST", url:"/odorik/recordings", headers:{"x-thsip-token":"ingress-secret", "content-type":`multipart/form-data; boundary=${boundary}`}, payload });
  assert.equal((await upload()).statusCode, 500);
  const retry = await upload();
  assert.equal(retry.statusCode, 201);
  assert.equal(store.recordingList.length, 1);
  assert.equal(store.recordingList[0]?.callId, "call-42");
  assert.equal(store.recordingList[0]?.size, 4);
  assert.equal(objects.values.size, 1);
  await app.close();
});

test("websocket streams new webhook events", async () => {
  const app = await createApp({ store:new MemoryStore(), objects:new MemoryObjects(), ingressToken:"ingress-secret", clientToken:"client-secret" });
  await app.listen({ host:"127.0.0.1", port:0 });
  const address = app.server.address();
  assert(address && typeof address !== "string");
  const socket = new WebSocket(`ws://127.0.0.1:${address.port}/v1/stream`, { headers:{authorization:"Bearer client-secret"} });
  await new Promise<void>((resolve, reject) => { socket.once("open", resolve); socket.once("error", reject); });
  const message = new Promise<Record<string,unknown>>((resolve, reject) => {
    socket.once("message", data => { try { resolve(JSON.parse(data.toString())); } catch (error) { reject(error); } });
  });
  await fetch(`http://127.0.0.1:${address.port}/odorik/hooks/99?token=ingress-secret&sip_in_callid=live-1&time=2026-07-18T12%3A00%3A00Z`);
  const payload = await message;
  assert.equal(payload.type, "event");
  assert.equal((payload.event as EventInput).callId, "live-1");
  socket.close();
  await app.close();
});
