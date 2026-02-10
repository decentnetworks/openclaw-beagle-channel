import { createSidecarClient, type BeagleAccount } from "./sidecarClient.js";

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
    outbound: {
      deliveryMode: "direct",
      sendText: async ({ cfg, accountId, chatId, text }: any) => {
        const account = resolveAccount(cfg, accountId);
        const client = createSidecarClient(account);
        await client.sendText({ peer: chatId, text });
        return { ok: true };
      },
      sendMedia: async ({ cfg, accountId, chatId, caption, mediaPath, mediaUrl, mediaType, filename }: any) => {
        const account = resolveAccount(cfg, accountId);
        const client = createSidecarClient(account);
        await client.sendMedia({
          peer: chatId,
          caption,
          mediaPath,
          mediaUrl,
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

        // Background poll loop for inbound messages.
        (async () => {
          while (!controller.signal.aborted) {
            try {
              const events = await client.pollEvents(controller.signal);
              if (events.length > 0) {
                api?.logger?.info?.(`[beagle] events=${events.length} account=${accountId}`);
              }
              for (const ev of events) {
                await handleInboundEvent(api, accountId, account, ev);
              }
            } catch (err: any) {
              const msg = err?.message ?? String(err);
              api?.logger?.warn?.(`beagle sidecar poll failed; retrying: ${msg}`);
              await sleep(1000);
            }
          }
        })();
      }
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

    const rawBody = ev?.text ?? "";
    const rawTs = ev?.ts ?? 0;
    const normalizedTs = rawTs > 10_000_000_000_000 ? Math.floor(rawTs / 1000) : rawTs;
    const timestamp = normalizedTs || Date.now();

    const route = core.channel.routing.resolveAgentRoute({
      cfg: api?.config ?? {},
      channel: "beagle",
      accountId,
      peer: {
        kind: "dm",
        id: String(ev?.peer ?? "")
      }
    });

    const storePath = core.channel.session.resolveStorePath(api?.config?.session?.store, {
      agentId: route.agentId
    });
    const previousTimestamp = core.channel.session.readSessionUpdatedAt({
      storePath,
      sessionKey: route.sessionKey
    });
    const body = rawBody;

    const ctxPayload = core.channel.reply.finalizeInboundContext({
      Body: body,
      BodyForAgent: body,
      BodyForCommands: rawBody,
      RawBody: rawBody,
      CommandBody: rawBody,
      From: `beagle:${String(ev?.peer ?? "")}`,
      To: `beagle:${String(ev?.peer ?? "")}`,
      SessionKey: route.sessionKey,
      AccountId: route.accountId,
      ChatType: "direct",
      ConversationLabel: String(ev?.peer ?? ""),
      SenderId: String(ev?.peer ?? ""),
      Provider: "beagle",
      Surface: "beagle",
      MessageSid: ev?.msgId,
      Timestamp: timestamp,
      OriginatingChannel: "beagle",
      OriginatingTo: `beagle:${String(ev?.peer ?? "")}`
    });

    api?.logger?.info?.(`[beagle] route agent=${route.agentId} session=${route.sessionKey}`);
    await core.channel.session.recordInboundSession({
      storePath,
      sessionKey: ctxPayload.SessionKey ?? route.sessionKey,
      ctx: ctxPayload,
      onRecordError: (err: any) => {
        api?.logger?.warn?.(`[beagle] failed updating session meta: ${String(err)}`);
      }
    });

    const client = createSidecarClient(account);
    const { queuedFinal } = await core.channel.reply.dispatchReplyWithBufferedBlockDispatcher({
      ctx: ctxPayload,
      cfg: api?.config ?? {},
      dispatcherOptions: {
        deliver: async (payload: any) => {
          const text = payload?.text ?? "";
          api?.logger?.info?.(`[beagle] deliver kind=${payload?.kind ?? "unknown"} text_len=${text.length}`);
          const mediaUrl = payload?.mediaUrl || (Array.isArray(payload?.mediaUrls) ? payload.mediaUrls[0] : "");
          if (mediaUrl) {
            api?.logger?.info?.("[beagle] sendMedia");
            await client.sendMedia({
              peer: String(ev?.peer ?? ""),
              caption: text,
              mediaUrl
            });
            return;
          }
          if (text) {
            api?.logger?.info?.("[beagle] sendText");
            await client.sendText({ peer: String(ev?.peer ?? ""), text });
          }
        },
        onError: (err: any, info: any) => {
          api?.logger?.warn?.(`[beagle] reply failed (${info?.kind ?? "unknown"}): ${String(err)}`);
        }
      }
    });
    api?.logger?.info?.(`[beagle] dispatch queuedFinal=${queuedFinal}`);
  } catch (err: any) {
    api?.logger?.warn?.(`[beagle] handleInboundEvent failed: ${String(err)}`);
  }
}

function sleep(ms: number) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
