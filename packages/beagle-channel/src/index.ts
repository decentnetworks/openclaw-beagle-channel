import { createSidecarClient, type BeagleAccount } from "./sidecarClient.js";
declare const require: any;
declare const process: any;
const { Buffer } = require("buffer");
const { existsSync } = require("fs");
const { readdirSync, statSync, mkdirSync, readFileSync, writeFileSync } = require("fs");
const { isAbsolute, resolve, join, basename } = require("path");

const INBOUND_SEEN_MAX = 10_000;
const inboundSeen = new Set<string>();
const inboundSeenOrder: string[] = [];
const subscriptionFanoutSeen = new Set<string>();
const subscriptionFanoutSeenOrder: string[] = [];
const inboundPollControllers = new Map<string, AbortController>();
const discordSubscriptionPollControllers = new Map<string, AbortController>();
const CARRIER_GROUP_MESSAGE_PREFIX = "CGP1 ";
const CARRIER_GROUP_REPLY_PREFIX = "CGR1 ";
const CARRIER_GROUP_STATUS_PREFIX = "CGS1 ";
const BEAGLE_STATUS_PREFIX = "BGS1 ";
const SUBS_STORE_PATH_DEFAULT = "~/.openclaw/workspace/memory/beagle-channel-subscriptions.json";

type CarrierGroupInboundEnvelope = {
  type?: string;
  version?: number;
  chat_type?: string;
  source?: string;
  group?: {
    userid?: string;
    address?: string;
    nickname?: string;
  };
  origin?: {
    userid?: string;
    friendid?: string;
    nickname?: string;
    user_info?: {
      userid?: string;
      name?: string;
      description?: string;
      has_avatar?: number;
      gender?: string;
      phone?: string;
      email?: string;
      region?: string;
    };
    friend_info?: {
      label?: string;
      status?: number;
      status_text?: string;
      presence?: number;
      presence_text?: string;
    };
  };
  message?: {
    text?: string;
    timestamp?: number;
  };
  render?: {
    plain?: string;
  };
  text?: string;
};

type ParsedGroupInbound = {
  envelope: CarrierGroupInboundEnvelope;
  groupUserId: string;
  groupAddress: string;
  groupNickname: string;
  originUserId: string;
  originNickname: string;
  messageText: string;
  timestamp?: number;
};

type AgentStatusState = "typing" | "thinking" | "tool" | "sending" | "idle" | "error";

type SubscribableChannel = {
  id: string;
  name?: string;
  description?: string;
};

type SubscriptionRecord = {
  accountId: string;
  peerId: string;
  chatType: "dm" | "group";
  channelId: string;
  channelName?: string;
  deliveryPeerId?: string;
  groupUserId?: string;
  groupAddress?: string;
  groupName?: string;
  createdAt: string;
  updatedAt: string;
};

type SubscriptionStore = {
  version: number;
  records: SubscriptionRecord[];
};

type DiscordMessageAuthor = {
  id?: string;
  username?: string;
  global_name?: string;
};

type DiscordMessageAttachment = {
  filename?: string;
  content_type?: string;
  url?: string;
  proxy_url?: string;
};

type DiscordChannelMessage = {
  id?: string;
  channel_id?: string;
  content?: string;
  timestamp?: string;
  author?: DiscordMessageAuthor;
  attachments?: DiscordMessageAttachment[];
};

function envTruthy(value: any) {
  const text = String(value ?? "").trim().toLowerCase();
  return text === "1" || text === "true" || text === "yes" || text === "on";
}

function buildStatusText({
  state,
  phase,
  ttlMs,
  seq,
  isGroup,
  parsedGroup
}: {
  state: AgentStatusState;
  phase?: string;
  ttlMs: number;
  seq: string;
  isGroup: boolean;
  parsedGroup?: ParsedGroupInbound | null;
}) {
  const payload: any = {
    type: "agent_status",
    version: 1,
    chat_type: isGroup ? "group" : "direct",
    source: "openclaw_beagle_channel",
    status: {
      state,
      phase,
      ttl_ms: ttlMs,
      seq,
      ts: Math.floor(Date.now() / 1000)
    }
  };

  if (isGroup) {
    payload.group = {
      userid: parsedGroup?.groupUserId || undefined,
      address: parsedGroup?.groupAddress || undefined,
      name: parsedGroup?.groupNickname || undefined
    };
  }

  return `${isGroup ? CARRIER_GROUP_STATUS_PREFIX : BEAGLE_STATUS_PREFIX}${JSON.stringify(payload)}`;
}

function parseCarrierGroupInbound(rawText: any): ParsedGroupInbound | null {
  const raw = String(rawText ?? "");
  if (!raw.startsWith(CARRIER_GROUP_MESSAGE_PREFIX)) return null;
  const payloadText = raw.slice(CARRIER_GROUP_MESSAGE_PREFIX.length).trim();
  if (!payloadText) return null;

  let parsed: CarrierGroupInboundEnvelope;
  try {
    parsed = JSON.parse(payloadText) as CarrierGroupInboundEnvelope;
  } catch {
    return null;
  }
  if (!parsed || typeof parsed !== "object") return null;
  if (String(parsed.type ?? "") !== "carrier_group_message") return null;
  if (String(parsed.chat_type ?? "") && String(parsed.chat_type ?? "") !== "group") return null;

  const groupUserId = String(parsed.group?.userid ?? "").trim();
  const groupAddress = String(parsed.group?.address ?? "").trim();
  const groupNickname = String(parsed.group?.nickname ?? "").trim();
  const originUserId = String(
    parsed.origin?.userid ?? parsed.origin?.friendid ?? parsed.origin?.user_info?.userid ?? ""
  ).trim();
  const originNickname = String(
    parsed.origin?.nickname ?? parsed.origin?.user_info?.name ?? ""
  ).trim();
  const messageText = String(parsed.message?.text ?? parsed.text ?? "").trim();
  const timestampNum = Number(parsed.message?.timestamp ?? 0);
  const timestamp = Number.isFinite(timestampNum) && timestampNum > 0 ? timestampNum : undefined;

  if (!groupAddress || !originUserId || !messageText) return null;

  return {
    envelope: parsed,
    groupUserId,
    groupAddress,
    groupNickname,
    originUserId,
    originNickname,
    messageText,
    timestamp
  };
}

function buildCarrierGroupReplyText(text: string, parsedGroup: ParsedGroupInbound) {
  const cleanText = String(text ?? "").trim();
  if (!cleanText) return "";

  const payload = {
    type: "carrier_group_reply",
    version: 1,
    chat_type: "group",
    source: "openclaw_beagle_channel",
    group: {
      userid: parsedGroup.groupUserId || undefined,
      address: parsedGroup.groupAddress
    },
    message: {
      text: cleanText
    }
  };
  return `${CARRIER_GROUP_REPLY_PREFIX}${JSON.stringify(payload)}`;
}

function normalizePeerId(value: any) {
  return String(value ?? "")
    .replace(/^beagle:\/\//i, "")
    .replace(/^beagle:/i, "")
    .replace(/^(user|channel|group):/i, "")
    .replace(/^[@#]/, "")
    .trim();
}

function isLikelyBeaglePeerId(value: any) {
  const raw = normalizePeerId(value);
  if (!raw) return false;
  // Carrier IDs are base58-like and case-sensitive; keep original case.
  return /^[1-9A-HJ-NP-Za-km-z]{30,120}$/.test(raw);
}

function normalizeAddress(value: any) {
  return String(value ?? "").trim();
}

function toNormalizedSet(values: any, normalize: (value: any) => string) {
  const set = new Set<string>();
  if (!Array.isArray(values)) return set;
  for (const value of values) {
    const normalized = normalize(value);
    if (normalized) set.add(normalized);
  }
  return set;
}

function parseMediaDirectiveInText(text: any) {
  const raw = String(text ?? "");
  const lines = raw.split(/\r?\n/);
  let mediaPath = "";
  const kept: string[] = [];
  for (const line of lines) {
    const unwrapped = line
      .trim()
      .replace(/^[-*]\s+/, "")
      .replace(/^([*_]+)\s*/, "")
      .replace(/\s*([*_]+)$/, "");
    const m = unwrapped.match(/^(.*?)(?:\s|^)MEDIA\s*:\s*(.+)$/i);
    if (!m) {
      kept.push(line);
      continue;
    }
    if (!mediaPath) {
      mediaPath = String(m[2] ?? "")
        .trim()
        .replace(/^(["'])/, "")
        .replace(/(["'])$/, "")
        .replace(/^([*_]+)/, "")
        .replace(/([*_]+)$/, "");
    }
    const prefix = String(m[1] ?? "").trim();
    if (prefix) kept.push(prefix);
  }
  const caption = kept.join("\n").trim();
  return { mediaPath, caption };
}

function resolveLocalMediaPath(inputPath: any) {
  const raw = String(inputPath ?? "").trim();
  if (!raw) return "";
  const home = process.env.HOME || "";
  const expandHome = raw.startsWith("~/") && home ? resolve(home, raw.slice(2)) : raw;
  const candidates: string[] = [];
  if (isAbsolute(expandHome)) {
    candidates.push(expandHome);
  } else {
    const cleaned = expandHome.replace(/^\.\//, "");
    candidates.push(resolve(process.cwd(), expandHome));
    if (home) {
      candidates.push(resolve(home, ".openclaw/workspace", cleaned));
      candidates.push(resolve(home, cleaned));
    }
  }
  for (const p of candidates) {
    try {
      if (existsSync(p)) return p;
    } catch {
      // ignore fs errors and keep trying
    }
  }
  return expandHome;
}

function resolveDefaultReplyImagePath() {
  const envPath = String(process.env.BEAGLE_DEFAULT_REPLY_IMAGE || "").trim();
  if (envPath) {
    const resolved = resolveLocalMediaPath(envPath);
    if (resolved && existsSync(resolved)) return resolved;
  }
  const carrierDir = resolveLocalMediaPath("~/.carrier/media");
  try {
    const entries = readdirSync(carrierDir);
    let bestImgPath = "";
    let bestImgMtime = 0;
    let bestAnyPath = "";
    let bestAnyMtime = 0;
    for (const name of entries) {
      if (!/\.(jpg|jpeg|png|webp)$/i.test(name)) continue;
      if (/^reply_(small|tiny)\.jpg$/i.test(name)) continue;
      if (/^test_send/i.test(name)) continue;
      const full = join(carrierDir, name);
      let st: any;
      try {
        st = statSync(full);
      } catch {
        continue;
      }
      if (!st?.isFile?.()) continue;
      const mt = Number(st.mtimeMs || 0);
      const isRealImg = /(^|_)IMG_/i.test(name);
      if (mt > bestAnyMtime) {
        bestAnyMtime = mt;
        bestAnyPath = full;
      }
      if (isRealImg && mt > bestImgMtime) {
        bestImgMtime = mt;
        bestImgPath = full;
      }
    }
    if (bestImgPath) return bestImgPath;
    if (bestAnyPath) return bestAnyPath;
  } catch {
    // ignore and fall back
  }
  return "";
}

function resolveSubscriptionStorePath() {
  const configured = String(process.env.BEAGLE_SUBSCRIPTION_STORE_PATH || "").trim();
  return resolveLocalMediaPath(configured || SUBS_STORE_PATH_DEFAULT);
}

function loadSubscriptionStore(): SubscriptionStore {
  const path = resolveSubscriptionStorePath();
  try {
    if (!existsSync(path)) return { version: 1, records: [] };
    const raw = readFileSync(path, "utf8");
    const parsed = JSON.parse(raw);
    if (!parsed || typeof parsed !== "object") return { version: 1, records: [] };
    const rawRecords = Array.isArray((parsed as any).records) ? (parsed as any).records : [];
    const records = rawRecords
      .map((record: any) => normalizeSubscriptionRecord(record))
      .filter(Boolean) as SubscriptionRecord[];
    return { version: 2, records };
  } catch {
    return { version: 1, records: [] };
  }
}

function saveSubscriptionStore(store: SubscriptionStore) {
  const path = resolveSubscriptionStorePath();
  mkdirSync(require("path").dirname(path), { recursive: true });
  writeFileSync(path, JSON.stringify({ version: 2, records: store.records.map((record) => normalizeSubscriptionRecord(record)).filter(Boolean) }, null, 2));
}

function normalizeSubscriptionRecord(record: any): SubscriptionRecord | null {
  if (!record || typeof record !== "object") return null;
  const accountId = String(record.accountId ?? "").trim() || "default";
  const channelId = String(record.channelId ?? "").trim();
  const peerId = normalizePeerId(record.peerId);
  const deliveryPeerId = normalizePeerId(record.deliveryPeerId ?? record.peerId);
  const chatType = String(record.chatType ?? "dm").trim().toLowerCase() === "group" ? "group" : "dm";
  if (!channelId || !peerId || !deliveryPeerId) return null;
  const createdAt = String(record.createdAt ?? "").trim() || new Date().toISOString();
  const updatedAt = String(record.updatedAt ?? "").trim() || createdAt;
  return {
    accountId,
    peerId,
    deliveryPeerId,
    chatType,
    channelId,
    channelName: String(record.channelName ?? "").trim() || undefined,
    groupUserId: String(record.groupUserId ?? "").trim() || undefined,
    groupAddress: String(record.groupAddress ?? "").trim() || undefined,
    groupName: String(record.groupName ?? "").trim() || undefined,
    createdAt,
    updatedAt
  };
}

function parseSubscribableChannels(account: BeagleAccount): SubscribableChannel[] {
  const raw = (account as any)?.subscribableChannels;
  if (!Array.isArray(raw)) return [];
  const out: SubscribableChannel[] = [];
  for (const item of raw) {
    if (!item) continue;
    if (typeof item === "string") {
      const id = item.trim();
      if (id) out.push({ id, name: id });
      continue;
    }
    const id = String((item as any).id ?? "").trim();
    if (!id) continue;
    out.push({ id, name: String((item as any).name ?? "").trim() || undefined, description: String((item as any).description ?? "").trim() || undefined });
  }
  return out;
}

function findChannelByQuery(channels: SubscribableChannel[], query: string) {
  const q = String(query || "").trim().toLowerCase();
  if (!q) return null;
  return channels.find((c) => c.id.toLowerCase() === q) || channels.find((c) => String(c.name || "").toLowerCase() === q) || channels.find((c) => String(c.name || "").toLowerCase().includes(q)) || null;
}

function maybeHandleLocalSubscriptionCommand(params: {
  accountId: string;
  account: BeagleAccount;
  peerId: string;
  deliveryPeerId: string;
  isGroup: boolean;
  groupUserId?: string;
  groupAddress?: string;
  groupName?: string;
  body: string;
}): string | null {
  const body = String(params.body || "").trim();
  if (!body.startsWith("/")) return null;
  const channels = parseSubscribableChannels(params.account);
  const parts = body.split(/\s+/);
  const cmd = String(parts.shift() || "").toLowerCase();
  const arg = parts.join(" ").trim();
  if (cmd === "/channels" || cmd === "/discover") {
    if (!channels.length) return "No channels configured yet. Ask admin to set subscribableChannels.";
    const lines = channels.map((c, i) => String(i + 1) + ". " + (c.name || c.id) + " (" + c.id + ")" + (c.description ? " - " + c.description : ""));
    return "Available channels:\n" + lines.join("\n") + "\n\nUse: /subscribe <channel-id-or-name>";
  }
  if (cmd === "/subscribe") {
    if (!arg) return "Usage: /subscribe <channel-id-or-name>";
    const ch = findChannelByQuery(channels, arg);
    if (!ch) return "Channel not found. Use /channels to see available options.";
    const store = loadSubscriptionStore();
    const now = new Date().toISOString();
    const peerId = normalizePeerId(params.peerId);
    const deliveryPeerId = normalizePeerId(params.deliveryPeerId);
    const existing = store.records.find((r: any) => r.accountId === params.accountId && r.peerId === peerId && r.channelId === ch.id);
    if (existing) {
      existing.updatedAt = now;
      existing.chatType = params.isGroup ? "group" : "dm";
      existing.deliveryPeerId = deliveryPeerId || existing.deliveryPeerId || existing.peerId;
      existing.groupUserId = params.groupUserId || existing.groupUserId;
      existing.groupAddress = params.groupAddress || existing.groupAddress;
      existing.groupName = params.groupName || existing.groupName;
      saveSubscriptionStore(store);
      return "Already subscribed: " + (ch.name || ch.id);
    }
    store.records.push({
      accountId: params.accountId,
      peerId,
      deliveryPeerId: deliveryPeerId || peerId,
      chatType: params.isGroup ? "group" : "dm",
      channelId: ch.id,
      channelName: ch.name,
      groupUserId: params.groupUserId,
      groupAddress: params.groupAddress,
      groupName: params.groupName,
      createdAt: now,
      updatedAt: now
    });
    saveSubscriptionStore(store);
    return "Subscribed: " + (ch.name || ch.id);
  }
  if (cmd === "/unsubscribe") {
    if (!arg) return "Usage: /unsubscribe <channel-id-or-name>";
    const ch = findChannelByQuery(channels, arg) || { id: arg, name: arg };
    const store = loadSubscriptionStore();
    const peerId = normalizePeerId(params.peerId);
    const next = store.records.filter((r: any) => !(r.accountId === params.accountId && r.peerId === peerId && r.channelId === ch.id));
    if (next.length === store.records.length) return "No active subscription found for: " + (ch.name || ch.id);
    store.records = next; saveSubscriptionStore(store); return "Unsubscribed: " + (ch.name || ch.id);
  }
  if (cmd === "/subscriptions" || cmd === "/subs") {
    const store = loadSubscriptionStore(); const peerId = normalizePeerId(params.peerId);
    const mine = store.records.filter((r: any) => r.accountId === params.accountId && r.peerId === peerId);
    if (!mine.length) return "No subscriptions yet. Use /channels then /subscribe <channel>.";
    return "Your subscriptions:\n" + mine.map((r: any, i: number) => String(i + 1) + ". " + (r.channelName || r.channelId) + " (" + r.channelId + ")").join("\n");
  }
  return null;
}
function coerceMediaInput(mediaPath: any, mediaUrl: any) {
  let nextMediaPath = String(mediaPath ?? "").trim();
  let nextMediaUrl = String(mediaUrl ?? "").trim();

  if (nextMediaPath) {
    nextMediaPath = resolveLocalMediaPath(nextMediaPath);
  }

  // Some callers pass local files via mediaUrl (e.g. CLI --media path).
  if (!nextMediaPath && nextMediaUrl) {
    const maybeFileUrl = nextMediaUrl.replace(/^file:\/\//i, "");
    const looksLocalPath = maybeFileUrl.startsWith("/") || maybeFileUrl.startsWith("~/") || maybeFileUrl.startsWith("./") || maybeFileUrl.startsWith("../");
    if (looksLocalPath) {
      nextMediaPath = resolveLocalMediaPath(maybeFileUrl);
      nextMediaUrl = "";
    }
  }

  return { mediaPath: nextMediaPath, mediaUrl: nextMediaUrl };
}

function isPictureRequest(text: any) {
  const t = String(text ?? "").toLowerCase();
  if (!t) return false;
  return /send me (a |an )?(picture|photo|image)/i.test(t) || /发.*(图|图片|照片)/.test(t);
}

function canonicalPeerId(value: any) {
  return normalizePeerId(value).toLowerCase();
}

function normalizeInboundText(value: any) {
  return String(value ?? "")
    .replace(/\u0000+/g, "")
    .replace(/[\u0001-\u0008\u000B\u000C\u000E-\u001F\u007F]/g, "")
    .trim();
}

function rememberInboundSignature(signature: string) {
  if (inboundSeen.has(signature)) return false;
  inboundSeen.add(signature);
  inboundSeenOrder.push(signature);
  if (inboundSeenOrder.length > INBOUND_SEEN_MAX) {
    const oldest = inboundSeenOrder.shift();
    if (oldest) inboundSeen.delete(oldest);
  }
  return true;
}

function rememberSubscriptionFanoutSignature(signature: string) {
  if (subscriptionFanoutSeen.has(signature)) return false;
  subscriptionFanoutSeen.add(signature);
  subscriptionFanoutSeenOrder.push(signature);
  if (subscriptionFanoutSeenOrder.length > INBOUND_SEEN_MAX) {
    const oldest = subscriptionFanoutSeenOrder.shift();
    if (oldest) subscriptionFanoutSeen.delete(oldest);
  }
  return true;
}

function abortDiscordSubscriptionPoller(key = "default", controller?: AbortController) {
  const active = discordSubscriptionPollControllers.get(key);
  if (!active) return;
  if (controller && active !== controller) return;
  active.abort();
  discordSubscriptionPollControllers.delete(key);
}

function abortDiscordSubscriptionPollers() {
  for (const controller of discordSubscriptionPollControllers.values()) controller.abort();
  discordSubscriptionPollControllers.clear();
}

async function sendBeagleSubscriptionText(params: {
  client: any;
  record: SubscriptionRecord;
  text: string;
}) {
  const text = String(params.text ?? "").trim();
  const deliveryPeerId = normalizePeerId(params.record.deliveryPeerId ?? params.record.peerId);
  if (!text || !deliveryPeerId) return;
  if (
    params.record.chatType === "group" &&
    params.record.groupAddress &&
    params.record.groupUserId
  ) {
    const groupReply = buildCarrierGroupReplyText(text, {
      envelope: {},
      groupUserId: params.record.groupUserId,
      groupAddress: params.record.groupAddress,
      groupNickname: params.record.groupName || params.record.channelName || "",
      originUserId: "",
      originNickname: "",
      messageText: text
    });
    await params.client.sendText({ peer: deliveryPeerId, text: groupReply });
    return;
  }
  await params.client.sendText({ peer: deliveryPeerId, text });
}

async function sendBeagleSubscriptionMedia(params: {
  client: any;
  record: SubscriptionRecord;
  caption?: string;
  mediaPath: string;
  mediaType?: string;
  filename?: string;
}) {
  const deliveryPeerId = normalizePeerId(params.record.deliveryPeerId ?? params.record.peerId);
  if (!deliveryPeerId || !params.mediaPath) return;
  if (params.record.chatType === "group") {
    // Group text relay is implemented through CGR1. Group media forwarding needs
    // a separate CarrierGroup-compatible media envelope, so fail safe to text.
    const fallback = `${params.caption || ""}\n[Attachment] ${params.filename || "file"}`.trim();
    if (fallback) {
      await sendBeagleSubscriptionText({
        client: params.client,
        record: params.record,
        text: fallback
      });
    }
    return;
  }
  await params.client.sendMedia({
    peer: deliveryPeerId,
    caption: params.caption || "",
    mediaPath: params.mediaPath,
    mediaType: params.mediaType,
    filename: params.filename,
    // iOS subscribers have been observed to miss auto->packed fallback payloads.
    // Request the Swift file-model JSON path explicitly for Discord subscription media.
    outFormat: "swift-json"
  });
}

function summarizeDiscordAttachments(attachments: DiscordMessageAttachment[] | undefined) {
  if (!Array.isArray(attachments) || attachments.length === 0) return "";
  const names = attachments
    .map((entry: any) => String(entry?.name ?? entry?.filename ?? "").trim())
    .filter(Boolean);
  if (!names.length) return `${attachments.length} attachment${attachments.length === 1 ? "" : "s"}`;
  return names.join(", ");
}

function formatDiscordSubscriptionForward(params: {
  message: DiscordChannelMessage;
  channelName?: string;
}) {
  const sender =
    String(
      params.message?.author?.global_name ||
        params.message?.author?.username ||
        params.message?.author?.id ||
        ""
    ).trim() || "unknown";
  const channelName = String(
    params.channelName || "discord"
  ).trim();
  const body = normalizeInboundText(params.message?.content ?? "");
  const attachmentSummary = summarizeDiscordAttachments(params.message?.attachments);
  const content = body || (attachmentSummary ? `[Attachment] ${attachmentSummary}` : "");
  if (!content) return "";
  return `[${channelName}] ${sender}: ${content}`;
}

function formatDiscordSubscriptionCaption(params: {
  message: DiscordChannelMessage;
  channelName?: string;
}) {
  const sender =
    String(
      params.message?.author?.global_name ||
        params.message?.author?.username ||
        params.message?.author?.id ||
        ""
    ).trim() || "unknown";
  const channelName = String(params.channelName || "discord").trim();
  const body = normalizeInboundText(params.message?.content ?? "");
  return body ? `[${channelName}] ${sender}: ${body}` : `[${channelName}] ${sender}`;
}

function snowflakeCompare(a: string, b: string) {
  try {
    const av = BigInt(String(a || "0"));
    const bv = BigInt(String(b || "0"));
    if (av === bv) return 0;
    return av > bv ? 1 : -1;
  } catch {
    return String(a || "").localeCompare(String(b || ""));
  }
}

function resolveDiscordBotToken(cfg: any) {
  const account = cfg?.channels?.discord?.accounts?.default;
  return String(account?.token || cfg?.channels?.discord?.token || process.env.DISCORD_BOT_TOKEN || "").trim();
}

async function discordApiJson(path: string, token: string, signal?: AbortSignal) {
  const res = await fetch(`https://discord.com/api/v10${path}`, {
    method: "GET",
    signal,
    headers: {
      authorization: `Bot ${token}`,
      "content-type": "application/json"
    }
  });
  if (!res.ok) {
    const body = await res.text().catch(() => "");
    throw new Error(`discord api ${path} failed: ${res.status} ${body}`);
  }
  return res.json();
}

async function downloadDiscordAttachmentToLocal(api: any, token: string, attachment: DiscordMessageAttachment) {
  const url = String(attachment?.url || attachment?.proxy_url || "").trim();
  if (!url) return null;
  const res = await fetch(url, {
    method: "GET",
    headers: {
      authorization: `Bot ${token}`
    }
  });
  if (!res.ok) {
    const body = await res.text().catch(() => "");
    throw new Error(`discord attachment fetch failed: ${res.status} ${body}`);
  }
  const buffer = Buffer.from(await res.arrayBuffer());
  const contentType = String(attachment?.content_type || res.headers.get("content-type") || "").trim() || undefined;
  const originalFilename = String(attachment?.filename || "").trim() || undefined;
  const saveMediaBuffer = api?.runtime?.channel?.media?.saveMediaBuffer;
  if (typeof saveMediaBuffer !== "function") {
    throw new Error("runtime media saver unavailable");
  }
  const saved = await saveMediaBuffer(buffer, contentType, "inbound", buffer.byteLength + 1024, originalFilename);
  return {
    path: String(saved?.path || "").trim(),
    mediaType: contentType,
    filename: originalFilename
  };
}

async function relayDiscordChannelMessage(api: any, token: string, message: DiscordChannelMessage, records: SubscriptionRecord[]) {
  const channelId = String(message.channel_id || "").trim();
  const formatted = formatDiscordSubscriptionForward({
    message,
    channelName: records.find((record) => record.channelName)?.channelName
  });
  if (!channelId) return;
  const signature = [
    channelId,
    String(message.id || "(no-msgid)"),
    String(message.timestamp || ""),
    normalizeInboundText(message.content || "")
  ].join("|");
  if (!rememberSubscriptionFanoutSignature(signature)) return;

  const clients = new Map<string, any>();
  const attachments = Array.isArray(message.attachments) ? message.attachments : [];
  const hasDownloadableAttachments = attachments.some((attachment) =>
    Boolean(String(attachment?.url || attachment?.proxy_url || "").trim())
  );
  for (const record of records) {
    const account = resolveAccount(api?.config ?? {}, record.accountId);
    const clientKey = `${record.accountId}:${account.sidecarBaseUrl}:${account.authToken || ""}`;
    let client = clients.get(clientKey);
    if (!client) {
      client = createSidecarClient(account);
      clients.set(clientKey, client);
    }
    try {
      let deliveredMedia = false;
      if (hasDownloadableAttachments) {
        const caption = formatDiscordSubscriptionCaption({
          message,
          channelName: record.channelName
        });
        let firstAttachment = true;
        for (const attachment of attachments) {
          const downloaded = await downloadDiscordAttachmentToLocal(api, token, attachment);
          if (!downloaded?.path) continue;
          await sendBeagleSubscriptionMedia({
            client,
            record,
            caption: firstAttachment ? caption : "",
            mediaPath: downloaded.path,
            mediaType: downloaded.mediaType,
            filename: downloaded.filename
          });
          firstAttachment = false;
          deliveredMedia = true;
          api?.logger?.info?.(
            `[beagle] subscription fanout sendMedia channel=${channelId} peer=${record.deliveryPeerId || record.peerId} file=${downloaded.filename || "(unnamed)"}`
          );
        }
      }
      if (!deliveredMedia && formatted) {
        await sendBeagleSubscriptionText({ client, record, text: formatted });
        api?.logger?.info?.(
          `[beagle] subscription fanout delivered channel=${channelId} peer=${record.deliveryPeerId || record.peerId}`
        );
      }
    } catch (err: any) {
      api?.logger?.warn?.(
        `[beagle] subscription fanout failed channel=${channelId} peer=${record.deliveryPeerId || record.peerId}: ${String(err)}`
      );
    }
  }
}

async function runDiscordSubscriptionPollLoop(api: any, abortSignal: AbortSignal, log?: any) {
  const pollerKey = "default";
  abortDiscordSubscriptionPoller(pollerKey);
  const externalController = new AbortController();
  const relayAbort = () => externalController.abort();
  abortSignal.addEventListener("abort", relayAbort, { once: true });
  discordSubscriptionPollControllers.set(pollerKey, externalController);

  const lastSeenByChannel = new Map<string, string>();

  try {
    while (!externalController.signal.aborted) {
      try {
        const cfg = api?.config ?? {};
        const token = resolveDiscordBotToken(cfg);
        if (!token) {
          log?.warn?.("[beagle] discord subscription relay disabled: missing Discord bot token");
          await sleep(5000);
          continue;
        }

        const store = loadSubscriptionStore();
        const subscribedChannelIds = [...new Set(store.records.map((record) => String(record.channelId || "").trim()).filter(Boolean))];
        if (!subscribedChannelIds.length) {
          await sleep(3000);
          continue;
        }

        for (const channelId of subscribedChannelIds) {
          const channelRecords = store.records.filter((record) => record.channelId === channelId);
          if (!channelRecords.length) continue;
          const lastSeen = lastSeenByChannel.get(channelId);
          const query = lastSeen
            ? `/channels/${channelId}/messages?after=${encodeURIComponent(lastSeen)}&limit=100`
            : `/channels/${channelId}/messages?limit=20`;
          const messages = (await discordApiJson(query, token, externalController.signal)) as DiscordChannelMessage[];
          if (!Array.isArray(messages) || !messages.length) continue;

          const ordered = [...messages].sort((a, b) => snowflakeCompare(String(a.id || ""), String(b.id || "")));
          if (!lastSeen) {
            const newestId = ordered[ordered.length - 1]?.id;
            if (newestId) lastSeenByChannel.set(channelId, String(newestId));
            continue;
          }

          for (const message of ordered) {
            const messageId = String(message?.id || "").trim();
            if (!messageId) continue;
            await relayDiscordChannelMessage(api, token, { ...message, channel_id: channelId }, channelRecords);
            lastSeenByChannel.set(channelId, messageId);
          }
        }
      } catch (err: any) {
        if (externalController.signal.aborted) break;
        log?.warn?.(`[beagle] discord subscription poll failed; retrying: ${String(err)}`);
        await sleep(2000);
      }

      await sleep(2500);
    }
  } finally {
    abortSignal.removeEventListener("abort", relayAbort);
    abortDiscordSubscriptionPoller(pollerKey, externalController);
  }
}

function abortInboundPoller(accountId: string, controller?: AbortController) {
  const active = inboundPollControllers.get(accountId);
  if (!active) return;
  if (controller && active !== controller) return;
  active.abort();
  inboundPollControllers.delete(accountId);
}

function abortInboundPollers() {
  for (const controller of inboundPollControllers.values()) controller.abort();
  inboundPollControllers.clear();
}

async function runInboundPollLoop({
  api,
  accountId,
  account,
  abortSignal,
  setStatus,
  log
}: {
  api: any;
  accountId: string;
  account: BeagleAccount;
  abortSignal: AbortSignal;
  setStatus?: (next: any) => void;
  log?: any;
}) {
  const client = createSidecarClient(account);
  const controller = new AbortController();
  const relayAbort = () => controller.abort();
  abortSignal.addEventListener("abort", relayAbort, { once: true });
  abortInboundPoller(accountId);
  inboundPollControllers.set(accountId, controller);

  const safeWarn = (message: string) => {
    try {
      log?.warn?.(message);
    } catch {
      // ignore logger failures
    }
  };

  const safeSetStatus = (next: any) => {
    if (!setStatus) return;
    try {
      setStatus(next);
    } catch (statusErr: any) {
      safeWarn(`[${accountId}] beagle status update failed: ${String(statusErr)}`);
    }
  };

  try {
    while (!controller.signal.aborted) {
      const requestController = new AbortController();
      const relayRequestAbort = () => requestController.abort();
      controller.signal.addEventListener("abort", relayRequestAbort, { once: true });
      try {
        const events = await client.pollEvents(requestController.signal);
        safeSetStatus({
          accountId,
          connected: true,
          lastError: null,
          lastConnectedAt: Date.now()
        });
        if (events.length > 0) {
          api?.logger?.info?.(`[beagle] events=${events.length} account=${accountId}`);
          safeSetStatus({
            accountId,
            lastEventAt: Date.now()
          });
        }
        for (const ev of events) {
          await handleInboundEvent(api, accountId, account, ev);
          safeSetStatus({
            accountId,
            lastInboundAt: Date.now()
          });
        }
      } catch (err: any) {
        if (controller.signal.aborted) break;
        const msg = err?.message ?? String(err);
        safeWarn(`[${accountId}] beagle sidecar poll failed; retrying: ${msg}`);
        safeSetStatus({
          accountId,
          connected: false,
          lastError: msg,
          lastDisconnect: {
            at: Date.now(),
            error: msg
          }
        });
        await sleep(1000);
      } finally {
        controller.signal.removeEventListener("abort", relayRequestAbort);
      }
    }
  } finally {
    safeWarn(`[${accountId}] beagle inbound poll loop exited (aborted=${controller.signal.aborted} upstreamAborted=${abortSignal.aborted})`);
    abortSignal.removeEventListener("abort", relayAbort);
    abortInboundPoller(accountId, controller);
  }
}

// OpenClaw plugin entrypoint. Types are intentionally loose to avoid
// coupling to a specific SDK version.
export default function register(api: any) {
  const pluginId = "beagle";

  const channelPlugin = {
    id: pluginId,
    meta: {
      id: pluginId,
      label: "Beagle Chat",
      selectionLabel: "Beagle Chat",
      blurb: "Beagle Chat provider via local sidecar daemon.",
      docsPath: "/channels/beagle",
      aliases: ["beagle-chat", "beagle"]
    },
    capabilities: {
      chatTypes: ["direct", "group"],
      media: true
    },
    config: {
      listAccountIds: (cfg: any) => {
        const ids = Object.keys(cfg?.channels?.beagle?.accounts ?? {});
        return ids.length > 0 ? ids : ["default"];
      },
      defaultAccountId: () => "default",
      isConfigured: () => true,
      describeAccount: () => ({ configured: true }),
      resolveAccount: (cfg: any, accountId?: string) => resolveAccount(cfg, accountId)
    },
    messaging: {
      normalizeTarget: (raw: any) => normalizePeerId(raw),
      targetResolver: {
        hint: "Use a Beagle peer ID, e.g. 7yZfjNbbUwtQuHQzkAH87ditQUzULhjfPz1LYdRnsqwh",
        looksLikeId: (raw: string, normalized: string) => isLikelyBeaglePeerId(normalized || raw)
      }
    },
    outbound: {
      deliveryMode: "direct",
      sendText: async ({ cfg, accountId, chatId, text }: any) => {
        const account = resolveAccount(cfg, accountId);
        const client = createSidecarClient(account);
        await client.sendText({ peer: normalizePeerId(chatId), text });
        return { ok: true };
      },
      sendMedia: async ({ cfg, accountId, chatId, caption, mediaPath, mediaUrl, mediaType, filename }: any) => {
        const account = resolveAccount(cfg, accountId);
        const client = createSidecarClient(account);
        const media = coerceMediaInput(mediaPath, mediaUrl);
        await client.sendMedia({
          peer: normalizePeerId(chatId),
          caption,
          mediaPath: media.mediaPath,
          mediaUrl: media.mediaUrl,
          mediaType,
          filename
        });
        return { ok: true };
      }
    },
    gateway: {
      startAccount: async ({ cfg, accountId, account, abortSignal, setStatus, log }: any) => {
        const resolvedAccount = account ?? resolveAccount(cfg, accountId);
        log?.info?.(`[${accountId}] beagle inbound poll start (${resolvedAccount.sidecarBaseUrl})`);
        await runInboundPollLoop({
          api,
          accountId,
          account: resolvedAccount,
          abortSignal,
          setStatus,
          log
        });
      },
      stopAccount: async ({ accountId }: any) => {
        abortInboundPoller(accountId);
      }
    }
  };

  api.registerChannel({ plugin: channelPlugin });
  api.registerService({
    id: "beagle-discord-subscription-relay",
    start: async () => {
      const controller = new AbortController();
      discordSubscriptionPollControllers.set("service", controller);
      void runDiscordSubscriptionPollLoop(api, controller.signal, api?.logger);
    },
    stop: async () => {
      abortDiscordSubscriptionPoller("service");
      abortDiscordSubscriptionPollers();
    }
  });
}

function resolveAccount(cfg: any, accountId?: string): BeagleAccount {
  const acc = cfg?.channels?.beagle?.accounts?.[accountId ?? "default"];
  if (!acc) {
    return {
      accountId: accountId ?? "default",
      sidecarBaseUrl: "http://127.0.0.1:39091"
    };
  }
  return {
    accountId: accountId ?? "default",
    sidecarBaseUrl: "http://127.0.0.1:39091",
    ...acc
  };
}

async function handleInboundEvent(api: any, accountId: string, account: BeagleAccount, ev: any) {
  try {
    const core = api?.runtime;
    api?.logger?.info?.(`[beagle] handleInboundEvent peer=${String(ev?.peer ?? "")} text_len=${(ev?.text ?? "").length}`);
    if (!core?.channel?.reply?.dispatchReplyWithBufferedBlockDispatcher) {
      api?.logger?.warn?.("[beagle] runtime channel reply dispatcher unavailable");
      return;
    }

    const rawBody = normalizeInboundText(ev?.text ?? "");
    const inboundMediaUrl = ev?.mediaUrl ?? "";
    const inboundMediaPath = ev?.mediaPath ?? "";
    const inboundMediaType = ev?.mediaType ?? "";
    const inboundFilename = ev?.filename ?? "";
    const inboundSize = ev?.size ?? 0;
    const hasInboundMedia = Boolean(inboundMediaUrl || inboundMediaPath);
    const rawTs = ev?.ts ?? 0;
    const normalizedTs = rawTs > 10_000_000_000_000 ? Math.floor(rawTs / 1000) : rawTs;
    const baseTimestamp = normalizedTs || Date.now();
    const dispatchStart = Date.now();

    const peerId = String(ev?.peer ?? "");
    const normalizedPeerId = normalizePeerId(peerId);
    const parsedGroup = parseCarrierGroupInbound(rawBody);
    const trustedPeerSet = toNormalizedSet(
      [...(account.groupPeers ?? []), ...(account.trustedGroupPeers ?? [])],
      normalizePeerId
    );
    const trustedAddressSet = toNormalizedSet(
      account.trustedGroupAddresses ?? [],
      normalizeAddress
    );
    const requireTrustedGroup = Boolean(account.requireTrustedGroup);
    const hasTrustConfig = trustedPeerSet.size > 0 || trustedAddressSet.size > 0;

    let trustedGroup = false;
    if (parsedGroup) {
      trustedGroup =
        trustedPeerSet.has(normalizedPeerId) || trustedAddressSet.has(parsedGroup.groupAddress);
      if (requireTrustedGroup && !trustedGroup) {
        api?.logger?.warn?.(
          `[beagle] reject untrusted group envelope peer=${normalizedPeerId} address=${parsedGroup.groupAddress}`
        );
      } else if (!trustedGroup && hasTrustConfig) {
        api?.logger?.warn?.(
          `[beagle] accept unsigned group envelope (not allowlisted) peer=${normalizedPeerId} address=${parsedGroup.groupAddress}`
        );
      }
    }

    const isGroup = Boolean(parsedGroup) && (!requireTrustedGroup || trustedGroup);
    const conversationId = isGroup ? (parsedGroup?.groupUserId || normalizedPeerId) : normalizedPeerId;
    const timestamp = isGroup ? (parsedGroup?.timestamp || baseTimestamp) : baseTimestamp;
    const senderId = isGroup ? (parsedGroup?.originUserId || peerId) : peerId;
    const senderName = isGroup ? (parsedGroup?.originNickname || senderId) : senderId;
    const canonicalId = canonicalPeerId(conversationId) || conversationId;
    const eventId = String(ev?.msgId ?? "");
    const dedupeKey = [
      accountId,
      canonicalId,
      eventId || "(no-msgid)",
      String(timestamp),
      rawBody,
      inboundMediaPath,
      inboundMediaUrl,
      inboundFilename
    ].join("|");
    if (!rememberInboundSignature(dedupeKey)) {
      api?.logger?.info?.(`[beagle] skip duplicate inbound event peer=${normalizedPeerId} msgId=${eventId || "(none)"}`);
      return;
    }

    let route: any;
    try {
      route = core.channel.routing.resolveAgentRoute({
        cfg: api?.config ?? {},
        channel: "beagle",
        accountId,
        peer: {
          kind: isGroup ? "group" : "dm",
          id: conversationId
        }
      });
    } catch (groupRouteErr: any) {
      if (!isGroup) throw groupRouteErr;
      api?.logger?.warn?.(
        `[beagle] group route fallback to dm: ${String(groupRouteErr)}`
      );
      route = core.channel.routing.resolveAgentRoute({
        cfg: api?.config ?? {},
        channel: "beagle",
        accountId,
        peer: {
          kind: "dm",
          id: conversationId
        }
      });
    }

    const sessionScope = isGroup ? "group" : "dm";
    const sessionKey = `beagle:${accountId}:${sessionScope}:${conversationId}`;

    const storePath = core.channel.session.resolveStorePath(api?.config?.session?.store, {
      agentId: route.agentId
    });
    const previousTimestamp = core.channel.session.readSessionUpdatedAt({
      storePath,
      sessionKey
    });
    const mediaHint = hasInboundMedia
      ? `Image attached${inboundFilename ? `: ${inboundFilename}` : ""}.`
      : "";
    const body = parsedGroup?.messageText || rawBody || mediaHint;

    const localCommandReply = maybeHandleLocalSubscriptionCommand({
      accountId,
      account,
      peerId: conversationId,
      deliveryPeerId: normalizedPeerId,
      isGroup,
      groupUserId: parsedGroup?.groupUserId,
      groupAddress: parsedGroup?.groupAddress,
      groupName: parsedGroup?.groupNickname,
      body
    });
    if (localCommandReply) {
      const client = createSidecarClient(account);
      await sendBeagleSubscriptionText({
        client,
        record: normalizeSubscriptionRecord({
          accountId,
          peerId: conversationId,
          deliveryPeerId: normalizedPeerId,
          chatType: isGroup ? "group" : "dm",
          channelId: "__local__",
          groupUserId: parsedGroup?.groupUserId,
          groupAddress: parsedGroup?.groupAddress,
          groupName: parsedGroup?.groupNickname,
          createdAt: new Date().toISOString(),
          updatedAt: new Date().toISOString()
        }) as SubscriptionRecord,
        text: localCommandReply
      });
      return;
    }
    const groupMetaNote = isGroup
      ? `\n[Beagle group context]\n` +
        `ChatType: group\n` +
        `GroupUserId: ${parsedGroup?.groupUserId || normalizedPeerId}\n` +
        `GroupAddress: ${parsedGroup?.groupAddress || ""}\n` +
        `GroupName: ${parsedGroup?.groupNickname || ""}\n` +
        `OriginalSenderUserId: ${parsedGroup?.originUserId || ""}\n` +
        `OriginalSenderName: ${parsedGroup?.originNickname || ""}`
      : "";
    const bodyForAgent =
      `${body}${groupMetaNote}\n\n` +
      `[Beagle channel note: do not call the "message" tool for this conversation. ` +
      `Reply with plain text. If you need to send media, include one line: MEDIA:<local_file_path>]`;

    const shouldComputeCommandAuthorized =
      core?.channel?.commands?.shouldComputeCommandAuthorized?.(body, api?.config ?? {}) === true;
    // Beagle currently trusts inbound peers through carrier friendship/trusted-group checks.
    // Mark command messages as authorized so built-in slash commands (/status, /help, etc.) can run.
    const commandAuthorized = shouldComputeCommandAuthorized ? true : undefined;

    const ctxPayload = core.channel.reply.finalizeInboundContext({
      Body: body,
      BodyForAgent: bodyForAgent,
      BodyForCommands: body,
      RawBody: rawBody || body,
      CommandBody: body,
      MediaUrl: inboundMediaUrl,
      MediaPath: inboundMediaPath,
      MediaType: inboundMediaType,
      MediaUrls: inboundMediaUrl ? [inboundMediaUrl] : undefined,
      MediaPaths: inboundMediaPath ? [inboundMediaPath] : undefined,
      MediaTypes: inboundMediaType ? [inboundMediaType] : undefined,
      Filename: inboundFilename,
      MediaSize: inboundSize,
      From: `beagle:${senderId}`,
      To: `beagle:${conversationId}`,
      SessionKey: sessionKey,
      AccountId: route.accountId,
      ChatType: isGroup ? "group" : "direct",
      ConversationLabel: isGroup
        ? (parsedGroup?.groupNickname || parsedGroup?.groupAddress || conversationId)
        : peerId,
      SenderId: senderId,
      SenderDisplayName: senderName,
      GroupId: isGroup ? (parsedGroup?.groupUserId || conversationId) : undefined,
      GroupAddress: isGroup ? parsedGroup?.groupAddress : undefined,
      GroupName: isGroup ? parsedGroup?.groupNickname : undefined,
      GroupPeer: isGroup ? normalizedPeerId : undefined,
      OriginSenderId: isGroup ? parsedGroup?.originUserId : undefined,
      OriginSenderName: isGroup ? parsedGroup?.originNickname : undefined,
      CarrierGroupEnvelope: isGroup ? parsedGroup?.envelope : undefined,
      Provider: "beagle",
      Surface: "beagle",
      MessageSid: ev?.msgId,
      Timestamp: timestamp,
      OriginatingChannel: "beagle",
      OriginatingTo: `beagle:${conversationId}`,
      CommandAuthorized: commandAuthorized
    });

    api?.logger?.info?.(
      `[beagle] route agent=${route.agentId} chat_type=${isGroup ? "group" : "direct"} session=${sessionKey} media_path=${inboundMediaPath || "(none)"} media_type=${inboundMediaType || "(none)"}`
    );
    await core.channel.session.recordInboundSession({
      storePath,
      sessionKey,
      ctx: ctxPayload,
      onRecordError: (err: any) => {
        api?.logger?.warn?.(`[beagle] failed updating session meta: ${String(err)}`);
      }
    });

    const client = createSidecarClient(account);
    if (!hasInboundMedia && isPictureRequest(body)) {
      const fallbackImage = resolveDefaultReplyImagePath();
      if (fallbackImage) {
        api?.logger?.info?.(`[beagle] picture shortcut sendMedia path=${fallbackImage}`);
        await client.sendMedia({
          peer: normalizedPeerId,
          caption: "",
          mediaPath: fallbackImage,
          mediaType: "image/jpeg",
          filename: basename(fallbackImage)
        });
        return;
      }
      api?.logger?.warn?.("[beagle] picture shortcut requested but no default image found");
    }

    const statusEnabled = envTruthy((globalThis as any)?.process?.env?.BEAGLE_STATUS_ENABLED);
    const statusTtlMs = Number((globalThis as any)?.process?.env?.BEAGLE_STATUS_TTL_MS || 12000);
    const statusMinIntervalMs = Number((globalThis as any)?.process?.env?.BEAGLE_STATUS_MIN_INTERVAL_MS || 2500);
    let lastStatusState = "";
    let lastStatusPhase = "";
    let lastStatusSentAt = 0;
    const sentStatusKeys = new Set<string>();
    const sendStatus = async (state: AgentStatusState, phase = "", force = false) => {
      if (!statusEnabled) return;
      const statusKey = `${state}\u0001${phase}`;
      if (sentStatusKeys.has(statusKey)) return;
      const now = Date.now();
      if (!force && state === lastStatusState && phase === lastStatusPhase && now - lastStatusSentAt < statusMinIntervalMs) {
        return;
      }
      const seq = `${now.toString(36)}${Math.random().toString(36).slice(2, 8)}`;
      const text = buildStatusText({
        state,
        phase,
        ttlMs: statusTtlMs > 0 ? statusTtlMs : 12000,
        seq,
        isGroup,
        parsedGroup
      });
      try {
        await client.sendStatus({
          peer: normalizedPeerId,
          state,
          phase,
          ttlMs: statusTtlMs > 0 ? statusTtlMs : 12000,
          chatType: isGroup ? "group" : "direct",
          groupUserId: parsedGroup?.groupUserId,
          groupAddress: parsedGroup?.groupAddress,
          groupName: parsedGroup?.groupNickname,
          seq
        });
        lastStatusState = state;
        lastStatusPhase = phase;
        lastStatusSentAt = now;
        sentStatusKeys.add(statusKey);
        api?.logger?.info?.(`[beagle] sendStatus state=${state} phase=${phase || "(none)"} text_len=${text.length}`);
      } catch (statusErr: any) {
        api?.logger?.warn?.(`[beagle] sendStatus failed state=${state}: ${String(statusErr)}`);
      }
    };

    let deliveredCount = 0;
    let dispatchTimedOut = false;
    const deliveredFingerprints = new Set<string>();
    const dispatchPromise = core.channel.reply.dispatchReplyWithBufferedBlockDispatcher({
      ctx: ctxPayload,
      cfg: api?.config ?? {},
      dispatcherOptions: {
        deliver: async (payload: any, info: any) => {
          if (dispatchTimedOut) {
            api?.logger?.warn?.("[beagle] suppress outbound payload after timeout");
            return;
          }
          deliveredCount += 1;
          if (info?.kind === "final") await sendStatus("sending", "final", true);
          const text = payload?.text ?? "";
          api?.logger?.info?.(`[beagle] deliver kind=${payload?.kind ?? "unknown"} text_len=${text.length}`);
          let mediaUrl = payload?.mediaUrl || (Array.isArray(payload?.mediaUrls) ? payload.mediaUrls[0] : "");
          let mediaPath = payload?.mediaPath || payload?.filePath || payload?.attachmentPath || "";
          const mediaType = payload?.mediaType || payload?.mimeType || payload?.mimetype || "";
          const filename = payload?.filename || payload?.fileName || "";
          let captionText = text;
          if (!mediaUrl && !mediaPath && text) {
            const parsed = parseMediaDirectiveInText(text);
            if (parsed.mediaPath) {
              mediaPath = parsed.mediaPath;
              captionText = parsed.caption;
              api?.logger?.info?.("[beagle] MEDIA directive parsed mediaPath=" + mediaPath);
            }
          }
          if (mediaPath) {
            const resolvedMediaPath = resolveLocalMediaPath(mediaPath);
            if (resolvedMediaPath !== mediaPath) {
              api?.logger?.info?.("[beagle] resolved mediaPath=" + resolvedMediaPath + " from=" + mediaPath);
              mediaPath = resolvedMediaPath;
            }
          }
          const media = coerceMediaInput(mediaPath, mediaUrl);
          mediaPath = media.mediaPath;
          mediaUrl = media.mediaUrl;
          const replyText = isGroup
            ? buildCarrierGroupReplyText(captionText, parsedGroup as ParsedGroupInbound)
            : captionText;
          const fingerprint = [
            payload?.kind ?? "unknown",
            replyText || "",
            mediaPath || "",
            mediaUrl || "",
            mediaType || "",
            filename || ""
          ].join("\u0001");
          if (deliveredFingerprints.has(fingerprint)) {
            api?.logger?.warn?.("[beagle] skip duplicate outbound payload");
            return;
          }
          deliveredFingerprints.add(fingerprint);
          if (mediaUrl || mediaPath) {
            api?.logger?.info?.("[beagle] sendMedia");
            await client.sendMedia({
              peer: normalizedPeerId,
              caption: replyText,
              mediaUrl,
              mediaPath,
              mediaType,
              filename
            });
            return;
          }
          if (replyText) {
            api?.logger?.info?.("[beagle] sendText");
            await client.sendText({ peer: normalizedPeerId, text: replyText });
          }
        },
        onReplyStart: () => {
          void sendStatus("typing", "thinking");
        },
        onIdle: () => {
          void sendStatus("idle", "complete", false);
        },
        onCleanup: () => {
          void sendStatus("idle", "complete", false);
        },
        onError: (err: any, info: any) => {
          void sendStatus("error", String(info?.kind ?? "unknown"), true);
          api?.logger?.warn?.(`[beagle] reply failed (${info?.kind ?? "unknown"}): ${String(err)}`);
        }
      }
    });
    const env = (globalThis as any)?.process?.env ?? {};
    const timeoutMs = Number(env.BEAGLE_DISPATCH_TIMEOUT_MS || 90000);
    const timeoutPromise = new Promise((_, reject) =>
      setTimeout(() => {
        dispatchTimedOut = true;
        reject(new Error(`dispatch timeout after ${timeoutMs}ms`));
      }, timeoutMs)
    );
    const extractDispatchResultText = (result: any): string => {
      const pickFirst = (...values: any[]): string => {
        for (const value of values) {
          if (typeof value === "string" && value.trim()) return value.trim();
        }
        return "";
      };
      const outputTextItems = Array.isArray(result?.response?.output)
        ? result.response.output
            .flatMap((item: any) => (Array.isArray(item?.content) ? item.content : []))
            .map((content: any) => (content?.type === "output_text" ? String(content?.text ?? "") : ""))
            .filter(Boolean)
        : [];
      return pickFirst(
        result?.finalText,
        result?.replyText,
        result?.text,
        result?.content,
        result?.final?.text,
        result?.final?.content,
        result?.response?.output_text,
        outputTextItems.join("\n")
      );
    };
    let queuedFinal: any;
    try {
      const result: any = await Promise.race([dispatchPromise, timeoutPromise]);
      queuedFinal = result?.queuedFinal;
      api?.logger?.info?.(`[beagle] dispatch queuedFinal=${queuedFinal} duration_ms=${Date.now() - dispatchStart}`);
      if (queuedFinal !== true && deliveredCount === 0) {
        const recoveredText = extractDispatchResultText(result);
        if (recoveredText) {
          const replyText = isGroup
            ? buildCarrierGroupReplyText(recoveredText, parsedGroup as ParsedGroupInbound)
            : recoveredText;
          api?.logger?.warn?.(
            `[beagle] dispatch ended without queued final; recovered text from result len=${recoveredText.length}`
          );
          await client.sendText({ peer: normalizedPeerId, text: replyText });
          await sendStatus("idle", "recovered_result_text", true);
          return;
        }
        const fallbackText = isGroup
          ? buildCarrierGroupReplyText(
              "I received your message but did not produce a final reply. Please resend.",
              parsedGroup as ParsedGroupInbound
            )
          : "I received your message but did not produce a final reply. Please resend.";
        api?.logger?.warn?.("[beagle] dispatch completed without outbound reply; sending fallback");
        await client.sendText({ peer: normalizedPeerId, text: fallbackText });
        await sendStatus("idle", "fallback", true);
      }
    } catch (err: any) {
      api?.logger?.warn?.(`[beagle] dispatch failed duration_ms=${Date.now() - dispatchStart}: ${String(err)}`);
      if (deliveredCount === 0) {
        const fallbackText = isGroup
          ? buildCarrierGroupReplyText(
              "I hit a timeout before final reply. Please resend once.",
              parsedGroup as ParsedGroupInbound
            )
          : "I hit a timeout before final reply. Please resend once.";
        api?.logger?.warn?.("[beagle] dispatch failed without outbound reply; sending timeout fallback");
        await client.sendText({ peer: normalizedPeerId, text: fallbackText });
        await sendStatus("idle", "timeout_fallback", true);
      }
    }
  } catch (err: any) {
    api?.logger?.warn?.(`[beagle] handleInboundEvent failed: ${String(err)}`);
  }
}

function sleep(ms: number) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
