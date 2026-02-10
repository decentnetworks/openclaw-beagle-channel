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

      for (const accountId of accounts) {
        const account = resolveAccount(cfg, accountId);
        if (account.enabled === false) continue;

        const client = createSidecarClient(account);
        const controller = new AbortController();

        // Background poll loop for inbound messages.
        (async () => {
          while (!controller.signal.aborted) {
            try {
              const events = await client.pollEvents(controller.signal);
              for (const ev of events) {
                await emitIncoming(api, {
                  channelId: pluginId,
                  accountId,
                  chatId: ev.peer,
                  text: ev.text ?? "",
                  messageId: ev.msgId,
                  timestamp: ev.ts,
                  mediaType: ev.mediaType,
                  mediaPath: ev.mediaPath,
                  mediaUrl: ev.mediaUrl,
                  filename: ev.filename
                });
              }
            } catch (err: any) {
              api?.logger?.warn?.({ err }, "beagle sidecar poll failed; retrying");
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

async function emitIncoming(api: any, payload: any) {
  const runtimeEmitter = api?.runtime?.channels?.emitIncoming;
  if (runtimeEmitter) return runtimeEmitter(payload);

  const gatewayEmitter = api?.gateway?.channels?.emitIncoming;
  if (gatewayEmitter) return gatewayEmitter(payload);

  api?.logger?.warn?.("No inbound emitter found for beagle channel");
}

function sleep(ms: number) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
