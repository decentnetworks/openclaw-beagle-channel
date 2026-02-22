export type BeagleAccount = {
  accountId: string;
  enabled?: boolean;
  sidecarBaseUrl: string;
  authToken?: string;
  trustedGroupPeers?: string[];
  trustedGroupAddresses?: string[];
  groupPeers?: string[];
  requireTrustedGroup?: boolean;
};

export type SidecarEvent = {
  peer: string;
  text?: string;
  mediaUrl?: string;
  mediaPath?: string;
  mediaType?: string;
  filename?: string;
  size?: number;
  ts?: number;
  msgId?: string;
};

export type SendTextRequest = {
  peer: string;
  text: string;
};

export type SendMediaRequest = {
  peer: string;
  caption?: string;
  mediaUrl?: string;
  mediaPath?: string;
  mediaType?: string;
  filename?: string;
};

export type SendStatusRequest = {
  peer: string;
  state: "typing" | "thinking" | "tool" | "sending" | "idle" | "error";
  phase?: string;
  ttlMs?: number;
  chatType?: "direct" | "group";
  groupUserId?: string;
  groupAddress?: string;
  groupName?: string;
  seq?: string;
};

export type SidecarClient = {
  sendText(req: SendTextRequest): Promise<void>;
  sendMedia(req: SendMediaRequest): Promise<void>;
  sendStatus(req: SendStatusRequest): Promise<void>;
  pollEvents(signal: AbortSignal): Promise<SidecarEvent[]>;
};

export function createSidecarClient(account: BeagleAccount): SidecarClient {
  async function request<T>(path: string, init?: RequestInit): Promise<T> {
    const headers: Record<string, string> = {
      "content-type": "application/json"
    };
    if (account.authToken) headers.authorization = `Bearer ${account.authToken}`;

    const res = await fetch(`${account.sidecarBaseUrl}${path}`, {
      ...init,
      headers: { ...headers, ...(init?.headers as Record<string, string> | undefined) }
    });

    if (!res.ok) {
      const body = await res.text().catch(() => "");
      throw new Error(`sidecar ${path} failed: ${res.status} ${body}`);
    }

    if (res.status === 204) return undefined as T;
    return (await res.json()) as T;
  }

  return {
    async sendText(req) {
      await request("/sendText", {
        method: "POST",
        body: JSON.stringify(req)
      });
    },
    async sendMedia(req) {
      await request("/sendMedia", {
        method: "POST",
        body: JSON.stringify(req)
      });
    },
    async sendStatus(req) {
      await request("/sendStatus", {
        method: "POST",
        body: JSON.stringify(req)
      });
    },
    async pollEvents(signal) {
      return request<SidecarEvent[]>("/events", {
        method: "GET",
        signal
      });
    }
  };
}
