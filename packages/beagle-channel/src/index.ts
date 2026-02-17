import { createSidecarClient, type BeagleAccount } from "./sidecarClient.js";
declare const require: any;
declare const process: any;
const { existsSync } = require("fs");
const { readdirSync, statSync } = require("fs");
const { isAbsolute, resolve, join, basename } = require("path");

const INBOUND_SEEN_MAX = 10_000;
const inboundSeen = new Set<string>();
const inboundSeenOrder: string[] = [];
const inboundPollControllers = new Set<AbortController>();

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

function abortInboundPollers() {
  for (const controller of inboundPollControllers) controller.abort();
  inboundPollControllers.clear();
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
      chatTypes: ["direct"],
      media: true
    },
    config: {
      listAccountIds: (cfg: any) => Object.keys(cfg?.channels?.beagle?.accounts ?? {}),
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
    }
  };

  api.registerChannel({ plugin: channelPlugin });

  api.registerService({
    id: "beagle-inbound",
    start: async () => {
      // Guard against duplicate poll loops if service start is invoked again.
      abortInboundPollers();
      const cfg = api?.config ?? {};
      const accountIds = Object.keys(cfg?.channels?.beagle?.accounts ?? {});
      const accounts = accountIds.length ? accountIds : ["default"];
      api?.logger?.info?.(`[beagle] inbound service start (accounts=${accounts.join(",")})`);
      try {
        const runtimeKeys = Object.keys(api?.runtime?.channels ?? {});
        const gatewayKeys = Object.keys(api?.gateway?.channels ?? {});
        api?.logger?.info?.(`[beagle] runtime.channels keys=${runtimeKeys.join(",") || "(none)"}`);
        api?.logger?.info?.(`[beagle] gateway.channels keys=${gatewayKeys.join(",") || "(none)"}`);
      } catch {
        // ignore introspection errors
      }

      for (const accountId of accounts) {
        const account = resolveAccount(cfg, accountId);
        if (account.enabled === false) continue;
        api?.logger?.info?.(`[beagle] polling ${accountId} at ${account.sidecarBaseUrl}`);

        const client = createSidecarClient(account);
        const controller = new AbortController();
        inboundPollControllers.add(controller);

        // Background poll loop for inbound messages.
        (async () => {
          while (!controller.signal.aborted) {
            const requestController = new AbortController();
            const relayAbort = () => requestController.abort();
            controller.signal.addEventListener("abort", relayAbort, { once: true });
            try {
              const events = await client.pollEvents(requestController.signal);
              if (events.length > 0) {
                api?.logger?.info?.(`[beagle] events=${events.length} account=${accountId}`);
              }
              for (const ev of events) {
                await handleInboundEvent(api, accountId, account, ev);
              }
            } catch (err: any) {
              if (controller.signal.aborted) break;
              const msg = err?.message ?? String(err);
              api?.logger?.warn?.(`beagle sidecar poll failed; retrying: ${msg}`);
              await sleep(1000);
            } finally {
              controller.signal.removeEventListener("abort", relayAbort);
            }
          }
          inboundPollControllers.delete(controller);
        })();
      }
    },
    stop: async () => {
      abortInboundPollers();
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
    const timestamp = normalizedTs || Date.now();
    const dispatchStart = Date.now();

    const peerId = String(ev?.peer ?? "");
    const normalizedPeerId = normalizePeerId(peerId);
    const canonicalId = canonicalPeerId(peerId) || normalizedPeerId;
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
    const route = core.channel.routing.resolveAgentRoute({
      cfg: api?.config ?? {},
      channel: "beagle",
      accountId,
      peer: {
        kind: "dm",
        id: normalizedPeerId
      }
    });

    const sessionKey = `beagle:${accountId}:${canonicalId}`;

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
    const body = rawBody || mediaHint;
    const bodyForAgent =
      `${body}\n\n` +
      `[Beagle channel note: do not call the "message" tool for this conversation. ` +
      `Reply with plain text. If you need to send media, include one line: MEDIA:<local_file_path>]`;

    const ctxPayload = core.channel.reply.finalizeInboundContext({
      Body: body,
      BodyForAgent: bodyForAgent,
      BodyForCommands: rawBody || body,
      RawBody: rawBody || body,
      CommandBody: rawBody || body,
      MediaUrl: inboundMediaUrl,
      MediaPath: inboundMediaPath,
      MediaType: inboundMediaType,
      MediaUrls: inboundMediaUrl ? [inboundMediaUrl] : undefined,
      MediaPaths: inboundMediaPath ? [inboundMediaPath] : undefined,
      MediaTypes: inboundMediaType ? [inboundMediaType] : undefined,
      Filename: inboundFilename,
      MediaSize: inboundSize,
      From: `beagle:${peerId}`,
      To: `beagle:${normalizedPeerId}`,
      SessionKey: sessionKey,
      AccountId: route.accountId,
      ChatType: "direct",
      ConversationLabel: peerId,
      SenderId: peerId,
      Provider: "beagle",
      Surface: "beagle",
      MessageSid: ev?.msgId,
      Timestamp: timestamp,
      OriginatingChannel: "beagle",
      OriginatingTo: `beagle:${normalizedPeerId}`
    });

    api?.logger?.info?.(
      `[beagle] route agent=${route.agentId} session=${sessionKey} media_path=${inboundMediaPath || "(none)"} media_type=${inboundMediaType || "(none)"}`
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
    if (!hasInboundMedia && isPictureRequest(rawBody)) {
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

    const dispatchPromise = core.channel.reply.dispatchReplyWithBufferedBlockDispatcher({
      ctx: ctxPayload,
      cfg: api?.config ?? {},
      dispatcherOptions: {
        deliver: async (payload: any) => {
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
          if (mediaUrl || mediaPath) {
            api?.logger?.info?.("[beagle] sendMedia");
            await client.sendMedia({
              peer: normalizedPeerId,
              caption: captionText,
              mediaUrl,
              mediaPath,
              mediaType,
              filename
            });
            return;
          }
          if (captionText) {
            api?.logger?.info?.("[beagle] sendText");
            await client.sendText({ peer: normalizedPeerId, text: captionText });
          }
        },
        onError: (err: any, info: any) => {
          api?.logger?.warn?.(`[beagle] reply failed (${info?.kind ?? "unknown"}): ${String(err)}`);
        }
      }
    });
    const env = (globalThis as any)?.process?.env ?? {};
    const timeoutMs = Number(env.BEAGLE_DISPATCH_TIMEOUT_MS || 30000);
    const timeoutPromise = new Promise((_, reject) =>
      setTimeout(() => reject(new Error(`dispatch timeout after ${timeoutMs}ms`)), timeoutMs)
    );
    let queuedFinal: any;
    try {
      const result: any = await Promise.race([dispatchPromise, timeoutPromise]);
      queuedFinal = result?.queuedFinal;
      api?.logger?.info?.(`[beagle] dispatch queuedFinal=${queuedFinal} duration_ms=${Date.now() - dispatchStart}`);
    } catch (err: any) {
      api?.logger?.warn?.(`[beagle] dispatch failed duration_ms=${Date.now() - dispatchStart}: ${String(err)}`);
    }
  } catch (err: any) {
    api?.logger?.warn?.(`[beagle] handleInboundEvent failed: ${String(err)}`);
  }
}

function sleep(ms: number) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
